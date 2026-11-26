#!/usr/bin/env python3
"""
Live system monitor applet for markee.

Samples CPU load, memory usage, and network byte throughput using Python 3
stdlib only (macOS + Linux) and renders a compact dashboard laid out to the
live display size reported by GET /info (designed around 72x16) with:
  - Rolling CPU sparkline (one sample per pixel column of the left pane)
  - Color-coded CPU & MEM percentages (3x5 font)
  - Memory progress bar + live network activity indicator

Usage:
  python3 examples/sysmon.py
  python3 examples/sysmon.py --interval 1.0
"""

import argparse
import collections
import json
import os
import platform
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Deque, Dict, List, Optional, Tuple


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"].rstrip("/")
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


class SystemSampler:
    def __init__(self) -> None:
        self._sys = platform.system()
        self._prev_cpu: Optional[Tuple[int, int]] = None
        self._prev_net: Optional[Tuple[float, int]] = None
        self._ncpu = max(1, os.cpu_count() or 1)
        self._mac_total_mem: Optional[int] = None
        if self._sys == "Darwin":
            try:
                out = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True, timeout=1.5).strip()
                self._mac_total_mem = int(out)
            except Exception:
                self._mac_total_mem = None

    def cpu_percent(self) -> float:
        if self._sys == "Linux" and os.path.exists("/proc/stat"):
            try:
                with open("/proc/stat", "r", encoding="utf-8") as f:
                    parts = f.readline().split()
                if parts and parts[0] == "cpu":
                    nums = [int(x) for x in parts[1:9]]
                    idle = nums[3] + (nums[4] if len(nums) > 4 else 0)
                    total = sum(nums)
                    if self._prev_cpu is not None:
                        dt = total - self._prev_cpu[0]
                        di = idle - self._prev_cpu[1]
                        self._prev_cpu = (total, idle)
                        if dt > 0:
                            return max(0.0, min(100.0, (dt - di) * 100.0 / dt))
                    self._prev_cpu = (total, idle)
            except OSError:
                pass
        try:
            load1, _, _ = os.getloadavg()
            return max(0.0, min(100.0, (load1 / self._ncpu) * 100.0))
        except OSError:
            return 0.0

    def mem_percent(self) -> float:
        if self._sys == "Linux" and os.path.exists("/proc/meminfo"):
            try:
                info: Dict[str, int] = {}
                with open("/proc/meminfo", "r", encoding="utf-8") as f:
                    for line in f:
                        if ":" in line:
                            k, v = line.split(":", 1)
                            toks = v.strip().split()
                            if toks and toks[0].isdigit():
                                info[k] = int(toks[0])
                total = info.get("MemTotal", 0)
                avail = info.get("MemAvailable", info.get("MemFree", 0))
                if total > 0:
                    return max(0.0, min(100.0, (total - avail) * 100.0 / total))
            except OSError:
                pass

        if self._sys == "Darwin" and self._mac_total_mem:
            try:
                out = subprocess.check_output(["vm_stat"], text=True, timeout=1.5)
                page_size = 4096
                m_ps = re.search(r"page size of (\d+) bytes", out)
                if m_ps:
                    page_size = int(m_ps.group(1))
                pages: Dict[str, int] = {}
                for line in out.splitlines():
                    if ":" in line:
                        k, v = line.split(":", 1)
                        v_clean = v.strip().rstrip(".")
                        if v_clean.isdigit():
                            pages[k.strip()] = int(v_clean)
                used_pages = (
                    pages.get("Pages active", 0)
                    + pages.get("Pages wired down", 0)
                    + pages.get("Pages occupied by compressor", 0)
                )
                used_bytes = used_pages * page_size
                return max(0.0, min(100.0, used_bytes * 100.0 / self._mac_total_mem))
            except Exception:
                pass
        return 0.0

    def net_kbps(self) -> float:
        total_bytes: Optional[int] = None
        if self._sys == "Linux" and os.path.exists("/proc/net/dev"):
            try:
                s = 0
                with open("/proc/net/dev", "r", encoding="utf-8") as f:
                    for line in f.readlines()[2:]:
                        if ":" not in line:
                            continue
                        iface, rest = line.split(":", 1)
                        if iface.strip() == "lo":
                            continue
                        cols = rest.split()
                        if len(cols) >= 9:
                            s += int(cols[0]) + int(cols[8])
                total_bytes = s
            except OSError:
                pass
        elif self._sys == "Darwin":
            try:
                out = subprocess.check_output(["netstat", "-ibn"], text=True, timeout=1.5)
                s = 0
                for line in out.splitlines()[1:]:
                    cols = line.split()
                    if len(cols) >= 10 and not cols[0].startswith("lo") and cols[2].startswith("<Link#"):
                        if cols[6].isdigit() and cols[9].isdigit():
                            s += int(cols[6]) + int(cols[9])
                total_bytes = s
            except Exception:
                pass

        now = time.monotonic()
        if total_bytes is None:
            return 0.0
        if self._prev_net is None:
            self._prev_net = (now, total_bytes)
            return 0.0
        dt = max(0.001, now - self._prev_net[0])
        db = max(0, total_bytes - self._prev_net[1])
        self._prev_net = (now, total_bytes)
        return (db / 1024.0) / dt


def level_color(pct: float) -> str:
    if pct >= 85.0:
        return "#ff3b30"
    if pct >= 60.0:
        return "amber"
    return "#34e89e"


class Layout:
    """Pixel positions for the two-pane dashboard, derived from the display size.

    At 72x16 this reproduces the original hand-tuned layout: a CPU pane on the
    left (x=1..37), a divider at x=39, and a MEM/NET pane on the right. The
    sparkline grows to fill the remaining height on taller displays.
    """

    def __init__(self, width: int, height: int) -> None:
        self.w = max(16, width)
        self.h = max(16, height)
        self.div = self.w // 2 + 3                       # vertical divider column
        self.spark_w = max(2, min(256, self.div - 3))    # sparkline width == history length
        self.spark_h = self.h - 8
        self.right_x = self.div + 3                      # left edge of the right pane
        self.right_w = max(1, self.w - 1 - self.right_x)
        self.net_bar_x = self.right_x + 14
        self.net_bar_w = max(1, self.w - 1 - self.net_bar_x)


def build_ops(cpu: float, mem: float, net_kbps: float, history: Deque[float], lay: Layout) -> List[Dict[str, Any]]:
    c_col = level_color(cpu)
    m_col = level_color(mem)
    net_bar = min(100.0, (net_kbps / 512.0) * 100.0)  # 512 KB/s = full scale
    h = lay.h

    return [
        {"op": "clear", "color": "#05070a"},
        # Left pane: CPU label + value + sparkline
        {"op": "text", "x": 1, "y": 1, "text": "CPU", "color": "#7b8a9e", "font": "3x5"},
        {"op": "text", "x": 15, "y": 1, "w": lay.div - 17, "align": "right", "text": f"{int(round(cpu))}%", "color": c_col, "font": "3x5"},
        {
            "op": "sparkline",
            "x": 1,
            "y": 7,
            "w": lay.spark_w,
            "h": lay.spark_h,
            "values": [round(v, 1) for v in history],
            "min": 0,
            "max": 100,
            "color": c_col,
            "fill": "#0d221c" if cpu < 60 else "#281408",
        },
        # Vertical divider
        {"op": "line", "x1": lay.div, "y1": 1, "x2": lay.div, "y2": h - 2, "color": "#1c2430"},
        # Right pane: MEM bar + NET bar
        {"op": "text", "x": lay.right_x, "y": 1, "text": "MEM", "color": "#7b8a9e", "font": "3x5"},
        {"op": "text", "x": lay.right_x + 13, "y": 1, "w": max(1, lay.right_w - 13), "align": "right", "text": f"{int(round(mem))}%", "color": m_col, "font": "3x5"},
        {
            "op": "progress",
            "x": lay.right_x,
            "y": 7,
            "w": lay.right_w,
            "h": 3,
            "value": mem,
            "max": 100,
            "color": m_col,
            "bg": "#151a22",
        },
        {"op": "text", "x": lay.right_x, "y": h - 5, "text": "NET", "color": "#55687d", "font": "3x5"},
        {
            "op": "progress",
            "x": lay.net_bar_x,
            "y": h - 4,
            "w": lay.net_bar_w,
            "h": 3,
            "value": net_bar,
            "max": 100,
            "color": "#38bdf8",
            "bg": "#151a22",
        },
    ]


def fetch_display_size(base_url: str, token: Optional[str]) -> Tuple[int, int]:
    """Return the live (width, height) of the matrix from GET /info."""
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    req = urllib.request.Request(f"{base_url.rstrip('/')}/info", headers=headers)
    with urllib.request.urlopen(req, timeout=5.0) as resp:
        info = json.loads(resp.read().decode("utf-8"))
    return int(info.get("width", 72)), int(info.get("height", 16))


def post_draw(base_url: str, token: Optional[str], ops: List[Dict[str, Any]]) -> None:
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/draw",
        data=json.dumps(ops).encode("utf-8"),
        headers=headers,
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=5.0) as resp:
        resp.read()


def main() -> int:
    parser = argparse.ArgumentParser(description="Live CPU/MEM/NET system monitor for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token")
    parser.add_argument("--interval", type=float, default=1.0, help="Refresh interval in seconds (default: 1.0)")
    parser.add_argument("--once", action="store_true", help="Sample once, print metrics, and exit without posting if --dry-run")
    parser.add_argument("--dry-run", action="store_true", help="Print sampled metrics and draw payload without contacting markee")
    args = parser.parse_args()

    width, height = 72, 16
    if not args.dry_run:
        try:
            width, height = fetch_display_size(args.url, args.token)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            print(f"Error: cannot read display size from {args.url}/info ({exc})", file=sys.stderr)
            return 1
    lay = Layout(width, height)

    sampler = SystemSampler()
    history: Deque[float] = collections.deque([0.0] * lay.spark_w, maxlen=lay.spark_w)

    if not args.dry_run:
        print(f"System monitor streaming to {args.url} ({width}x{height}) every {args.interval:.1f}s (Ctrl+C to stop)")

    try:
        while True:
            cpu = sampler.cpu_percent()
            mem = sampler.mem_percent()
            net = sampler.net_kbps()
            history.append(cpu)
            ops = build_ops(cpu, mem, net, history, lay)

            if args.dry_run:
                print(f"CPU={cpu:.1f}% MEM={mem:.1f}% NET={net:.1f}KB/s ({len(ops)} draw ops)")
                if args.once:
                    return 0
            else:
                try:
                    post_draw(args.url, args.token, ops)
                except urllib.error.URLError as exc:
                    print(f"Warning: cannot reach markee ({exc.reason})", file=sys.stderr)
                if args.once:
                    return 0

            time.sleep(max(0.2, args.interval))
    except KeyboardInterrupt:
        print("\nStopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
