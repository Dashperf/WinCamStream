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
- USB forwarding control from UI:
  - `Start iProxy` (video + control tunnels)
  - `Stop iProxy`
- Automatic bitrate calibration:
  - probes bitrate from `start` to `max`
  - detects saturation using `stats.dropped` and `stats.tx_ms`
  - applies best stable bitrate and re-enables auto bitrate window
- One-click low-latency preview launch with `ffplay`

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
2. Click `Start iProxy`.
3. Click `Start`.
4. Click `Calibrate`.
5. Click `Preview ffplay`.

No external install of `iproxy`/`ffplay` is required when using the built `bin` output.

## Calibration knobs
- `Cal start(M)`: first tested bitrate.
- `max(M)`: upper search bound.
- `step(M)`: increment per probe.
- `tx<=ms`: max accepted send latency.
- `drop<=`: max accepted dropped frames per status window.
- `settle(s)`: wait after each apply before evaluating.
