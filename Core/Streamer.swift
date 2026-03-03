import SwiftUI
import Foundation
import AVFoundation
import VideoToolbox
import Network
import CoreMedia
import UIKit
import CoreMotion

// MARK: - VTCompression output callback (C-style)
private func vtOutputCallback(_ outputCallbackRefCon: UnsafeMutableRawPointer?,
                              _ sourceFrameRefCon: UnsafeMutableRawPointer?,
                              _ status: OSStatus,
                              _ infoFlags: VTEncodeInfoFlags,
                              _ sampleBuffer: CMSampleBuffer?) {
    guard status == noErr, let sbuf = sampleBuffer, let refCon = outputCallbackRefCon else { return }
    let streamer = Unmanaged<Streamer>.fromOpaque(refCon).takeUnretainedValue()
    streamer.handleEncodedSampleBuffer(sbuf)
}

final class Streamer: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate {

    // MARK: Published/UI
    @Published var status: String = "Init…"
    @Published var isRunning: Bool = false
    @Published var isBusy: Bool = false
    @Published var metrics: String = ""
    @Published private(set) var configRevision: UInt64 = 0
    private let configRevisionLock = NSLock()
    private var configRevisionValue: UInt64 = 0

    // MARK: Queues
    private let controlQ = DispatchQueue(label: "Streamer.control")
    private let sessionQ = DispatchQueue(label: "Streamer.session") // capture + encode

    // MARK: Capture
    private let session = AVCaptureSession()
    private var device: AVCaptureDevice?
    private let videoOutput = AVCaptureVideoDataOutput()

    // MARK: Encoder
    private var vtSession: VTCompressionSession?

    // MARK: Réseau
    private var listener: NWListener?
    private var connection: NWConnection?
    private var controlListener: NWListener?
    private var activeControlPort: UInt16?
    private let maxControlLineBytes = 16 * 1024

    private struct ControlClient {
        var connection: NWConnection
        var buffer: Data
    }
    private var controlClients: [ObjectIdentifier: ControlClient] = [:]

    // MARK: Réglages (courants)
    var listenPort: UInt16 = 5000
    var targetWidth: Int = 1920
    var targetHeight: Int = 1080
    var targetFPS: Double = 120
    var intraOnly: Bool = false
    var bitrate: Int = 35_000_000
    var outputProtocol: OutputProtocol = .annexb
    var orientation: AVCaptureVideoOrientation = .portrait
    var autoRotate: Bool = false
    var profile: H264Profile = .high
    var entropy: H264Entropy = .cabac
    var autoBitrate: Bool = true
    var minBitrate: Int = 6_000_000
    var maxBitrate: Int = 120_000_000

    // MARK: Anti-dérive / sécurité
    fileprivate var sentCodecHeader = false
    fileprivate var forceIDRNext = false
    private var inFlightSends = 0
    private let maxInFlightSends = 3

    // Stats
    private var statsTimer: DispatchSourceTimer?
    private var bytesWindow: Int = 0
    private var framesWindow: Int = 0
    private var droppedWindow: Int = 0
    private var sendLatencyMsWindow: Double = 0
    private var sendEventsWindow: Int = 0
    private var lastFps: Int = 0
    private var lastDropped: Int = 0
    private var lastMbps: Double = 0
    private var lastTxMs: Double = 0

    // State
    private enum State { case idle, starting, running, stopping }
    private var state: State = .idle

    // Auto-rotate
    private var orientationObserver: NSObjectProtocol?
    private var didBeginOrientationNotifications = false
    private var orientationPoller: DispatchSourceTimer?
    private let motionManager = CMMotionManager()
    private var motionOrientation: AVCaptureVideoOrientation?

    override init() {
        super.init()
        controlQ.async {
            self.ensureControlAPI(on: self.controlPort(forVideoPort: self.listenPort))
        }
    }

    // MARK: Config API
    func setConfig(from p: PendingConfig) {
        listenPort   = p.port
        targetWidth  = p.resolution.width
        targetHeight = p.resolution.height
        targetFPS    = p.fps
        bitrate      = Int(p.bitrate)
        intraOnly    = p.intraOnly
        outputProtocol = p.outputProtocol
        orientation  = p.orientation
        autoRotate   = p.autoRotate
        profile      = p.profile
        entropy      = p.entropy
        autoBitrate  = p.autoBitrate
        minBitrate   = Int(p.minBitrate)
        maxBitrate   = Int(p.maxBitrate)

        normalizeBitrateBounds()
        if profile == .baseline { entropy = .cavlc }
        bitrate = clampBitrate(bitrate)
        bumpConfigRevision()
    }

    private func bumpConfigRevision() {
        configRevisionLock.lock()
        configRevisionValue &+= 1
        let rev = configRevisionValue
        configRevisionLock.unlock()
        DispatchQueue.main.async { self.configRevision = rev }
    }

    private func currentConfigRevision() -> UInt64 {
        configRevisionLock.lock()
        let rev = configRevisionValue
        configRevisionLock.unlock()
        return rev
    }

    /// Applique `pending` : live si possible, sinon restart propre.
    func applyOrRestart(with new: PendingConfig) {
        controlQ.async { self.applyOrRestartLocked(with: new) }
    }

    private func applyOrRestartLocked(with new: PendingConfig) {
        // Détecte si un rebuild est requis (change le "bitstream shape")
        let needsRestart =
            new.resolution.width  != self.targetWidth  ||
            new.resolution.height != self.targetHeight ||
            new.profile           != self.profile      ||
            new.entropy           != self.entropy      ||
            new.outputProtocol    != self.outputProtocol ||
            new.port              != self.listenPort

        setConfig(from: new)

        guard isRunning else {
            // Keep remote control reachable even when idle.
            ensureControlAPI(on: controlPort(forVideoPort: listenPort))
            return
        }

        if needsRestart {
            restart()
        } else {
            applyLiveTweaks()
        }
    }

    /// Modifs à chaud (bitrate, fps, GOP, orientation, auto-rotate)
    private func applyLiveTweaks() {
        sessionQ.async {
            if let vt = self.vtSession {
                self.applyEncoderRateControlProperties(vt)
            }
            if let dev = self.device {
                do {
                    try dev.lockForConfiguration()
                    let ts = CMTime(value: 1, timescale: CMTimeScale(max(1.0, self.targetFPS)))
                    dev.activeVideoMinFrameDuration = ts
                    dev.activeVideoMaxFrameDuration = ts
                    dev.unlockForConfiguration()
                } catch { /* ignore */ }
            }
            self.applyCaptureOrientation(self.orientation)
            if self.autoRotate {
                self.installOrientationObserverIfNeeded()
                DispatchQueue.main.async { [weak self] in self?.applyDeviceOrientation() }
            } else {
                self.removeOrientationObserver()
            }
            self.forceIDRNext = true // re-sync côté lecteur
            DispatchQueue.main.async { self.status = "Live updated (bitrate/fps/GOP/orientation)" }
        }
    }

    // MARK: Lifecycle
    func requestKeyframe() {
        controlQ.async { [weak self] in self?.forceIDRNext = true }
    }

    func start() {
        guard state == .idle else { return }
        DispatchQueue.main.async { self.isBusy = true; self.status = "Checking camera..." }

        ensureCameraAuthorized { [weak self] granted in
            guard let self = self else { return }
            guard granted else {
                DispatchQueue.main.async {
                    self.isBusy = false
                    self.status = "Accès caméra refusé (Réglages > Confidentialité > Caméra)"
                }
                return
            }

            self.controlQ.async {
                guard self.state == .idle else { return }
                self.state = .starting

                self.sentCodecHeader = false
                self.forceIDRNext = true
                self.inFlightSends = 0
                self.bytesWindow = 0
                self.framesWindow = 0
                self.droppedWindow = 0
                self.sendLatencyMsWindow = 0
                self.sendEventsWindow = 0

                DispatchQueue.main.async {
                    UIApplication.shared.isIdleTimerDisabled = true
                }

                self.setupTCP(on: self.listenPort)
                self.ensureControlAPI(on: self.controlPort(forVideoPort: self.listenPort))

                self.sessionQ.async {
                    self.setupCapture()
                    self.setupEncoder(width: self.targetWidth, height: self.targetHeight)
                    self.startStats()
                    DispatchQueue.main.async {
                        if self.autoRotate { self.installOrientationObserverIfNeeded() }
                        self.state = .running
                        self.isRunning = true
                        self.isBusy = false
                        self.status = "Running"
                    }
                }
            }
        }
    }

    func stop() {
        controlQ.async {
            guard self.state == .running else { return }
            self.state = .stopping
            DispatchQueue.main.async { self.isBusy = true }

            self.sessionQ.sync {
                self.videoOutput.setSampleBufferDelegate(nil, queue: nil)
                self.session.stopRunning()
                if let vt = self.vtSession {
                    self.vtSession = nil
                    VTCompressionSessionCompleteFrames(vt, untilPresentationTimeStamp: .invalid)
                    VTCompressionSessionInvalidate(vt)
                }
            }

            self.connection?.cancel(); self.connection = nil
            self.listener?.cancel(); self.listener = nil
            self.inFlightSends = 0

            DispatchQueue.main.async {
                UIApplication.shared.isIdleTimerDisabled = false
            }

            self.stopStats()
            self.removeOrientationObserver()

            self.state = .idle
            DispatchQueue.main.async {
                self.isRunning = false
                self.isBusy = false
                self.status = "Stopped"
                self.metrics = ""
            }
        }
    }

    func restart() {
        controlQ.async {
            switch self.state {
            case .running:
                self.stop()
                self.controlQ.asyncAfter(deadline: .now() + 0.3) { self.start() }
            case .idle:
                self.start()
            default:
                break
            }
        }
    }

    // MARK: Permissions
    private func ensureCameraAuthorized(_ completion: @escaping (Bool) -> Void) {
        let st = AVCaptureDevice.authorizationStatus(for: .video)
        switch st {
        case .authorized: completion(true)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                DispatchQueue.main.async { completion(granted) }
            }
        default: completion(false)
        }
    }

    // MARK: TCP
    private func makeTcpParameters() -> NWParameters {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let params = NWParameters(tls: nil, tcp: tcp)
        params.allowLocalEndpointReuse = true
        return params
    }

    private func setupTCP(on port: UInt16) {
        do {
            guard let p = NWEndpoint.Port(rawValue: port) else {
                DispatchQueue.main.async { self.status = "Invalid video port \(port)" }
                return
            }
            let params = makeTcpParameters()
            let lst = try NWListener(using: params, on: p)
            lst.stateUpdateHandler = { [weak self] st in
                DispatchQueue.main.async { self?.status = "Video listener(\(port)): \(st)" }
            }
            lst.newConnectionHandler = { [weak self] conn in
                guard let self = self else { return }
                self.connection?.cancel()
                self.connection = conn
                self.sentCodecHeader = false
                self.forceIDRNext = true
                self.inFlightSends = 0
                conn.stateUpdateHandler = { [weak self] st in
                    if case .failed = st { self?.inFlightSends = 0 }
                    if case .cancelled = st { self?.inFlightSends = 0 }
                    DispatchQueue.main.async { self?.status = "Video client: \(st)" }
                }
                conn.start(queue: self.sessionQ)
            }
            lst.start(queue: controlQ)
            self.listener = lst
        } catch {
            DispatchQueue.main.async { self.status = "Video TCP error: \(error.localizedDescription)" }
        }
    }

    // MARK: Control API (JSON lines)
    private func ensureControlAPI(on port: UInt16) {
        if controlListener != nil, activeControlPort == port {
            return
        }
        setupControlAPI(on: port)
    }

    private func setupControlAPI(on port: UInt16) {
        do {
            guard let p = NWEndpoint.Port(rawValue: port) else {
                DispatchQueue.main.async { self.status = "Invalid control port \(port)" }
                return
            }

            stopControlAPI()

            let params = makeTcpParameters()
            let lst = try NWListener(using: params, on: p)
            lst.newConnectionHandler = { [weak self] conn in
                self?.attachControlClient(conn)
            }
            lst.start(queue: controlQ)
            controlListener = lst
            activeControlPort = port
        } catch {
            activeControlPort = nil
            DispatchQueue.main.async { self.status = "Control TCP error: \(error.localizedDescription)" }
        }
    }

    private func stopControlAPI() {
        for (_, client) in controlClients {
            client.connection.cancel()
        }
        controlClients.removeAll()
        controlListener?.cancel()
        controlListener = nil
        activeControlPort = nil
    }

    private func attachControlClient(_ conn: NWConnection) {
        controlQ.async {
            let id = ObjectIdentifier(conn)
            self.controlClients[id] = ControlClient(connection: conn, buffer: Data())

            conn.stateUpdateHandler = { [weak self] st in
                guard let self = self else { return }
                self.controlQ.async {
                    switch st {
                    case .failed, .cancelled:
                        self.removeControlClient(conn)
                    default:
                        break
                    }
                }
            }

            conn.start(queue: self.controlQ)
            self.sendControlResponse([
                "type": "hello",
                "schema": 1,
                "video_port": Int(self.listenPort),
                "control_port": Int(self.controlPort(forVideoPort: self.listenPort))
            ], to: conn)
            self.receiveControlData(from: conn)
        }
    }

    private func removeControlClient(_ conn: NWConnection) {
        controlClients.removeValue(forKey: ObjectIdentifier(conn))
        conn.cancel()
    }

    private func receiveControlData(from conn: NWConnection) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: maxControlLineBytes) { [weak self] data, _, isComplete, error in
            guard let self = self else { return }
            self.controlQ.async {
                let id = ObjectIdentifier(conn)
                guard var client = self.controlClients[id] else { return }

                if let data = data, !data.isEmpty {
                    client.buffer.append(data)
                    self.controlClients[id] = client
                    self.processControlBuffer(for: id)
                }

                if isComplete || error != nil {
                    self.removeControlClient(conn)
                    return
                }

                self.receiveControlData(from: conn)
            }
        }
    }

    private func processControlBuffer(for id: ObjectIdentifier) {
        guard var client = controlClients[id] else { return }

        while let lineEnd = client.buffer.firstIndex(of: 0x0A) {
            let lineData = client.buffer.prefix(upTo: lineEnd)
            client.buffer.removeSubrange(client.buffer.startIndex...lineEnd)

            guard !lineData.isEmpty else { continue }
            guard lineData.count <= maxControlLineBytes else {
                sendControlResponse(["type": "error", "error": "line_too_large"], to: client.connection)
                continue
            }
            guard let line = String(data: lineData, encoding: .utf8) else {
                sendControlResponse(["type": "error", "error": "invalid_utf8"], to: client.connection)
                continue
            }

            handleControlLine(line, from: client.connection)
        }

        if client.buffer.count > maxControlLineBytes {
            client.buffer.removeAll(keepingCapacity: false)
            sendControlResponse(["type": "error", "error": "buffer_overflow"], to: client.connection)
        }

        controlClients[id] = client
    }

    private func handleControlLine(_ line: String, from conn: NWConnection) {
        guard let lineData = line.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: lineData) as? [String: Any] else {
            sendControlResponse(["type": "error", "error": "invalid_json"], to: conn)
            return
        }

        let cmd = (obj["cmd"] as? String)?.lowercased() ?? ""
        switch cmd {
        case "ping":
            sendControlResponse(["type": "pong", "ts": Date().timeIntervalSince1970], to: conn)
        case "status", "get_status":
            sendControlResponse(makeStatusPayload(), to: conn)
        case "start":
            start()
            sendControlResponse(["type": "ok", "cmd": "start"], to: conn)
        case "stop":
            stop()
            sendControlResponse(["type": "ok", "cmd": "stop"], to: conn)
        case "restart":
            restart()
            sendControlResponse(["type": "ok", "cmd": "restart"], to: conn)
        case "keyframe", "request_keyframe":
            requestKeyframe()
            sendControlResponse(["type": "ok", "cmd": "keyframe"], to: conn)
        case "apply":
            let patch = (obj["config"] as? [String: Any]) ?? obj
            applyControlPatch(patch, replyTo: conn)
        default:
            sendControlResponse(["type": "error", "error": "unknown_cmd", "cmd": cmd], to: conn)
        }
    }

    private func applyControlPatch(_ patch: [String: Any], replyTo conn: NWConnection) {
        var p = PendingConfig(from: self)

        if let v = intFromAny(patch["port"]), (1024...65534).contains(v) { p.port = UInt16(v) }
        if let v = stringFromAny(patch["resolution"]), let r = parseResolution(v) { p.resolution = r }
        if let v = doubleFromAny(patch["fps"]) { p.fps = max(1, min(240, v)) }
        if let v = doubleFromAny(patch["bitrate"]) { p.bitrate = max(250_000, v) }
        if let v = doubleFromAny(patch["bitrate_mbps"]) { p.bitrate = max(250_000, v * 1_000_000) }
        if let v = boolFromAny(patch["intra_only"]) { p.intraOnly = v }
        if let v = stringFromAny(patch["protocol"]), let proto = parseOutputProtocol(v) { p.outputProtocol = proto }
        if let v = stringFromAny(patch["orientation"]), let ori = parseOrientation(v) { p.orientation = ori }
        if let v = boolFromAny(patch["auto_rotate"]) { p.autoRotate = v }
        if let v = stringFromAny(patch["profile"]), let pr = parseProfile(v) { p.profile = pr }
        if let v = stringFromAny(patch["entropy"]), let en = parseEntropy(v) { p.entropy = en }
        if let v = boolFromAny(patch["auto_bitrate"]) { p.autoBitrate = v }
        if let v = doubleFromAny(patch["min_bitrate"]) { p.minBitrate = max(250_000, v) }
        if let v = doubleFromAny(patch["max_bitrate"]) { p.maxBitrate = max(250_000, v) }

        if p.minBitrate > p.maxBitrate { swap(&p.minBitrate, &p.maxBitrate) }
        if p.profile == .baseline { p.entropy = .cavlc }
        p.bitrate = min(max(p.bitrate, p.minBitrate), p.maxBitrate)

        applyOrRestartLocked(with: p)
        sendControlResponse([
            "type": "ok",
            "cmd": "apply",
            "config_revision": currentConfigRevision(),
            "config": makeStatusConfigPayload()
        ], to: conn)
    }

    private func sendControlResponse(_ payload: [String: Any], to conn: NWConnection) {
        guard JSONSerialization.isValidJSONObject(payload),
              let body = try? JSONSerialization.data(withJSONObject: payload) else {
            return
        }
        var line = body
        line.append(0x0A)
        conn.send(content: line, completion: .contentProcessed { _ in })
    }

    private func makeStatusPayload() -> [String: Any] {
        let revision = currentConfigRevision()
        return [
            "type": "status",
            "state": stateString(),
            "running": isRunning,
            "busy": isBusy,
            "status": status,
            "metrics": metrics,
            "config_revision": revision,
            "video_port": Int(listenPort),
            "control_port": Int(controlPort(forVideoPort: listenPort)),
            "stats": [
                "fps": lastFps,
                "dropped": lastDropped,
                "mbps": lastMbps,
                "tx_ms": lastTxMs,
                "bitrate": bitrate
            ],
            "config": makeStatusConfigPayload()
        ]
    }

    private func makeStatusConfigPayload() -> [String: Any] {
        [
            "resolution": resolutionString(width: targetWidth, height: targetHeight),
            "width": targetWidth,
            "height": targetHeight,
            "fps": targetFPS,
            "bitrate": bitrate,
            "intra_only": intraOnly,
            "protocol": outputProtocolString(outputProtocol),
            "orientation": orientationString(orientation),
            "auto_rotate": autoRotate,
            "profile": profileString(profile),
            "entropy": entropyString(entropy),
            "auto_bitrate": autoBitrate,
            "min_bitrate": minBitrate,
            "max_bitrate": maxBitrate
        ]
    }

    // MARK: Camera
    private func setupCapture() {
        session.beginConfiguration()
        session.sessionPreset = .inputPriority

        // Keep restart idempotent.
        for input in session.inputs { session.removeInput(input) }
        for output in session.outputs { session.removeOutput(output) }

        guard let cam = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
            DispatchQueue.main.async { self.status = "Caméra introuvable" }
            session.commitConfiguration()
            return
        }
        device = cam

        do {
            let input = try AVCaptureDeviceInput(device: cam)
            guard session.canAddInput(input) else {
                DispatchQueue.main.async { self.status = "Input refusé" }
                session.commitConfiguration()
                return
            }
            session.addInput(input)
        } catch {
            DispatchQueue.main.async { self.status = "Erreur input: \(error.localizedDescription)" }
            session.commitConfiguration()
            return
        }

        let maxF = maxSupportedFPS(width: targetWidth, height: targetHeight)
        if targetFPS > maxF { targetFPS = maxF }
        if !selectFormat(device: cam, width: targetWidth, height: targetHeight, fps: targetFPS) {
            _ = selectFormat(device: cam, width: targetWidth, height: targetHeight, fps: min(60.0, maxF))
        }

        videoOutput.alwaysDiscardsLateVideoFrames = true
        videoOutput.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        ]
        videoOutput.setSampleBufferDelegate(self, queue: sessionQ)
        guard session.canAddOutput(videoOutput) else {
            DispatchQueue.main.async { self.status = "Output refusé" }
            session.commitConfiguration()
            return
        }
        session.addOutput(videoOutput)

        applyCaptureOrientation(orientation)

        session.commitConfiguration()
        session.startRunning()
        DispatchQueue.main.async {
            self.status = "Capture OK (\(self.targetWidth)x\(self.targetHeight) @\(Int(self.targetFPS)) fps tentative)"
        }
    }

    private func selectFormat(device: AVCaptureDevice, width: Int, height: Int, fps: Double) -> Bool {
        var chosen: AVCaptureDevice.Format?
        for f in device.formats {
            let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            guard dims.width == width && dims.height == height else { continue }
            if f.videoSupportedFrameRateRanges.contains(where: { $0.maxFrameRate + 0.001 >= fps }) {
                chosen = f; break
            }
        }
        guard let fmt = chosen else { return false }
        do {
            try device.lockForConfiguration()
            let ts = CMTime(value: 1, timescale: CMTimeScale(fps))
            device.activeFormat = fmt
            device.activeVideoMinFrameDuration = ts
            device.activeVideoMaxFrameDuration = ts
            device.unlockForConfiguration()
            DispatchQueue.main.async { self.status = "Format fixé: \(width)x\(Int(height)) @\(Int(fps))" }
            return true
        } catch {
            DispatchQueue.main.async { self.status = "Format err: \(error.localizedDescription)" }
            return false
        }
    }

    /// FPS max pour une résolution donnée
    func maxSupportedFPS(width: Int, height: Int) -> Double {
        let dev = device ?? AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        guard let d = dev else { return 60 }
        var maxF: Double = 30
        for f in d.formats {
            let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            guard dims.width == width && dims.height == height else { continue }
            for r in f.videoSupportedFrameRateRanges { maxF = max(maxF, r.maxFrameRate) }
        }
        return maxF
    }

    // MARK: Encoder
    private func setupEncoder(width: Int, height: Int) {
        let refcon = UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque())
        let rc = VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height),
                                            codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                            imageBufferAttributes: nil, compressedDataAllocator: nil,
                                            outputCallback: vtOutputCallback, refcon: refcon, compressionSessionOut: &vtSession)
        guard rc == noErr, let vt = vtSession else {
            DispatchQueue.main.async { self.status = "VTCompressionSessionCreate failed \(rc)" }
            return
        }

        // Profil
        let profileCF: CFString = {
            switch profile {
            case .baseline: return kVTProfileLevel_H264_Baseline_AutoLevel
            case .main:     return kVTProfileLevel_H264_Main_AutoLevel
            case .high:     return kVTProfileLevel_H264_High_AutoLevel
            }
        }()
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_ProfileLevel, value: profileCF)

        // Entropy (Baseline => CAVLC)
        let useCabac = (profile != .baseline) && (entropy == .cabac)
        let entropyCF: CFString = useCabac ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_H264EntropyMode, value: entropyCF)

        // Temps réel + pas de B
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_RealTime,             value: kCFBooleanTrue)
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_MaxFrameDelayCount,   value: NSNumber(value: 1))

        applyEncoderRateControlProperties(vt)

        VTCompressionSessionPrepareToEncodeFrames(vt)
        DispatchQueue.main.async {
            self.statusUpdate("Encoder prêt (\(self.profile.label) \(useCabac ? "CABAC" : "CAVLC"), \(self.bitrate/1_000_000) Mb/s)")
        }
    }

    private func statusUpdate(_ s: String) { DispatchQueue.main.async { self.status = s } }

    private func applyEncoderRateControlProperties(_ vt: VTCompressionSession) {
        let boundedBitrate = clampBitrate(bitrate)
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_AverageBitRate,
                             value: NSNumber(value: boundedBitrate))
        let limits: [NSNumber] = [NSNumber(value: boundedBitrate/8), NSNumber(value: 1)]
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_DataRateLimits,
                             value: limits as CFArray)
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_ExpectedFrameRate,
                             value: NSNumber(value: Int32(max(1.0, targetFPS))))
        let gop: Int32
        if intraOnly {
            gop = 1
        } else {
            // Keep GOP short enough to recover quickly if a frame is lost in transport.
            gop = Int32(max(4, min(30, Int(targetFPS / 4.0))))
        }
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_MaxKeyFrameInterval,
                             value: NSNumber(value: gop))
        VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_AllowTemporalCompression,
                             value: intraOnly ? kCFBooleanFalse : kCFBooleanTrue)
    }

    // MARK: Capture → Encode (back-pressure : limite les envois en vol)
    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard self.connection != nil else { return }
        if inFlightSends >= maxInFlightSends {
            droppedWindow += 1
            return // évite de remplir la file quand le lien plafonne
        }

        guard let vt = vtSession,
              let imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        var frameProps: CFDictionary?
        if forceIDRNext {
            let dict: [String: Any] = [kVTEncodeFrameOptionKey_ForceKeyFrame as String: true]
            frameProps = dict as CFDictionary
            forceIDRNext = false
        }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        var flags: VTEncodeInfoFlags = []
        let st = VTCompressionSessionEncodeFrame(vt,
                                                 imageBuffer: imageBuffer,
                                                 presentationTimeStamp: pts,
                                                 duration: .invalid,
                                                 frameProperties: frameProps,
                                                 sourceFrameRefcon: nil,
                                                 infoFlagsOut: &flags)
        if st != noErr {
            droppedWindow += 1
            statusUpdate("Encode err \(st)")
        }
    }

    // MARK: Encoded output → TCP
    fileprivate func handleEncodedSampleBuffer(_ sbuf: CMSampleBuffer) {
        guard let conn = connection,
              let dataBuffer = CMSampleBufferGetDataBuffer(sbuf) else { return }

        // keyframe ?
        var isKey = true
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sbuf, createIfNecessary: false) as? [Any],
           let dict = arr.first as? [String: Any],
           let notSync = dict[kCMSampleAttachmentKey_NotSync as String] as? Bool {
            isKey = !notSync
        }

        var payload = Data()
        if let fmt = CMSampleBufferGetFormatDescription(sbuf) {
            switch outputProtocol {
            case .annexb:
                if let spspps = H264Packer.annexBParameterSets(from: fmt) {
                    if isKey { payload.append(spspps) }      // toujours SPS/PPS sur IDR
                    else if !sentCodecHeader { payload.append(spspps); sentCodecHeader = true }
                }
                if let nals = H264Packer.annexBFromSampleBuffer(dataBuffer: dataBuffer) { payload.append(nals) }
            case .avcc:
                if let spspps = H264Packer.avccParameterSets(from: fmt) {
                    if isKey { payload.append(spspps) }
                    else if !sentCodecHeader { payload.append(spspps); sentCodecHeader = true }
                }
                if let raw = H264Packer.rawFromSampleBuffer(dataBuffer: dataBuffer) { payload.append(raw) }
            }
        }

        if !payload.isEmpty {
            bytesWindow += payload.count
            framesWindow += 1
            inFlightSends += 1
            let sendStartNs = DispatchTime.now().uptimeNanoseconds
            conn.send(content: payload, completion: .contentProcessed { [weak self] err in
                guard let self = self else { return }
                let endNs = DispatchTime.now().uptimeNanoseconds
                let sendMs = Double(endNs - sendStartNs) / 1_000_000.0
                self.sendLatencyMsWindow += sendMs
                self.sendEventsWindow += 1
                if err != nil { self.droppedWindow += 1 }
                if self.inFlightSends > 0 { self.inFlightSends -= 1 }
            })
        } else {
            droppedWindow += 1
        }
    }

    // MARK: Stats
    private func startStats() {
        let t = DispatchSource.makeTimerSource(queue: sessionQ)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self = self else { return }

            let fps = self.framesWindow
            let dropped = self.droppedWindow
            let mbps = Double(self.bytesWindow) * 8.0 / 1_000_000.0
            let avgSendMs = self.sendEventsWindow > 0 ? (self.sendLatencyMsWindow / Double(self.sendEventsWindow)) : 0
            let metric = String(format: "~%2d fps | ~%.1f Mb/s | drop:%d | tx:%.1f ms", fps, mbps, dropped, avgSendMs)
                + (self.autoBitrate ? " | abr:\(self.bitrate/1_000_000)M" : "")
            DispatchQueue.main.async { self.metrics = metric }

            self.lastFps = fps
            self.lastDropped = dropped
            self.lastMbps = mbps
            self.lastTxMs = avgSendMs

            self.updateAdaptiveBitrate(sentFrames: fps, droppedFrames: dropped, avgSendMs: avgSendMs)
            if dropped > 0 {
                self.forceIDRNext = true
            }

            self.framesWindow = 0
            self.bytesWindow = 0
            self.droppedWindow = 0
            self.sendLatencyMsWindow = 0
            self.sendEventsWindow = 0
        }
        t.resume()
        self.statsTimer = t
    }

    private func stopStats() {
        statsTimer?.cancel(); statsTimer = nil
        framesWindow = 0; bytesWindow = 0
        droppedWindow = 0
        sendLatencyMsWindow = 0
        sendEventsWindow = 0
        lastFps = 0
        lastDropped = 0
        lastMbps = 0
        lastTxMs = 0
    }

    private func applyCaptureOrientation(_ newOrientation: AVCaptureVideoOrientation) {
        guard let conn = videoOutput.connection(with: .video) else { return }
        if #available(iOS 17.0, *) {
            let angle = rotationAngle(for: newOrientation)
            if conn.isVideoRotationAngleSupported(angle) {
                conn.videoRotationAngle = angle
                return
            }
        }
        if conn.isVideoOrientationSupported {
            conn.videoOrientation = newOrientation
        }
    }

    private func rotationAngle(for orientation: AVCaptureVideoOrientation) -> CGFloat {
        switch orientation {
        case .portrait: return 0
        case .landscapeRight: return 90
        case .landscapeLeft: return 270
        case .portraitUpsideDown: return 180
        @unknown default: return 0
        }
    }

    // MARK: Auto-rotate (notif + poller + CoreMotion fallback)
    private func installOrientationObserverIfNeeded() {
        removeOrientationObserver()
        guard autoRotate else { return }
        DispatchQueue.main.async {
            if !self.didBeginOrientationNotifications {
                UIDevice.current.beginGeneratingDeviceOrientationNotifications()
                self.didBeginOrientationNotifications = true
            }
            self.orientationObserver = NotificationCenter.default.addObserver(
                forName: UIDevice.orientationDidChangeNotification,
                object: nil, queue: .main
            ) { [weak self] _ in
                self?.applyDeviceOrientation()
            }
            self.startMotionUpdatesIfNeeded()
            self.startOrientationPoller() // fallback si la notif n'arrive pas
            self.applyDeviceOrientation()
        }
    }

    private func startOrientationPoller() {
        stopOrientationPoller()
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now() + 0.5, repeating: 0.5)
        t.setEventHandler { [weak self] in self?.applyDeviceOrientation() }
        t.resume()
        orientationPoller = t
    }

    private func stopOrientationPoller() {
        orientationPoller?.cancel(); orientationPoller = nil
    }

    private func startMotionUpdatesIfNeeded() {
        guard motionManager.isDeviceMotionAvailable, !motionManager.isDeviceMotionActive else { return }
        motionManager.deviceMotionUpdateInterval = 0.2
        motionManager.startDeviceMotionUpdates(to: .main) { [weak self] motion, _ in
            guard let self = self, let gravity = motion?.gravity else { return }
            if let gOrientation = self.orientationFromGravity(gravity), self.motionOrientation != gOrientation {
                self.motionOrientation = gOrientation
                self.applyDeviceOrientation()
            }
        }
    }

    private func stopMotionUpdates() {
        if motionManager.isDeviceMotionActive {
            motionManager.stopDeviceMotionUpdates()
        }
        motionOrientation = nil
    }

    private func removeOrientationObserver() {
        DispatchQueue.main.async {
            if let obs = self.orientationObserver {
                NotificationCenter.default.removeObserver(obs)
                self.orientationObserver = nil
            }
            self.stopOrientationPoller()
            self.stopMotionUpdates()
            if self.didBeginOrientationNotifications {
                UIDevice.current.endGeneratingDeviceOrientationNotifications()
                self.didBeginOrientationNotifications = false
            }
        }
    }

    private func applyDeviceOrientation() {
        guard autoRotate, let newOri = resolveAutoOrientation() else { return }
        if orientation != newOri {
            orientation = newOri
            bumpConfigRevision()
        }
        sessionQ.async { [weak self] in
            self?.applyCaptureOrientation(newOri)
        }
    }

    private func resolveAutoOrientation() -> AVCaptureVideoOrientation? {
        if let ori = orientationFromDeviceOrientation(UIDevice.current.orientation) {
            return ori
        }
        if let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first,
           let ori = orientationFromInterfaceOrientation(scene.interfaceOrientation) {
            return ori
        }
        return motionOrientation
    }

    private func orientationFromDeviceOrientation(_ value: UIDeviceOrientation) -> AVCaptureVideoOrientation? {
        switch value {
        case .portrait, .portraitUpsideDown:
            return .portrait
        case .landscapeLeft:
            return .landscapeRight
        case .landscapeRight:
            return .landscapeLeft
        default:
            return nil
        }
    }

    private func orientationFromInterfaceOrientation(_ value: UIInterfaceOrientation) -> AVCaptureVideoOrientation? {
        switch value {
        case .portrait, .portraitUpsideDown:
            return .portrait
        case .landscapeLeft:
            return .landscapeRight
        case .landscapeRight:
            return .landscapeLeft
        default:
            return nil
        }
    }

    private func orientationFromGravity(_ gravity: CMAcceleration) -> AVCaptureVideoOrientation? {
        let absX = abs(gravity.x)
        let absY = abs(gravity.y)
        // Ignore diagonal/noisy states to avoid rapid oscillations.
        if abs(absX - absY) < 0.12 {
            return nil
        }
        if absX > absY {
            return gravity.x >= 0 ? .landscapeRight : .landscapeLeft
        }
        return absY > 0.2 ? .portrait : nil
    }

    // MARK: Adaptive bitrate + helpers
    private func updateAdaptiveBitrate(sentFrames: Int, droppedFrames: Int, avgSendMs: Double) {
        guard autoBitrate, isRunning else { return }
        let total = sentFrames + droppedFrames
        guard total > 0 else { return }

        normalizeBitrateBounds()

        let frameBudgetMs = 1000.0 / max(1.0, targetFPS)
        let dropRatio = Double(droppedFrames) / Double(total)

        var next = bitrate
        if dropRatio > 0.12 || avgSendMs > (frameBudgetMs * 0.85) {
            next = Int(Double(bitrate) * 0.85)
        } else if dropRatio < 0.02 && avgSendMs > 0 && avgSendMs < (frameBudgetMs * 0.35) {
            next = Int(Double(bitrate) * 1.06)
        }

        next = clampBitrate(next)
        let threshold = max(500_000, bitrate / 20)
        if abs(next - bitrate) >= threshold {
            bitrate = next
            if let vt = vtSession { applyEncoderRateControlProperties(vt) }
        }
    }

    private func normalizeBitrateBounds() {
        minBitrate = max(250_000, minBitrate)
        maxBitrate = max(minBitrate, maxBitrate)
    }

    private func clampBitrate(_ value: Int) -> Int {
        normalizeBitrateBounds()
        return min(max(value, minBitrate), maxBitrate)
    }

    private func controlPort(forVideoPort port: UInt16) -> UInt16 {
        if port >= UInt16.max { return UInt16.max }
        return port &+ 1
    }

    private func stateString() -> String {
        switch state {
        case .idle: return "idle"
        case .starting: return "starting"
        case .running: return "running"
        case .stopping: return "stopping"
        }
    }

    private func intFromAny(_ value: Any?) -> Int? {
        if let v = value as? Int { return v }
        if let v = value as? NSNumber { return v.intValue }
        if let v = value as? Double { return Int(v) }
        if let v = value as? String { return Int(v.trimmingCharacters(in: .whitespacesAndNewlines)) }
        return nil
    }

    private func doubleFromAny(_ value: Any?) -> Double? {
        if let v = value as? Double { return v }
        if let v = value as? NSNumber { return v.doubleValue }
        if let v = value as? Int { return Double(v) }
        if let v = value as? String { return Double(v.trimmingCharacters(in: .whitespacesAndNewlines)) }
        return nil
    }

    private func boolFromAny(_ value: Any?) -> Bool? {
        if let v = value as? Bool { return v }
        if let v = value as? NSNumber { return v.boolValue }
        if let v = value as? String {
            switch v.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
            case "1", "true", "yes", "on": return true
            case "0", "false", "no", "off": return false
            default: return nil
            }
        }
        return nil
    }

    private func stringFromAny(_ value: Any?) -> String? {
        if let v = value as? String { return v }
        return nil
    }

    private func parseResolution(_ raw: String) -> Resolution? {
        switch raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "720p", "1280x720": return .r720p
        case "1080p", "1920x1080": return .r1080p
        case "4k", "2160p", "3840x2160": return .r4k
        default: return nil
        }
    }

    private func parseOutputProtocol(_ raw: String) -> OutputProtocol? {
        switch raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "annexb", "annex-b": return .annexb
        case "avcc": return .avcc
        default: return nil
        }
    }

    private func parseOrientation(_ raw: String) -> AVCaptureVideoOrientation? {
        switch raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "portrait": return .portrait
        case "landscape_right", "landscaperight", "right": return .landscapeRight
        case "landscape_left", "landscapeleft", "left": return .landscapeLeft
        default: return nil
        }
    }

    private func parseProfile(_ raw: String) -> H264Profile? {
        switch raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "baseline": return .baseline
        case "main": return .main
        case "high": return .high
        default: return nil
        }
    }

    private func parseEntropy(_ raw: String) -> H264Entropy? {
        switch raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "cavlc": return .cavlc
        case "cabac": return .cabac
        default: return nil
        }
    }

    private func resolutionString(width: Int, height: Int) -> String {
        switch (width, height) {
        case (1280, 720): return "720p"
        case (1920, 1080): return "1080p"
        case (3840, 2160): return "4k"
        default: return "\(width)x\(height)"
        }
    }

    private func outputProtocolString(_ p: OutputProtocol) -> String {
        switch p {
        case .annexb: return "annexb"
        case .avcc: return "avcc"
        }
    }

    private func orientationString(_ o: AVCaptureVideoOrientation) -> String {
        switch o {
        case .portrait: return "portrait"
        case .landscapeRight: return "landscape_right"
        case .landscapeLeft: return "landscape_left"
        case .portraitUpsideDown: return "portrait"
        @unknown default: return "portrait"
        }
    }

    private func profileString(_ p: H264Profile) -> String {
        switch p {
        case .baseline: return "baseline"
        case .main: return "main"
        case .high: return "high"
        }
    }

    private func entropyString(_ e: H264Entropy) -> String {
        switch e {
        case .cavlc: return "cavlc"
        case .cabac: return "cabac"
        }
    }
}


