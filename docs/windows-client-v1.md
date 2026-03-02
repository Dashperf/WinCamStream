# Windows Client V1 (PowerShell GUI)

File: `Win/Client/wcs-client.ps1`

## What it provides
- Live control of iPhone app via Control API:
  - status
  - start / stop / restart
  - keyframe request
  - apply settings at runtime
- Automatic bitrate calibration:
  - probes bitrate from `start` to `max`
  - stops at first saturation signal (`drop` or `tx_ms` threshold)
  - applies best stable bitrate
  - re-enables adaptive bitrate around selected point
- Preview launcher (`ffplay`) with low-latency flags.

## Run
Open PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Win\Client\wcs-client.ps1
```

## Typical setup
1. Start USB forwarding:
```bash
iproxy 5000 5000
iproxy 5001 5001
```
2. In GUI:
- Host: `127.0.0.1`
- VideoPort: `5000`
- CtrlPort: `5001`
3. Click `Start`, then `Calibrate`.
4. Click `Preview ffplay`.

## Calibration tuning
- `tx<=ms`: max acceptable send time per frame.
  - 120 fps stream budget is ~8.3 ms/frame.
  - for aggressive low-latency, use 3-5 ms.
- `drop<=`: max accepted dropped frames per 1s window.
- `settle(s)`: time to wait after each bitrate step.

## Notes
- Requires `ffplay` available in `PATH` for preview button.
- This V1 does not yet publish to a virtual camera driver. It prepares the control and calibration foundation for that phase.
