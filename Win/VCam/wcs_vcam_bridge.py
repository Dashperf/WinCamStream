#!/usr/bin/env python3
"""
WinCamStream virtual camera bridge.

Reads H.264 over TCP using ffmpeg and publishes decoded frames to a virtual
camera backend via pyvirtualcam (UnityCapture recommended on Windows).
"""

from __future__ import annotations

import argparse
import signal
import subprocess
import sys
import threading
import time

try:
    import numpy as np
except Exception as exc:  # pragma: no cover - runtime diagnostic
    print(f"ERR: missing numpy ({exc}). Run: pip install numpy", flush=True)
    sys.exit(2)

try:
    import pyvirtualcam
except Exception as exc:  # pragma: no cover - runtime diagnostic
    print(f"ERR: missing pyvirtualcam ({exc}). Run: pip install pyvirtualcam", flush=True)
    sys.exit(2)


STOP = False


def _on_signal(_sig, _frame):
    global STOP
    STOP = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="WinCamStream virtual camera bridge")
    parser.add_argument("--url", required=True, help="Input URL, e.g. tcp://127.0.0.1:5000?tcp_nodelay=1")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="Path to ffmpeg executable")
    parser.add_argument("--width", type=int, required=True, help="Output width")
    parser.add_argument("--height", type=int, required=True, help="Output height")
    parser.add_argument("--fps", type=int, required=True, help="Output FPS")
    parser.add_argument("--backend", default="unitycapture", help="pyvirtualcam backend (unitycapture/obs/auto)")
    return parser.parse_args()


def drain_stderr(stderr_pipe):
    if stderr_pipe is None:
        return
    try:
        for raw in iter(stderr_pipe.readline, b""):
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                print(f"[ffmpeg] {line}", flush=True)
    except Exception:
        pass


def start_ffmpeg(args: argparse.Namespace) -> subprocess.Popen:
    cmd = [
        args.ffmpeg,
        "-hide_banner",
        "-loglevel",
        "warning",
        "-fflags",
        "nobuffer",
        "-flags",
        "low_delay",
        "-probesize",
        "2048",
        "-analyzeduration",
        "0",
        "-f",
        "h264",
        "-i",
        args.url,
        "-an",
        "-sn",
        "-dn",
        "-threads",
        "1",
        "-vf",
        f"scale={args.width}:{args.height},format=rgb24",
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "-",
    ]
    print("Starting ffmpeg decode pipeline...", flush=True)
    process = subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=args.width * args.height * 3 * 2,
    )
    thread = threading.Thread(target=drain_stderr, args=(process.stderr,), daemon=True)
    thread.start()
    return process


def read_exact(pipe, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = pipe.read(remaining)
        if not chunk:
            return b""
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def main() -> int:
    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    args = parse_args()
    if args.width <= 0 or args.height <= 0 or args.fps <= 0:
        print("ERR: invalid width/height/fps", flush=True)
        return 2

    backend = args.backend if args.backend and args.backend.lower() != "auto" else None
    frame_size = args.width * args.height * 3
    reconnect_sleep = 0.30

    print(
        f"VCam bridge starting: {args.width}x{args.height}@{args.fps}, backend={backend or 'auto'}",
        flush=True,
    )

    try:
        with pyvirtualcam.Camera(
            width=args.width,
            height=args.height,
            fps=args.fps,
            fmt=pyvirtualcam.PixelFormat.RGB,
            backend=backend,
        ) as cam:
            print(f"Virtual camera opened: {cam.device}", flush=True)
            while not STOP:
                ff = start_ffmpeg(args)
                try:
                    while not STOP:
                        if ff.poll() is not None:
                            print(f"ffmpeg exited (code {ff.returncode}), reconnecting...", flush=True)
                            break

                        raw = read_exact(ff.stdout, frame_size)
                        if len(raw) != frame_size:
                            print("ffmpeg frame read underrun, reconnecting...", flush=True)
                            break

                        frame = np.frombuffer(raw, dtype=np.uint8).reshape((args.height, args.width, 3))
                        cam.send(frame)
                        cam.sleep_until_next_frame()
                finally:
                    try:
                        ff.kill()
                    except Exception:
                        pass
                    try:
                        ff.wait(timeout=1.0)
                    except Exception:
                        pass

                if not STOP:
                    time.sleep(reconnect_sleep)
    except Exception as exc:
        print(f"ERR: {exc}", flush=True)
        return 1

    print("VCam bridge stopped.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
