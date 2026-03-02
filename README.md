# WinCamStream
WinCamStream or WCS is a projet about ios/android camera direct streaming low latency to windows in a virtual camera

## iOS app
- Low-latency H.264 camera streaming over TCP (Annex-B or AVCC)
- Live runtime tuning from mobile UI
- Adaptive bitrate mode (link-driven)
- Remote control TCP API (JSON lines) on `video_port + 1`

See:
- `docs/control-protocol.md`
- `docs/windows-client-roadmap.md`
- `docs/windows-client-v1.md`
- `docs/windows-client-dotnet.md`
- `tools/wcs_control.py` (quick control client)
- `Win/Client/wcs-client.ps1` (Windows GUI client V1)
- `Win/ClientDotnet/WcsWinClient` (Windows GUI client .NET V1)
