#!/usr/bin/env python3
"""
Now Playing ticker applet for markee.

Polls the active media player:
  - macOS: Spotify and Apple Music (via osascript)
  - Linux: Any MPRIS player (via playerctl)
  - Fallback/Demo: `--demo` or `--track "Title - Artist"`

Displays a crisp music/pause icon on the left and a clipped marquee ticker
on the right (`clip: [13, 0, 58, 16]`) so icon and scrolling title never collide.

Zero external Python dependencies (stdlib only).

Usage:
  python3 examples/now_playing.py
  python3 examples/now_playing.py --demo
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional, Tuple


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"].rstrip("/")
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


def _run_cmd(args: List[str], timeout: float = 1.5) -> Optional[str]:
    try:
        res = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
        if res.returncode == 0:
            return res.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def poll_macos() -> Tuple[str, str]:
    """Return (state, label) where state is 'playing', 'paused', or 'idle'."""
    for app in ("Spotify", "Music"):
        script = (
            f'if application "{app}" is running then\n'
            f'  tell application "{app}"\n'
            f'    set pstate to (player state as text)\n'
            f'    if pstate is "playing" or pstate is "paused" then\n'
            f'      set tname to ""\n'
            f'      set tartist to ""\n'
            f'      try\n'
            f'        set tname to (name of current track) as text\n'
            f'      end try\n'
            f'      try\n'
            f'        set tartist to (artist of current track) as text\n'
            f'      end try\n'
            f'      return pstate & "|||" & tname & "|||" & tartist\n'
            f'    end if\n'
            f'  end tell\n'
            f'end if\n'
            f'return ""'
        )
        out = _run_cmd(["osascript", "-e", script])
        if out and "|||" in out:
            parts = out.split("|||")
            state = parts[0].strip().lower()
            tname = parts[1].strip() if len(parts) > 1 else ""
            tartist = parts[2].strip() if len(parts) > 2 else ""
            if tname and tartist:
                return state, f"{tname} - {tartist}"
            if tname:
                return state, tname
    return "idle", ""


def poll_linux() -> Tuple[str, str]:
    if not shutil.which("playerctl"):
        return "idle", ""
    status = (_run_cmd(["playerctl", "status"]) or "").lower()
    if status not in ("playing", "paused"):
        return "idle", ""
    meta = _run_cmd(["playerctl", "metadata", "--format", "{{title}} - {{artist}}"])
    if meta:
        return status, meta.strip(" -")
    return "idle", ""


def poll_player() -> Tuple[str, str]:
    sys_name = platform.system()
    if sys_name == "Darwin":
        return poll_macos()
    return poll_linux()


def post_draw(base_url: str, token: Optional[str], ops: List[Dict[str, Any]], transition: Optional[str] = None) -> None:
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    q = f"?transition={transition}" if transition else ""
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/draw{q}",
        data=json.dumps(ops).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5.0) as resp:
        resp.read()


def build_ops(state: str, label: str, speed: float, color: str, icon_color: str) -> List[Dict[str, Any]]:
    if state == "idle" or not label:
        return [
            {"op": "clear"},
            {"op": "icon", "name": "music", "x": 3, "y": 4, "color": "#445566"},
            {"op": "text", "x": 14, "y": 4, "text": "NO MUSIC", "color": "#556677", "font": "prop"},
        ]

    playing = state == "playing"
    ops: List[Dict[str, Any]] = [
        {"op": "clear", "color": "#05070c"},
        {"op": "icon", "name": "music" if playing else "pause", "x": 2, "y": 1, "color": icon_color if playing else "amber"},
        {"op": "line", "x1": 11, "y1": 2, "x2": 11, "y2": 13, "color": "#1e2736"},
    ]
    # Miniature equalizer bars along bottom-left cleanly beneath the icon (y=11..14).
    eq_heights = [2, 4, 2, 3] if playing else [1, 1, 1, 1]
    for i, h in enumerate(eq_heights):
        ops.append({"op": "rect", "x": 2 + i * 2, "y": 15 - h, "w": 1, "h": h, "fill": icon_color if playing else "#444a55"})

    text_op: Dict[str, Any] = {
        "op": "text",
        "x": 70 if playing else 14,
        "y": 4,
        "text": label,
        "color": color if playing else "#9aa5b1",
        "font": "prop",
        "clip": [13, 1, 58, 14],
    }
    if playing:
        text_op["scroll"] = True
        text_op["speed"] = speed
    ops.append(text_op)
    return ops


def main() -> int:
    parser = argparse.ArgumentParser(description="Now Playing LED ticker for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token")
    parser.add_argument("--interval", type=float, default=2.0, help="Poll interval in seconds (default: 2.0)")
    parser.add_argument("--speed", type=float, default=26.0, help="Marquee scroll speed in px/s (default: 26)")
    parser.add_argument("--color", default="warmwhite", help="Track title color (default: warmwhite)")
    parser.add_argument("--icon-color", default="#1db954", help="Music icon color (default: #1db954)")
    parser.add_argument("--track", help="Display a fixed track string instead of polling a media player")
    parser.add_argument("--demo", action="store_true", help="Cycle through sample demo tracks")
    args = parser.parse_args()

    demo_tracks = [
        ("playing", "Midnight City - M83"),
        ("playing", "Instant Crush - Daft Punk"),
        ("paused", "Resonance - HOME"),
    ]
    demo_idx = 0

    last_key: Optional[Tuple[str, str]] = None
    print(f"Now Playing ticker connected to {args.url} (Ctrl+C to stop)")

    try:
        while True:
            if args.track:
                state, label = "playing", args.track
            elif args.demo:
                state, label = demo_tracks[demo_idx % len(demo_tracks)]
                demo_idx += 1
            else:
                state, label = poll_player()

            key = (state, label)
            if key != last_key:
                ops = build_ops(state, label, args.speed, args.color, args.icon_color)
                try:
                    post_draw(args.url, args.token, ops, transition="fade" if last_key is not None else None)
                    print(f"[{state}] {label or '(idle)'}")
                    last_key = key
                except urllib.error.URLError as exc:
                    print(f"Warning: cannot reach markee ({exc.reason})", file=sys.stderr)

            if args.track:
                break
            time.sleep(5.0 if args.demo else max(0.5, args.interval))
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
