# markee

[![CI](https://github.com/sspencer/markee/actions/workflows/ci.yml/badge.svg)](https://github.com/sspencer/markee/actions/workflows/ci.yml)
[![CodeQL](https://github.com/sspencer/markee/actions/workflows/codeql.yml/badge.svg)](https://github.com/sspencer/markee/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A C11 + [raylib](https://www.raylib.com/) desktop simulator for a **72×16, 24-bit RGB LED matrix display**, inspired by the [BUSY Bar](https://busy.app/products/busy-bar).

`markee` renders the LED panel inside a molded plastic enclosure calibrated to appear **~6.35 inches wide** on screen, refreshes at **60 Hz**, and exposes a lightweight **HTTP/1.1 API** so any script or program can draw shapes and scrolling text, push 72×16 PNGs or raw 24-bit RGB frames, trigger gentle alert chimes, and read back the current display as a PNG.

![markee running in demo mode](docs/demo.png)

## Inspiration

`markee` began as a quick software take on the [BUSY Bar](https://busy.app/products/busy-bar)—built to get a feel for how a small, glanceable desk LED bar works in practice and what's fun to drive on it.

It is an independent hobby project, not a BUSY Bar emulator. Its HTTP API is intentionally minimal and is **not compatible** with the official BUSY Bar API or [busylib](https://busy-app.github.io/busylib-py/). For the actual hardware and official SDK, see the [BUSY Bar product page](https://busy.app/products/busy-bar) and the [busylib documentation](https://busy-app.github.io/busylib-py/).

## Features

- **72×16 true-color LED matrix** — 24-bit RGB per pixel, customizable from `1×1` to `1024×1024` via `--resolution`
- **True-to-size desk rendering** — reads monitor EDID dimensions to scale the matrix to ~6.35 inches wide (overridable with `--dpi` and `--zoom`), framed in a molded off-white PC/ABS shell, smoked polycarbonate faceplate, square or circular LED dies, and soft glow halos at **60 Hz**
- **Simple HTTP/1.1 API (Keep-Alive + open CORS)** — works out of the box with `curl`, Python, browser `fetch()`, Node.js, Go, or raw sockets:
  - **Atomic drawing (`POST /draw`):** batch `clear`, `pixel`, `line`, `rect` (with corner radius), `oval`, `polygon`, bitmap `text` (3 built-in fonts + smooth 60 Hz marquee scrolling), and placed `image` sprites (PNGs and static/animated GIFs)
  - **Frame I/O (`/frame`):** upload or read back lossless `W×H` PNGs, or stream raw 24-bit RGB (`3,456` bytes at 72×16) at 60 FPS
  - **Rich drawing extras:** text alignment and a proportional font, clipped scrolling regions, 16 built-in 8×8 `icon`s, linear `gradient`s, and per-shape `alpha` blending
  - **Autonomous widgets:** `progress` bars, `sparkline`s, and server-side `clock` and `countdown` ops that keep ticking with no client attached
  - **Scene transitions & notifications:** `?transition=fade|slide-left|slide-up|wipe` on `/draw`, and `/notify` (or `/draw?ttl=`) to show a temporary banner and then restore the previous screen
  - **Animated GIF playback (`/animate`):** crop, contain, or stretch fit, fill color, loop count, speed, and pause/resume
  - **GIF recording (`/record`):** capture the live display to an animated GIF (also hotkey `R`)
  - **Events (`/events`):** LED clicks, bezel taps, and key presses via JSON long-polling or Server-Sent Events
  - **Pixels & panel controls:** read/set individual pixels, solid fills, clear, GUI brightness (`0–100%`), idle dimming (night mode), and a non-destructive BT.709 monochrome phosphor tint filter
  - **Optional Bearer-token auth** (`--token` / `MARKEE_TOKEN`) for exposing the API beyond localhost
- **Built-in gentle alert sounds (`/alert`)** — 3 procedurally synthesized, non-startling chimes (`ding`, `chime`, `bell`) plus custom note-sequence melodies, with volume control and overlapping playback; zero external audio files required
- **Web Playground (`GET /ui`)** — a browser page with a live LED preview, a `/draw` JSON editor, GIF/PNG drag-and-drop, and alert buttons
- **Desktop widget modes** — always-on-top, borderless floating window, compass/coordinate placement with position persistence, and 4 enclosure color themes
- **Client CLI subcommands** — `markee say`, `notify`, `gif`, `timer`, `clock`, `alert`, `clear`, and `info` drive a running instance without `curl`
- **INI config file** — `~/.config/markee/config.ini` sets defaults for every startup option
- **Headless mode** — run `--headless` for scripts, daemons, and CI without opening a window
- **Integrations & packaging** — zero-dependency Python applets, an MCP server for AI coding agents, a man page, Bash/Zsh/Fish completions, and macOS `.app`/Homebrew/`.deb`/tarball packaging

## Quick start

Install `raylib` (see [Building](#building)), then build and launch:

```bash
make run
```

In another terminal, draw a status banner and play a gentle chime:

```bash
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear" },
  { "op": "rect", "x": 0, "y": 0, "w": 72, "h": 16, "color": "orange", "radius": 3 },
  { "op": "oval", "x": 4, "y": 4, "w": 8, "h": 8, "fill": "green" },
  { "op": "text", "x": 16, "y": 4, "text": "READY", "color": "warmwhite" }
]'

curl -X POST http://127.0.0.1:8080/alert/chime
curl -o capture.png http://127.0.0.1:8080/frame
```

## Building

### Prerequisites

- A C11 compiler (`cc`, `clang`, or `gcc`) and `make`
- `raylib` (5.x or 6.x) and `pkg-config`

**Install `raylib`:**

| Platform | Command |
|---|---|
| macOS (Homebrew) | `brew install raylib pkg-config` |
| Ubuntu / Debian | `sudo apt install build-essential pkg-config libraylib-dev` |
| Arch Linux | `sudo pacman -S base-devel raylib pkgconf` |

### Make targets

| Command | Description |
|---|---|
| `make` | Build `./markee` (`-O2`) |
| `make run` | Build and launch `./markee` |
| `make test` | Build and run the C unit & socket API test suite (`./test_api`) |
| `make sanitize` | Build and run `./test_api_san` under AddressSanitizer + UndefinedBehaviorSanitizer |
| `make fuzz-smoke` | Build and run standalone ASan/UBSan fuzz harnesses for GIF, JSON, `/draw`, and HTTP parsers |
| `make fuzz` | Build native LLVM libFuzzer binaries in `build/fuzz/` (`CC=clang`) |
| `make app` | Build a native macOS `build/Markee.app` bundle (see [`packaging/README.md`](packaging/README.md)) |
| `make tarball` | Build a versioned release tarball in `build/` |
| `make debug` / `make release` | Clean rebuild with `-O0 -g` or `-O3` |
| `make install` / `make uninstall` | Install or remove the binary, man page (`man markee`), docs (`README.md`, `API.md`, `LICENSE`), and Bash/Zsh/Fish completions under `~/.local` (or `/usr/local`; override with `PREFIX=` / `DESTDIR=`) |
| `make clean` / `make distclean` | Remove build artifacts (`distclean` also deletes `snapshot.png`) |
| `make help` | List all targets |

`Makefile` locates `raylib` via `pkg-config` and falls back to standard Homebrew/Linux flags if unavailable. Pass `CC=clang` or `CC=gcc` to pick a specific compiler.

## Usage

```bash
./markee [options]
```

On startup, `markee` prints its bound address and main endpoints:

```text
markee (72x16, zoom=1.00x, pixels=square) API listening on http://127.0.0.1:8080
```

Quit at any time with `Ctrl+C` (`SIGINT`), `SIGTERM`, `Q`/`ESC`, or by closing the window.

### CLI options

| Flag | Description | Default |
|---|---|---|
| `-r, --resolution <WxH>` | Matrix dimensions in LEDs (`1..1024` per axis, e.g. `128x32`, `64x64`) | `72x16` |
| `-z, --zoom <factor>` | Window zoom multiplier (`0.1..10.0`) | `1.0` |
| `--square`, `--circle` | Render LEDs as squares or circles | `--square` |
| `-s, --shape <style>` | Pixel shape: `square` or `circle` | `square` |
| `-m, --monochrome <color>` | Start with the BT.709 monochrome tint filter on (`#rrggbb`, `0xrrggbb`, `#rgb`, or a [color name](docs/API.md#color-formats) like `amber`). Alias: `--mono` | Off |
| `-H, --host <addr>` | Bind address: IPv4, IPv6 (`::1`, `::`), or hostname (`localhost`). Without `--token` the API is unauthenticated, so keep the default loopback address unless you set a token | `127.0.0.1` |
| `-p, --port <port>` | TCP port (`0..65535`; `0` picks a free ephemeral port). Overrides `MARKEE_PORT` | `8080` (or `$MARKEE_PORT`) |
| `--token <secret>` | Require `Authorization: Bearer <secret>` (or `?token=`) on API requests. Overrides `MARKEE_TOKEN` | Off (or `$MARKEE_TOKEN`) |
| `--dpi <value>` | Override monitor logical DPI (`> 0`) if EDID physical sizing is wrong | Auto-detected (fallback `110`) |
| `--always-on-top` | Keep the GUI window floating above other windows. Alias: `--topmost`. `--no-always-on-top` overrides a config file setting | Off |
| `--borderless` | Floating widget mode: hide OS titlebar, transparent backdrop, drag enclosure to move. Alias: `--frameless`. `--no-borderless` overrides a config file setting | Off |
| `--position <pos>` | Initial window placement: 9-point compass (`nw`, `n`, `ne`, `w`, `c`, `e`, `sw`, `s`, `se` or full words `northwest`, `north`, …), edge/corner names (`top-left`, `top`, `left`, `center`, `right`, `bottom`, …), or pixel coordinates `X,Y` where negative values inset from the right/bottom edge (`-10,10`, `-0,-0`) and either axis may be `center` (`center,-20`). Alias: `--pos` (also supports `--position=<pos>` and `--pos=<pos>`) | `center` |
| `--corner <place>` | Alias for named `--position` anchors (`nw`, `n`, `ne`, `w`, `c`, `e`, `sw`, `s`, `se`, `top-left`, `left`, `right`, `bottom`, …) | `center` |
| `--case <theme>` | Enclosure color theme: `white`, `charcoal` (`black`/`dark`), `orange`, `retro` (`beige`) | `white` |
| `--idle-dim <sec>` | Automatically dim GUI brightness after `sec` seconds with no API writes or input | Off (`0`) |
| `--idle-brightness <0..100>` | Dimmed GUI brightness level used by `--idle-dim` | `20` |
| `--demo`, `--blank` | Start with the built-in 60 Hz demo animation, or with a blank display (`--blank` overrides `demo = true` in a config file) | `--blank` |
| `--mute` | Disable alert sound playback. Alias: `--no-audio` | Off |
| `--headless` | Run the API server without a GUI window (60 Hz demo, scrolling text, `GET /frame`, and alert audio still run) | Off |
| `--config <path>` | Load default options from an INI file (`$XDG_CONFIG_HOME/markee/config.ini` or `~/.config/markee/config.ini` auto-loaded when present; CLI flags override) | Auto |
| `--no-config` | Skip loading the default config file | Off |
| `--save-position` | Persist the final GUI window `position = X,Y` to the active config file on exit | Off |
| `-h, --help` | Show usage and keyboard shortcuts, then exit | — |

**Examples:**

```bash
./markee                             # Default 72x16 blank display (~6.35" wide)
./markee --demo                      # Launch with the built-in demo animation
./markee --circle --zoom 2           # Circular LEDs at 2x size
./markee -r 128x32 -z 0.75           # Custom 128x32 matrix at 0.75x zoom
./markee -m amber                    # Amber phosphor monochrome look
./markee --headless --mute -p 0      # Silent API-only mode on any free port (CI/scripts)
```

### Configuration file

`markee` automatically reads `$XDG_CONFIG_HOME/markee/config.ini` (falling back to `~/.config/markee/config.ini`) on startup when the file exists. Use `--config <path>` to load a different file, or `--no-config` to skip it.

```ini
# ~/.config/markee/config.ini
zoom = 1.25
shape = circle
case = charcoal
always_on_top = true
borderless = true
corner = top-right
idle_dim = 300
idle_brightness = 20
save_position = true
```

Each line is `key = value`. Lines starting with `#` or `;` are comments, `[section]` headers are accepted and ignored, values may be wrapped in single or double quotes, and booleans accept `true`/`false`, `yes`/`no`, `on`/`off`, or `1`/`0`.

| Key (aliases) | Equivalent flag | Example value |
|---|---|---|
| `resolution` | `--resolution` | `128x32` |
| `zoom` | `--zoom` | `1.25` |
| `shape` (`pixel_shape`) | `--shape` | `square`, `circle` |
| `monochrome` (`mono`) | `--monochrome` | `amber`, `#00ff00`; `off`/`none` disables |
| `host` | `--host` | `127.0.0.1` |
| `port` | `--port` | `8080` |
| `token` | `--token` | `s3cret` |
| `dpi` | `--dpi` | `110` |
| `always_on_top` (`topmost`) | `--always-on-top` | `true` |
| `borderless` (`frameless`) | `--borderless` | `true` |
| `position` (`pos`) | `--position` | `-10,10`, `ne`; `auto`/`none` clears it |
| `corner` | `--corner` | `top-right` |
| `case` (`case_theme`) | `--case` | `charcoal` |
| `idle_dim` | `--idle-dim` | `300` |
| `idle_brightness` | `--idle-brightness` | `20` |
| `demo` | `--demo` | `true` |
| `mute` | `--mute` | `true` |
| `headless` | `--headless` | `false` |
| `save_position` | `--save-position` | `true` |

With `save_position = true` (or `--save-position`), `markee` writes the window's final `position = X,Y` back into the active config file on exit.

**Precedence (highest first):** CLI flags → environment variables (`MARKEE_PORT`, `MARKEE_TOKEN`) → config file → built-in defaults.

### Environment variables

| Variable | Used by | Purpose |
|---|---|---|
| `MARKEE_PORT` | Server and client subcommands | Default TCP port (`8080`) |
| `MARKEE_TOKEN` | Server and client subcommands | Bearer token to require (server) or send (client) |
| `MARKEE_HOST` | Client subcommands | Server address to connect to (`127.0.0.1`) |
| `MARKEE_URL` | Python examples | Full base URL; overrides `MARKEE_HOST`/`MARKEE_PORT` |
| `XDG_CONFIG_HOME` | Server | Base directory for `markee/config.ini` (defaults to `~/.config`) |

### Client CLI subcommands

The same binary can drive a running `markee` instance, so you don't have to write `curl` commands or JSON by hand. Each subcommand reads `MARKEE_HOST`, `MARKEE_PORT`, and `MARKEE_TOKEN`, or takes `-H`/`--host`, `-p`/`--port`, and `--token`.

```bash
./markee say "BUILD OK!" --color green --bg "#001408" --chime
./markee say "Deploying to production..." --scroll --speed 40 --transition slide-left
./markee notify "DEPLOY DONE" --ttl 5 --border green --sound bell
./markee gif examples/cat_chase-72x16.gif --fill white --fit contain --speed 1.5
./markee timer 25m --color amber --sound chime
./markee clock --color warmwhite --format 12h
./markee alert chime --volume 75
./markee clear
./markee info
```

| Subcommand | What it does | Options |
|---|---|---|
| `say <text>` | Clears the display and draws centered text (`POST /draw`) | `-c`/`--color` (default `warmwhite`), `--bg` (default `black`), `--font` (default `6x8`), `--scroll`, `--speed <px/s>` (scroll speed, default `24`), `--transition fade\|slide-left\|slide-up\|wipe`, `--sound <name>` or `--ding`/`--chime`/`--bell`, `--volume 0..100` |
| `notify <text>` | Shows a temporary banner, then restores the previous screen (`POST /notify`) | `--ttl <sec>` (default `5`), `-c`/`--color`, `--bg`, `--border`, `--font`, `--sound <name>` or `--ding`/`--chime`/`--bell` |
| `gif <file.gif>` | Uploads and plays a GIF, up to 4 MiB (`POST /animate`) | `--fill <color>`, `--fit crop\|contain\|stretch` (default `crop`), `--speed <x>` (default `1.0`), `--loop <n>` |
| `timer <duration>` | Starts an autonomous `countdown` widget. Durations look like `90s`, `25m`, or `1h30m` | `-c`/`--color`, `--bg`, `--font`, `--no-bar`, `--sound <name>` or `--ding`/`--chime`/`--bell` (plays when the timer finishes) |
| `clock` | Starts the autonomous `clock` widget | `-c`/`--color`, `--bg`, `--font`, `--format 24h\|24hm\|12h\|12hm` (default `24h`) |
| `alert [ding\|chime\|bell]` | Plays an alert sound (default `ding`) | `--volume 0..100` (default `100`) |
| `clear` | Clears the display | — |
| `info` | Prints the `GET /info` JSON | — |

### Keyboard shortcuts

Available when running in GUI mode:

| Key | Action |
|---|---|
| `1` / `2` / `3` | Preview the 3 built-in alert sounds (`ding`, `chime`, `bell`; keypad `1–3` also work) |
| `C` | Clear display to black |
| `D` | Toggle the built-in demo animation |
| `K` | Cycle enclosure color theme (`white` / `charcoal` / `orange` / `retro`) |
| `M` | Toggle the monochrome filter (reuses last tint color, or amber `#ffbf00`) |
| `P` | Toggle pixel shape (`square` / `circle`) |
| `R` | Start/stop animated GIF recording (saves `recording.gif` in the working directory) |
| `S` | Save current frame to `snapshot.png` in the working directory (includes monochrome filter if on) |
| `T` | Toggle always-on-top window mode |
| `SPACE` / `ENTER` | Emit an `action` button event on `GET /events` |
| `+` (`=`) / `-` | Increase or decrease simulated brightness by 10% (keypad `+`/`-` also work) |
| `Q` / `ESC` | Quit |

## HTTP API

`markee` serves an HTTP/1.1 API on `http://127.0.0.1:8080` by default, with persistent Keep-Alive connections and open CORS. Start with `--token <secret>` to require `Authorization: Bearer <secret>` on every request (see [Security note](docs/API.md#security-note)). Any write to the framebuffer automatically stops the demo animation if it is running.

**See [`docs/API.md`](docs/API.md) for the complete reference**—including every parameter, draw op, font, scrolling rule, color format, protocol limit, and client recipe.

| Method | Path | Purpose |
|---|---|---|
| `GET` | [`/info`](docs/API.md#get-info) | Resolution, settings, audio readiness, frame counter |
| `GET` | [`/ui`](docs/API.md#web-playground) | Built-in Web Playground (live LED preview, `/draw` JSON editor, drag-and-drop GIF/PNG, alerts) |
| `POST` `PUT` | [`/draw`](docs/API.md#post-draw) | Atomic JSON batch of shape, `text`, `image`, `icon`, `gradient`, and widget (`progress`, `sparkline`, `clock`, `countdown`) ops (optional `?ttl=<sec>` and `?transition=<effect>`) |
| `GET` `POST` `PUT` `DELETE` | [`/notify`](docs/API.md#post-notify) | Show a temporary notification banner/batch for `ttl` seconds, then auto-restore prior screen |
| `GET` | [`/frame`](docs/API.md#get-frame) | Read displayed pixels as a PNG (`/frame.rgb` or `?format=raw` for raw RGB24) |
| `POST` `PUT` | [`/frame`](docs/API.md#post-frame) | Replace the frame with a `W×H` PNG or `W×H×3` raw RGB24 bytes (`/frame.rgb`) |
| `GET` `POST` `PUT` `DELETE` | [`/animate`](docs/API.md#animated-gifs) | Play, inspect, or stop an animated GIF (optional `?fit=`, `?fill=`, `?loop=`, `?speed=`) |
| `POST` | [`/animate/pause`, `/animate/resume`, `/animate/speed`](docs/API.md#post-animate) | Pause, resume, or change the speed of GIF playback |
| `POST` | [`/clear`](docs/API.md#delete-frame-post-clear) | Clear to black (also `DELETE /frame`) |
| `POST` | [`/fill`](docs/API.md#post-fill) | Fill the entire display with one color |
| `GET` `POST` `PUT` | [`/pixel`](docs/API.md#pixels) | Read or set a single pixel |
| `GET` `POST` `PUT` | [`/brightness`](docs/API.md#post-brightness) | Read or set GUI LED brightness (`0–100`; readback unaffected) |
| `GET` `POST` `PUT` `DELETE` | [`/idle`](docs/API.md#idle-dimming) | Read, configure, or disable idle dimming (night mode) |
| `GET` `POST` `PUT` `DELETE` | [`/monochrome`](docs/API.md#post-monochrome) | Read or toggle the non-destructive BT.709 tint filter |
| `POST` | [`/demo`](docs/API.md#post-demo) | Turn the built-in demo animation on or off |
| `GET` `POST` | [`/alert`](docs/API.md#alert-sounds) | List or play the 3 gentle alert sounds (`POST /alert/1`..`3` or `ding`/`chime`/`bell`) or a custom `melody` |
| `GET` `POST` | [`/events`](docs/API.md#get-events) | Poll or stream (SSE) LED clicks, bezel/key button presses, and frame updates; `POST` injects an event |
| `GET` `POST` `DELETE` | [`/record`](docs/API.md#gif-recording) | Start, inspect, or stop recording the display to an animated GIF |

### More `curl` examples

```bash
# Scrolling ticker (place the scrolling text op last)
curl -X POST http://127.0.0.1:8080/draw -d '[
  { "op": "clear" },
  { "op": "text", "x": 72, "y": 4, "text": "Hello from markee", "color": "amber", "scroll": true, "speed": 30 }
]'

# Solid fill & single pixel (use 0x or bare hex in URLs—clients strip "#..." fragments)
curl -X POST "http://127.0.0.1:8080/fill?color=0xf25c19"
curl -X POST "http://127.0.0.1:8080/pixel?x=12&y=8&color=0x00ffb4"

# Upload a 72x16 PNG
curl -X POST --data-binary @frame.png http://127.0.0.1:8080/frame

# Amber monochrome tint on, then off
curl -X POST http://127.0.0.1:8080/monochrome -d amber
curl -X DELETE http://127.0.0.1:8080/monochrome

# Adjust brightness, play a soft bell at 70% volume, inspect status
curl -X POST "http://127.0.0.1:8080/brightness?value=60"
curl -X POST "http://127.0.0.1:8080/alert/bell?volume=70"
curl http://127.0.0.1:8080/info
```

> **Colors:** every color endpoint accepts `0xff5500`, `ff5500`, `#ff5500`, 3-digit shorthand (`0xf50`), and named colors (`amber`, `warmwhite`, `teal`, etc.). In query strings, avoid raw `#` (use `0x`, bare hex, or `%23`). See [Color formats](docs/API.md#color-formats).

### Python applets & MCP server

Zero-dependency Python 3 integrations live in `examples/`:

- [`examples/demo.py`](examples/demo.py) — `/info`, PNG round-trip, and 60 FPS raw RGB24 stream (`python3 examples/demo.py --seconds 8 --save readback.png`)
- [`examples/mcp_server.py`](examples/mcp_server.py) — stdio **Model Context Protocol (MCP)** server exposing `markee_say`, `markee_notify`, `markee_progress`, `markee_draw`, `markee_timer`, `markee_clock`, `markee_alert`, `markee_clear`, and `markee_info` to AI coding agents (reads `MARKEE_URL` / `MARKEE_TOKEN`)
- [`examples/pomodoro.py`](examples/pomodoro.py) — Focus / Pomodoro timer with countdown bar, color shift, completion melodies, and `Space`/bezel skip (`python3 examples/pomodoro.py --work 25 --short-break 5`)
- [`examples/now_playing.py`](examples/now_playing.py) — Now Playing ticker for macOS Spotify/Apple Music and Linux `playerctl` with icon, clipped scrolling title, and a Tesla-style bottom ambient LED strip tinted to the album art's highlight color (`python3 examples/now_playing.py --demo`)
- [`examples/sysmon.py`](examples/sysmon.py) — Live CPU sparkline, memory meter, and network activity monitor for macOS & Linux (`python3 examples/sysmon.py`)
- [`examples/git_watch.py`](examples/git_watch.py) — Local Git + test runner watcher (`--cmd "make test"`) or GitHub Actions CI monitor (`--github owner/repo`) with pass/fail notifications and melodies
- [`examples/carousel.py`](examples/carousel.py) — Rotating status carousel daemon (`clock`, `date`, `sys`, `git`, `weather`, custom `--card` slides) with smooth transitions and `Space`/bezel advance (`python3 examples/carousel.py --cards clock,date,sys,git`)

Every applet except `demo.py` takes `--url` and `--token`, or reads `MARKEE_URL` (or `MARKEE_HOST` + `MARKEE_PORT`) and `MARKEE_TOKEN`; `demo.py` takes `--host` and `--port`. Each applet reads the display size from `GET /info` at startup (the MCP server checks it on every call) and lays itself out to fit, so it works at any `--resolution`. The layouts are designed around 72×16, and very small displays will crop content. Run any script with `--help` for its full options. Sample 72×16 and 16×16 GIFs for `/animate` and `markee gif` are in `examples/*.gif`.

To register the MCP server with an MCP client such as Claude Desktop or Cursor:

```json
{
  "mcpServers": {
    "markee": {
      "command": "python3",
      "args": ["/path/to/markee/examples/mcp_server.py"],
      "env": { "MARKEE_URL": "http://127.0.0.1:8080" }
    }
  }
}
```

Minimal stdlib-only PNG upload and readback:

```python
import urllib.request

with open("frame.png", "rb") as f:
    req = urllib.request.Request(
        "http://127.0.0.1:8080/frame",
        data=f.read(),
        headers={"Content-Type": "image/png"},
        method="POST",
    )
    urllib.request.urlopen(req)

png_bytes = urllib.request.urlopen("http://127.0.0.1:8080/frame").read()
with open("screen.png", "wb") as f:
    f.write(png_bytes)
```

## Testing

```bash
make test
```

Builds and runs `./test_api` with no GUI window and silent audio. The suite covers physical layout sizing, framebuffer/PNG round-trips, BT.709 monochrome filtering, unified color parsing, synthesized alert PCM quality, the GIF decoder, INI config parsing, and live socket integration tests over an ephemeral localhost port (Keep-Alive, raw RGB24, custom resolutions, atomic `/draw` and the `image` op, widgets, `/notify` TTLs, `/animate`, `/events`, `/alert`, and HTTP error handling).

```bash
make sanitize     # same suite under AddressSanitizer + UndefinedBehaviorSanitizer
make fuzz-smoke   # standalone fuzz harnesses for the GIF, JSON, /draw, and HTTP parsers
```

GitHub Actions runs on every push and pull request:

- **CI** (`.github/workflows/ci.yml`): build with `-Werror`, `./test_api`, `make sanitize`, and `make fuzz-smoke` on macOS (clang) and Ubuntu (gcc and clang). Ubuntu ships no raylib package, so `.github/scripts/install-raylib.sh` builds it from source. Also byte-compiles the Python examples, syntax-checks the shell completions, and runs ShellCheck on the packaging scripts.
- **CodeQL** (`.github/workflows/codeql.yml`): C and Python security analysis, also run weekly.
- **Release** (`.github/workflows/release.yml`): pushing a `v*` tag builds tarballs for macOS arm64, Linux x86_64, and Linux aarch64 with raylib linked statically, then publishes them with `SHA256SUMS` to a GitHub Release.

To cut a release: `git tag v1.2.0 && git push origin v1.2.0`.

## Project structure

```text
markee/
├── .github/
│   ├── workflows/
│   │   ├── ci.yml       # macOS + Linux CI (strict -Werror, tests, ASan/UBSan, fuzz smoke)
│   │   ├── codeql.yml   # CodeQL static analysis (C, Python)
│   │   └── release.yml  # Tag-triggered release tarballs → GitHub Releases
│   ├── scripts/
│   │   └── install-raylib.sh  # Build raylib from source on Ubuntu
│   ├── ISSUE_TEMPLATE/  # Bug report / feature request forms
│   ├── pull_request_template.md
│   └── dependabot.yml   # Weekly GitHub Actions version bumps
├── Makefile             # Build, test, sanitize, fuzz-smoke, app, tarball, install (`make help`)
├── README.md            # Overview, build, config, and CLI usage
├── completions/
│   ├── markee.bash      # Bash tab completion
│   ├── _markee          # Zsh tab completion
│   └── markee.fish      # Fish tab completion
├── docs/
│   ├── API.md           # Complete HTTP API reference and recipes
│   ├── markee.1         # Manual page (`man markee`)
│   ├── ideas.md         # Product & engineering roadmap (all items done)
│   └── demo.png         # Screenshot of markee running in --demo mode
├── include/
│   ├── audio.h          # Procedural alert sound & custom melody synthesizer
│   ├── cli.h            # Built-in client subcommands (say, notify, gif, timer, clock, ...)
│   ├── config.h         # Display defaults and HTTP limits (72x16, 6.35", 60 Hz)
│   ├── display.h        # Thread-safe framebuffer, PNG/GIF I/O, widgets, transitions, events, recording
│   ├── draw.h           # Color parsing, bitmap/proportional fonts, icons, gradients, /draw executor
│   ├── gif.h            # Strict GIF87a/89a decoder & animated GIF89a encoder
│   ├── ini.h            # ~/.config/markee/config.ini parser and window position persistence
│   ├── json.h           # Minimal recursive-descent JSON parser
│   ├── renderer.h       # Monitor DPI sizing, case themes, borderless drag, LED renderer
│   ├── server.h         # Embedded POSIX TCP/HTTP server (Keep-Alive, SSE, Bearer auth)
│   └── webui.h          # Built-in single-page Web Playground (GET /ui)
├── src/                 # Corresponding C11 implementations + main.c
├── examples/
│   ├── demo.py          # /info, PNG round-trip, and 60 FPS raw RGB24 stream
│   ├── mcp_server.py    # Zero-dependency stdio MCP server for AI coding agents
│   ├── pomodoro.py      # Focus / Pomodoro timer with bezel skip and melodies
│   ├── now_playing.py   # macOS Spotify/Apple Music & Linux playerctl ticker
│   ├── sysmon.py        # Live CPU sparkline, memory meter, and network monitor
│   ├── git_watch.py     # Local Git/test runner & GitHub Actions CI watcher
│   ├── carousel.py      # Rotating status carousel daemon with transitions
│   └── *.gif            # Sample 72x16 and 16x16 GIFs for /animate
├── packaging/
│   ├── README.md        # Packaging & distribution guide
│   ├── build-macos-app.sh # Native macOS Markee.app bundle builder
│   ├── com.markee.display.plist # macOS LaunchAgent login item
│   ├── markee.rb        # Homebrew formula with brew services support
│   ├── build-deb.sh     # Debian/Ubuntu .deb package builder
│   └── build-tarball.sh # Portable release tarball builder
└── tests/
    ├── test_api.c       # Unit and live socket API integration tests (`make test`)
    └── fuzz/            # libFuzzer + standalone ASan/UBSan parser fuzzers (`make fuzz-smoke`)
```

## License

[MIT](LICENSE) © 2026 Steven Spencer
