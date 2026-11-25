# markee

A C11 + [raylib](https://www.raylib.com/) hardware simulator for a **72×16, 24-bit RGB LED matrix display** inspired by the [BUSY Bar](https://busy.app/products/busy-bar).

`markee` recreates the front LED display inside a molded plastic enclosure calibrated to appear **~6.35 inches wide** on screen, refreshes at **60 Hz**, and exposes a lightweight **HTTP/1.1 over TCP socket API** so any script or program can push 72×16 PNGs (or raw 24-bit RGB frames) to the display and read back the currently displayed pixels as a PNG.

## Inspiration

markee was inspired by the [BUSY Bar](https://busy.app/products/busy-bar), a real hardware LED display. I wanted to see and imagine how the device works, so I quickly vibe coded a software version of it.

markee is an independent hobby project. It is **not** an emulator for the BUSY Bar, and its HTTP API is **not compatible** with the official BUSY Bar API or its Python library, [busylib](https://busy-app.github.io/busylib-py/). Code written for one will not work with the other. For the real device, see the [BUSY Bar product page](https://busy.app/products/busy-bar) and the [busylib documentation](https://busy-app.github.io/busylib-py/).

## Features

- **72×16 LED matrix, 24-bit true color** (16,777,216 colors, 8 bits per R/G/B channel)
- **Physical ~6.35-inch sizing** — automatically reads your monitor's resolution and physical width (via EDID) to size the LED matrix to ~6.35 inches wide (with `--dpi` manual override if needed)
- **Plastic enclosure & diffused LED rendering** — molded off-white PC/ABS outer shell, recessed smoked black polycarbonate faceplate, faint unlit pixel diffuser cells, and soft per-LED glow halos
- **60 Hz refresh rate** locked via raylib
- **Simple bidirectional Socket / HTTP API** — zero-boilerplate integration with `curl`, Python, Node.js, Go, or raw sockets:
  - Upload a **72×16 PNG** directly via `POST /frame`
  - Download the displayed pixels as a **72×16 PNG** via `GET /frame`
  - Stream **raw 24-bit RGB** (`3456` bytes) at 60 FPS over persistent HTTP Keep-Alive connections
  - Draw shapes and bitmap text, including scrolling tickers, with one atomic `POST /draw` JSON batch
  - Play 3 gentle, procedurally synthesized alert sounds (`ding`, `chime`, `bell`) via `POST /alert`
  - Set/read individual pixels, solid fills, clear, brightness (`0–100%`) and a monochrome tint filter
- **Custom resolutions** from 1×1 to 1024×1024 via `--resolution`

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

# Install to /usr/local/bin (override with PREFIX=... and/or DESTDIR=...)
sudo make install
sudo make uninstall

# Clean build artifacts (distclean also removes snapshot.png)
make clean
make distclean

# List all targets
make help
```

The Makefile finds raylib with `pkg-config`. If that fails, it falls back to Homebrew's raylib prefix on macOS, or to `-lraylib -lGL -lm -ldl -lrt -lX11` on Linux. Set `CC=clang` or `CC=gcc` to choose the compiler.

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
| `-m, --monochrome <color>` | Start with the BT.709 monochrome filter on (`0xrrggbb`, `#rrggbb`, `#rgb`, or a [color name](docs/API.md#color-formats) such as `amber`). `--mono` is an alias. | Off |
| `-H, --host <addr>` | Bind address: IPv4, IPv6 (`::1`, `::`) or a hostname (`localhost`). Use `0.0.0.0` or `::` to listen on all interfaces; the API has **no authentication**. | `127.0.0.1` |
| `-p, --port <port>` | TCP port. `0` picks a free port, which is printed at startup. Overrides `MARKEE_PORT`. A value that isn't a whole number from 0 to 65535 (in either place) is an error. | `8080`, or `$MARKEE_PORT` |
| `--dpi <value>` | Set the monitor's logical DPI by hand for physical sizing (use when EDID detection is wrong) | Auto-detected (falls back to 110) |
| `--blank` | Start with a blank black screen instead of the startup demo | Off |
| `--mute` | Disable alert sound playback (`--no-audio` is an alias) | Off |
| `--headless` | Run only the API server, with no window. The demo and scrolling text still animate at 60 Hz, so `GET /frame` works. | Off |
| `-h, --help` | Print usage information and exit | — |

```bash
./markee                       # Default 72x16 square LEDs (~6.35" wide) at 1.0x zoom
./markee --circle              # 72x16 circular LEDs
./markee --circle --zoom 2     # Circular LEDs at 2x size
./markee -r 128x32 -z 0.75     # Custom 128x32 LED matrix at 0.75x zoom
./markee -m amber              # Amber monochrome panel look
./markee --headless --blank -p 0   # API only, blank screen, any free port (for scripts and CI)
./markee --help                # Print all CLI flags and shortcuts
```

At startup markee prints the address it is listening on:

```text
markee (72x16, zoom=1.00x, pixels=square) API listening on http://127.0.0.1:8080
```

Stop it with `Ctrl+C` (SIGINT), SIGTERM, `Q`/`ESC`, or by closing the window.

### Keyboard Shortcuts (GUI Mode)

| Key | Action |
|---|---|
| `1` / `2` / `3` | Preview the 3 built-in alert sounds (`ding`, `chime`, `bell`) |
| `C` | Clear display to black |
| `D` | Toggle built-in startup animation |
| `M` | Toggle the monochrome filter (uses the last filter color, or amber `#ffbf00` if none was set) |
| `P` | Toggle pixel shape (`square` / `circle`) |
| `S` | Save the current frame to `snapshot.png` in the working directory (with the monochrome filter applied if on) |
| `+` (`=`) / `-` | Increase / decrease simulated brightness by 10% (keypad `+`/`-` also work) |
| `Q` / `ESC` | Quit |

## HTTP API

`markee` serves a small **HTTP/1.1 API** at `http://127.0.0.1:8080` by default, with Keep-Alive and open CORS. Any write to the framebuffer stops the startup demo animation.

📖 **The full reference is in [`docs/API.md`](docs/API.md).** It covers every endpoint's parameters, request formats, responses and errors, plus the `/draw` op and font reference, scrolling text, color formats, limits and timeouts, and client recipes.

| Method | Path | Purpose |
|---|---|---|
| `GET` | [`/info`](docs/API.md#get-info) | Resolution, brightness, filter state, frame counter |
| `GET` | [`/frame`](docs/API.md#get-frame) | Displayed pixels as a PNG (`/frame.rgb` or `?format=raw` for raw RGB) |
| `POST` `PUT` | [`/frame`](docs/API.md#post-frame) | Replace the frame with a `W×H` PNG or `W×H×3` raw RGB bytes (`/frame.rgb`) |
| `POST` | [`/clear`](docs/API.md#delete-frame-post-clear) | Clear to black (also `DELETE /frame`) |
| `POST` | [`/fill`](docs/API.md#post-fill) | Fill with one color |
| `GET` `POST` | [`/pixel`](docs/API.md#pixels) | Read or set one pixel |
| `POST` | [`/draw`](docs/API.md#post-draw) | Draw an atomic JSON batch of `clear`, `pixel`, `line`, `rect`, `oval`, `polygon` and `text` ops (text can scroll) |
| `GET` `POST` | [`/brightness`](docs/API.md#post-brightness) | Simulated LED brightness `0–100` (GUI only; readback is unaffected) |
| `GET` `POST` `DELETE` | [`/monochrome`](docs/API.md#post-monochrome) | Single-color tint filter that leaves stored colors unchanged |
| `POST` | [`/demo`](docs/API.md#post-demo) | Turn the startup animation on or off |
| `GET` `POST` | [`/alert`](docs/API.md#alert-sounds) | List or play the 3 gentle alert sounds (`POST /alert/1`..`3` or `ding`/`chime`/`bell`) |

### Quick examples

```bash
# Send a 72x16 PNG; read the display back as a PNG
curl -X POST --data-binary @frame.png http://127.0.0.1:8080/frame
curl -o capture.png http://127.0.0.1:8080/frame

# Solid fill. Use 0x or bare hex in URLs: curl never sends the "#..." part of a URL
curl -X POST "http://127.0.0.1:8080/fill?color=0xf25c19"

# One pixel at (12, 8)
curl -X POST "http://127.0.0.1:8080/pixel?x=12&y=8&color=0x00ffb4"

# Shapes and text in one atomic batch
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear" },
  { "op": "rect", "x": 0, "y": 0, "w": 72, "h": 16, "color": "orange", "radius": 3 },
  { "op": "oval", "x": 4, "y": 4, "w": 8, "h": 8, "fill": "red" },
  { "op": "text", "x": 16, "y": 4, "text": "READY", "color": "warmwhite" }
]'

# Scrolling ticker (put the scrolling text op last)
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear" },
  { "op": "text", "x": 72, "y": 4, "text": "Hello from markee", "color": "amber", "scroll": true, "speed": 30 }
]'

# Amber monochrome filter on, then off
curl -X POST http://127.0.0.1:8080/monochrome -d amber
curl -X DELETE http://127.0.0.1:8080/monochrome

# Brightness 60%, play a gentle chime, then check status
curl -X POST "http://127.0.0.1:8080/brightness?value=60"
curl -X POST http://127.0.0.1:8080/alert/chime
curl http://127.0.0.1:8080/info
```

> **Colors:** every endpoint accepts `0xff5500`, `ff5500`, `#ff5500`, 3-digit forms such as `0xf50`, and named colors such as `amber`, `warmwhite` and `teal`. In URLs, use the `0x` or bare form, because `#` is never sent. See [Color formats](docs/API.md#color-formats).

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

## Testing

```bash
make test
```

This builds `./test_api` and runs unit tests for layout sizing, the framebuffer and PNG round-trip, `/draw` and the monochrome filter. It also starts a real server on a free port and checks the HTTP API over sockets, including keep-alive, raw RGB, custom resolutions and atomic `/draw`. No window is opened.

## Project Structure

```text
markee/
├── Makefile             # Build, test, run, install, clean targets (`make help`)
├── README.md            # Overview, build and CLI usage
├── docs/
│   └── API.md           # Full HTTP API reference
├── include/
│   ├── audio.h          # Procedural alert sound synthesis (ding/chime/bell) and playback queue
│   ├── config.h         # Hardware specs and limits (72x16, 6.35", 60 Hz, ports, body/header limits)
│   ├── display.h        # Thread-safe 24-bit framebuffer, PNG I/O, brightness/monochrome/demo state
│   ├── draw.h           # Color parsing, bitmap fonts, /draw JSON op executor
│   ├── json.h           # Minimal JSON parser used by /draw and /monochrome
│   ├── renderer.h       # Monitor DPI sizing and plastic-enclosure LED renderer
│   └── server.h         # Embedded POSIX TCP/HTTP server
├── src/
│   ├── main.c           # CLI parsing, lifecycle, 60 Hz main loop, hotkeys
│   ├── audio.c          # Additive bell/chime synthesis and polyphonic raylib playback
│   ├── display.c        # Framebuffer, PNG encode/decode, BT.709 monochrome filter, demo and scroll animation
│   ├── draw.c           # Shape rasterizers (line/rect/oval/polygon), 5x7 and 3x5 fonts, atomic /draw
│   ├── json.c           # Recursive-descent JSON parser
│   ├── renderer.c       # 6.35" calibration, molded bezel and LED glow rendering
│   └── server.c         # Thread-per-connection HTTP/1.1 Keep-Alive server and routes
├── examples/
│   └── demo.py          # Zero-dependency Python client: /info, PNG upload/readback, 60 FPS raw stream
└── tests/
    └── test_api.c       # Unit tests and live socket API integration tests (`make test`)
```

## License

[MIT](LICENSE) © 2026 Steven Spencer
