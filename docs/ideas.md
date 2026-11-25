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
- [ ] **Custom alert melodies & audio clips:** Allow simple note-sequence alerts (e.g., `"C5:120,E5:120,G5:240"`) or short uploaded WAV/PCM sounds in `POST /alert`.
- [ ] **GIF recording (`POST /record` or hotkey `R`):** Capture a live sequence of displayed frames directly to an animated `.gif` for sharing demos, docs, and bug reports.

---

## 4. Integrations & Example Applets (`examples/`)

- [ ] **AI Agent / MCP Server integration:** Expose `markee` as an MCP tool so coding agents can show task progress, build results, and trigger a gentle chime + animation when long runs finish.
- [ ] **Focus / Pomodoro timer applet:** Countdown display, progress bar, color shift, and end-of-session chime.
- [ ] **Now Playing ticker:** Show current track/artist from macOS Now Playing / Spotify / MPRIS with an animated equalizer or icon.
- [ ] **System monitor applet:** Live CPU, memory, and network sparklines/meters.
- [ ] **CI / Git status watcher:** Watch GitHub Actions / local test runs and flash pass/fail banners with alerts.
- [ ] **Rotating carousel daemon:** Cycle through pluggable cards (clock, weather icon + temp, calendar next meeting, stocks/metrics) on a timer.

---

## 5. Engineering, Testing & Distribution

- [ ] **CI workflow (GitHub Actions on macOS & Linux):** Build cleanly with `-Wall -Wextra -Wpedantic`, run `make test`, and run an AddressSanitizer + UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`) pass on every commit/PR.
- [ ] **Fuzz testing for C parsers:** Add libFuzzer harnesses for [gif.c](../src/gif.c), [json.c](../src/json.c), base64 decoding in [draw.c](../src/draw.c), and HTTP request parsing in [server.c](../src/server.c).
- [ ] **Config file support (`~/.config/markee/config.ini`):** Persist default zoom, pixel shape, monochrome tint, port, and widget flags without repeating CLI arguments.
- [ ] **Packaging & native bundles:**
  - macOS `.app` bundle (with optional menu bar tray icon and "launch at login").
  - Homebrew formula (`brew install ...`) and Linux `.deb` / release tarballs.
