# markee

A C11 + [raylib](https://www.raylib.com/) hardware simulator for a **72×16, 24-bit RGB LED matrix display** inspired by the [BUSY Bar](https://busy.app/products/busy-bar).

`markee` recreates the front LED display inside a molded plastic enclosure calibrated to appear **~6.35 inches wide** on screen, refreshes at **60 Hz**, and exposes a lightweight **HTTP/1.1 over TCP socket API** so any script or program can push 72×16 PNGs (or raw 24-bit RGB frames) to the display and read back the currently displayed pixels as a PNG.

## Features

- **72×16 LED matrix, 24-bit true color** (16,777,216 colors, 8 bits per R/G/B channel)
- **Physical ~6.35-inch sizing** — automatically reads your monitor's resolution and physical width (via EDID) to size the LED matrix to ~6.35 inches wide (with `--dpi` manual override if needed)
- **Plastic enclosure & diffused LED rendering** — molded off-white PC/ABS outer shell with BUSY-style orange side accents, recessed smoked black polycarbonate faceplate, faint unlit pixel diffuser cells, and soft per-LED glow halos
- **60 Hz refresh rate** locked via raylib
- **Simple bidirectional Socket / HTTP API** — zero-boilerplate integration with `curl`, Python, Node.js, Go, or raw sockets:
  - Upload a **72×16 PNG** directly via `POST /frame`
  - Download the displayed pixels as a **72×16 PNG** via `GET /frame`
  - Stream **raw 24-bit RGB** (`3456` bytes) at 60 FPS over persistent HTTP Keep-Alive connections
  - Set/read individual pixels, solid fills, clear, and brightness (`0–100%`)

## Prerequisites

- A C11 compiler (`cc`, `clang`, or `gcc`)
- `make`
- `raylib` (5.x or 6.x) and `pkg-config`

### Installing raylib

- **macOS (Homebrew):**
  ```bash
  brew install raylib pkg-config
  ```
- **Ubuntu / Debian:**
  ```bash
  sudo apt install build-essential pkg-config libraylib-dev
  ```
- **Arch Linux:**
  ```bash
  sudo pacman -S base-devel raylib pkgconf
  ```

## Building

```bash
# Build the simulator binary (./markee)
make

# Build and run immediately
make run

# Build and run the C unit & socket API integration test suite
make test

# Build debug (-O0 -g) or optimized release (-O3)
make debug
make release

# Install to /usr/local/bin (override with PREFIX=...)
sudo make install

# Clean build artifacts
make clean
```

## Running

```bash
./markee [options]
```

### CLI Options

| Flag | Description | Default |
|---|---|---|
| `-r, --resolution <WxH>` | LED matrix resolution in pixels (`1..1024` per axis, e.g. `128x32`, `64x64`) | `72x16` |
| `-z, --zoom <factor>` | Display zoom multiplier (`0.1..10.0`, e.g. `0.5`, `1.5`, `2.0`) | `1.0` |
| `--square` | Render each LED pixel as a square (matches default graphics) | On |
| `--circle` | Render each LED pixel as a circle | Off |
| `-s, --shape <style>` | Pixel display style: `square` or `circle` | `square` |
| `-m, --monochrome <color>` | Apply non-destructive BT.709 monochrome filter (`#rrggbb`, `#rgb`, or color name like `amber`, `green`, `red`) | Off |
| `-H, --host <ip>` | IP address to bind the API socket | `127.0.0.1` |
| `-p, --port <port>` | TCP port (also reads `MARKEE_PORT` env var) | `8080` |
| `--dpi <value>` | Override monitor logical DPI for physical sizing | Auto-detected |
| `--blank` | Start with a blank black screen instead of the startup demo | Off |
| `--headless` | Run API server only without opening a GUI window | Off |
| `-h, --help` | Print usage information and exit | — |

```bash
./markee                       # Default 72x16 square LEDs (~6.35" wide) at 1.0x zoom
./markee --circle              # 72x16 circular LEDs
./markee --circle --zoom 2     # Circular LEDs at 2x size
./markee -r 128x32 -z 0.75     # Custom 128x32 LED matrix at 0.75x zoom
./markee --help                # Print all CLI flags and shortcuts
```

### Keyboard Shortcuts (GUI Mode)

| Key | Action |
|---|---|
| `C` | Clear display to black |
| `D` | Toggle built-in startup animation |
| `M` | Toggle monochrome filter on/off (defaults to amber `#ffbf00`) |
| `P` | Toggle pixel shape (`square` / `circle`) |
| `S` | Save the current frame to `snapshot.png` |
| `+` / `-` | Increase / decrease simulated brightness by 10% |
| `Q` / `ESC` | Quit |

## Socket / HTTP API

Rather than requiring a custom binary protocol or multi-step asset upload, `markee` serves a minimal **HTTP/1.1 API over TCP** (`http://127.0.0.1:8080` by default) with Keep-Alive and CORS enabled. Writing any frame automatically replaces the startup demo animation.

### Endpoints

| Method & Path | Description |
|---|---|
| `GET /frame` (or `/frame.png`) | Returns the currently displayed 72×16 pixels as a **24-bit `image/png`**. |
| `GET /frame?format=raw` (or `/frame.rgb`) | Returns the currently displayed pixels as **3456 raw bytes** (`72 × 16 × 3`, row-major `R,G,B`). |
| `POST /frame` or `PUT /frame` | Sends a **72×16 PNG** (auto-detected by PNG signature) or **3456 raw RGB bytes** as the request body. Transparent PNGs are alpha-composited onto black; opaque 24-bit PNGs are lossless. Wrong dimensions return `400 Bad Request`. |
| `DELETE /frame` or `POST /clear` | Clears all pixels to black (`#000000`). |
| `POST /fill` | Fills the entire screen with one color: `?color=ff5500`, `?r=255&g=85&b=0`, or JSON `{"r":255,"g":85,"b":0}`. |
| `POST /pixel` | Sets a single pixel: `?x=10&y=5&color=00ffcc`, `?x=10&y=5&r=0&g=255&b=204`, or JSON `{"x":10,"y":5,"r":0,"g":255,"b":204}`. |
| `GET /pixel?x=10&y=5` | Returns JSON `{"x":10,"y":5,"r":...,"g":...,"b":...,"hex":"#rrggbb"}`. |
| `POST /draw` | Atomically draws a JSON array of one or more shape/text ops (`[ { "op": ... }, ... ]`). Supports `line`, `rect`, `oval`, `polygon`, `text`, `pixel`, and `clear`. Colors accept `#RGB`, `#RRGGBB`, `[r,g,b]`, or named colors (`white`, `red`, `yellow`, etc.). |
| `GET /monochrome` | Returns current monochrome filter status (`{"enabled":true,"hex":"#ffbf00",...}` or `{"enabled":false,"color":null}`). |
| `POST /monochrome` | Sets a non-destructive ITU-R BT.709 perceptual-luminance monochrome filter (`{"color":"#00ff00"}`, `?color=ffbf00`, or plain `#ff0000`). Pass `{"color":null}`, `off`, `none`, or `DELETE /monochrome` to disable and restore full color. |
| `GET /brightness` | Returns `{"brightness":100}`. |
| `POST /brightness` | Sets display brightness (`0–100`): `?value=75`, plain text `75`, or JSON `{"brightness":75}`. |
| `POST /demo` | Re-enables (`?enabled=1`) or disables (`?enabled=0`) the built-in startup animation. |
| `GET /info` (or `GET /`) | Returns JSON device metadata (`width`, `height`, `refresh_hz`, `brightness`, `frames_received`, etc.). |

### `curl` Examples

```bash
# Send a 72x16 PNG file to the display
curl -X POST --data-binary @frame.png http://127.0.0.1:8080/frame

# Read back the displayed pixels as a 72x16 PNG
curl http://127.0.0.1:8080/frame -o capture.png

# Fill the display with BUSY orange
curl -X POST "http://127.0.0.1:8080/fill?color=f25c19"

# Set a single pixel at (12, 8)
curl -X POST "http://127.0.0.1:8080/pixel?x=12&y=8&r=0&g=255&b=180"

# Draw one or more shapes and text atomically via POST /draw (JSON array)
curl -X POST http://127.0.0.1:8080/draw -H "Content-Type: application/json" -d '[
  { "op": "clear" },
  { "op": "line",    "x1": 0, "y1": 15, "x2": 71, "y2": 0, "color": "#00FF00" },
  { "op": "rect",    "x": 2, "y": 2, "w": 30, "h": 12, "color": "white", "fill": "#002040", "radius": 2 },
  { "op": "oval",    "x": 40, "y": 0, "w": 16, "h": 16, "color": "yellow", "fill": "yellow" },
  { "op": "polygon", "points": [[60,14],[66,2],[71,14]], "color": "red", "fill": "red" },
  { "op": "text",    "x": 4, "y": 4, "text": "OK", "font": "6x8", "color": "#FFF", "scroll": true }
]'

# Apply a phosphor-green or amber monochrome filter (non-destructive)
curl -X POST http://127.0.0.1:8080/monochrome -d '{"color":"#00ff00"}'
curl -X POST http://127.0.0.1:8080/monochrome -d '#ffbf00'
# Disable monochrome filter and restore full 24-bit color
curl -X DELETE http://127.0.0.1:8080/monochrome

# Dim display brightness to 60%
curl -X POST "http://127.0.0.1:8080/brightness?value=60"

# Clear the screen
curl -X POST http://127.0.0.1:8080/clear

# Check device status
curl http://127.0.0.1:8080/info
```

### Python Example

A zero-dependency Python 3 demo script is included in [`examples/demo.py`](examples/demo.py). With `./markee` running:

```bash
python3 examples/demo.py --seconds 8 --save readback.png
```

Minimal snippet for PNG round-trip in Python (stdlib only):

```python
import urllib.request

# Upload a 72x16 PNG
with open("frame.png", "rb") as f:
    req = urllib.request.Request(
        "http://127.0.0.1:8080/frame",
        data=f.read(),
        headers={"Content-Type": "image/png"},
        method="POST",
    )
    urllib.request.urlopen(req)

# Download the currently displayed pixels as a 72x16 PNG
png_bytes = urllib.request.urlopen("http://127.0.0.1:8080/frame").read()
with open("screen.png", "wb") as f:
    f.write(png_bytes)
```

## Project Structure

```text
markee/
├── Makefile             # Standard C build, test, run, install, clean targets
├── README.md            # Documentation & API reference
├── include/
│   ├── config.h         # Hardware specs (72x16, 6.35", 60 Hz, defaults)
│   ├── display.h        # Thread-safe 24-bit framebuffer & PNG encode/decode API
│   ├── renderer.h       # Monitor DPI sizing & plastic-encased LED renderer API
│   └── server.h         # Embedded POSIX TCP/HTTP server API
├── src/
│   ├── main.c           # CLI options, lifecycle, 60 Hz main loop, hotkeys
│   ├── display.c        # Framebuffer, raylib PNG I/O, built-in startup demo
│   ├── renderer.c       # 6.35" calibration, molded plastic bezel & LED glow rendering
│   └── server.c         # Multi-client HTTP/1.1 Keep-Alive socket server
├── examples/
│   └── demo.py          # Zero-dependency Python PNG & 60 FPS RGB demo client
└── tests/
    └── test_api.c       # C unit & live socket API integration tests
```
