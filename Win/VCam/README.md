# WinCamStream Virtual Camera Bridge (No OBS)

Preferred path (native):
- `wcs_native_vcam.exe` (C++ / FFmpeg decode)
- UnityCapture shared-memory sender protocol
- UnityCapture virtual camera driver (`Unity Video Capture`)

Fallback path:
- `wcs_vcam_bridge.py` (Python + pyvirtualcam unitycapture)

## Prerequisites
- UnityCapture driver installed (device should appear as `Unity Video Capture`)
  - bundled installer: `Win/VCam/UnityCapture/Install/Install.bat`

Native build:
```powershell
powershell -ExecutionPolicy Bypass -File Win/Native/build_native_vcam.ps1
```

Python fallback only:
- Python 3 available as `python` or `py -3`
- deps: `pip install -r Win/VCam/requirements.txt`

## Manual run (native)
```powershell
Win/Native/Runtime/wcs_native_vcam.exe --url "tcp://127.0.0.1:5000?tcp_nodelay=1" --width 1920 --height 1080 --fps 60
```

## Notes
- Keep iOS protocol in `annexb` for compatibility.
- This path does not require OBS runtime.
