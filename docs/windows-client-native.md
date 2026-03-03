# Windows Client Native (C++)

New native toolchain (no .NET runtime required for core workflows):

- `Win/Native/Runtime/wcs_native_client.exe`
  - control API commands (`status/start/stop/restart/keyframe/apply`)
  - optional `iproxy` tunnel launch
  - optional `ffplay` preview launch
  - optional native virtual camera bridge launch

- `Win/Native/Runtime/wcs_native_ui.exe`
  - native Win32 desktop UI
  - same core features as legacy Windows UI:
  - restart iProxy, status/start/stop/restart/keyframe/apply
  - preview ffplay
  - start/stop native virtual camera bridge
  - launch UnityCapture installer

- `Win/Native/Runtime/wcs_native_vcam.exe`
  - H.264 TCP ingest (`tcp://127.0.0.1:5000?tcp_nodelay=1`)
  - FFmpeg decode
  - UnityCapture shared-memory output (`Unity Video Capture` device)

- `Win/Native/Runtime/UnityCapture/*`
  - embedded virtual camera driver installer files (`Install.bat`, DLLs)

## Build
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_all.ps1
```

## Typical usage
1. Install UnityCapture once (admin):
```powershell
Win/Native/Runtime/UnityCapture/Install.bat
```

2. Start everything from native launcher:
```powershell
Win/Native/Runtime/wcs_native_client.exe --start-iproxy --cmd start --start-vcam --preview --wait
```

3. Send live control command:
```powershell
Win/Native/Runtime/wcs_native_client.exe --cmd status
Win/Native/Runtime/wcs_native_client.exe --cmd keyframe
Win/Native/Runtime/wcs_native_client.exe --cmd apply --config-json "{\"bitrate_mbps\":35,\"fps\":60,\"auto_bitrate\":true}"
```

## Notes
- Keep iOS output protocol set to `annexb` for preview/bridge compatibility.
- For desktop usage, run:
```powershell
Win/Native/Runtime/wcs_native_ui.exe
```
