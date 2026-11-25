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
- [ ] **Built-in autonomous widgets (`clock`, `countdown`, `progress`, `sparkline`):**
  - Server-side live `clock` and `countdown` timer ops that tick smoothly at 60 Hz without a background client polling every second (great for focus/Pomodoro timers).
  - Compact `progress` bar and `sparkline` primitives for dashboards.
- [ ] **Scene transitions (`transition: "fade"|"slide-left"|"slide-up"|"wipe"`):** Brief 150–300 ms transitions when switching screens or clearing notifications.
- [ ] **Animation playback controls:** Speed multiplier (`?speed=1.5` / `"speed"` on animated GIFs), pause/resume endpoint, and optional scaling modes (`?fit=crop|contain|stretch`) on `/animate`.
- [ ] **Built-in monochrome pixel icon set:** Small named icons (`check`, `cross`, `bell`, `wifi`, `flame`, `play`, `pause`, `battery`, etc.) usable directly in `/draw` without shipping base64 payloads for common glyphs.
- [ ] **Shape alpha / gradients:** Optional `"alpha"` (`0.0..1.0`) on shapes/fills and simple linear gradients.

---

## 3. HTTP API & Client Ergonomics

- [ ] **Built-in Web Playground (`GET /ui`):** Serve a self-contained single-page HTML/JS studio from `markee` to preview the display live, experiment with `/draw` JSON, drag-and-drop PNGs/GIFs, test alert sounds, and tweak brightness/monochrome.
- [ ] **Client helper CLI (or subcommands):** Quick commands like `markee say "BUILD OK" --color green --chime`, `markee gif cat.gif --fill white`, or `markee timer 25m` so shell scripts don't need hand-built JSON, base64, and `curl` flags.
- [x] **Live event stream (`GET /events` via SSE or WebSocket):** JSON polling (`?since=`, `?wait=`) and Server-Sent Events (`?stream=1` / `Accept: text/event-stream`) stream frame updates, LED clicks, and bezel button presses.
- [ ] **Optional API token auth (`--token` / `MARKEE_TOKEN`):** Allow safe LAN binding (`--host 0.0.0.0`) with Bearer token protection.
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
