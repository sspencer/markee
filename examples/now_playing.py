#!/usr/bin/env python3
"""
Now Playing ticker applet for markee with album-art ambient highlight bar.

Polls the active media player:
  - macOS: Spotify and Apple Music (via osascript)
  - Linux: Any MPRIS player (via playerctl)
  - Fallback/Demo: `--demo` or `--track "Title - Artist"`

Features:
  - Extracts the vibrant highlight color from the current track's album art
    (via Apple Music artwork / Spotify artwork URL / MPRIS mpris:artUrl /
    iTunes Search API fallback) and lights up a full-width ambient LED strip
    along the bottom edge of the display (inspired by Tesla Model Y ambient LEDs)
  - Music/pause icon + miniature equalizer bars on the left
  - Clipped scrolling track/artist marquee on the right (`clip: [13, 1, 58, 12]`)

Zero external Python dependencies (stdlib only; uses built-in `sips` on macOS).

Usage:
  python3 examples/now_playing.py
  python3 examples/now_playing.py --demo
"""

import argparse
import colorsys
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple


DEFAULT_ACCENT = "#1db954"


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"].rstrip("/")
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


def _run_cmd(args: List[str], timeout: float = 2.0) -> Optional[str]:
    try:
        res = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
        if res.returncode == 0:
            return res.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def _read_bmp_rgb(bmp_path: str) -> List[Tuple[int, int, int]]:
    """Parse an uncompressed 24-bit or 32-bit BMP into a list of (r, g, b) tuples."""
    with open(bmp_path, "rb") as f:
        data = f.read()
    if len(data) < 54 or data[:2] != b"BM":
        return []
    pix_off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if w <= 0 or h == 0 or bpp not in (24, 32) or comp not in (0, 3):
        return []
    abs_h = abs(h)
    bytes_pp = bpp // 8
    row_stride = ((w * bytes_pp + 3) // 4) * 4
    pixels: List[Tuple[int, int, int]] = []
    for y in range(abs_h):
        off = pix_off + y * row_stride
        if off + w * bytes_pp > len(data):
            break
        for x in range(w):
            p = off + x * bytes_pp
            b, g, r = data[p], data[p + 1], data[p + 2]
            pixels.append((r, g, b))
    return pixels


def _image_file_to_rgb(img_path: str, tmp_dir: str) -> List[Tuple[int, int, int]]:
    """Convert any JPEG/PNG/TIFF artwork file into 24x24 RGB tuples using system tools."""
    bmp_path = os.path.join(tmp_dir, "thumb.bmp")
    if shutil.which("sips"):
        res = subprocess.run(
            ["sips", "-z", "24", "24", "-s", "format", "bmp", img_path, "--out", bmp_path],
            capture_output=True,
            timeout=3.0,
            check=False,
        )
        if res.returncode == 0 and os.path.exists(bmp_path):
            return _read_bmp_rgb(bmp_path)

    if shutil.which("convert"):
        res = subprocess.run(
            ["convert", img_path, "-resize", "24x24!", "BMP3:" + bmp_path],
            capture_output=True,
            timeout=3.0,
            check=False,
        )
        if res.returncode == 0 and os.path.exists(bmp_path):
            return _read_bmp_rgb(bmp_path)

    if shutil.which("ffmpeg"):
        res = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", img_path, "-vf", "scale=24:24", bmp_path],
            capture_output=True,
            timeout=3.0,
            check=False,
        )
        if res.returncode == 0 and os.path.exists(bmp_path):
            return _read_bmp_rgb(bmp_path)

    return []


def extract_highlight_color(pixels: List[Tuple[int, int, int]]) -> Optional[str]:
    """
    Pick a vivid, Tesla-style ambient highlight color from sampled RGB pixels.

    Buckets chromatic pixels across 36 hue slices (10 deg each), weighting by
    saturation and luminance so luminous accent colors beat dark shadows and
    muddy grays, then boosts brightness/saturation for crisp LED readability.
    """
    if not pixels:
        return None

    buckets: List[List[Tuple[float, int, int, int]]] = [[] for _ in range(36)]
    for r, g, b in pixels:
        h, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        if s < 0.15 or v < 0.15:
            continue
        if s < 0.18 and v > 0.88:
            continue
        # Favor rich, luminous mid-to-bright accents over muddy shadows
        weight = (s ** 1.6) * (v ** 0.8) * (1.0 - abs(v - 0.65) * 0.35)
        buckets[int(h * 36) % 36].append((weight, r, g, b))

    totals = [sum(item[0] for item in bkt) for bkt in buckets]
    best_idx = max(range(36), key=lambda i: totals[i])

    if totals[best_idx] <= 0.0:
        # Monochrome / grayscale album cover: return a clean cool-silver highlight
        avg_luma = sum(0.2126 * r + 0.7152 * g + 0.0722 * b for r, g, b in pixels) / len(pixels)
        return "#e8edf5" if avg_luma >= 90 else "#8aa0b8"

    # Blend winning bucket with immediate neighbors for smooth hue accuracy
    pts = buckets[best_idx] + buckets[(best_idx - 1) % 36] + buckets[(best_idx + 1) % 36]
    tw = sum(item[0] for item in pts)
    rr = sum(item[0] * item[1] for item in pts) / tw
    gg = sum(item[0] * item[2] for item in pts) / tw
    bb = sum(item[0] * item[3] for item in pts) / tw

    h, s, v = colorsys.rgb_to_hsv(rr / 255.0, gg / 255.0, bb / 255.0)
    nr, ng, nb = colorsys.hsv_to_rgb(h, max(s, 0.60), max(v, 0.85))
    return f"#{int(round(nr * 255)):02x}{int(round(ng * 255)):02x}{int(round(nb * 255)):02x}"


def _download_to_file(url: str, dst_path: str, timeout: float = 3.5) -> bool:
    try:
        if url.startswith("file://"):
            local = urllib.parse.unquote(urllib.parse.urlparse(url).path)
            if os.path.isfile(local):
                shutil.copyfile(local, dst_path)
                return True
            return False
        if url.startswith("http://") or url.startswith("https://"):
            req = urllib.request.Request(url, headers={"User-Agent": "markee-now-playing"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = resp.read(4 * 1024 * 1024)
            with open(dst_path, "wb") as f:
                f.write(data)
            return len(data) > 0
    except Exception:
        pass
    return False


def _fetch_itunes_artwork_url(track: str, artist: str) -> Optional[str]:
    term = f"{artist} {track}".strip()
    if not term:
        return None
    try:
        q = urllib.parse.urlencode({"term": term, "entity": "song", "limit": "1"})
        req = urllib.request.Request(
            f"https://itunes.apple.com/search?{q}",
            headers={"User-Agent": "markee-now-playing"},
        )
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            payload = json.loads(resp.read().decode("utf-8", errors="replace"))
        results = payload.get("results") or []
        if results and results[0].get("artworkUrl100"):
            return str(results[0]["artworkUrl100"])
    except Exception:
        pass
    return None


def _extract_apple_music_artwork(dst_path: str) -> bool:
    escaped = dst_path.replace("\\", "\\\\").replace('"', '\\"')
    script = (
        'tell application "Music"\n'
        '  if (count of artworks of current track) > 0 then\n'
        '    set rawData to raw data of artwork 1 of current track\n'
        f'    set fRef to open for access (POSIX file "{escaped}") with write permission\n'
        '    try\n'
        '      set eof fRef to 0\n'
        '      write rawData to fRef\n'
        '      close access fRef\n'
        '      return "OK"\n'
        '    on error\n'
        '      try\n'
        '        close access fRef\n'
        '      end try\n'
        '    end try\n'
        '  end if\n'
        'end tell\n'
        'return ""'
    )
    out = _run_cmd(["osascript", "-e", script], timeout=3.0)
    return out == "OK" and os.path.isfile(dst_path) and os.path.getsize(dst_path) > 0


def resolve_album_highlight(
    app: str,
    track: str,
    artist: str,
    art_url: str,
) -> Optional[str]:
    """Retrieve album art for the current track and return its highlight hex color."""
    try:
        with tempfile.TemporaryDirectory(prefix="markee-art-") as td:
            img_path = os.path.join(td, "cover.img")
            have_art = False

            if app == "Music" and platform.system() == "Darwin":
                have_art = _extract_apple_music_artwork(img_path)

            if not have_art and art_url:
                have_art = _download_to_file(art_url, img_path)

            if not have_art and (track or artist):
                fallback_url = _fetch_itunes_artwork_url(track, artist)
                if fallback_url:
                    have_art = _download_to_file(fallback_url, img_path)

            if not have_art:
                return None

            pixels = _image_file_to_rgb(img_path, td)
            return extract_highlight_color(pixels)
    except Exception:
        return None


def poll_macos() -> Tuple[str, str, str, str, str]:
    """Return (state, app, track, artist, art_url)."""
    for app in ("Spotify", "Music"):
        art_expr = (
            'try\n        set artUrl to (artwork url of current track) as text\n      end try'
            if app == "Spotify"
            else ''
        )
        script = (
            f'if application "{app}" is running then\n'
            f'  tell application "{app}"\n'
            f'    set pstate to (player state as text)\n'
            f'    if pstate is "playing" or pstate is "paused" then\n'
            f'      set tname to ""\n'
            f'      set tartist to ""\n'
            f'      set artUrl to ""\n'
            f'      try\n'
            f'        set tname to (name of current track) as text\n'
            f'      end try\n'
            f'      try\n'
            f'        set tartist to (artist of current track) as text\n'
            f'      end try\n'
            f'      {art_expr}\n'
            f'      return pstate & "|||" & tname & "|||" & tartist & "|||" & artUrl\n'
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
            art_url = parts[3].strip() if len(parts) > 3 else ""
            if tname:
                return state, app, tname, tartist, art_url
    return "idle", "", "", "", ""


def poll_linux() -> Tuple[str, str, str, str, str]:
    if not shutil.which("playerctl"):
        return "idle", "", "", "", ""
    status = (_run_cmd(["playerctl", "status"]) or "").lower()
    if status not in ("playing", "paused"):
        return "idle", "", "", "", ""
    meta = _run_cmd(["playerctl", "metadata", "--format", "{{title}}|||{{artist}}|||{{mpris:artUrl}}"])
    if meta and "|||" in meta:
        parts = meta.split("|||")
        tname = parts[0].strip()
        tartist = parts[1].strip() if len(parts) > 1 else ""
        art_url = parts[2].strip() if len(parts) > 2 else ""
        if tname:
            return status, "mpris", tname, tartist, art_url
    return "idle", "", "", "", ""


def poll_player() -> Tuple[str, str, str, str, str]:
    if platform.system() == "Darwin":
        return poll_macos()
    return poll_linux()


def fetch_display_size(base_url: str, token: Optional[str]) -> Tuple[int, int]:
    """Return the live (width, height) of the matrix from GET /info."""
    headers = {"Authorization": f"Bearer {token}"} if token else {}
    req = urllib.request.Request(f"{base_url.rstrip('/')}/info", headers=headers)
    with urllib.request.urlopen(req, timeout=5.0) as resp:
        info = json.loads(resp.read().decode("utf-8"))
    return int(info.get("width", 72)), int(info.get("height", 16))


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


def build_ops(
    size: Tuple[int, int],
    state: str,
    label: str,
    speed: float,
    color: str,
    icon_color: str,
    accent_color: Optional[str] = None,
) -> List[Dict[str, Any]]:
    w, h = size
    # Icon, equalizer and ticker sit in a 16-row band centered vertically; the
    # ambient accent strip always spans the full width of the bottom two rows.
    top = max(0, (h - 16) // 2)
    if state == "idle" or not label:
        return [
            {"op": "clear"},
            {"op": "icon", "name": "music", "x": 3, "y": max(0, (h - 8) // 2), "color": "#445566"},
            {"op": "text", "x": 14, "y": max(0, (h - 7) // 2), "text": "NO MUSIC", "color": "#556677", "font": "prop"},
        ]

    playing = state == "playing"
    effective_icon_color = icon_color if playing else "amber"

    ops: List[Dict[str, Any]] = [
        {"op": "clear", "color": "#05070c"},
        {"op": "icon", "name": "music" if playing else "pause", "x": 2, "y": top + 1, "color": effective_icon_color},
        {"op": "line", "x1": 11, "y1": top + 1, "x2": 11, "y2": top + 12, "color": "#1e2736"},
    ]

    # Miniature equalizer bars cleanly between icon (y=1..8) and bottom accent strip (y=14..15)
    eq_heights = [2, 4, 2, 3] if playing else [1, 1, 1, 1]
    for i, bar_h in enumerate(eq_heights):
        ops.append({
            "op": "rect",
            "x": 2 + i * 2,
            "y": top + 14 - bar_h,
            "w": 1,
            "h": bar_h,
            "fill": effective_icon_color if playing else "#444a55",
        })

    # Tesla-style ambient album-art highlight strip along the bottom of the matrix
    if accent_color:
        glow_alpha = 0.24 if playing else 0.10
        bar_alpha = 1.0 if playing else 0.45
        ops.append({
            "op": "line",
            "x1": 0,
            "y1": h - 2,
            "x2": w - 1,
            "y2": h - 2,
            "color": accent_color,
            "alpha": glow_alpha,
        })
        ops.append({
            "op": "line",
            "x1": 0,
            "y1": h - 1,
            "x2": w - 1,
            "y2": h - 1,
            "color": accent_color,
            "alpha": bar_alpha,
        })

    text_op: Dict[str, Any] = {
        "op": "text",
        "x": w - 2 if playing else 14,
        "y": top + 3,
        "text": label,
        "color": color if playing else "#9aa5b1",
        "font": "prop",
        "clip": [13, top + 1, max(1, w - 14), 12],
    }
    if playing:
        text_op["scroll"] = True
        text_op["speed"] = speed
    ops.append(text_op)
    return ops


def main() -> int:
    parser = argparse.ArgumentParser(description="Now Playing LED ticker with album-art highlight bar")
    parser.add_argument("--url", default=default_url(), help="markee base URL")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token")
    parser.add_argument("--interval", type=float, default=2.0, help="Poll interval in seconds (default: 2.0)")
    parser.add_argument("--speed", type=float, default=26.0, help="Marquee scroll speed in px/s (default: 26)")
    parser.add_argument("--color", default="warmwhite", help="Track title color (default: warmwhite)")
    parser.add_argument("--icon-color", help="Override music icon color (defaults to album art highlight color)")
    parser.add_argument("--accent-color", help="Override bottom highlight bar color instead of sampling album art")
    parser.add_argument("--no-accent", action="store_true", help="Disable the bottom album-art highlight bar")
    parser.add_argument("--track", help="Display a fixed track string instead of polling a media player")
    parser.add_argument("--demo", action="store_true", help="Cycle through sample demo tracks with vibrant album highlights")
    args = parser.parse_args()

    demo_tracks = [
        ("playing", "Midnight City - M83", "#e03cff"),
        ("playing", "Instant Crush - Daft Punk", "#ff9d2e"),
        ("paused", "Resonance - HOME", "#24e0d0"),
    ]
    demo_idx = 0

    try:
        size = fetch_display_size(args.url, args.token)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        print(f"Error: cannot read display size from {args.url}/info ({exc})", file=sys.stderr)
        return 1

    last_key: Optional[Tuple[str, str, Optional[str]]] = None
    accent_cache: Dict[str, Optional[str]] = {}
    print(f"Now Playing ticker connected to {args.url} at {size[0]}x{size[1]} (Ctrl+C to stop)")

    try:
        while True:
            accent: Optional[str] = None
            if args.track:
                state, label = "playing", args.track
                accent = None if args.no_accent else (args.accent_color or DEFAULT_ACCENT)
            elif args.demo:
                state, label, demo_accent = demo_tracks[demo_idx % len(demo_tracks)]
                demo_idx += 1
                accent = None if args.no_accent else (args.accent_color or demo_accent)
            else:
                state, app, track, artist, art_url = poll_player()
                label = f"{track} - {artist}" if (track and artist) else track
                if state != "idle" and label and not args.no_accent:
                    if args.accent_color:
                        accent = args.accent_color
                    else:
                        cache_key = f"{app}|{track}|{artist}|{art_url}"
                        if cache_key not in accent_cache:
                            accent_cache[cache_key] = resolve_album_highlight(app, track, artist, art_url) or DEFAULT_ACCENT
                        accent = accent_cache[cache_key]

            icon_color = args.icon_color or accent or DEFAULT_ACCENT
            key = (state, label, accent)
            if key != last_key:
                ops = build_ops(size, state, label, args.speed, args.color, icon_color, accent_color=accent)
                try:
                    post_draw(args.url, args.token, ops, transition="fade" if last_key is not None else None)
                    acc_note = f" [accent {accent}]" if accent else ""
                    print(f"[{state}] {label or '(idle)'}{acc_note}")
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
