import Foundation
import AVFoundation

enum Resolution: CaseIterable {
    case r720p, r1080p, r4k
    var width: Int  { switch self { case .r720p: 1280; case .r1080p: 1920; case .r4k: 3840 } }
    var height: Int { switch self { case .r720p:  720; case .r1080p: 1080; case .r4k: 2160 } }
    var label: String { switch self { case .r720p: "720p"; case .r1080p: "1080p"; case .r4k: "4K" } }
}

enum OutputProtocol { case annexb, avcc }

enum H264Profile: CaseIterable {
    case baseline, main, high
    var label: String {
        switch self { case .baseline: return "Baseline"; case .main: return "Main"; case .high: return "High" }
    }
}

enum H264Entropy: CaseIterable {
    case cavlc, cabac
    var label: String { self == .cavlc ? "CAVLC" : "CABAC" }
}

struct PendingConfig {
    var port: UInt16 = 5000
    var resolution: Resolution = .r1080p
    var fps: Double = 120
    var bitrate: Double = 35_000_000
    var intraOnly: Bool = false
    var outputProtocol: OutputProtocol = .annexb
    var orientation: AVCaptureVideoOrientation = .portrait
    var autoRotate: Bool = false
    var profile: H264Profile = .high
    var entropy: H264Entropy = .cabac
    var autoBitrate: Bool = true
    var minBitrate: Double = 6_000_000
    var maxBitrate: Double = 120_000_000

    init() {}

    init(from s: Streamer) {
        port = s.listenPort
        let candidates: [Resolution] = [.r720p, .r1080p, .r4k]
        resolution = candidates.first { $0.width == s.targetWidth && $0.height == s.targetHeight } ?? .r1080p
        fps = s.targetFPS
        bitrate = Double(s.bitrate)
        intraOnly = s.intraOnly
        outputProtocol = s.outputProtocol
        orientation = s.orientation
        autoRotate = s.autoRotate
        profile = s.profile
        entropy = s.entropy
        autoBitrate = s.autoBitrate
        minBitrate = Double(s.minBitrate)
        maxBitrate = Double(s.maxBitrate)
    }
}
