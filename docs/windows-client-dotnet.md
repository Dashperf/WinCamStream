# Windows Client .NET V1

Project: `Win/ClientDotnet/WcsWinClient`

## Capabilities
- Live control of iOS app via TCP JSONL control API:
  - `status`
  - `start`, `stop`, `restart`
  - `request_keyframe`
  - `apply` (full config)
- Embedded Windows tooling in output `bin`:
  - `iproxy.exe` + required `libimobiledevice` DLLs
  - `ffplay.exe` + FFmpeg shared DLLs
  - `wcs_native_vcam.exe` (native C++ UnityCapture bridge)
  - `UnityCapture/Install/*` (virtual camera installer bundle)
  - `wcs_vcam_bridge.py` (Python fallback bridge)
- USB forwarding control from UI:
  - `Restart iProxy` (video + control tunnels)
- Control API handshake support:
  - ignores initial `hello` frame and waits for the real command response
- Local config draft mode:
  - remote status no longer overwrites in-progress local edits
  - `Apply` keeps local edits authoritative
- Virtual camera bridge (no OBS runtime):
  - `Start VCam` launches native C++ bridge to UnityCapture
  - auto-prompts UnityCapture install if driver is missing
  - `Stop VCam` stops bridge process

## Build
```powershell
"C:\Program Files\dotnet\dotnet.exe" build Win\ClientDotnet\WcsWinClient\WcsWinClient.csproj -c Release
```

## Run
From source:
```powershell
"C:\Program Files\dotnet\dotnet.exe" run --project Win\ClientDotnet\WcsWinClient\WcsWinClient.csproj -c Release
```

Direct executable:
- `Win\ClientDotnet\WcsWinClient\bin\Release\net10.0-windows\WcsWinClient.exe`

## Typical flow
1. In app:
- host: `127.0.0.1`
- video port: `5000`
- control port: `5001`
2. iProxy starts automatically (or click `Restart iProxy`).
3. Click `Start`.
4. For preview, click `Preview ffplay` or use Streamlabs/OBS media input URL:
   - `tcp://127.0.0.1:5000?tcp_nodelay=1`
5. For virtual camera, click `Start VCam`, then select `Unity Video Capture` in target apps.

No external install of `iproxy`/`ffplay` is required when using the built `bin` output.

For virtual camera mode:
- Native path: no Python required (uses `wcs_native_vcam.exe`).
- To build native bridge from source: run `Win/Native/build_native_vcam.ps1` (requires Visual Studio C++ toolchain).
- Python is only fallback mode.
- UnityCapture driver must be installed.
