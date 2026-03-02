# Windows Client + Virtual Camera Roadmap

## Goal
Build a Windows app that:
- receives and decodes WinCamStream low-latency stream,
- can remotely control iOS app settings/sessions,
- publishes decoded frames to a Windows virtual camera.

## Current V1 delivered
- `Win/Client/wcs-client.ps1`:
  - controls iOS app over TCP JSONL API
  - runs automatic bitrate calibration using `stats.tx_ms` and `stats.dropped`
  - launches low-latency `ffplay` preview
- `Win/ClientDotnet/WcsWinClient`:
  - WinForms GUI application (compiled .NET client)
  - same control/calibration/preview workflow as script, ready to evolve to production UI

## Proposed architecture
1. Control plane
- TCP JSONL client using `docs/control-protocol.md`
- Features: connect, read status, apply config, start/stop/restart, force keyframe

2. Media ingest and decode
- Input: `tcp://127.0.0.1:<video_port>` from iProxy
- Decode: FFmpeg (D3D11VA preferred, software fallback)
- Convert output to a stable pixel format for virtual-cam path (e.g. NV12 or BGRA)

3. Preview UI
- Live preview panel
- Runtime stats (fps, decode latency, drop)
- Full iOS config panel bound to control API

4. Virtual camera output
- Option A (fast path): OBS plugin interoperability
- Option B (clean long-term): custom DirectShow/Media Foundation virtual camera driver

## Delivery phases

### Phase 1 (MVP control + preview)
- Windows desktop app (C# + native decode bridge, or C++ Qt)
- Control API integration
- Decoded preview with low-latency tuning

### Phase 2 (production ingest)
- robust reconnect logic
- decoder fallback ladder
- profiling and latency instrumentation end-to-end

### Phase 3 (virtual camera)
- first usable virtual camera binding
- compatibility tests: Teams, Zoom, Meet, OBS, browser WebRTC

### Phase 4 (hardening)
- installer + signed binaries
- crash reporting
- diagnostics export for support

## Latency target strategy (< 80 ms)
- Keep no-B-frame H.264 path
- Keep capture drop policy on congestion (no backlog growth)
- tune decoder queue depth on Windows
- use USB forwarding (`iproxy`) and avoid additional buffering layers
- instrument each segment:
  - capture->encode
  - encode->send
  - receive->decode
  - decode->display/virtual-cam
