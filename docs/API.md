# markee HTTP API Reference

`markee` embeds a lightweight HTTP/1.1 server alongside the simulated LED matrix. Anything that can speak HTTP or open a TCP socket—`curl`, Python's standard library, browser `fetch()`, Node.js, Go, or `nc`—can drive the display, read pixels back, and trigger alert sounds.

- **Default base URL:** `http://127.0.0.1:8080` (configurable via `--host`, `--port`, or `MARKEE_PORT`; see [CLI options](../README.md#cli-options))
- **Default resolution:** **72×16** (24-bit RGB, **3,456** raw bytes). When launched with `--resolution WxH`, PNGs must be `W×H` and raw frames must be `W × H × 3` bytes. Query [`GET /info`](#get-info) at runtime to discover the active size.
- **Zero auth, open CORS:** All endpoints return JSON (except `GET /frame` image/raw readback) and allow cross-origin browser requests.

---

## Contents

- [Quick start](#quick-start)
- [Endpoint reference at a glance](#endpoint-reference-at-a-glance)
- [Core concepts](#core-concepts)
  - [Display state pipeline](#display-state-pipeline)
  - [Coordinates and raw RGB layout](#coordinates-and-raw-rgb-layout)
  - [Color formats](#color-formats)
- [Status](#status)
  - [GET /info](#get-info)
- [Drawing](#drawing)
  - [POST /draw](#post-draw)
  - [Draw operations](#draw-operations)
  - [Fonts](#fonts)
  - [Scrolling text](#scrolling-text)
- [Frames](#frames)
  - [GET /frame](#get-frame)
  - [POST /frame](#post-frame)
  - [DELETE /frame, POST /clear](#delete-frame-post-clear)
  - [POST /fill](#post-fill)
- [Animated GIFs](#animated-gifs)
  - [POST /animate](#post-animate)
  - [GET /animate](#get-animate)
  - [DELETE /animate](#delete-animate)
- [Pixels](#pixels)
  - [GET /pixel](#get-pixel)
  - [POST /pixel](#post-pixel)
- [Display settings](#display-settings)
  - [GET /brightness](#get-brightness)
  - [POST /brightness](#post-brightness)
  - [GET /monochrome](#get-monochrome)
  - [POST /monochrome](#post-monochrome)
  - [DELETE /monochrome](#delete-monochrome)
  - [POST /demo](#post-demo)
- [Alert sounds](#alert-sounds)
  - [GET /alert](#get-alert)
  - [POST /alert](#post-alert)
- [Recipes](#recipes)
- [Protocol details and limits](#protocol-details-and-limits)

---

## Quick start

```bash
# 1. Check device resolution and status
curl http://127.0.0.1:8080/info

# 2. Draw a banner with text and a status indicator in one atomic request
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear" },
  { "op": "oval", "x": 3, "y": 4, "w": 8, "h": 8, "fill": "green" },
  { "op": "text", "x": 15, "y": 4, "text": "ONLINE", "color": "warmwhite" }
]'

# 3. Play a gentle chime to catch the user'\''s attention
curl -X POST http://127.0.0.1:8080/alert/chime

# 4. Capture what is currently displayed as a 72x16 PNG
curl -o capture.png http://127.0.0.1:8080/frame
```

---

## Endpoint reference at a glance

| Group | Method | Path (aliases) | Summary |
|---|---|---|---|
| **Status** | `GET` | [`/info`](#get-info) (`/`, `/api/info`) | Resolution, settings, audio readiness, frame counter |
| **Drawing** | `POST` `PUT` | [`/draw`](#post-draw) (`/api/draw`) | Run an atomic JSON batch of shapes, pixels, text, and images (optional `?ttl=<sec>`) |
| **Notify** | `GET` `POST` `PUT` `DELETE` | [`/notify`](#post-notify) (`/api/notify`) | Show a transient notification banner or draw batch for `ttl` seconds, then restore prior state |
| **Frames** | `GET` | [`/frame`](#get-frame) (`/frame.png`, `/frame.rgb`, `/api/frame`) | Read displayed pixels as a PNG or raw RGB24 |
| | `POST` `PUT` | [`/frame`](#post-frame) (`/frame.png`, `/frame.rgb`, `/api/frame`) | Replace the entire frame with a PNG or raw RGB24 |
| | `DELETE` | [`/frame`](#delete-frame-post-clear) (`/api/frame`) | Clear display to black |
| | `POST` | [`/clear`](#delete-frame-post-clear) | Clear display to black |
| | `POST` | [`/fill`](#post-fill) | Fill the entire display with one color |
| **GIFs** | `POST` `PUT` | [`/animate`](#post-animate) (`/api/animate`) | Play an animated (or static) GIF, center-cropped or centered to fit |
| | `GET` | [`/animate`](#get-animate) (`/api/animate`) | Check active GIF animation status and current frame |
| | `DELETE` | [`/animate`](#delete-animate) (`/api/animate`) | Stop the active GIF animation (keeps current frame on screen) |
| **Pixels** | `GET` | [`/pixel`](#get-pixel) | Read a single pixel |
| | `POST` `PUT` | [`/pixel`](#post-pixel) | Set a single pixel |
| **Settings** | `GET` | [`/brightness`](#get-brightness) | Read GUI brightness (`0–100`) |
| | `POST` `PUT` | [`/brightness`](#post-brightness) | Set GUI brightness (`0–100`) |
| | `GET` `POST` `PUT` `DELETE` | [`/idle`](#idle-dimming) | Configure or disable idle dimming / night mode |
| | `GET` | [`/monochrome`](#get-monochrome) (`/mono`) | Read monochrome tint filter state |
| | `POST` `PUT` | [`/monochrome`](#post-monochrome) (`/mono`) | Enable or disable the monochrome tint filter |
| | `DELETE` | [`/monochrome`](#delete-monochrome) (`/mono`) | Disable the monochrome tint filter |
| | `POST` | [`/demo`](#post-demo) | Turn the built-in demo animation on or off |
| **Alerts** | `GET` | [`/alert`](#get-alert) (`/alerts`, `/api/alert`, `/api/alerts`) | List built-in alert sounds and audio status |
| | `POST` | [`/alert`](#post-alert), `/alert/<id\|name>` (`/api/alert`) | Play one of the 3 gentle alert sounds |
| **Events** | `GET` | [`/events`](#get-events) (`/api/events`) | Poll or stream (SSE) display click, bezel button, and frame update events |
| | `POST` | [`/events`](#get-events) (`/api/events`) | Inject a click or button event (useful for automation & headless testing) |
| **CORS** | `OPTIONS` | `*` | CORS preflight (`204 No Content`) |

---

## Core concepts

### Display state pipeline

Understanding how each layer interacts makes every endpoint predictable:

```text
  API Writes (/frame, /draw, /fill, /pixel, /clear)
        │  (also stops demo & prior scroller; increments frames_received)
        ▼
  ┌─────────────────────────┐
  │  Framebuffer (W×H RGB)  │◄── 60 Hz Demo or Scrolling Text (when active)
  └───────────┬─────────────┘
              ▼
  ┌─────────────────────────┐
  │ Monochrome Tint Filter  │  (non-destructive; applied on read)
  └──────┬──────────┬───────┘
         │          │
         ▼          ▼
    GET /frame   GUI Window ◄── Brightness (0–100%, GUI only)
    GET /pixel
```

- **Framebuffer writes** (`POST /frame`, `POST /draw`, `POST /fill`, `POST /pixel`, `DELETE /frame`, `POST /clear`):
  - Turn off the demo animation (if active).
  - Stop any active [scrolling text](#scrolling-text) (unless `/draw` defines a new scrolling `text` op).
  - Increment `frames_received` in [`GET /info`](#get-info).
- **Monochrome filter** is applied non-destructively whenever pixels are read (GUI, `GET /frame`, `GET /pixel`). The underlying RGB framebuffer keeps full color, so disabling the filter restores the original colors immediately.
- **Brightness (`0–100`)** dims only the simulated LEDs in the GUI window. Pixel readback (`GET /frame`, `GET /pixel`) is unaffected so read-modify-write loops never lose precision.
- **Settings and alerts** (`/brightness`, `/monochrome`, `/demo`, `/alert`) do not count as framebuffer writes and do not increment `frames_received`. Playing an alert never interrupts the display, demo, or scrolling text.

### Coordinates and raw RGB layout

- Origin `(0, 0)` is the **top-left** LED; `x` increases right (`0..W-1`) and `y` increases down (`0..H-1`).
- A **raw frame** is `W × H × 3` tightly packed bytes in row-major order (top to bottom, left to right), 3 bytes per pixel (`R, G, B`), with no header, padding, or alpha channel.
- Byte offset for pixel `(x, y)`: `(y * W + x) * 3`. At `72×16`, a raw frame is **3,456 bytes**.

### Color formats

Every color-accepting endpoint (`/draw`, `/fill`, `/pixel`, `/monochrome`) and the `--monochrome` CLI flag share the same parser. All hex digits, `0x` prefixes, and color names are **case-insensitive**.

| Format | Examples | Notes |
|---|---|---|
| 6-digit hex | `ff5500`, `0xff5500`, `#ff5500` | Standard 24-bit RGB |
| 3-digit hex | `f50`, `0xf50`, `#f50` | Shorthand (`f50` expands to `ff5500`) |
| Named color | `amber`, `warmwhite`, `teal` | See palette below |
| Channel fields (`0–255`) | `?r=255&g=85&b=0`, `{"r":255,"g":85,"b":0}` | Supported by `/fill`, `/pixel`, and `/draw` |
| RGB array (`0–255`) | `[255, 85, 0]` | Supported by `/draw` |
| Clear / none | `null`, `"none"`, `"transparent"` | Suppresses stroke/fill in `/draw`; `off`/`none` disables `/monochrome` |

**Built-in color names:**

| Family | Names |
|---|---|
| Neutrals | `black`, `white`, `warmwhite` (`#fff4e0`), `silver`, `gray` / `grey` (`#808080`) |
| Warm | `red`, `crimson`, `maroon`, `coral`, `orange` (`#ff8000`), `amber` (`#ffbf00`), `gold`, `yellow`, `brown`, `olive` |
| Cool | `green` / `lime` (`#00ff00`), `darkgreen` (`#008000`), `teal`, `turquoise`, `cyan` / `aqua`, `blue`, `navy` |
| Purples & pinks | `indigo`, `purple` (`#a020f0`), `violet`, `magenta` / `fuchsia`, `pink` (`#ff69b4`) |

> **URL tip: never put a raw `#` in a query string.** Clients like `curl` and browsers treat `#` as a URL fragment and strip everything after it before sending the request, causing a `400`. In URLs, write `?color=0xff5500`, `?color=ff5500`, `?color=amber`, or URL-encoded `%23ff5500`. Inside JSON bodies, `"#ff5500"` works directly.

---

## Status

### GET /info

**Methods:** `GET` · **Aliases:** `/`, `/api/info`

Returns device capabilities and live state. Clients that support custom resolutions should query this on startup to discover `width`, `height`, and `raw_frame_bytes`.

```bash
curl http://127.0.0.1:8080/info
```

```json
{
  "name": "markee",
  "width": 72,
  "height": 16,
  "bits_per_pixel": 24,
  "raw_frame_bytes": 3456,
  "refresh_hz": 60,
  "physical_width_inches": 6.35,
  "brightness": 100,
  "monochrome": null,
  "demo_active": false,
  "animation_active": false,
  "frames_received": 0,
  "audio_ready": true,
  "alert_sounds": 3
}
```

| Field | Type | Description |
|---|---|---|
| `name` | string | Always `"markee"` |
| `width`, `height` | int | Active LED matrix dimensions |
| `bits_per_pixel` | int | Always `24` |
| `raw_frame_bytes` | int | Exact raw RGB payload size (`width × height × 3`) |
| `refresh_hz` | int | Refresh rate (`60`) |
| `physical_width_inches` | number | Simulated physical width (`6.35` at 72 px wide; scales with `width` at fixed LED pitch) |
| `brightness` | int | GUI brightness (`0–100`) |
| `monochrome` | string \| null | Active tint color as `"#rrggbb"`, or `null` when off |
| `demo_active` | bool | Whether the built-in demo animation is running |
| `animation_active` | bool | Whether an animated GIF uploaded via `/animate` is currently playing |
| `frames_received` | int | Count of successful framebuffer writes since startup |
| `audio_ready` | bool | Whether an audio output device is ready for alert chimes |
| `alert_sounds` | int | Number of built-in alert sounds (`3`) |

---

## Drawing

### POST /draw

**Methods:** `POST`, `PUT` · **Aliases:** `/api/draw`

Executes a **JSON array** of one or more drawing operations in order, composited onto the current framebuffer.

- **Incremental by default:** the display is not cleared automatically—start the array with `{"op":"clear"}` when drawing a fresh screen.
- **Atomic:** operations run on a staged copy of the framebuffer. If any op fails validation, the server returns `400` and leaves the displayed frame unchanged.
- **Auto-clipped:** pixels, shapes, and text extending beyond the display bounds are clipped without error.
- **Array required:** a single operation must still be wrapped in an array (`[{...}]`). `Content-Type` is ignored.

```bash
curl -X POST http://127.0.0.1:8080/draw -H "Content-Type: application/json" -d '[
  { "op": "clear" },
  { "op": "rect",    "x": 0,  "y": 0, "w": 72, "h": 16, "color": "orange", "radius": 3 },
  { "op": "oval",    "x": 4,  "y": 4, "w": 8,  "h": 8,  "fill": "red" },
  { "op": "polygon", "points": [[60,12],[64,4],[68,12]], "fill": "yellow" },
  { "op": "text",    "x": 16, "y": 4, "text": "READY", "color": "warmwhite" }
]'
```

```json
{"ok":true,"ops":5}
```

**Common `400` errors:** empty body, non-array or empty array `[]`, JSON syntax error, missing `"op"`, unknown op name, invalid `"color"`/`"fill"` value, or op-specific validation errors (see below).

**Optional query parameters on `POST /draw`:**
- `?ttl=<sec>` (`0..86400`) — display the batch temporarily and automatically restore the previous display state when the timer expires.
- `?transition=<fade|slide-left|slide-up|wipe>&transition_ms=<50..5000>` — animate the switch from the previous frame to the new frame (default `250` ms).

### POST /notify

**Methods:** `GET`, `POST`, `PUT`, `DELETE` · **Alias:** `/api/notify`

Displays a temporary notification for `ttl` seconds (default `5`), then automatically restores whatever was previously on screen (static pixels, demo mode, scrolling text, or animated GIFs).

Accepts either:
- A JSON object (`text`, `color`, `bg`, `border`, `font`, `ttl`, `scroll`, `speed`, `sound`, `volume`),
- A plain-text body or query parameters (`?text=...&color=...&ttl=...&sound=chime`), or
- A `/draw` JSON array (`POST /notify?ttl=5 -d '[...]'`).

```bash
curl -X POST http://127.0.0.1:8080/notify \
  -d '{"text":"BUILD OK!","color":"warmwhite","bg":"#001808","border":"green","ttl":5,"sound":"chime"}'

# Check remaining time or dismiss early
curl http://127.0.0.1:8080/notify
curl -X DELETE http://127.0.0.1:8080/notify
```

### Draw operations

Every operation is a JSON object with a case-insensitive `"op"` field. Numeric coordinates and sizes are rounded to the nearest integer; omitted or non-numeric optional fields fall back to their defaults (typically `0`).

#### Stroke and fill rules

Drawing ops accept color through three fields:

| Field | Role |
|---|---|
| `color` | Stroke / foreground color: 1-px outline for `rect`/`oval`/`polygon`, or drawing color for `pixel`/`line`/`text` |
| `fill` | Interior fill color. `true` reuses `color` (or `white` if unset); `false`, `null`, or `"none"` disables fill |
| `r`, `g`, `b` | Fallback stroke color (`0–255`), used **only** when neither `color` nor `fill` is provided |

When **no color fields at all** are provided:

| Operation | Default behavior |
|---|---|
| `clear` | Fills with `black` |
| `pixel`, `line`, `text` | Draws in `white` (uses `fill` if only `fill` is given; for `pixel`, `"color":"none"`/`null` draws nothing) |
| `rect`, `oval`, `polygon` | Draws a `white` 1-px outline with no fill. If `fill` is given without `color`, only the fill is drawn; set `"color":"none"` or `null` to explicitly omit the outline |

#### `clear`

Fills the entire framebuffer with one color and stops any active scrolling text.

| Field | Default | Description |
|---|---|---|
| `color` (or `fill`) | `black` | Fill color |

```json
{ "op": "clear", "color": "#001020" }
```

#### `pixel`

Sets a single pixel. Out-of-bounds coordinates are silently clipped.

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Pixel coordinates |
| `color` | `white` | Pixel color (`"none"` or `null` draws nothing) |

```json
{ "op": "pixel", "x": 10, "y": 5, "color": "amber" }
```

#### `line`

Draws a 1-pixel Bresenham line including both endpoints.

| Field | Default | Description |
|---|---|---|
| `x1`, `y1`, `x2`, `y2` | `0` | Start and end coordinates |
| `color` | `white` | Line color |

```json
{ "op": "line", "x1": 0, "y1": 15, "x2": 71, "y2": 0, "color": "#00ff00" }
```

#### `rect`

Draws an axis-aligned rectangle with optional rounded corners. Nothing is drawn if `w ≤ 0` or `h ≤ 0`.

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Top-left corner |
| `w`, `h` | `0` | Width and height in pixels |
| `radius` | `0` | Corner radius, clamped to `min(w, h) / 2` |
| `color` | `white` (if no `fill`) | 1-px outline color |
| `fill` | none | Interior fill color |

```json
{ "op": "rect", "x": 2, "y": 2, "w": 30, "h": 12, "color": "white", "fill": "#002040", "radius": 2 }
```

#### `oval`

Draws an ellipse inscribed inside the bounding box `(x, y, w, h)`. Equal `w` and `h` draw a circle.

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Top-left of bounding box |
| `w`, `h` | `0` | Bounding box width and height |
| `color` | `white` (if no `fill`) | 1-px outline color |
| `fill` | none | Interior fill color |

```json
{ "op": "oval", "x": 4, "y": 4, "w": 8, "h": 8, "fill": "red" }
```

#### `polygon`

Draws a closed polygon (automatically closing the last vertex back to the first). Interior fill uses the even-odd scanline rule.

| Field | Default | Description |
|---|---|---|
| `points` | **required** | Array of 2–256 `[x, y]` coordinate pairs (fill requires ≥ 3 points) |
| `color` | `white` (if no `fill`) | 1-px outline color |
| `fill` | none | Interior fill color |

```json
{ "op": "polygon", "points": [[60,12],[64,4],[68,12]], "fill": "yellow" }
```

**Validation errors (`400`):** fewer than 2 points, more than 256 points, or any point that is not a 2-element numeric `[x, y]` array.

#### `text`

Draws single-line bitmap text without wrapping (overflowing glyphs are clipped).

| Field | Default | Description |
|---|---|---|
| `text` | **required** | String to render; characters outside printable ASCII (`0x20–0x7E`) render as `?` |
| `x`, `y` | `0` | Top-left origin of the first glyph (or anchor when `align` is set) |
| `w` | `0` | Optional box width used with `align` (`center`/`right`) when `clip` is not set |
| `align` | `"left"` | Horizontal alignment: `"left"`, `"center"`, or `"right"` (aligns within `clip`, `[x, x+w)`, full screen if `x` omitted, or anchored at `x`) |
| `font` | `"6x8"` | Font name (see [Fonts](#fonts)) |
| `color` | `white` | Text color |
| `scroll` | `false` | `true` enables horizontal marquee scrolling (see [Scrolling text](#scrolling-text)) |
| `speed` | `24` | Scroll speed in pixels/second (values `≤ 0` fall back to `24`) |
| `clip` | full screen | Optional `[x, y, w, h]` clip rectangle (`w > 0, h > 0`). Confines both static and scrolling text inside the box so tickers stay clear of side icons/GIFs |

```json
{ "op": "text", "x": 16, "y": 4, "text": "READY", "font": "6x8", "color": "warmwhite" }
```

#### `image`

Draws a PNG or GIF (`GIF87a`/`GIF89a`, static or animated) placed at `(x, y)` (top-left origin), clipped to the display bounds. Up to **8** animated GIF `image` ops can run simultaneously in a `/draw` batch (static PNGs and single-frame GIFs are baked directly into the framebuffer and do not count against this limit), and they can be combined freely with shapes, text, and scrolling text.

| Field | Default | Description |
|---|---|---|
| `data` | **required** | Base64-encoded PNG or GIF bytes (optional `data:image/png;base64,` or `data:image/gif;base64,` URI prefix accepted; whitespace ignored) |
| `x`, `y` | `0` | Top-left placement on the display (may be negative; clipped to display bounds) |
| `loop` | GIF setting | **Animated GIFs only:** `0` (or `true`) = loop forever, `1` (or `false`) = play once and hold last frame, `N > 0` = play `N` times. If omitted or `null`, honors `NETSCAPE2.0` (multi-frame GIFs without `NETSCAPE2.0` loop infinitely). Safely ignored for PNGs and single-frame GIFs |
| `fill` | transparent (`none`) | Color shown beneath transparent/semi-transparent pixels inside the image's `(x, y, w, h)` rectangle (any [color format](#color-formats) including `[r,g,b]`). PNG alpha is blended smoothly over `fill` (or over existing canvas pixels when `fill` is omitted, `null`, `"none"`, or `"transparent"`) |

```json
{ "op": "image", "x": 2, "y": 2, "data": "iVBORw0KGgo...", "fill": "white" }
{ "op": "image", "x": 2, "y": 0, "data": "R0lGODlh...", "loop": 0, "fill": "white" }
```

**Validation errors (`400`):** missing/empty `data`, invalid base64, unsupported or corrupt PNG/GIF data, negative or non-integer `loop` (for GIFs), invalid `fill` color, or more than 8 animated GIF `image` ops in a single batch.

#### `progress`

Draws a horizontal progress bar.

| Field | Default | Description |
|---|---|---|
| `value` | **required** | Current value (`0.0..1.0`, or `0..100` if `value > 1` and `max` is omitted) |
| `max` | `1.0` | Maximum value |
| `x`, `y`, `w`, `h` | `0`, `H-4`, `W`, `4` | Bar bounding box |
| `color` | `#00dc5a` | Filled bar color |
| `bg` | `#181a20` | Track color (`"none"` or `null` to omit) |
| `border` | none | Optional 1-px outer border color |

#### `sparkline`

Plots a compact numeric series (`1..256` values) as a polyline (`"style": "line"`, default) or vertical bars (`"style": "bars"`).

| Field | Default | Description |
|---|---|---|
| `values` | **required** | Array of 1–256 numbers |
| `x`, `y`, `w`, `h` | `0`, `0`, `W`, `H` | Plot bounding box |
| `min`, `max` | auto | Optional Y-axis bounds (defaults to data min/max) |
| `color` | `#00dcff` | Line or bar color |
| `fill` | none | Optional area fill under the line |
| `style` | `"line"` | `"line"` or `"bars"` |

#### `clock` & `countdown` (autonomous widgets)

Live widgets that update automatically at 60 Hz without client polling.

- **`clock`:** `format` (`"24h"` `%H:%M:%S`, `"24hm"` `%H:%M`, `"12h"`, `"12hm"`), `x`, `y`, `font`, `color`, `align` (defaults to centered when `x` omitted).
- **`countdown`:** `seconds` (**required** `> 0`), `x`, `y`, `font`, `color`, `done_color` (default green), `done_text` (default `"00:00"`), `bar` (`true` draws a 2-px bottom progress bar), `bar_color`, and optional `sound` (`1..3` or `ding`/`chime`/`bell`) played automatically when the timer reaches zero (also emits a `button`/`countdown_done` event on `/events`).

```json
[
  { "op": "clear" },
  { "op": "countdown", "seconds": 1500, "color": "amber", "bar": true, "sound": "chime" }
]
```

#### `icon`

Draws one of 16 built-in 8×8 monochrome pixel icons (`check`, `cross`, `bell`, `wifi`, `flame`, `play`, `pause`, `battery`, `heart`, `star`, `clock`, `sun`, `moon`, `mail`, `bolt`, `music`) at `(x, y)` with optional integer `scale` (`1..4`, default `1`) and `color` (default `white`).

```json
{ "op": "icon", "name": "check", "x": 4, "y": 4, "color": "green", "scale": 1 }
```

#### `gradient` & shape `alpha`

Draws a horizontal (`"horizontal"`/`"h"`, default) or vertical (`"vertical"`/`"v"`) linear color gradient from `from` to `to` inside `(x, y, w, h)` (defaults to full display).

All static shape ops (`pixel`, `line`, `rect`, `oval`, `polygon`, `icon`, `gradient`) also accept an optional `"alpha"` (`0.0..1.0`, default `1.0`) for smooth compositing over underlying pixels.

```json
{ "op": "gradient", "from": "#001040", "to": "#400020", "direction": "horizontal" }
{ "op": "rect", "x": 0, "y": 0, "w": 72, "h": 16, "fill": "black", "alpha": 0.5 }
```

### Fonts

| `font` values | Glyph size | Advance | Supported characters | Fit on 72×16 |
|---|---|---|---|---|
| `"6x8"` (default), `"5x7"`, `"medium"`, `"default"`, or unknown | 5×7 | 6 px | Printable ASCII (`0x20–0x7E`) | 12 chars/line; `y: 4` centers vertically |
| `"8x8"`, `"large"`, `"bold"` | 6×7 (bold 5×7) | 7 px | Printable ASCII (`0x20–0x7E`) | 10 chars/line; `y: 4` centers vertically |
| `"4x6"`, `"3x5"`, `"small"` | 3×5 | 4 px | `A–Z` (case-insensitive), `0–9`, space, `! . : - + /` (others blank) | 18 chars/line; two rows fit at `y: 1` and `y: 9` |
| `"prop"`, `"proportional"`, `"compact"` | variable×7 | glyph width + 1 px | Printable ASCII (`0x20–0x7E`) | Variable (~14–24 chars/line; narrow glyphs like `i`, `l`, `1`, `:` save space) |

Total rendered width is `strlen(text) × advance` pixels.

### Scrolling text

Setting `"scroll": true` on a `text` op scrolls the string smoothly from right to left at `speed` px/s (60 Hz) and wraps seamlessly back in from the right edge once it leaves the left edge.

- **Background preservation:** whatever was drawn earlier in the same `/draw` batch is snapshotted as the static background behind the moving text.
- **Place the scrolling `text` op last:** ops placed after it in the same batch are drawn only on the first frame and overwritten on the next 60 Hz tick.
- **Single scroller:** only one scrolling `text` op can be active at a time. A later `text` or `clear` op in the same batch replaces or stops it; any subsequent framebuffer write (`/frame`, `/fill`, `/pixel`, `/clear`, non-scrolling `/draw`) or `POST /demo?enabled=1` stops it and leaves the current pixels in place.
- **Headless & readback:** scrolling continues at 60 Hz in `--headless` mode, and `GET /frame` captures the text at its live position.
- **Length cap:** scrolling strings are truncated at 511 bytes.

```bash
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear", "color": "#100800" },
  { "op": "line", "x1": 0, "y1": 15, "x2": 71, "y2": 15, "color": "orange" },
  { "op": "text", "x": 72, "y": 4, "text": "Now playing: something good", "color": "amber", "scroll": true, "speed": 30 }
]'
```

---

## Frames

### GET /frame

**Methods:** `GET` · **Aliases:** `/frame.png`, `/frame.rgb`, `/api/frame`

Reads back the currently displayed pixels (including active demo or scrolling text position). The monochrome tint filter is applied if enabled; GUI brightness is not applied.

| Output format | How to select | `Content-Type` |
|---|---|---|
| **PNG** (default) | `GET /frame`, `GET /frame.png` | `image/png` (`W×H`, 24-bit RGB, no alpha) |
| **Raw RGB24** | `GET /frame.rgb`, `?format=raw`, `?format=rgb`, or header `Accept: application/octet-stream` | `application/octet-stream` (`W × H × 3` bytes) |

```bash
curl -o capture.png http://127.0.0.1:8080/frame
curl -o capture.rgb "http://127.0.0.1:8080/frame?format=raw"   # 3456 bytes at 72x16
```

### POST /frame

**Methods:** `POST`, `PUT` · **Aliases:** `/frame.png`, `/frame.rgb`, `/api/frame`

Replaces the entire framebuffer from a PNG image or raw 24-bit RGB bytes.

**Payload format detection (in order):**
1. Path is `/frame.rgb` → always parsed as **raw RGB24**.
2. Body starts with the 8-byte PNG signature, `Content-Type` contains `image/png`, or path is `/frame.png` → parsed as **PNG**.
3. Body length is exactly `W × H × 3` bytes → parsed as **raw RGB24**.
4. Otherwise → `400 Bad Request`.

**PNG rules:**
- Dimensions must match the active resolution (`W×H`) exactly; markee does not scale images.
- Any PNG color type supported by `stb_image` (RGB, RGBA, grayscale, indexed palette) is accepted.
- Alpha is premultiplied/composited onto **black** (`rgb × alpha / 255`); opaque PNGs are loaded losslessly.

```bash
# Upload a 72x16 PNG
curl -X POST --data-binary @frame.png http://127.0.0.1:8080/frame

# Upload raw RGB24 (3456 bytes at 72x16)
curl -X PUT --data-binary @frame.rgb http://127.0.0.1:8080/frame.rgb
```

> Use `--data-binary` with `curl`. Using `-d` strips newlines and corrupts binary files.

```json
{"ok":true,"format":"png","width":72,"height":16}
{"ok":true,"format":"rgb24","width":72,"height":16}
```

**`400` errors:** empty body, wrong PNG dimensions (`expected 72x16 PNG, got 64x32`), corrupt/unreadable PNG data, or wrong raw byte length.

### DELETE /frame, POST /clear

**Methods:** `DELETE /frame` (alias `/api/frame`), `POST /clear`

Clears every LED to black (`#000000`). To clear to another color, use [`POST /fill`](#post-fill) or a `clear` [draw op](#clear).

```bash
curl -X POST http://127.0.0.1:8080/clear
```

```json
{"ok":true}
```

### POST /fill

**Methods:** `POST`

Fills every pixel with a single color. The color is read from the **first** source present (if that source is invalid, the request returns `400` without falling through):

| Priority | Source | Example |
|---|---|---|
| 1 | Query `color` | `?color=0xff5500`, `?color=amber` |
| 2 | Query `r`, `g`, `b` (all three) | `?r=255&g=85&b=0` |
| 3 | JSON body `"color"` | `{"color":"#ff5500"}` |
| 4 | JSON body `"r"`, `"g"`, `"b"` | `{"r":255,"g":85,"b":0}` |

```bash
curl -X POST "http://127.0.0.1:8080/fill?color=0xf25c19"
curl -X POST "http://127.0.0.1:8080/fill?color=amber"
curl -X POST http://127.0.0.1:8080/fill -d '{"r":0,"g":32,"b":64}'
```

```json
{"ok":true,"r":242,"g":92,"b":25,"hex":"#f25c19"}
```

---

## Animated GIFs

### POST /animate

**Methods:** `POST`, `PUT` · **Alias:** `/api/animate`

Decodes a GIF87a or GIF89a image (animated or static) and plays it on the LED matrix. Frame 0 is displayed immediately, and subsequent frames advance in `markee`'s 60 Hz loop using each frame's own delay time.

**Sizing (per axis, independently):**
- **Too big:** center-cropped to the display resolution (`W×H`).
- **Too small:** centered on the fill color (black by default).
- **Mixed:** e.g. a `100×8` GIF on a `72×16` display is center-cropped horizontally and centered vertically.

**Supported GIF features:** Global and per-frame Local Color Tables, interlacing, sub-frame rectangles, 1-bit transparency (composited over the `fill` color, black by default), all 4 disposal methods, per-frame delays (delays `< 20 ms` are clamped to `100 ms` matching standard browser behavior), and `NETSCAPE2.0` loop counts.

**Optional query parameters:**
- `?fill=<color>` — background color displayed beneath transparent GIF pixels and centered margins (accepts any [color string](#color-formats) such as `white`, `0x202028`, `e0e0e0`, `amber`; defaults to black `#000000`).
- `?loop=<n>` — override loop count (`0` = loop forever, `1` = play once and hold last frame, `N` = play `N` times). If omitted, honors the GIF's `NETSCAPE2.0` loop count (multi-frame GIFs without `NETSCAPE2.0` loop infinitely).
- `?fit=<crop|contain|stretch>` — sizing mode (`crop` default center-crops/centers; `contain` scales preserving aspect ratio to fit inside the display; `stretch` fills exact display dimensions).
- `?speed=<0.1..10.0>` — playback speed multiplier (default `1.0`).

**Playback control endpoints:**
- `POST /animate/pause` — freeze playback on the current frame.
- `POST /animate/resume` — resume paused playback.
- `POST /animate/speed?value=<0.1..10.0>` (or JSON `{"speed":2.0}`) — adjust playback speed live.

```bash
# Play an animated GIF (use --data-binary so curl does not alter binary bytes)
curl -X POST --data-binary @nyan.gif http://127.0.0.1:8080/animate

# Composite a dark-foreground transparent GIF over a light background
curl -X POST --data-binary @examples/cat_chase1.gif "http://127.0.0.1:8080/animate?fill=white"

# Play once over a custom hex background and hold on the last frame
curl -X POST --data-binary @intro.gif "http://127.0.0.1:8080/animate?fill=0x303040&loop=1"
```

```json
{
  "ok": true,
  "format": "gif",
  "width": 72,
  "height": 16,
  "source_width": 100,
  "source_height": 20,
  "frames": 12,
  "duration_ms": 1200,
  "loop": 0,
  "fill": "#000000",
  "active": true
}
```

Any subsequent framebuffer write (`/frame`, `/draw`, `/fill`, `/pixel`, `/clear`, another `/animate`) or `POST /demo?enabled=1` automatically stops the active GIF animation.

**`400` errors:** empty body, invalid `?fill=` or `?loop=` value, corrupt/truncated GIF, or exceeding the decoder's safety caps (4096×4096 source dimensions, 4096 frames, or 32 MiB decoded frame buffer).

### GET /animate

**Methods:** `GET` · **Alias:** `/api/animate`

Returns whether a GIF animation is currently playing, along with its dimensions, frame count, fill color, and current frame index.

```json
{"active":true,"width":72,"height":16,"source_width":100,"source_height":20,"frames":12,"current_frame":3,"duration_ms":1200,"loop":0,"fill":"#000000"}
{"active":false}
```

### DELETE /animate

**Methods:** `DELETE` · **Alias:** `/api/animate`

Stops the active GIF animation while keeping the currently displayed frame on screen. Returns `{"ok":true,"active":false}`.

---

## Pixels

### GET /pixel

**Methods:** `GET`

Reads one pixel's displayed RGB color (with the monochrome filter applied if enabled). Requires integer query parameters `x` and `y` within `0..W-1` and `0..H-1`.

```bash
curl "http://127.0.0.1:8080/pixel?x=10&y=5"
```

```json
{"x":10,"y":5,"r":0,"g":255,"b":204,"hex":"#00ffcc"}
```

**`400` errors:** missing/non-integer `x` or `y`, or coordinates outside the matrix bounds.

### POST /pixel

**Methods:** `POST`, `PUT`

Sets a single pixel.

- **Coordinates:** read from query `x` and `y`; if either is absent, falls back to JSON body `"x"` and `"y"`.
- **Color:** resolved using the exact same priority order as [`POST /fill`](#post-fill). Coordinates and color may be mixed across query and JSON body.

```bash
curl -X POST "http://127.0.0.1:8080/pixel?x=10&y=5&color=0x00ffcc"
curl -X POST "http://127.0.0.1:8080/pixel?x=10&y=5&r=0&g=255&b=204"
curl -X POST http://127.0.0.1:8080/pixel -d '{"x":10,"y":5,"color":"#00ffcc"}'
```

```json
{"ok":true,"x":10,"y":5,"r":0,"g":255,"b":204}
```

Out-of-range coordinates return `400`. Because each call counts as a full framebuffer write, prefer a batched [`POST /draw`](#post-draw) (using `"op": "pixel"`) or [`POST /frame`](#post-frame) when updating multiple pixels.

---

## Display settings

### GET /brightness

**Methods:** `GET`

Returns the current simulated LED brightness (`0–100`).

```bash
curl http://127.0.0.1:8080/brightness
```

```json
{"brightness":100}
```

### POST /brightness

**Methods:** `POST`, `PUT`

Sets simulated LED brightness to an integer `0–100`. Affects only GUI rendering; stored pixels and readback do not change.

| Priority | Source | Example |
|---|---|---|
| 1 | Query `value` | `?value=75` |
| 2 | JSON `"brightness"` | `{"brightness":75}` |
| 3 | JSON `"value"` | `{"value":75}` |
| 4 | Bare integer body (optional whitespace) | `75` |

Non-integers, trailing characters (`75abc`), decimals (`7.5`), and values outside `0..100` return `400`.

```bash
curl -X POST "http://127.0.0.1:8080/brightness?value=60"
curl -X PUT  http://127.0.0.1:8080/brightness -d 60
```

```json
{"ok":true,"brightness":60}
```

### Idle dimming

**Methods:** `GET`, `POST`, `PUT`, `DELETE` · **Path:** `/idle`

Automatically dims GUI brightness to a target level (`0–100`, default `20`) after `timeout_sec` seconds without API writes or user interaction, smoothly fading over 1 second and waking immediately on the next write or click.

```bash
curl -X POST http://127.0.0.1:8080/idle -d '{"timeout_sec":300,"brightness":15}'
curl http://127.0.0.1:8080/idle
curl -X DELETE http://127.0.0.1:8080/idle
```

```json
{"enabled":true,"timeout_sec":300.00,"brightness":15,"dimmed":false}
```

### GET /monochrome

**Methods:** `GET` · **Alias:** `/mono`

Returns whether the BT.709 monochrome tint filter is active and its tint color.

```json
{"enabled":true,"r":255,"g":191,"b":0,"hex":"#ffbf00"}
{"enabled":false,"color":null}
```

### POST /monochrome

**Methods:** `POST`, `PUT` · **Alias:** `/mono`

Applies a single-color phosphor/LED tint while preserving each pixel's perceptual luminance (ITU-R BT.709 in linear light). Stored colors remain intact underneath and reappear when the filter is turned off.

Accepts any [color string](#color-formats) (not `[r,g,b]` arrays or separate `r`/`g`/`b` channels), checked in this order:

| Priority | Source | Enable | Disable |
|---|---|---|---|
| 1 | Query `color` | `?color=0xffbf00`, `?color=amber` | `?color=off`, `none`, `false`, `null` |
| 2 | Query `enabled` | — | `?enabled=0`, `?enabled=false` |
| 3 | JSON object | `{"color":"#00ff00"}` | `{"color":null}`, `{"color":"off"}`, `{"enabled":false}` |
| 4 | JSON string | `"#00ff00"` | `"off"`, `"none"` |
| 5 | JSON `null` | — | `null` |
| 6 | Plain-text body | `#ffbf00`, `0xffbf00`, `amber`, `#0f0` | `off`, `none`, `false`, `null` |

```bash
curl -X POST http://127.0.0.1:8080/monochrome -d '{"color":"#00ff00"}'
curl -X POST http://127.0.0.1:8080/monochrome -d amber
curl -X POST "http://127.0.0.1:8080/monochrome?color=off"
```

```json
{"ok":true,"enabled":true,"r":0,"g":255,"b":0,"hex":"#00ff00"}
{"ok":true,"enabled":false,"color":null}
```

Returns `400` if no valid color or disable token is provided (including `{"enabled":true}` without a color).

### DELETE /monochrome

**Methods:** `DELETE` · **Alias:** `/mono`

Turns the monochrome filter off. Returns `{"ok":true,"enabled":false,"color":null}`.

### POST /demo

**Methods:** `POST`

Starts or stops the built-in 60 Hz demo animation (off by default unless launched with `--demo`).

| Request | Behavior |
|---|---|
| `POST /demo` or `?enabled=1` | Starts the demo animation and stops any scrolling text |
| `POST /demo?enabled=0` or `?enabled=false` | Stops the demo; the last rendered demo frame stays on screen |

```bash
curl -X POST "http://127.0.0.1:8080/demo?enabled=0"
```

```json
{"ok":true,"demo_active":false}
```

---

## Alert sounds

`markee` synthesizes 3 gentle, non-alarming notification sounds in software—no external audio files required. Up to 4 overlapping instances of each sound can ring out simultaneously.

| ID | Name | Character | Duration |
|---|---|---|---|
| `1` | `ding` | Warm single desk-bell ding (`G5`) | 650 ms |
| `2` | `chime` | Gentle two-note ascending fifth chime (`C5 → G5`) | 850 ms |
| `3` | `bell` | Soft three-note sparkling major triad (`D5 → F#5 → A5`) | 1050 ms |

### GET /alert

**Methods:** `GET` · **Aliases:** `/alerts`, `/api/alert`, `/api/alerts`

Returns audio playback readiness, total alerts triggered since startup, and the catalog of available sounds.

```bash
curl http://127.0.0.1:8080/alert
```

```json
{
  "audio_ready": true,
  "alerts_played": 2,
  "sounds": [
    {"id":1,"name":"ding","description":"Warm single desk-bell ding","duration_ms":650},
    {"id":2,"name":"chime","description":"Gentle two-note ascending chime","duration_ms":850},
    {"id":3,"name":"bell","description":"Soft three-note sparkling bell triad","duration_ms":1050}
  ]
}
```

### POST /alert

**Methods:** `POST` · **Paths:** `/alert`, `/alert/<id|name>` · **Aliases:** `/api/alert`, `/api/alert/<id|name>`

Plays one of the 3 built-in alert sounds. Specify the sound in the path (`/alert/1`, `/alert/2`, `/alert/3`, `/alert/ding`, `/alert/chime`, `/alert/bell`) or via `POST /alert`.

On `POST /alert`, the sound is selected from the first source present (defaulting to `1` / `ding` if omitted):

| Priority | Source | Example |
|---|---|---|
| 1 | Query `sound` or `id` | `?sound=2`, `?sound=chime`, `?id=3` |
| 2 | JSON object `"sound"` or `"id"` | `{"sound":2}`, `{"sound":"bell"}` |
| 3 | JSON number or string | `2`, `"chime"` |
| 4 | Plain-text body | `2`, `chime` |

Optional **`volume`** (`0–100`, default `100`) may be supplied via `?volume=75` or JSON `{"volume":75}` on either `/alert` or `/alert/<id|name>`.

```bash
# Direct path (ID or name)
curl -X POST http://127.0.0.1:8080/alert/1
curl -X POST http://127.0.0.1:8080/alert/chime
curl -X POST "http://127.0.0.1:8080/alert/bell?volume=70"

# Default sound (ding) or parameter-based selection
curl -X POST http://127.0.0.1:8080/alert
curl -X POST "http://127.0.0.1:8080/alert?sound=2&volume=80"
curl -X POST http://127.0.0.1:8080/alert -d '{"sound":"bell","volume":60}'
```

```json
{"ok":true,"id":2,"name":"chime","volume":80,"played":true}
```

`"played"` is `false` when markee was launched with `--mute` (`--no-audio`) or when no audio output device is available.

| Status | Cause |
|---|---|
| `404 Not Found` | Unknown sound in path (`/alert/99`, `/alert/siren`) |
| `400 Bad Request` | Invalid `sound`/`id` parameter or body, or `volume` outside `0..100` |
| `405 Method Not Allowed` | Wrong HTTP method (e.g., `GET /alert/1` or `DELETE /alert`) |

---

## Events

### GET /events

**Methods:** `GET`, `POST` · **Alias:** `/api/events`

Exposes a 64-entry ring buffer of interactive GUI clicks (`click` with LED pixel `x`, `y` and `left`/`right`), bezel taps or `Space`/`Enter` presses (`button`), and API framebuffer updates (`frame`).

- **JSON polling (default):** `GET /events?since=<id>&wait=<ms>` (`wait` `0..5000` ms enables long-polling).
- **Server-Sent Events (SSE):** send `Accept: text/event-stream` or `?stream=1` (optional `&max=<n>` closes after `n` events).

```bash
# Poll recent events
curl "http://127.0.0.1:8080/events?since=0"

# Stream live events via SSE
curl -N "http://127.0.0.1:8080/events?stream=1"
```

```json
{
  "last_id": 2,
  "events": [
    {"id":1,"type":"click","x":12,"y":7,"detail":"left"},
    {"id":2,"type":"button","detail":"bezel"}
  ]
}
```

`POST /events` accepts `{"type":"button"|"click","x":12,"y":7,"detail":"action"}` to inject a synthetic event for scripts or headless testing.

---

## Recipes

### Show a build notification with a chime (shell)

```bash
curl -s -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear", "color": "#001408" },
  { "op": "rect", "x": 0, "y": 0, "w": 72, "h": 16, "color": "green" },
  { "op": "text", "x": 8, "y": 4, "text": "BUILD OK!", "color": "warmwhite" }
]' >/dev/null
curl -s -X POST http://127.0.0.1:8080/alert/chime >/dev/null
```

### Live digital clock in a shell loop

```bash
while true; do
  curl -s -X POST http://127.0.0.1:8080/draw \
    -d "[{\"op\":\"clear\"},{\"op\":\"text\",\"x\":12,\"y\":4,\"text\":\"$(date +%H:%M:%S)\",\"color\":\"amber\"}]" >/dev/null
  sleep 1
done
```

### Stream raw frames at 60 FPS (Python, stdlib only)

Reuse a **single HTTP/1.1 Keep-Alive connection** for smooth 60 FPS streaming:

```python
import http.client, time

W, H = 72, 16
conn = http.client.HTTPConnection("127.0.0.1", 8080)
t0 = time.monotonic()
for n in range(600):                                    # ~10 seconds
    frame = bytearray(W * H * 3)
    x = n % W
    for y in range(H):
        i = (y * W + x) * 3
        frame[i:i + 3] = b"\xff\x80\x00"                # orange column sweeping across
    conn.request("PUT", "/frame.rgb", body=bytes(frame))
    conn.getresponse().read()                           # drain response before next request
    time.sleep(max(0, t0 + (n + 1) / 60 - time.monotonic()))
```

See [`examples/demo.py`](../examples/demo.py) for a complete example that discovers resolution from `/info`, uploads and reads back a PNG, and streams a 60 FPS plasma animation.

### Upload a PNG rendered with Pillow

```python
from PIL import Image, ImageDraw
import io, urllib.request

img = Image.new("RGB", (72, 16), "black")
ImageDraw.Draw(img).text((2, 2), "hello", fill=(255, 191, 0))
buf = io.BytesIO()
img.save(buf, "PNG")
urllib.request.urlopen(urllib.request.Request(
    "http://127.0.0.1:8080/frame", data=buf.getvalue(), method="POST",
    headers={"Content-Type": "image/png"}))
```

### Resize any image with ImageMagick

```bash
magick photo.jpg -resize 72x16^ -gravity center -extent 72x16 png24:- \
  | curl -X POST --data-binary @- http://127.0.0.1:8080/frame
```

### Browser or Node.js (`fetch`)

CORS is enabled for all origins, so browser pages can call markee directly:

```js
await fetch("http://127.0.0.1:8080/draw", {
  method: "POST",
  body: JSON.stringify([
    { op: "clear" },
    { op: "text", x: 2, y: 4, text: new Date().toLocaleTimeString(), color: "cyan" },
  ]),
});

const png = await (await fetch("http://127.0.0.1:8080/frame")).blob();
```

### Raw TCP socket (no HTTP library)

```bash
printf 'POST /fill?color=0x00ff00 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n' \
  | nc 127.0.0.1 8080
```

---

## Protocol details and limits

### Responses, errors, and CORS

- Successful write endpoints return `200 OK` with JSON containing `"ok": true`.
- Errors return valid, JSON-escaped `{"error": "<message>"}`.
- All JSON request bodies (`/draw`, `/fill`, `/pixel`, `/brightness`, `/monochrome`, `/alert`) are parsed with a strict recursive-descent JSON parser. Query parameters are URL-decoded (`%XX` and `+` → space).
- Every response includes `Content-Length` and open CORS headers:

```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, Accept
```

| Status | Meaning |
|---|---|
| `200 OK` | Request succeeded |
| `204 No Content` | `OPTIONS` CORS preflight |
| `400 Bad Request` | Missing/invalid parameter, wrong frame dimensions, invalid PNG/JSON, or malformed HTTP (malformed HTTP also closes the connection) |
| `404 Not Found` | Unknown path or unknown `/alert/<id\|name>` sound |
| `405 Method Not Allowed` | Known path called with an unsupported method; the `Allow` header lists valid methods |
| `414 URI Too Long` | Path > 255 bytes, query > 511 bytes, or request line > 1023 bytes (closes the connection) |
| `500 Internal Server Error` | PNG encoding failure or out of memory |
| `503 Service Unavailable` | All 32 concurrent client slots are in use (closes the connection) |

### Connections and resource limits

| Setting | Value |
|---|---|
| Concurrency model | One detached thread per TCP connection; requests on a connection are processed sequentially |
| Keep-Alive | Enabled by default for HTTP/1.1, disabled for HTTP/1.0 (overridden by `Connection` header) |
| Idle / read / write timeout | 5 seconds |
| Max concurrent connections | 32 (additional connections receive `503`) |
| Max request body | 4 MiB (`Content-Length` or `Transfer-Encoding: chunked`; honors `Expect: 100-continue`) |
| Max total request headers | 16 KiB |
| Max request line / path / query | 1,023 / 255 / 511 bytes |
| Bind addresses | IPv4, IPv6, or hostname via `--host` (defaults to `127.0.0.1`) |

### Security note

The HTTP API has **no authentication** and open CORS. Anyone who can reach the bound port can change the display or play alert sounds. Keep the default loopback bind (`127.0.0.1`) unless you intentionally want LAN access via `--host 0.0.0.0` or `::`.
