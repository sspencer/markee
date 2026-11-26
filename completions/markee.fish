# fish completion for markee

set -l __markee_subcommands say notify gif timer clock alert clear info
set -l __markee_positions nw n ne w c e sw s se center top-left top top-right left right bottom-left bottom bottom-right
set -l __markee_colors white warmwhite red green blue cyan magenta yellow amber orange purple pink black
set -l __markee_cases white charcoal orange retro
set -l __markee_sounds ding chime bell
set -l __markee_fonts 4x6 5x7 6x8 8x8 prop
set -l __markee_fits crop contain stretch
set -l __markee_transitions fade slide-left slide-up wipe
set -l __markee_formats 24h 24hm 12h 12hm

# Disable default file completions except where explicitly enabled
complete -c markee -f

# Top-level subcommands
complete -c markee -n "__fish_use_subcommand" -a say -d "Display text on a running markee server"
complete -c markee -n "__fish_use_subcommand" -a notify -d "Show a transient notification banner with TTL"
complete -c markee -n "__fish_use_subcommand" -a gif -d "Play an animated GIF on a running markee server"
complete -c markee -n "__fish_use_subcommand" -a timer -d "Start a live countdown timer widget"
complete -c markee -n "__fish_use_subcommand" -a clock -d "Display the built-in digital clock widget"
complete -c markee -n "__fish_use_subcommand" -a alert -d "Play a synthesized alert sound"
complete -c markee -n "__fish_use_subcommand" -a clear -d "Clear the LED matrix to black"
complete -c markee -n "__fish_use_subcommand" -a info -d "Show server status and capabilities JSON"

# Server flags (when no client subcommand is active)
set -l __markee_no_sub "not __fish_seen_subcommand_from $__markee_subcommands"
complete -c markee -n "$__markee_no_sub" -s r -l resolution -x -a "72x16 128x32 64x32 64x64 32x8" -d "LED matrix resolution WxH"
complete -c markee -n "$__markee_no_sub" -s z -l zoom -x -a "0.5 0.75 1.0 1.25 1.5 2.0" -d "Window zoom multiplier"
complete -c markee -n "$__markee_no_sub" -l square -d "Render LEDs as squares"
complete -c markee -n "$__markee_no_sub" -l circle -d "Render LEDs as circles"
complete -c markee -n "$__markee_no_sub" -s s -l shape -x -a "square circle" -d "Pixel shape"
complete -c markee -n "$__markee_no_sub" -s m -l monochrome -l mono -x -a "$__markee_colors" -d "Monochrome phosphor tint color"
complete -c markee -n "$__markee_no_sub" -s H -l host -x -d "Bind address (default 127.0.0.1)"
complete -c markee -n "$__markee_no_sub" -s p -l port -x -d "TCP listen port (default 8080)"
complete -c markee -n "$__markee_no_sub" -l token -x -d "Require Bearer auth token"
complete -c markee -n "$__markee_no_sub" -l dpi -x -d "Override monitor logical DPI"
complete -c markee -n "$__markee_no_sub" -l always-on-top -l topmost -d "Keep window above other windows"
complete -c markee -n "$__markee_no_sub" -l no-always-on-top -d "Disable always-on-top"
complete -c markee -n "$__markee_no_sub" -l borderless -l frameless -d "Floating borderless widget mode"
complete -c markee -n "$__markee_no_sub" -l no-borderless -d "Disable borderless mode"
complete -c markee -n "$__markee_no_sub" -l position -l pos -x -a "$__markee_positions" -d "Window placement (compass, anchor, or X,Y)"
complete -c markee -n "$__markee_no_sub" -l corner -x -a "$__markee_positions" -d "Named window anchor"
complete -c markee -n "$__markee_no_sub" -l case -x -a "$__markee_cases" -d "Enclosure color theme"
complete -c markee -n "$__markee_no_sub" -l idle-dim -x -d "Dim brightness after N idle seconds"
complete -c markee -n "$__markee_no_sub" -l idle-brightness -x -d "Dimmed brightness level (0..100)"
complete -c markee -n "$__markee_no_sub" -l demo -d "Start with built-in demo animation"
complete -c markee -n "$__markee_no_sub" -l blank -d "Start with blank display"
complete -c markee -n "$__markee_no_sub" -l mute -l no-audio -d "Disable alert sound playback"
complete -c markee -n "$__markee_no_sub" -l headless -d "Run API server without GUI window"
complete -c markee -n "$__markee_no_sub" -l config -r -F -d "Load INI configuration file"
complete -c markee -n "$__markee_no_sub" -l no-config -d "Skip default config file"
complete -c markee -n "$__markee_no_sub" -l save-position -d "Save window position to config on exit"
complete -c markee -n "$__markee_no_sub" -s h -l help -d "Show usage and exit"

# Shared client subcommand connection flags
complete -c markee -n "__fish_seen_subcommand_from $__markee_subcommands" -s H -l host -x -d "Server host"
complete -c markee -n "__fish_seen_subcommand_from $__markee_subcommands" -s p -l port -x -d "Server TCP port"
complete -c markee -n "__fish_seen_subcommand_from $__markee_subcommands" -l token -x -d "Bearer auth token"

# say
complete -c markee -n "__fish_seen_subcommand_from say" -s c -l color -x -a "$__markee_colors" -d "Text color"
complete -c markee -n "__fish_seen_subcommand_from say" -l bg -x -a "$__markee_colors" -d "Background color"
complete -c markee -n "__fish_seen_subcommand_from say" -l font -x -a "$__markee_fonts" -d "Bitmap font"
complete -c markee -n "__fish_seen_subcommand_from say" -l scroll -d "Scroll text horizontally"
complete -c markee -n "__fish_seen_subcommand_from say" -l speed -x -d "Scroll speed (px/s)"
complete -c markee -n "__fish_seen_subcommand_from say" -l transition -x -a "$__markee_transitions" -d "Transition effect"
complete -c markee -n "__fish_seen_subcommand_from say" -l sound -x -a "$__markee_sounds" -d "Alert sound"
complete -c markee -n "__fish_seen_subcommand_from say" -l ding -d "Play ding sound"
complete -c markee -n "__fish_seen_subcommand_from say" -l chime -d "Play chime sound"
complete -c markee -n "__fish_seen_subcommand_from say" -l bell -d "Play bell sound"
complete -c markee -n "__fish_seen_subcommand_from say" -l volume -x -d "Alert volume (0..100)"

# notify
complete -c markee -n "__fish_seen_subcommand_from notify" -l ttl -x -d "Duration in seconds"
complete -c markee -n "__fish_seen_subcommand_from notify" -s c -l color -x -a "$__markee_colors" -d "Text color"
complete -c markee -n "__fish_seen_subcommand_from notify" -l bg -x -a "$__markee_colors" -d "Background color"
complete -c markee -n "__fish_seen_subcommand_from notify" -l border -x -a "$__markee_colors" -d "Border color"
complete -c markee -n "__fish_seen_subcommand_from notify" -l font -x -a "$__markee_fonts" -d "Bitmap font"
complete -c markee -n "__fish_seen_subcommand_from notify" -l sound -x -a "$__markee_sounds" -d "Alert sound"
complete -c markee -n "__fish_seen_subcommand_from notify" -l ding -d "Play ding sound"
complete -c markee -n "__fish_seen_subcommand_from notify" -l chime -d "Play chime sound"
complete -c markee -n "__fish_seen_subcommand_from notify" -l bell -d "Play bell sound"

# gif (allow file completion)
complete -c markee -n "__fish_seen_subcommand_from gif" -F
complete -c markee -n "__fish_seen_subcommand_from gif" -l fill -x -a "$__markee_colors" -d "Transparent background fill color"
complete -c markee -n "__fish_seen_subcommand_from gif" -l fit -x -a "$__markee_fits" -d "Scaling mode"
complete -c markee -n "__fish_seen_subcommand_from gif" -l speed -x -d "Playback speed multiplier"
complete -c markee -n "__fish_seen_subcommand_from gif" -l loop -x -d "Loop count (0 = infinite)"

# timer
complete -c markee -n "__fish_seen_subcommand_from timer" -a "30s 1m 5m 10m 15m 25m 1h" -d "Countdown duration"
complete -c markee -n "__fish_seen_subcommand_from timer" -s c -l color -x -a "$__markee_colors" -d "Timer color"
complete -c markee -n "__fish_seen_subcommand_from timer" -l bg -x -a "$__markee_colors" -d "Background color"
complete -c markee -n "__fish_seen_subcommand_from timer" -l font -x -a "$__markee_fonts" -d "Bitmap font"
complete -c markee -n "__fish_seen_subcommand_from timer" -l no-bar -d "Hide bottom progress bar"
complete -c markee -n "__fish_seen_subcommand_from timer" -l sound -x -a "$__markee_sounds" -d "Completion alert sound"
complete -c markee -n "__fish_seen_subcommand_from timer" -l ding -d "Play ding on finish"
complete -c markee -n "__fish_seen_subcommand_from timer" -l chime -d "Play chime on finish"
complete -c markee -n "__fish_seen_subcommand_from timer" -l bell -d "Play bell on finish"

# clock
complete -c markee -n "__fish_seen_subcommand_from clock" -s c -l color -x -a "$__markee_colors" -d "Clock color"
complete -c markee -n "__fish_seen_subcommand_from clock" -l bg -x -a "$__markee_colors" -d "Background color"
complete -c markee -n "__fish_seen_subcommand_from clock" -l font -x -a "$__markee_fonts" -d "Bitmap font"
complete -c markee -n "__fish_seen_subcommand_from clock" -l format -x -a "$__markee_formats" -d "Clock format"

# alert
complete -c markee -n "__fish_seen_subcommand_from alert" -a "$__markee_sounds" -d "Built-in alert sound"
complete -c markee -n "__fish_seen_subcommand_from alert" -l volume -x -d "Volume (0..100)"
complete -c markee -n "__fish_seen_subcommand_from alert" -l ding -d "Play ding sound"
complete -c markee -n "__fish_seen_subcommand_from alert" -l chime -d "Play chime sound"
complete -c markee -n "__fish_seen_subcommand_from alert" -l bell -d "Play bell sound"
