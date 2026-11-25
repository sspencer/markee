#!/usr/bin/env python3
"""
Focus / Pomodoro timer applet for markee.

Drives alternating Focus and Break countdown sessions with:
  - Server-side 60 Hz countdown + bottom progress bar
  - Built-in icon (flame for focus, sun for break) and cycle dots
  - Color shift to bright orange/red in the final 20% of focus
  - Gentle end-of-phase chimes / custom melodies
  - Interactive skip/advance when Space/Enter or the bezel is clicked (/events)

Zero external dependencies (Python 3 stdlib only).

Usage:
  python3 examples/pomodoro.py --work 25 --short-break 5 --cycles 4
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"].rstrip("/")
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


class Client:
    def __init__(self, base_url: str, token: Optional[str]):
        self.base_url = base_url.rstrip("/")
        self.token = token

    def _req(self, method: str, path: str, payload: Any = None, timeout: float = 5.0) -> Any:
        headers: Dict[str, str] = {}
        data = None
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        req = urllib.request.Request(f"{self.base_url}{path}", data=data, headers=headers, method=method)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            return json.loads(raw) if raw.strip() else {}

    def draw(self, ops: List[Dict[str, Any]], transition: Optional[str] = None) -> None:
        q = f"?transition={urllib.parse.quote(transition)}" if transition else ""
        self._req("POST", f"/draw{q}", ops)

    def alert(self, sound: Optional[str] = None, melody: Optional[str] = None, volume: int = 75) -> None:
        if melody:
            self._req("POST", "/alert", {"melody": melody, "volume": volume})
        elif sound:
            self._req("POST", "/alert", {"sound": sound, "volume": volume})

    def poll_events(self, since: int, wait_s: float = 1.0) -> Dict[str, Any]:
        return self._req("GET", f"/events?since={since}&wait={wait_s:.2f}", timeout=wait_s + 3.0)


def render_phase(
    client: Client,
    phase: str,
    remaining_s: float,
    total_s: float,
    cycle: int,
    cycles: int,
    transition: Optional[str] = None,
) -> None:
    is_focus = phase == "focus"
    frac_left = remaining_s / total_s if total_s > 0 else 0.0
    if is_focus:
        if frac_left <= 0.15:
            color = "#ff4422"
            icon_color = "#ff5533"
        elif frac_left <= 0.35:
            color = "#ff9922"
            icon_color = "orange"
        else:
            color = "amber"
            icon_color = "orange"
        icon = "flame"
        tag = f"F{cycle}"
    else:
        color = "#38e8b0"
        icon_color = "#44ccff"
        icon = "sun"
        tag = "LB" if phase == "long_break" else "BR"

    ops: List[Dict[str, Any]] = [
        {"op": "clear"},
        {"op": "icon", "name": icon, "x": 2, "y": 3, "color": icon_color},
        {"op": "text", "x": 12, "y": 4, "text": tag, "color": "#8899aa", "font": "3x5"},
        {
            "op": "countdown",
            "x": 24,
            "y": 3,
            "w": 46,
            "align": "right",
            "seconds": max(0.0, remaining_s),
            "color": color,
            "bar": True,
            "bar_color": color,
            "font": "prop",
        },
    ]

    # Cycle progress indicator dots along top-left edge (y=0).
    for i in range(min(cycles, 8)):
        dot_color = color if i < cycle else "#222830"
        ops.append({"op": "rect", "x": 2 + i * 3, "y": 0, "w": 2, "h": 1, "fill": dot_color})

    client.draw(ops, transition=transition)


def run_phase(
    client: Client,
    phase: str,
    duration_s: float,
    cycle: int,
    cycles: int,
    last_event_id: int,
) -> int:
    start = time.monotonic()
    deadline = start + duration_s
    last_band = -1

    render_phase(client, phase, duration_s, duration_s, cycle, cycles, transition="slide-left")

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break

        # Re-render when entering warning color bands during focus.
        frac = remaining / duration_s if duration_s > 0 else 0.0
        band = 2 if frac <= 0.15 else (1 if frac <= 0.35 else 0)
        if phase == "focus" and band != last_band and last_band != -1:
            render_phase(client, phase, remaining, duration_s, cycle, cycles)
        last_band = band

        wait_s = min(1.0, max(0.1, remaining))
        try:
            ev_resp = client.poll_events(last_event_id, wait_s=wait_s)
            last_event_id = int(ev_resp.get("last_id", last_event_id))
            for ev in ev_resp.get("events", []):
                if ev.get("type") in ("button", "click"):
                    print(f"[{phase}] skipped via {ev.get('type')} event")
                    return last_event_id
        except (urllib.error.URLError, TimeoutError, OSError):
            time.sleep(wait_s)

    return last_event_id


def main() -> int:
    parser = argparse.ArgumentParser(description="Focus / Pomodoro timer for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token")
    parser.add_argument("--work", type=float, default=25.0, help="Focus duration in minutes (default: 25)")
    parser.add_argument("--short-break", type=float, default=5.0, help="Short break duration in minutes (default: 5)")
    parser.add_argument("--long-break", type=float, default=15.0, help="Long break duration in minutes (default: 15)")
    parser.add_argument("--cycles", type=int, default=4, help="Focus cycles before long break (default: 4)")
    parser.add_argument("--seconds", action="store_true", help="Interpret --work / --short-break / --long-break as seconds (great for demos)")
    parser.add_argument("--mute", action="store_true", help="Disable end-of-phase chimes")
    args = parser.parse_args()

    scale = 1.0 if args.seconds else 60.0
    work_s = max(1.0, args.work * scale)
    short_s = max(1.0, args.short_break * scale)
    long_s = max(1.0, args.long_break * scale)
    cycles = max(1, args.cycles)

    client = Client(args.url, args.token)

    try:
        ev0 = client.poll_events(0, wait_s=0.0)
        last_event_id = int(ev0.get("last_id", 0))
    except Exception as exc:
        print(f"Error connecting to markee at {args.url}: {exc}", file=sys.stderr)
        return 1

    print(f"Pomodoro started on {args.url} ({cycles} cycles; press Space/bezel to skip phase, Ctrl+C to stop)")

    try:
        for cycle in range(1, cycles + 1):
            print(f"Cycle {cycle}/{cycles}: FOCUS ({work_s:.0f}s)")
            last_event_id = run_phase(client, "focus", work_s, cycle, cycles, last_event_id)
            if not args.mute:
                client.alert(melody="C5:110,E5:110,G5:220")

            is_last = cycle == cycles
            brk_phase = "long_break" if is_last else "break"
            brk_s = long_s if is_last else short_s
            print(f"Cycle {cycle}/{cycles}: {'LONG BREAK' if is_last else 'BREAK'} ({brk_s:.0f}s)")
            last_event_id = run_phase(client, brk_phase, brk_s, cycle, cycles, last_event_id)
            if not args.mute:
                client.alert(sound="chime" if not is_last else "bell")

        client.draw(
            [
                {"op": "clear", "color": "#04160c"},
                {"op": "icon", "name": "check", "x": 4, "y": 4, "color": "green"},
                {"op": "text", "x": 15, "y": 4, "w": 55, "align": "left", "text": "DONE!", "color": "warmwhite"},
            ],
            transition="fade",
        )
        print("Pomodoro session complete!")
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
