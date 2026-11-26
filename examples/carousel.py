#!/usr/bin/env python3
"""
Rotating status carousel daemon for markee.

Cycles through pluggable information cards with smooth transitions
(`slide-left`, `fade`, `slide-up`, `wipe`) on a configurable interval, and
advances immediately when Space/Enter or the bezel is clicked via GET /events.

Built-in cards (`--cards clock,date,sys,git,weather`):
  - clock    Live 60 Hz server-side digital clock widget
  - date     Day of week + calendar date with subtle gradient accent
  - sys      CPU load & memory meter
  - git      Local repository branch & clean/dirty badge (--repo)
  - weather  Optional city weather via wttr.in (--city "San Francisco")
  - Custom cards via --card "TITLE|VALUE|COLOR|ICON" (repeatable)

Zero external Python dependencies (stdlib only).

Usage:
  python3 examples/carousel.py --cards clock,date,sys,git --interval 6
  python3 examples/carousel.py --card "NEXT|14:00 STANDUP|cyan|bell"
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Dict, List, Optional, Tuple


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

    def draw(self, ops: List[Dict[str, Any]], transition: Optional[str], transition_ms: int) -> None:
        q = ""
        if transition and transition != "none":
            q = f"?transition={urllib.parse.quote(transition)}&transition_ms={transition_ms}"
        self._req("POST", f"/draw{q}", ops)

    def poll_events(self, since: int, wait_s: float) -> Dict[str, Any]:
        wait_ms = max(0, min(5000, int(round(wait_s * 1000.0))))
        return self._req("GET", f"/events?since={since}&wait={wait_ms}", timeout=wait_s + 3.0)


def card_clock() -> List[Dict[str, Any]]:
    return [
        {"op": "clear", "color": "#04060c"},
        {"op": "icon", "name": "clock", "x": 3, "y": 3, "color": "amber"},
        {
            "op": "clock",
            "x": 42,
            "y": 4,
            "align": "center",
            "format": "24h",
            "color": "warmwhite",
        },
    ]


def card_date() -> List[Dict[str, Any]]:
    now = datetime.datetime.now()
    day = now.strftime("%a").upper()
    date_str = now.strftime("%b %d").upper()
    return [
        {"op": "gradient", "x": 0, "y": 0, "w": 72, "h": 16, "from": "#060c1a", "to": "#10081a", "direction": "horizontal"},
        {"op": "icon", "name": "star", "x": 3, "y": 3, "color": "#ffbf00"},
        {"op": "text", "x": 14, "y": 4, "text": f"{day} {date_str}", "color": "warmwhite", "font": "prop"},
    ]


def card_sys() -> List[Dict[str, Any]]:
    ncpu = max(1, os.cpu_count() or 1)
    try:
        load1, _, _ = os.getloadavg()
        cpu = max(0.0, min(100.0, (load1 / ncpu) * 100.0))
    except OSError:
        cpu = 0.0
    col = "#ff4433" if cpu >= 85 else ("amber" if cpu >= 60 else "#34e89e")
    return [
        {"op": "clear", "color": "#05070a"},
        {"op": "icon", "name": "bolt", "x": 3, "y": 3, "color": col},
        {"op": "text", "x": 14, "y": 2, "text": f"CPU {int(round(cpu))}%", "color": "warmwhite", "font": "3x5"},
        {"op": "progress", "x": 14, "y": 9, "w": 54, "h": 4, "value": cpu, "max": 100, "color": col, "bg": "#18202a"},
    ]


def make_git_card(repo: str) -> Callable[[], List[Dict[str, Any]]]:
    def _card() -> List[Dict[str, Any]]:
        try:
            br = subprocess.check_output(["git", "-C", repo, "rev-parse", "--abbrev-ref", "HEAD"], text=True, timeout=2.0).strip()
            st = subprocess.check_output(["git", "-C", repo, "status", "--porcelain"], text=True, timeout=2.0)
            dirty = len([l for l in st.splitlines() if l.strip()])
        except Exception:
            br, dirty = "NO GIT", 0
        col = "amber" if dirty else "#38e8b0"
        icon = "flame" if dirty else "check"
        sub = f"{dirty} MOD" if dirty else "CLEAN"
        return [
            {"op": "clear", "color": "#060a0e"},
            {"op": "icon", "name": icon, "x": 3, "y": 3, "color": col},
            {"op": "text", "x": 14, "y": 2, "text": br[:11], "color": "warmwhite", "font": "3x5"},
            {"op": "text", "x": 14, "y": 9, "text": sub, "color": col, "font": "3x5"},
        ]

    return _card


def make_weather_card(city: Optional[str]) -> Callable[[], List[Dict[str, Any]]]:
    cache: Dict[str, Any] = {"ts": 0.0, "temp": "--", "cond": "SUNNY"}

    def _card() -> List[Dict[str, Any]]:
        now = time.monotonic()
        if city and (now - cache["ts"] > 600.0):
            try:
                url = f"https://wttr.in/{urllib.parse.quote(city)}?format=%t|%C"
                req = urllib.request.Request(url, headers={"User-Agent": "markee-carousel"})
                with urllib.request.urlopen(req, timeout=4.0) as resp:
                    raw = resp.read().decode("utf-8", errors="replace").strip()
                if "|" in raw:
                    t, c = raw.split("|", 1)
                    cache["temp"] = t.strip().replace("+", "")[:6]
                    cache["cond"] = c.strip().upper()[:10]
                    cache["ts"] = now
            except Exception:
                pass
        icon = "sun" if "SUN" in cache["cond"] or "CLEAR" in cache["cond"] else "moon"
        label = (city or "LOCAL").upper()[:9]
        return [
            {"op": "clear", "color": "#050a14"},
            {"op": "icon", "name": icon, "x": 3, "y": 3, "color": "#ffcc33"},
            {"op": "text", "x": 14, "y": 2, "text": label, "color": "#88aacc", "font": "3x5"},
            {"op": "text", "x": 14, "y": 9, "text": f"{cache['temp']} {cache['cond']}"[:14], "color": "warmwhite", "font": "3x5"},
        ]

    return _card


def make_custom_card(spec: str) -> Tuple[str, Callable[[], List[Dict[str, Any]]]]:
    parts = [p.strip() for p in spec.split("|")]
    title = parts[0] if len(parts) > 0 else "INFO"
    value = parts[1] if len(parts) > 1 else ""
    color = parts[2] if len(parts) > 2 and parts[2] else "cyan"
    icon = parts[3] if len(parts) > 3 and parts[3] else "bell"

    def _card() -> List[Dict[str, Any]]:
        return [
            {"op": "clear", "color": "#06080f"},
            {"op": "icon", "name": icon, "x": 3, "y": 3, "color": color},
            {"op": "text", "x": 14, "y": 2, "text": title[:11], "color": "#99aabb", "font": "3x5"},
            {"op": "text", "x": 14, "y": 9, "text": value[:12], "color": color, "font": "3x5"},
        ]

    return f"custom:{title}", _card


def add_page_dots(ops: List[Dict[str, Any]], idx: int, total: int) -> List[Dict[str, Any]]:
    if total <= 1:
        return ops
    out = list(ops)
    span = total * 4 - 2
    x0 = max(2, (72 - span) // 2)
    for i in range(total):
        out.append({
            "op": "rect",
            "x": x0 + i * 4,
            "y": 15,
            "w": 2,
            "h": 1,
            "fill": "#ffffff" if i == idx else "#2c3440",
        })
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Rotating status carousel daemon for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token")
    parser.add_argument("--cards", default="clock,date,sys,git", help="Comma-separated built-in cards: clock,date,sys,git,weather")
    parser.add_argument("--card", action="append", default=[], help='Add custom card: "TITLE|VALUE|COLOR|ICON" (repeatable)')
    parser.add_argument("--repo", default=".", help="Git repo path for 'git' card (default: .)")
    parser.add_argument("--city", help="City name for optional 'weather' card (via wttr.in)")
    parser.add_argument("--interval", type=float, default=6.0, help="Seconds per card (default: 6.0)")
    parser.add_argument("--transition", default="slide-left", choices=["none", "fade", "slide-left", "slide-up", "wipe"], help="Card transition effect")
    parser.add_argument("--transition-ms", type=int, default=350, help="Transition duration in ms (default: 350)")
    parser.add_argument("--once", action="store_true", help="Render every card once and exit")
    parser.add_argument("--dry-run", action="store_true", help="Print card payloads without contacting markee")
    args = parser.parse_args()

    registry: Dict[str, Callable[[], List[Dict[str, Any]]]] = {
        "clock": card_clock,
        "date": card_date,
        "sys": card_sys,
        "git": make_git_card(args.repo),
        "weather": make_weather_card(args.city),
    }

    cards: List[Tuple[str, Callable[[], List[Dict[str, Any]]]]] = []
    for name in [c.strip().lower() for c in args.cards.split(",") if c.strip()]:
        if name in registry:
            cards.append((name, registry[name]))
        else:
            print(f"Unknown card '{name}' (available: {', '.join(registry)})", file=sys.stderr)
            return 1
    for spec in args.card:
        cards.append(make_custom_card(spec))

    if not cards:
        print("No cards selected.", file=sys.stderr)
        return 1

    client = Client(args.url, args.token)
    last_event_id = 0
    if not args.dry_run:
        try:
            last_event_id = int(client.poll_events(0, wait_s=0.0).get("last_id", 0))
        except Exception:
            pass
        print(f"Carousel started on {args.url} ({len(cards)} cards, {args.interval:.1f}s each; Space/bezel advances)")

    idx = 0
    try:
        while True:
            name, builder = cards[idx]
            ops = add_page_dots(builder(), idx, len(cards))
            if args.dry_run:
                print(f"[card {idx + 1}/{len(cards)}] {name}: {len(ops)} draw ops")
            else:
                try:
                    client.draw(ops, transition=args.transition, transition_ms=args.transition_ms)
                    print(f"[card {idx + 1}/{len(cards)}] {name}")
                except urllib.error.URLError as exc:
                    print(f"Warning: cannot reach markee ({exc.reason})", file=sys.stderr)

            idx = (idx + 1) % len(cards)
            if args.once and idx == 0:
                return 0

            if args.dry_run:
                continue

            deadline = time.monotonic() + max(1.0, args.interval)
            while True:
                rem = deadline - time.monotonic()
                if rem <= 0:
                    break
                wait_s = min(1.0, max(0.1, rem))
                try:
                    ev_resp = client.poll_events(last_event_id, wait_s=wait_s)
                    last_event_id = int(ev_resp.get("last_id", last_event_id))
                    if any(ev.get("type") in ("button", "click") for ev in ev_resp.get("events", [])):
                        break
                except Exception:
                    time.sleep(wait_s)
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
