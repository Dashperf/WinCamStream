import SwiftUI
import AVFoundation

struct StreamerView: View {
    @ObservedObject var streamer: Streamer
    @Binding var pending: PendingConfig
    @State private var localDirty = false
    @State private var remoteChangedWhileDirty = false
    @State private var lastRemoteConfig = PendingConfig()

    private func maxFPSForPending() -> Int {
        Int(streamer.maxSupportedFPS(width: pending.resolution.width,
                                     height: pending.resolution.height).rounded(.down))
    }

    private func controlPortForPending() -> UInt16 {
        pending.port == UInt16.max ? pending.port : pending.port &+ 1
    }

    private func normalizeAutoBitrateBounds() {
        if pending.minBitrate > pending.maxBitrate {
            pending.maxBitrate = pending.minBitrate
        }
        if pending.bitrate < pending.minBitrate {
            pending.bitrate = pending.minBitrate
        }
        if pending.bitrate > pending.maxBitrate {
            pending.bitrate = pending.maxBitrate
        }
    }

    private func syncPendingFromStreamer(force: Bool) {
        let remote = PendingConfig(from: streamer)
        if !force && localDirty {
            if remote != lastRemoteConfig {
                lastRemoteConfig = remote
                remoteChangedWhileDirty = true
            }
            return
        }
        lastRemoteConfig = remote
        pending = remote
        localDirty = false
        remoteChangedWhileDirty = false
    }

    private func markLocalDirty() {
        localDirty = pending != lastRemoteConfig
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            // Status + stats
            HStack(spacing: 8) {
                Circle()
                    .fill(streamer.isRunning ? Color.green : Color.gray)
                    .frame(width: 10, height: 10)
                VStack(alignment: .leading, spacing: 2) {
                    Text(streamer.status)
                        .font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .minimumScaleFactor(0.8)
                    if !streamer.metrics.isEmpty {
                        Text(streamer.metrics)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                }
                Spacer()
            }

            // Controls
            HStack(spacing: 12) {
                Button(streamer.isRunning ? "Stop" : "Start") {
                    if streamer.isRunning { streamer.stop() } else { streamer.start() }
                }
                .buttonStyle(.borderedProminent)
                .disabled(streamer.isBusy)

                Button("Apply changes") {
                    // Clamp FPS vs device
                    let maxF = streamer.maxSupportedFPS(width: pending.resolution.width,
                                                        height: pending.resolution.height)
                    if pending.fps > maxF { pending.fps = maxF }

                    // Baseline => CAVLC
                    if pending.profile == .baseline { pending.entropy = .cavlc }

                    normalizeAutoBitrateBounds()
                    streamer.applyOrRestart(with: pending)
                    lastRemoteConfig = pending
                    localDirty = false
                    remoteChangedWhileDirty = false
                }
                .buttonStyle(.bordered)
                .disabled(streamer.isBusy)

                Button("Sync remote") {
                    syncPendingFromStreamer(force: true)
                }
                .buttonStyle(.bordered)
                .disabled(streamer.isBusy || (!localDirty && !remoteChangedWhileDirty))

                Button("Force keyframe") { streamer.requestKeyframe() }
                    .buttonStyle(.bordered)
                    .disabled(!streamer.isRunning || streamer.isBusy)
            }

            if remoteChangedWhileDirty {
                Text("Remote settings changed. Tap Apply changes to keep local edits or Sync remote to load device values.")
                    .font(.caption)
                    .foregroundStyle(.orange)
            } else if localDirty {
                Text("Local edits pending. Tap Apply changes.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Divider()

            // Network
            Group {
                Text("Network").font(.headline)
                HStack {
                    Text("Port")
                    Spacer()
                    Stepper(value: $pending.port, in: 1024...65534, step: 1) {
                        Text("\(pending.port)")
                            .frame(minWidth: 60, alignment: .trailing)
                    }
                }
                HStack {
                    Text("Control API")
                    Spacer()
                    Text("\(controlPortForPending())")
                        .font(.system(.body, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
                Picker("Protocol", selection: $pending.outputProtocol) {
                    Text("H.264 Annex-B (recommended)").tag(OutputProtocol.annexb)
                    Text("H.264 AVCC (experimental)").tag(OutputProtocol.avcc)
                }
                .pickerStyle(.segmented)
            }

            Divider()

            // Video
            Group {
                Text("Video").font(.headline)

                Picker("Resolution", selection: $pending.resolution) {
                    ForEach(Resolution.allCases, id: \.self) { r in
                        Text(r.label).tag(r)
                    }
                }
                .pickerStyle(.segmented)

                Text("Device max FPS: \(maxFPSForPending())")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                HStack {
                    Text("FPS: \(Int(pending.fps))")
                    Slider(value: $pending.fps,
                           in: 24...Double(max(24, maxFPSForPending())),
                           step: 1)
                }

                HStack {
                    Text("Bitrate: \(Int(pending.bitrate/1_000_000)) Mb/s")
                    Slider(value: $pending.bitrate,
                           in: 2_000_000...240_000_000,
                           step: 1_000_000)
                }

                Toggle("Auto bitrate (link adaptive)", isOn: $pending.autoBitrate)

                if pending.autoBitrate {
                    HStack {
                        Text("Auto min: \(Int(pending.minBitrate/1_000_000)) Mb/s")
                        Slider(value: $pending.minBitrate,
                               in: 2_000_000...120_000_000,
                               step: 1_000_000)
                    }

                    HStack {
                        Text("Auto max: \(Int(pending.maxBitrate/1_000_000)) Mb/s")
                        Slider(value: $pending.maxBitrate,
                               in: 2_000_000...240_000_000,
                               step: 1_000_000)
                    }
                }

                Toggle("All-I (GOP=1, minimum latency)", isOn: $pending.intraOnly)

                Picker("Orientation", selection: $pending.orientation) {
                    Text("Portrait").tag(AVCaptureVideoOrientation.portrait)
                    Text("Landscape right").tag(AVCaptureVideoOrientation.landscapeRight)
                    Text("Landscape left").tag(AVCaptureVideoOrientation.landscapeLeft)
                }
                .pickerStyle(.segmented)

                Toggle("Auto-rotate", isOn: $pending.autoRotate)
            }

            Divider()

            // H.264
            Group {
                Text("H.264").font(.headline)

                Picker("Profile", selection: $pending.profile) {
                    ForEach(H264Profile.allCases, id: \.self) { p in
                        Text(p.label).tag(p)
                    }
                }
                .pickerStyle(.segmented)

                Picker("Entropy", selection: $pending.entropy) {
                    ForEach(H264Entropy.allCases, id: \.self) { e in
                        Text(e.label).tag(e)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(pending.profile == .baseline)

                if pending.profile == .baseline {
                    Text("Baseline forces CAVLC (CABAC unavailable)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Divider()

            Text("Tip: after Apply, run `iproxy \(pending.port) \(pending.port)` then `ffplay -fflags nobuffer -flags low_delay -probesize 2048 -analyzeduration 0 -vsync drop -use_wallclock_as_timestamps 1 -i tcp://127.0.0.1:\(pending.port)?tcp_nodelay=1`.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .lineLimit(3)
                .minimumScaleFactor(0.9)
            Text("Remote control: `iproxy \(controlPortForPending()) \(controlPortForPending())` then send JSON lines to tcp://127.0.0.1:\(controlPortForPending()).")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .minimumScaleFactor(0.9)
        }
        .onAppear { syncPendingFromStreamer(force: true) }
        .onChange(of: streamer.configRevision) { _ in
            syncPendingFromStreamer(force: false)
        }
        .onChange(of: pending) { _ in
            markLocalDirty()
        }
    }
}
