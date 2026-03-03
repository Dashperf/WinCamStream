# UnityCapture Driver Bundle

This folder embeds UnityCapture install assets for a real Windows DirectShow virtual camera.

Included from upstream project:
- https://github.com/schellingb/UnityCapture

Files under `Install/` are used to register/unregister:
- `UnityCaptureFilter64.dll`
- `Install.bat`
- `Uninstall.bat`

## Install
Run as Administrator:
```powershell
Win\VCam\UnityCapture\Install\Install.bat
```

The camera device appears as `Unity Video Capture`.

## License
- UnityCaptureFilter: MIT
- UnityCapturePlugin/shared protocol: zlib

See upstream repository for full license texts and notices.
