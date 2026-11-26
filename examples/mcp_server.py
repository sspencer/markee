#!/usr/bin/env python3
"""
Zero-dependency Model Context Protocol (MCP) stdio server for markee.

Exposes a running markee LED matrix display as MCP tools so AI coding agents
can show live task progress, build results, notifications, countdowns, and
gentle completion chimes or melodies.

Configuration via environment variables or CLI flags:
  MARKEE_URL    Full base URL (default: http://127.0.0.1:8080)
  MARKEE_HOST   Host when MARKEE_URL is unset (default: 127.0.0.1)
  MARKEE_PORT   Port when MARKEE_URL is unset (default: 8080)
  MARKEE_TOKEN  Optional Bearer token matching `markee --token`

Example MCP client config (Claude Desktop / Cursor / Antigravity):
  {
    "mcpServers": {
      "markee": {
        "command": "python3",
        "args": ["/path/to/markee/examples/mcp_server.py"],
        "env": { "MARKEE_URL": "http://127.0.0.1:8080" }
      }
    }
  }
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, List, Optional, Tuple


SERVER_INFO = {"name": "markee-mcp", "version": "1.0.0"}
PROTOCOL_VERSION = "2024-11-05"

TOOLS: List[Dict[str, Any]] = [
    {
        "name": "markee_info",
        "description": "Get markee display resolution, state, brightness, monochrome, and active widgets.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "markee_say",
        "description": "Display a status message on the LED matrix with optional icon, border, scrolling, transition, and alert sound/melody.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "Text to display."},
                "color": {"type": "string", "description": "Foreground color (e.g. green, amber, #ff8800)."},
                "bg": {"type": "string", "description": "Background fill color."},
                "border": {"type": "string", "description": "Optional rounded border color."},
                "icon": {
                    "type": "string",
                    "description": "Optional 8x8 built-in icon on left (check, cross, bell, wifi, flame, play, pause, battery, heart, star, clock, sun, moon, mail, bolt, music).",
                },
                "icon_color": {"type": "string", "description": "Icon color (defaults to text color)."},
                "scroll": {"type": "boolean", "description": "Scroll text horizontally if true."},
                "speed": {"type": "number", "description": "Scroll speed in pixels/sec (default 28)."},
                "font": {"type": "string", "description": "Font: 5x7, 3x5, or prop."},
                "transition": {"type": "string", "description": "Optional transition: fade, slide-left, slide-up, wipe."},
                "sound": {"type": "string", "description": "Optional alert sound: ding, chime, bell."},
                "melody": {"type": "string", "description": "Optional custom melody e.g. C5:120,E5:120,G5:240."},
            },
            "required": ["text"],
        },
    },
    {
        "name": "markee_notify",
        "description": "Show a transient notification banner for ttl seconds, then automatically restore the previous display.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "Notification message."},
                "ttl": {"type": "number", "description": "Duration in seconds (default 5)."},
                "color": {"type": "string", "description": "Text color (default warmwhite)."},
                "bg": {"type": "string", "description": "Background color (default #000000)."},
                "border": {"type": "string", "description": "Border color (default amber)."},
                "sound": {"type": "string", "description": "Optional alert sound: ding, chime, bell."},
                "melody": {"type": "string", "description": "Optional custom alert melody."},
            },
            "required": ["text"],
        },
    },
    {
        "name": "markee_progress",
        "description": "Display a task progress indicator with label and progress bar (ideal for long builds, tests, or agent runs).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "label": {"type": "string", "description": "Short top label (e.g. BUILD, TESTS, STEP 3/5)."},
                "value": {"type": "number", "description": "Current progress value."},
                "max": {"type": "number", "description": "Maximum value (default 100)."},
                "color": {"type": "string", "description": "Progress bar fill color (default cyan)."},
                "text_color": {"type": "string", "description": "Label text color (default warmwhite)."},
                "icon": {"type": "string", "description": "Optional 8x8 icon name on the left."},
            },
            "required": ["label", "value"],
        },
    },
    {
        "name": "markee_draw",
        "description": "Execute a raw array of markee POST /draw operations atomically.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "ops": {
                    "type": "array",
                    "description": "Array of draw operation objects (clear, pixel, line, rect, oval, polygon, text, icon, gradient, progress, sparkline, clock, countdown, image).",
                    "items": {"type": "object"},
                },
                "transition": {"type": "string", "description": "Optional transition: fade, slide-left, slide-up, wipe."},
                "transition_ms": {"type": "integer", "description": "Transition duration in ms (50..5000)."},
                "ttl": {"type": "number", "description": "Optional transient TTL in seconds."},
            },
            "required": ["ops"],
        },
    },
    {
        "name": "markee_timer",
        "description": "Start a live server-side countdown timer widget on the LED matrix.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "seconds": {"type": "number", "description": "Countdown duration in seconds."},
                "label": {"type": "string", "description": "Optional short label shown above/beside timer."},
                "color": {"type": "string", "description": "Timer color (default amber)."},
                "bar": {"type": "boolean", "description": "Show bottom progress bar (default true)."},
                "sound": {"type": "string", "description": "Completion alert sound: ding, chime, bell (default chime)."},
            },
            "required": ["seconds"],
        },
    },
    {
        "name": "markee_clock",
        "description": "Display the built-in live digital clock widget.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "format": {"type": "string", "description": "Clock format: HH:MM or HH:MM:SS (default HH:MM:SS)."},
                "color": {"type": "string", "description": "Clock text color (default warmwhite)."},
            },
        },
    },
    {
        "name": "markee_alert",
        "description": "Play a gentle synthesized alert sound or custom note melody on markee.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "sound": {"type": "string", "description": "Built-in sound: ding, chime, or bell (default chime)."},
                "melody": {"type": "string", "description": "Custom note melody e.g. C5:120,E5:120,G5:240 (overrides sound)."},
                "volume": {"type": "integer", "description": "Volume 0..100 (default 80)."},
            },
        },
    },
    {
        "name": "markee_clear",
        "description": "Clear the markee LED matrix to black.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


class MarkeeClient:
    def __init__(self, base_url: str, token: Optional[str] = None, timeout: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        body: Optional[bytes] = None,
        content_type: Optional[str] = None,
    ) -> Any:
        url = f"{self.base_url}{path}"
        headers: Dict[str, str] = {}
        if content_type:
            headers["Content-Type"] = content_type
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        req = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
                try:
                    return json.loads(raw)
                except json.JSONDecodeError:
                    return {"raw": raw}
        except urllib.error.HTTPError as exc:
            err_body = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {err_body}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"Cannot reach markee at {self.base_url}: {exc.reason}") from exc

    def post_json(self, path: str, payload: Any) -> Any:
        data = json.dumps(payload).encode("utf-8")
        return self.request("POST", path, body=data, content_type="application/json")


def display_size(client: MarkeeClient) -> Tuple[int, int]:
    """Return the live (width, height) of the matrix from GET /info.

    Queried on every layout-dependent tool call, so the layout stays correct
    even if markee is restarted with a different --resolution.
    """
    info = client.request("GET", "/info")
    if not isinstance(info, dict):
        return 72, 16
    return max(1, int(info.get("width", 72))), max(1, int(info.get("height", 16)))


def font_height(font: str) -> int:
    """Glyph height in pixels for a markee font name."""
    f = font.lower()
    if f in ("3x5", "4x6", "small"):
        return 5
    if f in ("8x8", "large", "bold"):
        return 8
    return 7  # 5x7 / 6x8 / prop


def execute_tool(client: MarkeeClient, name: str, args: Dict[str, Any]) -> Any:
    if name == "markee_info":
        return client.request("GET", "/info")

    if name == "markee_clear":
        return client.request("POST", "/clear")

    if name == "markee_alert":
        vol = int(args.get("volume", 80))
        melody = args.get("melody")
        if melody:
            return client.post_json("/alert", {"melody": str(melody), "volume": vol})
        sound = str(args.get("sound", "chime"))
        return client.post_json("/alert", {"sound": sound, "volume": vol})

    if name == "markee_notify":
        payload: Dict[str, Any] = {
            "text": str(args["text"]),
            "ttl": float(args.get("ttl", 5.0)),
            "color": str(args.get("color", "warmwhite")),
            "bg": str(args.get("bg", "#000000")),
            "border": str(args.get("border", "amber")),
        }
        if args.get("sound"):
            payload["sound"] = str(args["sound"])
        res = client.post_json("/notify", payload)
        if args.get("melody"):
            client.post_json("/alert", {"melody": str(args["melody"])})
        return res

    if name == "markee_say":
        text = str(args["text"])
        color = str(args.get("color", "warmwhite"))
        bg = args.get("bg")
        border = args.get("border")
        icon = args.get("icon")
        icon_color = str(args.get("icon_color", color))
        font = str(args.get("font", "prop"))
        scroll = bool(args.get("scroll", False))
        speed = float(args.get("speed", 28))
        w, h = display_size(client)
        text_y = max(0, (h - font_height(font)) // 2)
        right = w - 2  # right edge of the text area (2 px margin)

        ops: List[Dict[str, Any]] = []
        if bg:
            ops.append({"op": "clear", "color": str(bg)})
        else:
            ops.append({"op": "clear"})
        if border:
            ops.append({"op": "rect", "x": 0, "y": 0, "w": w, "h": h, "color": str(border), "radius": 2})

        text_x = 2
        clip_x = 2
        if icon:
            ops.append({"op": "icon", "name": str(icon), "x": 3, "y": max(0, (h - 8) // 2), "color": icon_color})
            text_x = 13
            clip_x = 13

        text_op: Dict[str, Any] = {
            "op": "text",
            "x": right if scroll else text_x,
            "y": text_y,
            "text": text,
            "color": color,
            "font": font,
        }
        if scroll:
            text_op["scroll"] = True
            text_op["speed"] = speed
            text_op["clip"] = [clip_x, 1, max(1, right - clip_x), max(1, h - 2)]
        else:
            text_op["w"] = max(1, right - text_x)
            text_op["align"] = "left" if icon else "center"
        ops.append(text_op)

        query = ""
        if args.get("transition"):
            query = "?" + urllib.parse.urlencode({"transition": str(args["transition"])})
        res = client.post_json(f"/draw{query}", ops)
        if args.get("melody"):
            client.post_json("/alert", {"melody": str(args["melody"])})
        elif args.get("sound"):
            client.post_json("/alert", {"sound": str(args["sound"])})
        return res

    if name == "markee_progress":
        label = str(args["label"])
        val = float(args["value"])
        max_val = float(args.get("max", 100.0))
        color = str(args.get("color", "cyan"))
        text_color = str(args.get("text_color", "warmwhite"))
        icon = args.get("icon")

        w, h = display_size(client)
        # Label row + 5 px bar form a 13-row block centered vertically.
        top = max(0, (h - 13) // 2)
        right = w - 2
        ops: List[Dict[str, Any]] = [{"op": "clear"}]
        left = 2
        if icon:
            ops.append({"op": "icon", "name": str(icon), "x": 2, "y": max(0, (h - 8) // 2), "color": color})
            left = 12
        max_chars = max(1, (right - left + 1) // 4 - 5)  # leave room for the right-aligned "100%"
        pct = int(round(max(0.0, min(1.0, val / max_val if max_val > 0 else 0.0)) * 100))
        ops.append({
            "op": "text",
            "x": left,
            "y": top,
            "text": label[:max_chars],
            "color": text_color,
            "font": "3x5",
        })
        ops.append({
            "op": "text",
            "x": left,
            "y": top,
            "w": max(1, right - left),
            "align": "right",
            "text": f"{pct}%",
            "color": color,
            "font": "3x5",
        })
        ops.append({
            "op": "progress",
            "x": left,
            "y": top + 8,
            "w": max(1, right - left),
            "h": 5,
            "value": val,
            "max": max_val,
            "color": color,
            "bg": "#181c22",
            "border": "#333c48",
        })
        return client.post_json("/draw", ops)

    if name == "markee_draw":
        params: Dict[str, str] = {}
        if args.get("transition"):
            params["transition"] = str(args["transition"])
        if args.get("transition_ms") is not None:
            params["transition_ms"] = str(int(args["transition_ms"]))
        if args.get("ttl") is not None:
            params["ttl"] = str(float(args["ttl"]))
        q = ("?" + urllib.parse.urlencode(params)) if params else ""
        return client.post_json(f"/draw{q}", args["ops"])

    if name == "markee_timer":
        secs = float(args["seconds"])
        color = str(args.get("color", "amber"))
        bar = bool(args.get("bar", True))
        sound = str(args.get("sound", "chime"))
        label = args.get("label")
        w, h = display_size(client)
        digits_y = max(0, (h - 7) // 2)
        ops: List[Dict[str, Any]] = [{"op": "clear"}]
        if label:
            # "00:00" in prop is ~23 px wide; give the label whatever is left.
            label_chars = max(1, (w - 2 - 2 - 26) // 4)
            ops.append({"op": "text", "x": 2, "y": digits_y + 1, "text": str(label)[:label_chars], "color": "warmwhite", "font": "3x5"})
            ops.append({
                "op": "countdown",
                "x": w - 2,
                "y": digits_y,
                "align": "right",
                "seconds": secs,
                "color": color,
                "bar": bar,
                "sound": sound,
                "font": "prop",
            })
        else:
            ops.append({
                "op": "countdown",
                "y": digits_y,
                "align": "center",
                "seconds": secs,
                "color": color,
                "bar": bar,
                "sound": sound,
            })
        return client.post_json("/draw", ops)

    if name == "markee_clock":
        raw_fmt = str(args.get("format", "24h"))
        fmt_map = {"HH:MM:SS": "24h", "HH:MM": "24hm"}
        fmt = fmt_map.get(raw_fmt, raw_fmt)
        color = str(args.get("color", "warmwhite"))
        _, h = display_size(client)
        ops = [
            {"op": "clear"},
            {"op": "clock", "y": max(0, (h - 7) // 2), "align": "center", "format": fmt, "color": color},
        ]
        return client.post_json("/draw", ops)

    raise ValueError(f"Unknown tool: {name}")


def handle_message(client: MarkeeClient, msg: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    method = msg.get("method")
    req_id = msg.get("id")
    params = msg.get("params") or {}

    # Notifications have no id and expect no response.
    if req_id is None:
        return None

    if method == "initialize":
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {}},
                "serverInfo": SERVER_INFO,
            },
        }

    if method == "ping":
        return {"jsonrpc": "2.0", "id": req_id, "result": {}}

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": TOOLS}}

    if method == "tools/call":
        tool_name = params.get("name", "")
        tool_args = params.get("arguments") or {}
        try:
            result = execute_tool(client, tool_name, tool_args)
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "content": [{"type": "text", "text": json.dumps(result, indent=2)}],
                    "isError": False,
                },
            }
        except Exception as exc:
            return {
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "content": [{"type": "text", "text": f"Error: {exc}"}],
                    "isError": True,
                },
            }

    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "error": {"code": -32601, "message": f"Method not found: {method}"},
    }


def serve_stdio(client: MarkeeClient) -> int:
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError as exc:
            err = {
                "jsonrpc": "2.0",
                "id": None,
                "error": {"code": -32700, "message": f"Parse error: {exc}"},
            }
            sys.stdout.write(json.dumps(err) + "\n")
            sys.stdout.flush()
            continue

        resp = handle_message(client, msg)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    return 0


def default_url() -> str:
    if os.environ.get("MARKEE_URL"):
        return os.environ["MARKEE_URL"]
    host = os.environ.get("MARKEE_HOST", "127.0.0.1")
    port = os.environ.get("MARKEE_PORT", "8080")
    return f"http://{host}:{port}"


def main() -> int:
    parser = argparse.ArgumentParser(description="MCP stdio server for markee")
    parser.add_argument("--url", default=default_url(), help="markee base URL (default: $MARKEE_URL or http://127.0.0.1:8080)")
    parser.add_argument("--token", default=os.environ.get("MARKEE_TOKEN"), help="Optional Bearer token (default: $MARKEE_TOKEN)")
    args = parser.parse_args()

    client = MarkeeClient(args.url, token=args.token)
    return serve_stdio(client)


if __name__ == "__main__":
    raise SystemExit(main())
