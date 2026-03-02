# WinCamStream iOS Control API (TCP JSONL)

The iOS app exposes a control TCP socket on `video_port + 1`.

Example:
- Video stream: `5000`
- Control API: `5001`

Use USB forwarding:

```bash
iproxy 5000 5000
iproxy 5001 5001
```

## Transport
- Protocol: TCP
- Payload format: JSON Lines (`\n` delimited JSON objects)
- One request = one JSON object per line
- One response = one JSON object per line

## Commands

### `ping`
Request:
```json
{"cmd":"ping"}
```

Response:
```json
{"type":"pong","ts":1739999999.123}
```

### `get_status`
Request:
```json
{"cmd":"get_status"}
```

Response (`type=status`) includes runtime state, structured stats and current config.

Example:
```json
{
  "type": "status",
  "state": "running",
  "running": true,
  "busy": false,
  "status": "Running",
  "metrics": "~118 fps | ~29.4 Mb/s | drop:0 | tx:1.8 ms | abr:35M",
  "video_port": 5000,
  "control_port": 5001,
  "stats": {
    "fps": 118,
    "dropped": 0,
    "mbps": 29.4,
    "tx_ms": 1.8,
    "bitrate": 35000000
  },
  "config": { "...": "..." }
}
```

### `start`
```json
{"cmd":"start"}
```

### `stop`
```json
{"cmd":"stop"}
```

### `restart`
```json
{"cmd":"restart"}
```

### `request_keyframe`
```json
{"cmd":"request_keyframe"}
```

### `apply`
Request shape:
```json
{
  "cmd":"apply",
  "config": {
    "port": 5000,
    "resolution": "1080p",
    "fps": 120,
    "bitrate": 35000000,
    "intra_only": false,
    "protocol": "annexb",
    "orientation": "portrait",
    "auto_rotate": false,
    "profile": "high",
    "entropy": "cabac",
    "auto_bitrate": true,
    "min_bitrate": 6000000,
    "max_bitrate": 120000000
  }
}
```

Supported values:
- `resolution`: `720p`, `1080p`, `4k`
- `protocol`: `annexb`, `avcc`
- `orientation`: `portrait`, `landscape_right`, `landscape_left`
- `profile`: `baseline`, `main`, `high`
- `entropy`: `cavlc`, `cabac`
- `port`: `1024..65534` (`control_port = port + 1`)

Notes:
- `baseline` forces `entropy=cavlc`
- If `min_bitrate > max_bitrate`, values are swapped
- `bitrate` is clamped in `[min_bitrate, max_bitrate]`
- Some changes are applied live, others trigger restart (same logic as mobile UI)

## Error responses
Invalid request examples:
```json
{"type":"error","error":"invalid_json"}
{"type":"error","error":"unknown_cmd","cmd":"foo"}
```

## Quick test client
Use:
```bash
python tools/wcs_control.py ping --port 5001
python tools/wcs_control.py status --port 5001
python tools/wcs_control.py apply --port 5001 --config "{\"bitrate_mbps\":25,\"auto_bitrate\":true}"
python tools/wcs_control.py calibrate --port 5001 --start-mbps 12 --max-mbps 120 --step-mbps 4 --tx-limit-ms 4 --drop-limit 1
```
