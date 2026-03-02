#!/usr/bin/env python3
import argparse
import json
import socket
import time
import sys


def send_json_line(host: str, port: int, payload: dict, timeout: float) -> dict:
    data = (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")

    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(data)

        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk

    line = buf.split(b"\n", 1)[0].decode("utf-8", errors="replace").strip()
    if not line:
        return {"type": "error", "error": "empty_response"}

    try:
        return json.loads(line)
    except json.JSONDecodeError:
        return {"type": "error", "error": "invalid_json_response", "raw": line}


def apply_config(host: str, port: int, timeout: float, cfg: dict) -> dict:
    return send_json_line(host, port, {"cmd": "apply", "config": cfg}, timeout)


def get_status(host: str, port: int, timeout: float) -> dict:
    return send_json_line(host, port, {"cmd": "get_status"}, timeout)


def calibrate(host: str, port: int, timeout: float, start_mbps: int, max_mbps: int, step_mbps: int,
              tx_limit_ms: float, drop_limit: int, settle_s: float) -> dict:
    start_mbps = max(2, start_mbps)
    max_mbps = max(start_mbps, max_mbps)
    step_mbps = max(1, step_mbps)
    settle_s = max(0.5, settle_s)

    best = start_mbps
    saturated = False

    apply_config(host, port, timeout, {
        "auto_bitrate": False,
        "min_bitrate": start_mbps * 1_000_000,
        "max_bitrate": max_mbps * 1_000_000,
    })

    for mbps in range(start_mbps, max_mbps + 1, step_mbps):
        apply_config(host, port, timeout, {
            "bitrate_mbps": mbps,
            "auto_bitrate": False,
        })
        time.sleep(settle_s)

        st = get_status(host, port, timeout)
        stats = st.get("stats", {})
        drop = int(stats.get("dropped", 0))
        tx_ms = float(stats.get("tx_ms", 0.0))
        fps = int(stats.get("fps", 0))
        mbps_real = float(stats.get("mbps", 0.0))
        print(f"probe={mbps}M fps={fps} drop={drop} tx={tx_ms:.2f}ms link={mbps_real:.1f}Mb/s")

        if drop > drop_limit or tx_ms > tx_limit_ms:
            saturated = True
            break
        best = mbps

    auto_min = max(2, int(best * 0.6))
    auto_max = max(best, int(best * 1.2))
    apply_resp = apply_config(host, port, timeout, {
        "bitrate_mbps": best,
        "auto_bitrate": True,
        "min_bitrate": auto_min * 1_000_000,
        "max_bitrate": auto_max * 1_000_000,
    })

    return {
        "type": "calibrate_result",
        "saturated": saturated,
        "selected_mbps": best,
        "auto_min_mbps": auto_min,
        "auto_max_mbps": auto_max,
        "apply_response": apply_resp,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="WinCamStream iOS control API client")
    parser.add_argument("cmd", choices=["ping", "status", "start", "stop", "restart", "keyframe", "apply", "calibrate"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5001, help="Control API port (video_port + 1)")
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--config", default="{}", help="JSON object for apply command")
    parser.add_argument("--start-mbps", type=int, default=12)
    parser.add_argument("--max-mbps", type=int, default=120)
    parser.add_argument("--step-mbps", type=int, default=4)
    parser.add_argument("--tx-limit-ms", type=float, default=4.0)
    parser.add_argument("--drop-limit", type=int, default=1)
    parser.add_argument("--settle-s", type=float, default=2.0)
    args = parser.parse_args()

    if args.cmd == "calibrate":
        result = calibrate(
            host=args.host,
            port=args.port,
            timeout=args.timeout,
            start_mbps=args.start_mbps,
            max_mbps=args.max_mbps,
            step_mbps=args.step_mbps,
            tx_limit_ms=args.tx_limit_ms,
            drop_limit=args.drop_limit,
            settle_s=args.settle_s,
        )
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0

    payload = {"cmd": "get_status" if args.cmd == "status" else args.cmd}

    if args.cmd == "keyframe":
        payload["cmd"] = "request_keyframe"

    if args.cmd == "apply":
        try:
            cfg = json.loads(args.config)
            if not isinstance(cfg, dict):
                raise ValueError("config must be a JSON object")
        except Exception as exc:
            print(f"Invalid --config: {exc}", file=sys.stderr)
            return 2
        payload = {"cmd": "apply", "config": cfg}

    response = send_json_line(args.host, args.port, payload, args.timeout)
    print(json.dumps(response, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
