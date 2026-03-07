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

- `Win/Native/Runtime/wcs_native_winui.exe`
  - native launcher executable for primary WinUI 3 desktop UI (C++)
  - actual app payload: `Win/Native/Runtime/WcsNativeWinUI/WcsNativeWinUI.exe`
  - diagnostics log: `Win/Native/Runtime/wcs_native_winui-launcher.log`
  - modern layout and same live controls:
  - restart iProxy, status/start/stop/restart/keyframe/apply
  - Apple USB prerequisite check at startup
  - if missing: Yes/No prompt, Yes runs installer, No closes the app
  - preview ffplay
  - start/stop native virtual camera bridge

- `Win/Native/Runtime/wcs_native_winui.cmd`
  - command-line fallback launcher for the same WinUI app

- `Win/Native/Runtime/wcs_native_vcam.exe`
  - H.264 TCP ingest (`tcp://127.0.0.1:5000?tcp_nodelay=1`)
  - FFmpeg decode
  - UnityCapture shared-memory output (`Unity Video Capture` device)
  - dynamic source resolution by default
  - linear resize mode enabled by default for capture-output mismatch compatibility

- `Win/Native/Runtime/UnityCapture/*`
  - embedded virtual camera driver installer files (`Install.bat`, DLLs)

- `Win/Native/Runtime/Deps/*` (optional)
  - embedded Apple USB dependency installer(s), e.g. `AppleMobileDeviceSupport64.msi`
  - used by WinUI button: `Install Apple USB`

## Build
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_all.ps1
```

Build a full Windows installer (`Setup.exe`) with embedded runtime:
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_installer.ps1 -Version 1.0.0
```

Auto-fetch Apple USB dependency (official winget package) during build:
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_winui.ps1 -FetchAppleUsb
```

Download/install Apple USB dependency standalone:
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/bootstrap_apple_usb.ps1 -DownloadOnly
powershell -ExecutionPolicy Bypass -File Win/Native/bootstrap_apple_usb.ps1 -Install
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
- iProxy over USB needs Apple Mobile Device Support (driver + service).
- Preferred source is official winget package: `Apple.AppleMobileDeviceSupport`.
- If installer is not present in `Runtime/Deps`, the startup prerequisite flow falls back to elevated `winget install`.
- Optional offline embedding:
  - put installer file(s) in `Win/Deps/` (example: `AppleMobileDeviceSupport64.msi`)
  - or use `build_native_winui.ps1 -FetchAppleUsb` to download automatically into `Win/Deps`.
- For desktop usage, run:
```powershell
Win/Native/Runtime/wcs_native_winui.exe
```
- Legacy Win32 UI is still available:
```powershell
Win/Native/Runtime/wcs_native_ui.exe
```
- VCam launch from native UI now uses dynamic sizing (no forced 1080p).
- Green/mismatch refresh pattern is mitigated by:
  - `--resize-mode linear`
  - `--timeout-ms 0` (hold-last-frame mode; avoids UnityCapture green timeout flashes)
