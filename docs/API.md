# markee HTTP API Reference

markee runs a small HTTP/1.1 server alongside the simulated display. Anything that can open a TCP socket can drive it: `curl`, a browser `fetch()`, Python's standard library, or a hand-written socket client.

- **Base URL:** `http://127.0.0.1:8080` by default. Change it with `--host`, `--port`, or the `MARKEE_PORT` environment variable (see the [README](../README.md#cli-options)).
- **Resolution:** every size in this document assumes the default **72×16** matrix. When markee runs with `--resolution WxH`, PNGs must be `W×H` and raw frames must be `W × H × 3` bytes. To find the current size, call [`GET /info`](#get-info).

## Contents

- [Conventions](#conventions)
  - [Coordinates and pixel format](#coordinates-and-pixel-format)
  - [Color formats](#color-formats)
  - [Responses and errors](#responses-and-errors)
  - [Connections, limits and timeouts](#connections-limits-and-timeouts)
  - [Display state model](#display-state-model)
- [Endpoint summary](#endpoint-summary)
- [Status](#status)
  - [GET /info](#get-info)
- [Frames](#frames)
  - [GET /frame](#get-frame)
  - [POST /frame](#post-frame)
  - [DELETE /frame, POST /clear](#delete-frame-post-clear)
  - [POST /fill](#post-fill)
- [Pixels](#pixels)
  - [GET /pixel](#get-pixel)
  - [POST /pixel](#post-pixel)
- [Drawing](#drawing)
  - [POST /draw](#post-draw)
  - [Draw operations](#draw-operations)
  - [Fonts](#fonts)
  - [Scrolling text](#scrolling-text)
- [Display settings](#display-settings)
  - [GET /brightness](#get-brightness)
  - [POST /brightness](#post-brightness)
  - [GET /monochrome](#get-monochrome)
  - [POST /monochrome](#post-monochrome)
  - [DELETE /monochrome](#delete-monochrome)
  - [POST /demo](#post-demo)
- [Alert sounds](#alert-sounds)
  - [GET /alert](#get-alert)
  - [POST /alert, POST /alert/&lt;id|name&gt;](#post-alert-post-alertidname)
- [Recipes](#recipes)
- [Known limitations](#known-limitations)

---

## Conventions

### Coordinates and pixel format

- The origin `(0, 0)` is the **top-left** LED. `x` grows to the right (`0..W-1`) and `y` grows downward (`0..H-1`).
- A **raw frame** is `W × H × 3` bytes: rows from top to bottom, pixels from left to right, each pixel as `R, G, B` with one byte per channel. There is no header, padding or alpha. At 72×16 that is **3456 bytes**.
- Pixel offset in a raw frame: `(y * W + x) * 3`.

### Color formats

Every endpoint that takes a color string (`/fill`, `/pixel`, `/draw`, `/monochrome`), and the `--monochrome` flag, uses the same parser. A color string can be any of these:

| Format | Examples |
|---|---|
| Hex, no prefix | `ff5500`, `f50` |
| Hex with `0x` | `0xff5500`, `0XFF5500`, `0xf50` |
| Hex with `#` | `#ff5500`, `#f50` |
| Named color | `amber`, `red`, `warmwhite` (list below) |

Hex values have 6 digits (`rrggbb`) or 3 (`rgb`, where `f50` means `ff5500`). Hex digits, the `0x` prefix and color names are case-insensitive.

Some endpoints also accept other forms:

| Form | Where | Example |
|---|---|---|
| Separate channels (`0–255`) | `/fill`, `/pixel` (query or JSON); `/draw` (as fields on the op) | `r=255&g=85&b=0`, `{"r":255,"g":85,"b":0}` |
| `[r, g, b]` array | `/draw` | `[255, 85, 0]` |
| No color | `/draw` (no stroke or fill); `/monochrome` (turns the filter off) | `null`, `"none"`, `"transparent"`; `off`, `none` |

**Named colors:** `black`, `white`, `warmwhite` (#fff4e0), `red`, `green` / `lime` (#00ff00), `darkgreen` (#008000), `blue`, `yellow`, `cyan` / `aqua`, `magenta` / `fuchsia`, `orange` (#ff8000), `amber` (#ffbf00), `gold` (#ffd700), `purple` (#a020f0), `violet`, `indigo`, `pink` (#ff69b4), `coral`, `crimson`, `brown` (#964b00), `maroon`, `olive`, `navy`, `teal`, `turquoise`, `gray` / `grey` (#808080), `silver`.

> **Don't put `#` in a URL.** In a URL, `#` starts the fragment, and clients such as curl and browsers never send it to the server. `curl -X POST "http://127.0.0.1:8080/fill?color=#ff5500"` therefore fails with `400`. In query strings, use `?color=0xff5500` or `?color=ff5500`. The URL-encoded form `%23ff5500` also works. Inside a JSON body, `"#ff5500"` works as written.

### Responses and errors

- Successful writes return `200 OK` with a small JSON object containing `"ok": true`.
- Failures return a JSON object with one field, `{"error": "<human-readable message>"}`.
- Every response carries `Content-Length` and these CORS headers, so browser pages on any origin can call the API:

  ```http
  Access-Control-Allow-Origin: *
  Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS
  Access-Control-Allow-Headers: Content-Type, Accept
  ```

- `OPTIONS` on any path returns `204 No Content` (CORS preflight).

| Status | Meaning |
|---|---|
| `200 OK` | Success |
| `204 No Content` | `OPTIONS` preflight |
| `400 Bad Request` | Missing or invalid parameters, wrong frame size, invalid PNG or JSON, malformed HTTP. A malformed HTTP request also closes the connection. |
| `404 Not Found` | Unknown path |
| `405 Method Not Allowed` | Known path, unsupported method (for example `GET /fill`). The `Allow` header lists the supported methods. |
| `414 URI Too Long` | Path longer than 255 bytes, query longer than 511 bytes, or request line longer than 1023 bytes. The connection is closed. |
| `500 Internal Server Error` | PNG encoding failed or the server ran out of memory |
| `503 Service Unavailable` | Too many concurrent connections (see below). The connection is closed. |

### Connections, limits and timeouts

| Behavior | Value |
|---|---|
| Keep-Alive | On by default for HTTP/1.1, off for HTTP/1.0. `Connection: close` or `Connection: keep-alive` overrides the default. |
| Idle / read timeout | 5 seconds. A keep-alive connection that receives nothing for 5 s is closed. |
| Max concurrent connections | 32; the next one gets `503` |
| Max request body | 4 MiB |
| Max total header size | 16 KiB |
| Max request line | 1023 bytes (longer: `414`) |
| Max path | 255 bytes (longer: `414`) |
| Max query string | 511 bytes (longer: `414`) |
| Request bodies | `Content-Length` or `Transfer-Encoding: chunked`. `Expect: 100-continue` is honored. |
| Binding | IPv4 address, IPv6 address or hostname (`--host`) |

Query values are **URL-decoded**: `%XX` becomes the byte it encodes, and `+` becomes a space. Error messages are JSON-escaped, so every error body is valid JSON. JSON bodies (`/fill`, `/pixel`, `/brightness`, `/draw`) go through a real JSON parser.

Each connection gets its own thread and requests are handled in order. To stream frames at 60 FPS, reuse **one keep-alive connection**. Opening a new connection per frame works but adds latency.

### Display state model

Knowing what each call changes explains most of the API's behavior:

- **Framebuffer:** the `W×H` RGB pixels. These calls write to it: `POST /frame`, `POST /fill`, `POST /pixel`, `POST /draw`, `DELETE /frame` and `POST /clear`. Every successful write:
  - turns off the startup demo animation,
  - stops any [scrolling text](#scrolling-text) (`/draw` may start a new scroller),
  - adds 1 to `frames_received` in [`/info`](#get-info).
- **Brightness** (`0–100`) affects only how the GUI renders the LEDs. Stored pixels and readback (`GET /frame`, `GET /pixel`) **do not change** with brightness.
- **Monochrome filter** is applied when pixels are **read**: by the GUI, `GET /frame` and `GET /pixel`. Stored pixels keep full color, so turning the filter off brings the original colors back.
- **Demo animation** redraws the framebuffer 60 times per second while it is active. markee starts with the demo on unless you pass `--blank`.

Brightness, monochrome, demo and alert changes do not count as writes and do not change `frames_received`.

---

## Endpoint summary

| Method | Path (aliases) | Purpose |
|---|---|---|
| `GET` | `/info` (`/`, `/api/info`) | Device metadata and current state |
| `GET` | `/frame` (`/frame.png`, `/frame.rgb`, `/api/frame`) | Read the displayed pixels as a PNG or raw RGB |
| `POST` `PUT` | `/frame` (`/frame.png`, `/frame.rgb`, `/api/frame`) | Replace the whole frame with a PNG or raw RGB |
| `DELETE` | `/frame` (`/api/frame`) | Clear to black |
| `POST` | `/clear` | Clear to black |
| `POST` | `/fill` | Fill with one color |
| `GET` | `/pixel` | Read one pixel |
| `POST` `PUT` | `/pixel` | Set one pixel |
| `POST` `PUT` | `/draw` (`/api/draw`) | Draw shapes and text in one atomic batch |
| `GET` | `/brightness` | Read brightness |
| `POST` `PUT` | `/brightness` | Set brightness |
| `GET` | `/monochrome` (`/mono`) | Read monochrome filter state |
| `POST` `PUT` | `/monochrome` (`/mono`) | Turn the monochrome filter on or off |
| `DELETE` | `/monochrome` (`/mono`) | Turn the monochrome filter off |
| `POST` | `/demo` | Turn the startup animation on or off |
| `GET` | `/alert` (`/alerts`, `/api/alert`, `/api/alerts`) | List built-in alert sounds and audio status |
| `POST` | `/alert`, `/alert/<id\|name>` (`/api/alert`) | Play one of the 3 built-in gentle alert sounds |
| `OPTIONS` | any | CORS preflight (`204`) |

---

## Status

### GET /info

Aliases: `GET /`, `GET /api/info`

Returns the device's metadata and current state.

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
  "demo_active": true,
  "frames_received": 0,
  "audio_ready": true,
  "alert_sounds": 3
}
```

| Field | Type | Description |
|---|---|---|
| `name` | string | Always `"markee"` |
| `width`, `height` | int | Matrix resolution in LEDs |
| `bits_per_pixel` | int | Always `24` |
| `raw_frame_bytes` | int | Size of a raw frame (`width × height × 3`) |
| `refresh_hz` | int | Always `60` |
| `physical_width_inches` | number | Simulated physical width. It is `6.35` at 72 wide and scales with `width` at the same LED pitch. |
| `brightness` | int | `0–100` |
| `monochrome` | string \| null | Filter color as `"#rrggbb"`, or `null` when the filter is off |
| `demo_active` | bool | Whether the startup animation is running |
| `frames_received` | int | Count of successful framebuffer writes since startup |
| `audio_ready` | bool | Whether an audio output device is ready to play alert sounds |
| `alert_sounds` | int | Number of built-in alert sounds (`3`) |

---

## Frames

### GET /frame

Aliases: `/frame.png`, `/frame.rgb`, `/api/frame`

Returns the currently displayed pixels, with the monochrome filter applied if it is on. Brightness is not applied.

| Response format | How to request it | `Content-Type` |
|---|---|---|
| **PNG** (default) | `GET /frame`, `GET /frame.png` | `image/png` (24-bit RGB, no alpha, `W×H`) |
| **Raw RGB** | `GET /frame.rgb`, `?format=raw` or `?format=rgb`, or the header `Accept: application/octet-stream` | `application/octet-stream` (`W×H×3` bytes) |

```bash
curl -o capture.png http://127.0.0.1:8080/frame
curl -o capture.rgb "http://127.0.0.1:8080/frame?format=raw"   # 3456 bytes
```

Readback shows what is on screen at that moment, including the current position of scrolling text or the demo animation.

### POST /frame

Methods: `POST` or `PUT`. Aliases: `/frame.png`, `/frame.rgb`, `/api/frame`

Replaces the entire framebuffer with the request body.

The server picks the body format in this order:

1. On `/frame.rgb`, the body is always read as **raw RGB**.
2. Otherwise the body is read as a **PNG** if it starts with the PNG signature, if `Content-Type` contains `image/png`, or if the path is `/frame.png`.
3. Otherwise, a body of exactly `W×H×3` bytes is read as **raw RGB**.
4. Any other body returns `400`.

PNG details:

- The PNG must be exactly `W×H` pixels. markee does not scale images.
- markee accepts any PNG color type its decoder (raylib/stb_image) supports, including grayscale, palette and RGBA.
- Alpha is composited onto **black** (`rgb × alpha / 255`). Opaque PNGs load losslessly.

```bash
# PNG
curl -X POST --data-binary @frame.png http://127.0.0.1:8080/frame

# Raw RGB (must be exactly 3456 bytes at 72x16)
curl -X PUT --data-binary @frame.rgb http://127.0.0.1:8080/frame.rgb
```

> Send files with `--data-binary`. curl's `-d` strips newlines and corrupts binary data.

**Responses**

```json
{"ok":true,"format":"png","width":72,"height":16}
{"ok":true,"format":"rgb24","width":72,"height":16}
```

| Error (`400`) | Cause |
|---|---|
| `empty body; send a 72x16 PNG or 3456 raw RGB bytes` | No body |
| `expected 72x16 PNG, got 64x32` | PNG has the wrong dimensions |
| `invalid PNG data` | Body was read as a PNG but failed to decode |
| `expected 72x16 PNG or 3456 raw RGB bytes, got N bytes` | Not a PNG, and not the raw frame size |

### DELETE /frame, POST /clear

`DELETE /frame` (or `/api/frame`) and `POST /clear` both set every pixel to black.

```bash
curl -X POST http://127.0.0.1:8080/clear
```

```json
{"ok":true}
```

To clear to a color other than black, use [`/fill`](#post-fill) or the `clear` [draw op](#clear).

### POST /fill

Sets every pixel to one color. Only `POST` is accepted.

The color comes from the **first** of these sources that is present:

| # | Source | Example |
|---|---|---|
| 1 | Query `color` ([color string](#color-formats)) | `?color=0xff5500`, `?color=amber` |
| 2 | Query `r`, `g`, `b` (all three) | `?r=255&g=85&b=0` |
| 3 | JSON body `"color"` ([color string](#color-formats)) | `{"color":"#ff5500"}`, `{"color":"teal"}` |
| 4 | JSON body `"r"`, `"g"`, `"b"` | `{"r":255,"g":85,"b":0}` |

If the first source present is invalid, the request fails with `400`; later sources are not tried.

```bash
curl -X POST "http://127.0.0.1:8080/fill?color=0xf25c19"
curl -X POST "http://127.0.0.1:8080/fill?color=amber"
curl -X POST http://127.0.0.1:8080/fill -d '{"r":0,"g":32,"b":64}'
```

```json
{"ok":true,"r":242,"g":92,"b":25,"hex":"#f25c19"}
```

---

## Pixels

### GET /pixel

Reads one pixel. Both `x` and `y` are required query parameters.

```bash
curl "http://127.0.0.1:8080/pixel?x=10&y=5"
```

```json
{"x":10,"y":5,"r":0,"g":255,"b":204,"hex":"#00ffcc"}
```

As with `GET /frame`, the value has the monochrome filter applied if it is on.

| Error (`400`) | Cause |
|---|---|
| `provide integer query parameters x and y` | `x` or `y` missing or not an integer |
| `pixel coordinates out of range (0..71, 0..15)` | Coordinates are outside the matrix |

### POST /pixel

Methods: `POST` or `PUT`

Sets one pixel.

- **Position:** the query parameters `x` and `y`. If either is missing, `"x"` and `"y"` are read from the JSON body.
- **Color:** the same sources and order as [`/fill`](#post-fill).

The position and the color can come from different places, for example coordinates in the query and the color in the body.

```bash
curl -X POST "http://127.0.0.1:8080/pixel?x=10&y=5&color=0x00ffcc"
curl -X POST "http://127.0.0.1:8080/pixel?x=10&y=5&r=0&g=255&b=204"
curl -X POST http://127.0.0.1:8080/pixel -d '{"x":10,"y":5,"r":0,"g":255,"b":204}'
```

```json
{"ok":true,"x":10,"y":5,"r":0,"g":255,"b":204}
```

Out-of-range coordinates return `400`, as with `GET /pixel`. Each call is one full write: it stops the demo and scrolling text. To set many pixels, send one [`/draw`](#post-draw) batch or one full frame instead of many `/pixel` calls.

---

## Drawing

### POST /draw

Methods: `POST` or `PUT`. Alias: `/api/draw`

Runs a **JSON array** of drawing operations in order, on top of the current frame. The frame is not cleared first; start with `{"op":"clear"}` if you want a blank canvas.

- **Atomic:** the ops are drawn on a copy of the frame. If any op is invalid, the request returns `400` and the display does not change.
- **Clipped:** shapes and text that extend past the edges are cut off without an error.
- The body must be a non-empty array. A single op still has to be wrapped: `[{...}]`.
- `Content-Type` is ignored.

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

| Error (`400`) | Cause |
|---|---|
| `empty body; send a JSON array of one or more draw operations` | No body |
| `JSON body must be an array of one or more draw operations` | The body is an object or an empty array |
| A JSON parse message | Invalid JSON syntax |
| `missing required string field 'op'` | An element has no `"op"` |
| `unknown op 'xyz' (supported: …)` | Unknown op name |
| `invalid 'color' value` / `invalid 'fill' value` | Unrecognized color |
| Op-specific messages | See the ops below |

### Draw operations

Every op is an object with an `"op"` field (case-insensitive). Numeric fields are rounded to the nearest integer. A missing or non-numeric field uses its default (usually `0`).

#### Stroke and fill

Shapes take two independent color fields:

| Field | Meaning |
|---|---|
| `color` | **Stroke:** a 1-pixel outline (`rect`, `oval`, `polygon`), or the drawing color (`line`, `text`, `pixel`) |
| `fill` | Interior color. `true` means "use the stroke color, or white if there is none"; `false` or `null` means no fill. |
| `r`, `g`, `b` | Used as the stroke color, but **only** when neither `color` nor `fill` is given |

To draw a shape with no outline, set `"color":"none"` (or `null`) and give a `fill`.

Defaults when no color is given:

| Op | Default color |
|---|---|
| `line`, `text` | white (`fill` is used if only `fill` is given) |
| `pixel` | white (`fill` is used if only `fill` is given; `"color":"none"` draws nothing) |
| `rect`, `oval`, `polygon` | white 1-pixel outline, no fill. An explicit `"color":"none"` or `null` means no outline. |
| `clear` | black |

#### `clear`

Fills the whole frame with one color and stops any scrolling text.

| Field | Default | Description |
|---|---|---|
| `color` (or `fill`) | black | Color to clear to |

```json
{ "op": "clear", "color": "#001020" }
```

#### `pixel`

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Position. Off-screen positions are ignored. |
| `color` | black | Pixel color |

#### `line`

A 1-pixel Bresenham line. Both end points are included.

| Field | Default | Description |
|---|---|---|
| `x1`, `y1`, `x2`, `y2` | `0` | End points |
| `color` | white | Line color |

#### `rect`

An axis-aligned rectangle, optionally with rounded corners.

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Top-left corner |
| `w`, `h` | `0` | Size in pixels. Nothing is drawn if `w ≤ 0` or `h ≤ 0`. |
| `radius` | `0` | Corner radius, capped at `min(w, h) / 2` |
| `color` | none | Outline |
| `fill` | none | Interior |

#### `oval`

An ellipse inscribed in the box `x, y, w, h`. Equal `w` and `h` give a circle.

| Field | Default | Description |
|---|---|---|
| `x`, `y` | `0` | Top-left of the bounding box |
| `w`, `h` | `0` | Bounding box size |
| `color` | none | Outline |
| `fill` | none | Interior |

#### `polygon`

A closed polygon. The outline joins the last point back to the first. Fill uses the even-odd rule.

| Field | Default | Description |
|---|---|---|
| `points` | **required** | `[[x, y], …]` with 2–256 points. Fill needs at least 3. |
| `color` | none | Outline |
| `fill` | none | Interior |

Errors: `'polygon' requires 'points' array with at least 2 [x,y] pairs`, `'polygon' supports at most 256 points`, `each polygon point must be a [x, y] number pair`.

#### `text`

Bitmap text on one line. markee does not wrap text; text past the edge is cut off.

| Field | Default | Description |
|---|---|---|
| `text` | **required** | String to draw. Characters outside printable ASCII (`0x20–0x7E`) are drawn as `?`. |
| `x`, `y` | `0` | Top-left of the first glyph |
| `font` | `"6x8"` | See [Fonts](#fonts) |
| `color` | white | Text color |
| `scroll` | `false` | `true` scrolls the text. See [Scrolling text](#scrolling-text). |
| `speed` | `24` | Scroll speed in pixels per second. Values `≤ 0` mean `24`. |

Error: `'text' op requires string field 'text'`.

### Fonts

| `font` value | Glyph size | Advance per character | Characters |
|---|---|---|---|
| `"6x8"` (default), `"5x7"`, `"medium"`, `"default"`, or any unknown name | 5×7 | 6 px | Printable ASCII |
| `"8x8"`, `"large"`, `"bold"` | 6×7 (bold 5×7) | 7 px | Printable ASCII |
| `"4x6"`, `"3x5"`, `"small"` | 3×5 | 4 px | `A–Z` (lowercase is drawn as uppercase), `0–9`, space and `! . : - + /`. Other characters leave a blank gap. |

Text width in pixels is `length × advance`. At 72 px wide, a line holds 12 characters in `6x8`, 10 in `8x8` and 18 in `4x6`. On a 16-px-tall display, `y: 4` roughly centers `6x8` text, and two rows of `4x6` fit at `y: 1` and `y: 9`.

### Scrolling text

`"scroll": true` makes a `text` op scroll from right to left at `speed` pixels per second. The text wraps around: once it has fully left the left edge, it comes back in from the right. `x` is the starting position.

- **Background:** when the scrolling text op runs, markee saves the frame drawn so far in the batch as the background. Every frame (60 per second), it redraws that background with the text at its new position.
- **Put the scrolling text last.** Ops after it in the same batch are drawn once and then wiped on the next tick.
- Only **one** scroller can be active. A later `text` op in the same batch replaces it, with or without `scroll`, and so does a `clear` op.
- Any later write stops the scroll and leaves the text where it was: `/frame`, `/fill`, `/pixel`, `/clear`, or a `/draw` batch without a scrolling text op. `POST /demo?enabled=1` also stops it.
- Text longer than 511 bytes is cut short while scrolling.
- Scrolling also runs in `--headless` mode, and `GET /frame` returns the current position.

```bash
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear", "color": "#100800" },
  { "op": "line", "x1": 0, "y1": 15, "x2": 71, "y2": 15, "color": "orange" },
  { "op": "text", "x": 72, "y": 4, "text": "Now playing: something good", "color": "amber", "scroll": true, "speed": 30 }
]'
```

---

## Display settings

### GET /brightness

```bash
curl http://127.0.0.1:8080/brightness
```

```json
{"brightness":100}
```

### POST /brightness

Methods: `POST` or `PUT`

Sets the simulated LED brightness. The value is an integer from `0` to `100`. Brightness only changes how the GUI renders the LEDs; it does not change stored pixels or readback.

The value comes from the first of these that is present:

| # | Source | Example |
|---|---|---|
| 1 | Query `value` | `?value=75` |
| 2 | JSON `"brightness"` | `{"brightness":75}` |
| 3 | JSON `"value"` | `{"value":75}` |
| 4 | Plain-text body starting with an integer | `75` |

```bash
curl -X POST "http://127.0.0.1:8080/brightness?value=60"
curl -X PUT  http://127.0.0.1:8080/brightness -d 60
```

```json
{"ok":true,"brightness":60}
```

A plain-text body must be a whole integer, optionally surrounded by whitespace: `60` works, `75abc` and `7.5` return `400`.

Error (`400`): `brightness must be an integer 0..100`.

### GET /monochrome

Alias: `/mono`

```json
{"enabled":true,"r":255,"g":191,"b":0,"hex":"#ffbf00"}
{"enabled":false,"color":null}
```

### POST /monochrome

Methods: `POST` or `PUT`. Alias: `/mono`

Turns on a monochrome tint, like a single-color phosphor or LED panel. Each pixel's perceived brightness (ITU-R BT.709 luminance in linear light) is kept, and the pixel is redrawn as a shade of the tint color. The filter is applied when pixels are read, so stored colors are untouched and come back when the filter is turned off.

The request is read in this order:

| # | Source | Turns the filter on | Turns it off |
|---|---|---|---|
| 1 | Query `color` | `?color=0xffbf00`, `?color=ffbf00`, `?color=amber` | `?color=off`, `none`, `false`, `null` |
| 2 | Query `enabled` | — | `?enabled=0`, `?enabled=false` |
| 3 | JSON object | `{"color":"#00ff00"}` | `{"color":null}`, `{"color":"off"}`, `{"enabled":false}` |
| 4 | JSON string | `"#00ff00"` | `"off"`, `"none"` |
| 5 | JSON `null` | — | `null` |
| 6 | Plain-text body | `#ffbf00`, `0xffbf00`, `amber`, `#0f0` | `off`, `none`, `false`, `null` |

Accepted colors: any [color string](#color-formats). `[r,g,b]` arrays and separate channels are not accepted.

```bash
curl -X POST http://127.0.0.1:8080/monochrome -d '{"color":"#00ff00"}'   # green phosphor
curl -X POST http://127.0.0.1:8080/monochrome -d amber
curl -X POST "http://127.0.0.1:8080/monochrome?color=off"
```

```json
{"ok":true,"enabled":true,"r":0,"g":255,"b":0,"hex":"#00ff00"}
{"ok":true,"enabled":false,"color":null}
```

Error (`400`): `provide a 24-bit color (e.g. #00ff00, #ffbf00, amber) or 'off'/'none' to disable`. This is also returned for `{"enabled":true}` without a color.

### DELETE /monochrome

Alias: `/mono`

Turns the filter off. The response is `{"ok":true,"enabled":false,"color":null}`.

### POST /demo

Turns the built-in startup animation on or off.

| Request | Effect |
|---|---|
| `POST /demo` or `?enabled=1` | Turns the demo on and stops any scrolling text |
| `?enabled=0` or `?enabled=false` | Turns the demo off. The last demo frame stays on screen. |

```bash
curl -X POST "http://127.0.0.1:8080/demo?enabled=0"
```

```json
{"ok":true,"demo_active":false}
```

Any framebuffer write also turns the demo off.

---

## Alert sounds

markee includes 3 gentle, procedurally synthesized notification sounds designed to catch attention without being startling:

| ID | Name | Character | Duration |
|---|---|---|---|
| `1` | `ding` | Warm single desk-bell ding (G5) | 650 ms |
| `2` | `chime` | Gentle two-note ascending chime (C5 → G5) | 850 ms |
| `3` | `bell` | Soft three-note sparkling bell triad (D5 → F#5 → A5) | 1050 ms |

Playing an alert does not change the framebuffer, demo, scrolling text, or `frames_received`. Up to 4 overlapping instances per sound can ring out simultaneously.

### GET /alert

Aliases: `GET /alerts`, `GET /api/alert`, `GET /api/alerts`

Returns audio playback readiness, total alert count, and the catalog of built-in sounds.

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

### POST /alert, POST /alert/<id|name>

Aliases: `POST /api/alert`, `POST /api/alert/<id|name>`

Plays one of the 3 built-in alert sounds. You can address the sound directly in the path (`/alert/1`, `/alert/2`, `/alert/3`, or `/alert/ding`, `/alert/chime`, `/alert/bell`) or pass it to `POST /alert`.

On `POST /alert`, the sound comes from the first of these that is present (defaulting to `1` / `ding` if omitted):

| # | Source | Example |
|---|---|---|
| 1 | Query `sound` or `id` | `?sound=2`, `?sound=chime`, `?id=3` |
| 2 | JSON object `"sound"` or `"id"` | `{"sound":2}`, `{"sound":"bell"}` |
| 3 | JSON number or string | `2`, `"chime"` |
| 4 | Plain-text body | `2`, `chime` |

An optional **`volume`** (`0–100`, default `100`) can be passed via `?volume=75` or JSON `{"volume":75}` on either `/alert` or `/alert/<id|name>`.

```bash
# By path (ID or friendly name)
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

`"played"` is `false` when markee was started with `--mute` or when no audio output device is available.

| Error | Cause |
|---|---|
| `404 Not Found` | Unknown sound in path (`/alert/99`, `/alert/siren`) |
| `400 Bad Request` | Invalid `sound`/`id` query or body, or `volume` outside `0..100` |
| `405 Method Not Allowed` | Unsupported HTTP method (e.g. `GET /alert/1` or `DELETE /alert`) |

---

## Recipes

### Stream raw frames at 60 FPS (Python, stdlib only)

```python
import http.client, time

W, H = 72, 16
conn = http.client.HTTPConnection("127.0.0.1", 8080)   # one keep-alive connection
t0 = time.monotonic()
for n in range(600):                                    # about 10 seconds
    frame = bytearray(W * H * 3)
    x = n % W
    for y in range(H):
        i = (y * W + x) * 3
        frame[i:i + 3] = b"\xff\x80\x00"                 # orange column sweeping across
    conn.request("PUT", "/frame.rgb", body=bytes(frame))
    conn.getresponse().read()                           # always read the response before the next request
    time.sleep(max(0, t0 + (n + 1) / 60 - time.monotonic()))
```

[`examples/demo.py`](../examples/demo.py) is a fuller version. It reads the resolution from `/info`, encodes a PNG in memory, reads the frame back and streams a plasma animation.

### Upload a PNG made with Pillow

```python
from PIL import Image, ImageDraw
import io, urllib.request

img = Image.new("RGB", (72, 16), "black")
ImageDraw.Draw(img).text((2, 2), "hello", fill=(255, 191, 0))
buf = io.BytesIO(); img.save(buf, "PNG")
urllib.request.urlopen(urllib.request.Request(
    "http://127.0.0.1:8080/frame", data=buf.getvalue(), method="POST",
    headers={"Content-Type": "image/png"}))
```

### Resize any image with ImageMagick

```bash
magick photo.jpg -resize 72x16^ -gravity center -extent 72x16 png24:- \
  | curl -X POST --data-binary @- http://127.0.0.1:8080/frame
```

### Browser / Node.js (`fetch`)

CORS is open, so any page can call the API directly:

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

### Raw socket (no HTTP library)

```bash
printf 'POST /fill?color=0x00ff00 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n' \
  | nc 127.0.0.1 8080
```

### A clock in a shell loop

```bash
while true; do
  curl -s -X POST http://127.0.0.1:8080/draw \
    -d "[{\"op\":\"clear\"},{\"op\":\"text\",\"x\":12,\"y\":4,\"text\":\"$(date +%H:%M:%S)\",\"color\":\"amber\"}]" >/dev/null
  sleep 1
done
```

---

## Known limitations

- **No authentication.** Anyone who can reach the port can change the display. The default bind address is `127.0.0.1`; think before using `--host 0.0.0.0`.
- **`#` can't be sent in a URL.** This is a client rule, not a server one: everything after `#` is a fragment and never leaves the client. Use `0x`, bare hex, or `%23`.
- **Brightness is not applied to readback.** It's a property of the simulated panel, not of the pixels. Applying it to `GET /frame` would make a read-modify-write round trip lose data.
- **Scrolling text** is truncated to 511 bytes.
