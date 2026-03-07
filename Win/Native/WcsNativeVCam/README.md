# WCS Native VCam Bridge (C++)

Native Windows bridge:
- input: WinCamStream H.264 TCP (`tcp://127.0.0.1:5000?tcp_nodelay=1`)
- decode: FFmpeg (`libavformat/libavcodec/libswscale`)
- output: UnityCapture shared-memory sender (real DirectShow virtual camera)

## Build
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_vcam.ps1
```

This generates:
- `Win/Native/Runtime/wcs_native_vcam.exe`

Requirements:
- CMake
- Visual Studio C++ Build Tools (or Visual Studio with MSVC toolchain)

## Run (manual)
```powershell
Win/Native/Runtime/wcs_native_vcam.exe --url "tcp://127.0.0.1:5000?tcp_nodelay=1" --width 1920 --height 1080 --fps 60
```

Options:
- `--cap N` UnityCapture device index (default `0`)
- `--resize-mode linear|disabled` (default `linear`)
- `--timeout-ms N` stale-frame threshold (default `0`, interpreted as hold-last-frame mode)
- `--reconnect-ms N` reconnect delay (default `300`)

## Notes
- Requires UnityCapture virtual camera installed and opened by a receiver app.
- Max shared image size follows UnityCapture limits (4K RGBA).
- For dynamic source resolution (recommended), omit `--width/--height`.
