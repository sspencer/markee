#!/usr/bin/env python3
"""Example client for markee (zero third-party dependencies).

Demonstrates:
  1. Reading device info from GET /info
  2. Generating a 72x16 24-bit PNG in memory and uploading it to POST /frame
  3. Reading the displayed pixels back as a PNG from GET /frame
  4. Streaming a smooth 60 Hz 24-bit RGB plasma animation over a persistent socket
"""

import argparse
import http.client
import json
import math
import struct
import time
import zlib

WIDTH = 72
HEIGHT = 16


def encode_png_rgb24(rgb_bytes: bytes, width: int = WIDTH, height: int = HEIGHT) -> bytes:
    """Minimal stdlib-only 24-bit RGB PNG encoder."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw_rows = bytearray()
    stride = width * 3
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        raw_rows.extend(rgb_bytes[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw_rows), 6))
        + chunk(b"IEND", b"")
    )


def hsv_to_rgb(h: float, s: float, v: float) -> tuple[int, int, int]:
    h = h % 360.0
    c = v * s
    x = c * (1.0 - abs((h / 60.0) % 2.0 - 1.0))
    m = v - c
    if h < 60:
        rf, gf, bf = c, x, 0.0
    elif h < 120:
        rf, gf, bf = x, c, 0.0
    elif h < 180:
        rf, gf, bf = 0.0, c, x
    elif h < 240:
        rf, gf, bf = 0.0, x, c
    elif h < 300:
        rf, gf, bf = x, 0.0, c
    else:
        rf, gf, bf = c, 0.0, x
    return (
        int((rf + m) * 255 + 0.5),
        int((gf + m) * 255 + 0.5),
        int((bf + m) * 255 + 0.5),
    )


def make_gradient_frame(t: float) -> bytearray:
    buf = bytearray(WIDTH * HEIGHT * 3)
    i = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            v1 = math.sin(x * 0.14 + t * 2.4)
            v2 = math.sin((y * 0.35 - t * 1.8))
            v3 = math.sin((x * 0.09 + y * 0.22) + t * 1.5)
            hue = ((v1 + v2 + v3 + 3.0) / 6.0) * 360.0 + t * 40.0
            val = 0.35 + 0.65 * (0.5 + 0.5 * math.sin(x * 0.18 - y * 0.3 + t * 3.0))
            r, g, b = hsv_to_rgb(hue, 0.92, val)
            buf[i] = r
            buf[i + 1] = g
            buf[i + 2] = b
            i += 3
    return buf


def main() -> None:
    parser = argparse.ArgumentParser(description="markee 72x16 LED demo client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--seconds", type=float, default=6.0, help="Animation duration")
    parser.add_argument("--save", default="", help="Optional path to save readback PNG")
    args = parser.parse_args()

    conn = http.client.HTTPConnection(args.host, args.port, timeout=5)

    # 1. Inspect device info
    conn.request("GET", "/info")
    resp = conn.getresponse()
    info = json.loads(resp.read().decode())
    print("Connected to:", info)

    # 2. Upload a 72x16 PNG
    png_data = encode_png_rgb24(bytes(make_gradient_frame(0.0)))
    conn.request("POST", "/frame", body=png_data, headers={"Content-Type": "image/png"})
    resp = conn.getresponse()
    print("POST /frame (PNG):", resp.status, resp.read().decode())

    # 3. Read back the displayed pixels as a PNG
    conn.request("GET", "/frame")
    resp = conn.getresponse()
    readback_png = resp.read()
    print(f"GET /frame (PNG): {resp.status}, {len(readback_png)} bytes")
    if args.save:
        with open(args.save, "wb") as f:
            f.write(readback_png)
        print(f"Saved readback PNG to {args.save}")

    # 4. Stream 60 Hz 24-bit RGB plasma over the same Keep-Alive TCP socket
    print(f"Streaming 60 Hz 24-bit RGB animation for {args.seconds:.1f}s...")
    t0 = time.monotonic()
    frames = 0
    while True:
        now = time.monotonic() - t0
        if now >= args.seconds:
            break
        frame = make_gradient_frame(now)
        conn.request(
            "PUT",
            "/frame.rgb",
            body=frame,
            headers={"Content-Type": "application/octet-stream"},
        )
        r = conn.getresponse()
        r.read()
        frames += 1

        next_tick = t0 + frames / 60.0
        sleep_s = next_tick - time.monotonic()
        if sleep_s > 0:
            time.sleep(sleep_s)

    print(f"Sent {frames} frames ({frames / max(args.seconds, 0.01):.1f} FPS)")
    conn.close()


if __name__ == "__main__":
    main()
