# markee — Ideas & Roadmap

A running backlog of product, API, integration, and engineering ideas for `markee`.

---

## 1. Desktop Widget & GUI Experience

- [x] **Always-on-top mode (`--always-on-top`, hotkey `T`):** Keep the LED bar visible above editors and terminals (`FLAG_WINDOW_TOPMOST`) so it works as a true glanceable desk status display.
- [x] **Borderless floating widget (`--borderless`):** Hide the OS titlebar and render only the molded enclosure with a transparent window background (`FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TRANSPARENT`) plus click-and-drag repositioning.
- [x] **Window placement & persistence (`--position X,Y`, `--corner top-right|bottom-right|...`):** Park the bar in a predictable corner on launch (position persistence covered with config file support).
- [x] **Enclosure color themes (`--case white|charcoal|orange|retro`):** Support alternate shell finishes (e.g., matte black, classic off-white, bright accent) via CLI flag or hotkey (`K`).
- [x] **Interactive bezel / click events:** Clicks on the LED matrix (with exact pixel `x,y`), clicks on the bezel, and `Space`/`Enter` presses emit events over `GET /events`.
- [x] **Idle dimming / night mode:** `--idle-dim <sec>`, `--idle-brightness <0..100>`, and `GET`/`POST`/`DELETE /idle` smoothly dim GUI brightness after inactivity and wake on the next write or interaction.

---

## 2. Display & Drawing Capabilities

- [x] **Transient notifications with TTL (`POST /notify` or `?ttl=<sec>`):** Temporarily display an alert (banner JSON, plain text, or `/draw` batch + optional alert sound) for `ttl` seconds and automatically restore the previous display state (pixels, demo, scroller, or GIFs). `GET /notify` inspects remaining time; `DELETE /notify` dismisses early.
- [x] **Clipped scrolling regions (`clip: [x, y, w, h]` on scrolling `text`):** Constrain static and marquee scrolling text to a sub-rectangle so an animated icon/GIF on the left (`x: 0..15`) and a scrolling ticker on the right (`x: 18..71`) never overlap.
- [x] **Text alignment & proportional font option (`align: "left"|"center"|"right"`, `font: "prop"`):** Align text automatically within the screen, `w` box, or `clip` region, and use `"prop"` (`"proportional"`/`"compact"`) for tight variable-width glyphs.
- [x] **Built-in autonomous widgets (`clock`, `countdown`, `progress`, `sparkline`):**
  - Server-side live `clock` and `countdown` timer ops (with optional bottom progress bar and completion alert sound) tick autonomously at 60 Hz.
  - Compact `progress` bar and `sparkline` primitives for dashboards.
- [x] **Scene transitions (`transition: "fade"|"slide-left"|"slide-up"|"wipe"`):** Supported via `?transition=<type>&transition_ms=<50..5000>` on `POST /draw` for smooth 60 Hz crossfades, slides, and wipes.
- [x] **Animation playback controls:** Speed multiplier (`?speed=0.1..10.0`, `POST /animate/speed`), `POST /animate/pause` & `POST /animate/resume`, and scaling modes (`?fit=crop|contain|stretch`) on `/animate`.
- [x] **Built-in monochrome pixel icon set:** 16 named 8×8 icons (`check`, `cross`, `bell`, `wifi`, `flame`, `play`, `pause`, `battery`, `heart`, `star`, `clock`, `sun`, `moon`, `mail`, `bolt`, `music`) via `"op": "icon"` with optional `scale` (`1..4`).
- [x] **Shape alpha / gradients:** Optional `"alpha"` (`0.0..1.0`) on shapes/icons/gradients and linear `"op": "gradient"` (`horizontal`/`vertical`).

---

## 3. HTTP API & Client Ergonomics

- [x] **Built-in Web Playground (`GET /ui`):** Self-contained single-page HTML/JS studio served directly by `markee` with live matrix preview, `/draw` JSON editor & presets, drag-and-drop PNG/GIF uploader, alert buttons, brightness slider, and monochrome presets.
- [x] **Client helper CLI (or subcommands):** Built-in `markee say`, `notify`, `gif`, `timer`, `clock`, `alert`, `clear`, and `info` subcommands talk directly to a running `markee` instance with zero external tools.
- [x] **Live event stream (`GET /events` via SSE or WebSocket):** JSON polling (`?since=`, `?wait=`) and Server-Sent Events (`?stream=1` / `Accept: text/event-stream`) stream frame updates, LED clicks, and bezel button presses.
- [x] **Optional API token auth (`--token` / `MARKEE_TOKEN`):** Constant-time Bearer token (`Authorization: Bearer <token>` or `?token=`) authentication returning `401 Unauthorized` when configured for safe LAN binding.
- [x] **Custom alert melodies:** Synthesize arbitrary note sequences (`POST /alert` with `?melody=` or `{"melody":"C5:120,E5:120,G5:240"}`, `R` for rests, up to 32 notes) using the built-in bell/chime synthesizer.
- [x] **GIF recording (`POST /record` or hotkey `R`):** Capture live frames at configurable FPS into a lossless/quantized animated GIF89a via `POST`/`GET`/`DELETE /record` or hotkey `R` (`recording.gif`).

---

## 4. Integrations & Example Applets (`examples/`)

- [x] **AI Agent / MCP Server integration:** Zero-dependency stdio JSON-RPC 2.0 MCP server in [`examples/mcp_server.py`](../examples/mcp_server.py) exposing `markee_info`, `markee_say`, `markee_notify`, `markee_progress`, `markee_draw`, `markee_timer`, `markee_clock`, `markee_alert`, and `markee_clear`.
- [x] **Focus / Pomodoro timer applet:** [`examples/pomodoro.py`](../examples/pomodoro.py) drives work/break cycles with server-side `countdown` + progress bar, final-stretch color shift, end-of-phase chimes/melodies, and bezel/`Space` skip via `GET /events`.
- [x] **Now Playing ticker:** [`examples/now_playing.py`](../examples/now_playing.py) polls macOS Spotify/Apple Music (`osascript`) or Linux MPRIS (`playerctl`), renders a music/pause icon + clipped scrolling track/artist marquee, and extracts the album artwork's highlight color for a Tesla-style ambient LED strip along the bottom edge (`--demo` supported).
- [x] **System monitor applet:** [`examples/sysmon.py`](../examples/sysmon.py) samples real CPU load, memory usage, and network throughput (macOS + Linux) and renders a live `sparkline` + `progress` meter dashboard.
- [x] **CI / Git status watcher:** [`examples/git_watch.py`](../examples/git_watch.py) watches local git repos + optional test commands (`--cmd "make test"`) or GitHub Actions (`--github owner/repo`) and flashes pass/fail banners with alerts.
- [x] **Rotating carousel daemon:** [`examples/carousel.py`](../examples/carousel.py) cycles pluggable cards (`clock`, `date`, `sys`, `git`, `weather`, and custom `--card` slides) with smooth transitions and bezel/`Space` advance via `GET /events`.

---

## 5. Engineering, Testing & Distribution

- [x] **CI workflow (GitHub Actions on macOS & Linux):** [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) builds with `-Wall -Wextra -Wpedantic -Werror`, runs `./test_api`, runs `make sanitize` (ASan + UBSan), and validates `examples/*.py` on `macos-latest` and `ubuntu-latest`.
- [x] **Fuzz testing for C parsers:** LibFuzzer + standalone ASan/UBSan smoke harnesses in [`tests/fuzz/`](../tests/fuzz/) covering `gif.c` (`fuzz_gif.c`), `json.c` (`fuzz_json.c`), `draw.c` + base64/PNG/GIF `image` op (`fuzz_draw.c`), and HTTP request parsing in `server.c` (`fuzz_http.c`), runnable via `make fuzz-smoke` and `make fuzz`.
- [x] **Config file support (`~/.config/markee/config.ini`):** [`include/ini.h`](../include/ini.h) / [`src/ini.c`](../src/ini.c) loads defaults from `$XDG_CONFIG_HOME/markee/config.ini`, `~/.config/markee/config.ini`, or `--config <path>` (`--no-config` to skip), with CLI overrides and `--save-position` / `save_position = true` window position persistence on exit.
- [x] **Packaging & native bundles:**
  - [x] macOS `.app` bundle (`make app`, [`packaging/build-macos-app.sh`](../packaging/build-macos-app.sh)) with `LSUIElement` agent mode, standalone dylib bundling, and LaunchAgent login plist ([`packaging/com.markee.display.plist`](../packaging/com.markee.display.plist)).
  - [x] Homebrew formula ([`packaging/markee.rb`](../packaging/markee.rb)), Debian/Ubuntu `.deb` builder ([`packaging/build-deb.sh`](../packaging/build-deb.sh)), and portable release tarballs (`make tarball`, [`packaging/build-tarball.sh`](../packaging/build-tarball.sh)).
