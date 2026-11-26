#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "cli.h"
#include "config.h"
#include "display.h"
#include "draw.h"
#include "ini.h"
#include "raylib.h"
#include "renderer.h"
#include "server.h"

static atomic_bool g_quit = false;

static void on_signal(int sig) {
    (void)sig;
    atomic_store(&g_quit, true);
}

static bool parse_resolution(const char *s, int *out_w, int *out_h) {
    if (!s || !*s || !out_w || !out_h) return false;
    char *sep = NULL;
    long w = strtol(s, &sep, 10);
    if (sep == s || (*sep != 'x' && *sep != 'X')) return false;
    const char *h_start = sep + 1;
    if (!isdigit((unsigned char)*h_start)) return false;
    char *end = NULL;
    long h = strtol(h_start, &end, 10);
    if (end == h_start || *end != '\0') return false;
    if (w < MARKEE_MIN_DIM || w > MARKEE_MAX_DIM ||
        h < MARKEE_MIN_DIM || h > MARKEE_MAX_DIM) {
        return false;
    }
    *out_w = (int)w;
    *out_h = (int)h;
    return true;
}

static bool parse_zoom(const char *s, float *out_zoom) {
    if (!s || !*s || !out_zoom) return false;
    char *end = NULL;
    float z = strtof(s, &end);
    if (end == s || *end != '\0') return false;
    if (z < MARKEE_MIN_ZOOM || z > MARKEE_MAX_ZOOM) return false;
    *out_zoom = z;
    return true;
}

/* Strict TCP port parser: whole string must be an integer 0..65535 (0 = ephemeral). */
static bool parse_port(const char *s, uint16_t *out_port) {
    if (!s || !*s || !out_port) return false;
    char *end = NULL;
    long p = strtol(s, &end, 10);
    if (end == s || *end != '\0' || p < 0 || p > 65535) return false;
    *out_port = (uint16_t)p;
    return true;
}

static bool parse_dpi(const char *s, float *out_dpi) {
    if (!s || !*s || !out_dpi) return false;
    char *end = NULL;
    float d = strtof(s, &end);
    if (end == s || *end != '\0' || !(d > 0.0f) || d > 10000.0f) return false;
    *out_dpi = d;
    return true;
}

static bool parse_pixel_shape(const char *s, MarkeePixelShape *out_shape) {
    if (!s || !out_shape) return false;
    if (strcmp(s, "square") == 0) {
        *out_shape = MARKEE_PIXEL_SQUARE;
        return true;
    }
    if (strcmp(s, "circle") == 0) {
        *out_shape = MARKEE_PIXEL_CIRCLE;
        return true;
    }
    return false;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n"
           "Simulated 24-bit RGB LED matrix display (default %dx%d, 6.35\" wide, 60 Hz)\n"
           "with an HTTP/TCP socket API for PNG and raw RGB frame I/O.\n\n"
           "Options:\n"
           "  -r, --resolution <WxH>  LED matrix resolution in pixels (default: %dx%d,\n"
           "                          range: %d..%d per axis, e.g. 128x32, 64x64)\n"
           "  -z, --zoom <factor>     Display zoom multiplier (default: %.1f,\n"
           "                          range: %.1f..%.1f, e.g. 0.5, 1.5, 2.0)\n"
           "      --square            Render each LED pixel as a square (default)\n"
           "      --circle            Render each LED pixel as a circle\n"
           "  -s, --shape <style>     Pixel display style: square or circle (default: square)\n"
           "  -m, --monochrome <hex>  Apply BT.709 perceptual monochrome filter (e.g. #00ff00,\n"
           "                          #ffbf00, amber, red, green)\n"
           "  -H, --host <addr>       Bind address: IPv4, IPv6, or hostname (default: %s)\n"
           "  -p, --port <port>       TCP port to listen on (default: %d, or MARKEE_PORT)\n"
           "      --token <secret>    Require Bearer token auth (or MARKEE_TOKEN env var)\n"
           "      --dpi <value>       Override monitor logical DPI for physical sizing\n"
           "      --always-on-top     Keep the GUI window above other windows (alias: --topmost)\n"
           "      --borderless        Floating widget mode: hide titlebar, transparent background,\n"
           "                          click-and-drag enclosure to move (alias: --frameless)\n"
           "      --position <pos>    Initial window position: compass direction (nw, n, ne, w,\n"
           "                          c, e, sw, s, se), edge/corner name (top-left, top, left,\n"
           "                          center, right, bottom-right, ...), or coordinates X,Y\n"
           "                          (negative values inset from right/bottom, e.g. -10,10;\n"
           "                          either axis may be 'center', e.g. center,-20). Alias: --pos\n"
           "      --corner <place>    Alias for named --position anchors (nw, n, ne, w, c, e,\n"
           "                          sw, s, se, top-left, top, left, center, right, bottom, ...)\n"
           "      --case <theme>      Enclosure color theme: white, charcoal, orange, retro\n"
           "                          (default: white)\n"
           "      --idle-dim <sec>    Dim GUI brightness after N seconds with no API writes\n"
           "      --idle-brightness <0..100>  Dimmed brightness level for --idle-dim (default: 20)\n"
           "      --demo              Start with the built-in demo animation (default: blank)\n"
           "      --mute              Disable alert sound playback (alias: --no-audio)\n"
           "      --headless          Run API server only without opening a GUI window\n"
           "      --config <path>     Load defaults from an INI config file (default:\n"
           "                          ~/.config/markee/config.ini if present)\n"
           "      --no-config         Ignore ~/.config/markee/config.ini\n"
           "      --save-position     Save window X,Y position to config file on exit\n"
           "  -h, --help              Show this help message and exit\n\n",
           prog,
           MARKEE_DEFAULT_WIDTH, MARKEE_DEFAULT_HEIGHT,
           MARKEE_DEFAULT_WIDTH, MARKEE_DEFAULT_HEIGHT,
           MARKEE_MIN_DIM, MARKEE_MAX_DIM,
           MARKEE_DEFAULT_ZOOM, MARKEE_MIN_ZOOM, MARKEE_MAX_ZOOM,
           MARKEE_DEFAULT_HOST, MARKEE_DEFAULT_PORT);

    printf("Keyboard shortcuts (GUI mode):\n"
           "  1 / 2 / 3               Preview alert sounds (ding, chime, bell)\n"
           "  C                       Clear display\n"
           "  D                       Toggle built-in demo animation\n"
           "  K                       Cycle enclosure case color theme\n"
           "  M                       Toggle monochrome filter on/off (default amber #ffbf00)\n"
           "  P                       Toggle pixel shape (square / circle)\n"
           "  R                       Start/stop recording animated GIF (saves recording.gif)\n"
           "  S                       Save current frame to snapshot.png\n"
           "  T                       Toggle always-on-top window mode\n"
           "  SPACE / ENTER           Emit an 'action' button event on GET /events\n"
           "  + / -                   Adjust brightness (+/- 10%%)\n"
           "  Q / ESC                 Quit\n\n"
           "Client subcommands (talk to a running markee server):\n"
           "  %s say <text> [--color C] [--bg C] [--scroll] [--chime|--ding|--bell]\n"
           "  %s notify <text> [--ttl S] [--color C] [--border C] [--sound S]\n"
           "  %s gif <file.gif> [--fill C] [--fit crop|contain|stretch] [--speed S] [--loop N]\n"
           "  %s timer <duration> [--color C] [--sound S]   # e.g. 25m, 90s, 1h30m\n"
           "  %s clock [--color C] [--format 24h|24hm|12h|12hm]\n"
           "  %s alert [ding|chime|bell] [--volume 0..100]\n"
           "  %s clear | info\n\n"
           "Examples:\n"
           "  %s                           # Default 72x16 square LEDs at 1.0x zoom\n"
           "  %s --circle --zoom 2         # 72x16 circular LEDs at 2x size\n"
           "  %s -m \"#ffbf00\"              # Monochrome amber phosphor filter\n"
           "  %s -r 128x32 -z 0.75         # Custom 128x32 LED matrix at 0.75x zoom\n",
           prog, prog, prog, prog, prog, prog, prog,
           prog, prog, prog, prog);
}

static void save_snapshot_png(MarkeeDisplay *disp, const char *filename) {
    uint8_t *png = NULL;
    size_t len = 0;
    if (display_encode_png(disp, &png, &len) != MARKEE_OK) {
        fprintf(stderr, "Failed to encode snapshot PNG\n");
        return;
    }
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen");
        display_free_buffer(png);
        return;
    }
    if (fwrite(png, 1, len, fp) == len) {
        printf("Saved %s (%zu bytes)\n", filename, len);
    } else {
        fprintf(stderr, "Failed writing %s\n", filename);
    }
    fclose(fp);
    display_free_buffer(png);
}

int main(int argc, char **argv) {
    if (argc >= 2 && cli_is_subcommand(argv[1])) {
        signal(SIGPIPE, SIG_IGN);
        return cli_run(argc, argv);
    }

    MarkeeConfig cfg;
    markee_config_defaults(&cfg);

    /* Pre-scan for --config / --no-config so config file defaults apply before CLI overrides */
    const char *explicit_config = NULL;
    bool skip_config = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-config") == 0) {
            skip_config = true;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            explicit_config = argv[++i];
        }
    }

    char active_config_path[1024] = {0};
    if (!skip_config) {
        if (explicit_config) {
            snprintf(active_config_path, sizeof(active_config_path), "%s", explicit_config);
            char cerr[256] = {0};
            if (!markee_config_load_file(active_config_path, &cfg, NULL, cerr, sizeof(cerr))) {
                fprintf(stderr, "Config error (%s): %s\n", active_config_path, cerr);
                return 1;
            }
        } else if (markee_config_default_path(active_config_path, sizeof(active_config_path))) {
            bool not_found = false;
            char cerr[256] = {0};
            if (!markee_config_load_file(active_config_path, &cfg, &not_found, cerr, sizeof(cerr)) &&
                !not_found) {
                fprintf(stderr, "Warning: ignoring %s (%s)\n", active_config_path, cerr);
            }
        }
    }

    int width = cfg.width;
    int height = cfg.height;
    float zoom = cfg.zoom;
    MarkeePixelShape pixel_shape = cfg.pixel_shape;
    bool mono_enabled = cfg.mono_enabled;
    MarkeeColor mono_color = cfg.mono_color;
    const char *host = cfg.host;
    uint16_t port = cfg.port;
    const char *auth_token = cfg.token[0] ? cfg.token : NULL;
    float override_dpi = cfg.override_dpi;
    bool always_on_top = cfg.always_on_top;
    bool borderless = cfg.borderless;
    bool has_position = cfg.has_position;
    bool center_x = cfg.center_x;
    bool center_y = cfg.center_y;
    bool from_right = cfg.from_right;
    bool from_bottom = cfg.from_bottom;
    int pos_x = cfg.pos_x, pos_y = cfg.pos_y;
    MarkeeWindowCorner corner = cfg.corner;
    MarkeeCaseTheme case_theme = cfg.case_theme;
    double idle_dim_sec = cfg.idle_dim_sec;
    uint8_t idle_brightness = cfg.idle_brightness;
    bool start_demo = cfg.start_demo;
    bool mute = cfg.mute;
    bool headless = cfg.headless;
    bool save_position = cfg.save_position;

    const char *env_token = getenv("MARKEE_TOKEN");
    if (env_token && *env_token) auth_token = env_token;

    const char *env_port = getenv("MARKEE_PORT");
    if (env_port && *env_port && !parse_port(env_port, &port)) {
        fprintf(stderr, "Invalid MARKEE_PORT '%s' (expected an integer 0..65535)\n", env_port);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resolution") == 0) && i + 1 < argc) {
            if (!parse_resolution(argv[++i], &width, &height)) {
                fprintf(stderr, "Invalid resolution '%s' (expected WxH, each %d..%d)\n",
                        argv[i], MARKEE_MIN_DIM, MARKEE_MAX_DIM);
                return 1;
            }
        } else if ((strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--zoom") == 0) && i + 1 < argc) {
            if (!parse_zoom(argv[++i], &zoom)) {
                fprintf(stderr, "Invalid zoom '%s' (expected %.1f..%.1f)\n",
                        argv[i], MARKEE_MIN_ZOOM, MARKEE_MAX_ZOOM);
                return 1;
            }
        } else if (strcmp(argv[i], "--square") == 0) {
            pixel_shape = MARKEE_PIXEL_SQUARE;
        } else if (strcmp(argv[i], "--circle") == 0) {
            pixel_shape = MARKEE_PIXEL_CIRCLE;
        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--shape") == 0) && i + 1 < argc) {
            if (!parse_pixel_shape(argv[++i], &pixel_shape)) {
                fprintf(stderr, "Invalid pixel shape '%s' (expected 'square' or 'circle')\n",
                        argv[i]);
                return 1;
            }
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monochrome") == 0 ||
                    strcmp(argv[i], "--mono") == 0) && i + 1 < argc) {
            if (!draw_parse_color_string(argv[++i], &mono_color)) {
                fprintf(stderr, "Invalid monochrome color '%s' (expected #rrggbb or color name)\n",
                        argv[i]);
                return 1;
            }
            mono_enabled = true;
        } else if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            if (!parse_port(argv[++i], &port)) {
                fprintf(stderr, "Invalid port '%s' (expected an integer 0..65535)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            auth_token = argv[++i];
        } else if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc) {
            if (!parse_dpi(argv[++i], &override_dpi)) {
                fprintf(stderr, "Invalid DPI value '%s' (expected a positive number)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--always-on-top") == 0 || strcmp(argv[i], "--topmost") == 0) {
            always_on_top = true;
        } else if (strcmp(argv[i], "--borderless") == 0 || strcmp(argv[i], "--frameless") == 0) {
            borderless = true;
        } else if (((strcmp(argv[i], "--position") == 0 || strcmp(argv[i], "--pos") == 0) && i + 1 < argc) ||
                   strncmp(argv[i], "--position=", 11) == 0 || strncmp(argv[i], "--pos=", 6) == 0) {
            const char *pval = (strncmp(argv[i], "--position=", 11) == 0)
                                   ? (argv[i] + 11)
                                   : ((strncmp(argv[i], "--pos=", 6) == 0) ? (argv[i] + 6) : argv[++i]);
            MarkeeWindowOpts parsed_pos = {0};
            if (!renderer_parse_position(pval, &parsed_pos)) {
                fprintf(stderr, "Invalid position '%s' (expected nw/n/ne/w/c/e/sw/s/se, top-left/..., or X,Y)\n",
                        pval);
                return 1;
            }
            has_position = parsed_pos.has_position;
            center_x = parsed_pos.center_x;
            center_y = parsed_pos.center_y;
            from_right = parsed_pos.from_right;
            from_bottom = parsed_pos.from_bottom;
            pos_x = parsed_pos.pos_x;
            pos_y = parsed_pos.pos_y;
            if (!parsed_pos.has_position) corner = parsed_pos.corner;
        } else if (strcmp(argv[i], "--corner") == 0 && i + 1 < argc) {
            if (!renderer_parse_corner(argv[++i], &corner)) {
                fprintf(stderr, "Invalid corner '%s' (expected nw, n, ne, w, c, e, sw, s, se, top-left, top, left, center, right, bottom, ...)\n",
                        argv[i]);
                return 1;
            }
            has_position = false;
            center_x = false;
            center_y = false;
            from_right = false;
            from_bottom = false;
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            if (!renderer_parse_case_theme(argv[++i], &case_theme)) {
                fprintf(stderr, "Invalid case theme '%s' (expected white, charcoal, orange, or retro)\n",
                        argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--idle-dim") == 0 && i + 1 < argc) {
            char *end = NULL;
            double sec = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || sec < 0.0 || sec > 86400.0) {
                fprintf(stderr, "Invalid --idle-dim '%s' (expected seconds 0..86400)\n", argv[i]);
                return 1;
            }
            idle_dim_sec = sec;
        } else if (strcmp(argv[i], "--idle-brightness") == 0 && i + 1 < argc) {
            char *end = NULL;
            long b = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || b < 0 || b > 100) {
                fprintf(stderr, "Invalid --idle-brightness '%s' (expected integer 0..100)\n", argv[i]);
                return 1;
            }
            idle_brightness = (uint8_t)b;
        } else if (strcmp(argv[i], "--demo") == 0) {
            start_demo = true;
        } else if (strcmp(argv[i], "--blank") == 0) {
            start_demo = false;
        } else if (strcmp(argv[i], "--mute") == 0 || strcmp(argv[i], "--no-audio") == 0) {
            mute = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            i++; /* already loaded in pre-scan */
        } else if (strcmp(argv[i], "--no-config") == 0) {
            /* already handled in pre-scan */
        } else if (strcmp(argv[i], "--save-position") == 0) {
            save_position = true;
        } else if (strcmp(argv[i], "--no-always-on-top") == 0) {
            always_on_top = false;
        } else if (strcmp(argv[i], "--no-borderless") == 0) {
            borderless = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    SetTraceLogLevel(LOG_WARNING);

    MarkeeDisplay display;
    if (!display_init(&display, width, height, start_demo)) {
        fprintf(stderr, "Failed to initialize %dx%d display state\n", width, height);
        return 1;
    }
    if (mono_enabled) {
        display_set_monochrome(&display, true, mono_color.r, mono_color.g, mono_color.b);
    }
    if (idle_dim_sec > 0.0) {
        display_set_idle_dim(&display, idle_dim_sec, idle_brightness);
    }

    MarkeeAudio *audio = audio_create(!mute);
    if (!audio) {
        fprintf(stderr, "Failed to initialize audio state\n");
        display_destroy(&display);
        return 1;
    }

    MarkeeServer *srv = server_start_ex(&display, audio, host, port);
    if (!srv) {
        fprintf(stderr, "Failed to start API server on %s:%u\n", host, (unsigned)port);
        audio_destroy(audio);
        display_destroy(&display);
        return 1;
    }
    if (auth_token && *auth_token) {
        server_set_token(srv, auth_token);
    }

    uint16_t actual_port = server_get_port(srv);
    const bool is_v6 = strchr(host, ':') != NULL;
    const char *lb = is_v6 ? "[" : "";
    const char *rb = is_v6 ? "]" : "";
    if (mono_enabled) {
        printf("markee (%dx%d, zoom=%.2fx, pixels=%s, monochrome=#%02x%02x%02x) API listening on http://%s%s%s:%u\n",
               width, height, zoom,
               pixel_shape == MARKEE_PIXEL_CIRCLE ? "circle" : "square",
               mono_color.r, mono_color.g, mono_color.b,
               lb, host, rb, (unsigned)actual_port);
    } else {
        printf("markee (%dx%d, zoom=%.2fx, pixels=%s) API listening on http://%s%s%s:%u\n",
               width, height, zoom,
               pixel_shape == MARKEE_PIXEL_CIRCLE ? "circle" : "square",
               lb, host, rb, (unsigned)actual_port);
    }
    printf("  POST /frame   Upload %dx%d PNG (or %zu-byte raw RGB24)\n",
           width, height, display.frame_bytes);
    printf("  GET  /frame   Download displayed %dx%d pixels as PNG (?format=raw for RGB24)\n",
           width, height);
    printf("  POST /animate Upload animated GIF (auto-cropped/centered to %dx%d)\n",
           width, height);
    printf("  POST /draw    Draw JSON ops (clear, pixel, line, rect, oval, polygon, text, image)\n");
    printf("  POST /alert   Play alert sound (/alert/1..3 or ding, chime, bell; GET /alert to list)\n");
    printf("  GET  /info    Device status & capabilities\n");
    printf("  GET  /ui      Built-in Web Playground (open in a browser)\n");
    fflush(stdout);

    if (headless) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16666667L}; /* ~60 Hz */
        double t = 0.0;
        while (!atomic_load(&g_quit)) {
            display_tick(&display, t);
            int pending_snd = display_take_pending_alert(&display);
            if (pending_snd > 0) audio_trigger(audio, pending_snd, 100);
            audio_tick(audio);
            nanosleep(&ts, NULL);
            t += 1.0 / (double)MARKEE_REFRESH_HZ;
        }
    } else {
        uint8_t *frame_buf = malloc(display.frame_bytes);
        if (!frame_buf) {
            fprintf(stderr, "Failed to allocate render buffer\n");
            server_stop(srv);
            audio_destroy(audio);
            display_destroy(&display);
            return 1;
        }

        MarkeeWindowOpts win_opts = {
            .always_on_top = always_on_top,
            .borderless = borderless,
            .has_position = has_position,
            .center_x = center_x,
            .center_y = center_y,
            .from_right = from_right,
            .from_bottom = from_bottom,
            .pos_x = pos_x,
            .pos_y = pos_y,
            .corner = corner,
            .case_theme = case_theme
        };
        MarkeeLayout layout = renderer_init_window_ex(width, height, zoom, pixel_shape,
                                                      host, actual_port, override_dpi,
                                                      &win_opts);

        while (!WindowShouldClose() && !atomic_load(&g_quit)) {
            if (IsKeyPressed(KEY_Q)) break;
            renderer_handle_drag(&layout);
            {
                int cx = -1, cy = -1;
                const char *cbtn = NULL;
                int hit = renderer_poll_click(&layout, &cx, &cy, &cbtn);
                if (hit == 1) {
                    display_push_event(&display, MARKEE_EVT_CLICK, cx, cy, cbtn);
                } else if (hit == 2) {
                    display_push_event(&display, MARKEE_EVT_BUTTON, -1, -1, "bezel");
                }
            }
            if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                display_push_event(&display, MARKEE_EVT_BUTTON, -1, -1, "action");
            }
            if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_KP_1)) {
                audio_trigger(audio, 1, 100);
            }
            if (IsKeyPressed(KEY_TWO) || IsKeyPressed(KEY_KP_2)) {
                audio_trigger(audio, 2, 100);
            }
            if (IsKeyPressed(KEY_THREE) || IsKeyPressed(KEY_KP_3)) {
                audio_trigger(audio, 3, 100);
            }
            if (IsKeyPressed(KEY_C)) {
                display_clear(&display, true);
            }
            if (IsKeyPressed(KEY_D)) {
                display_set_demo(&display, !display_is_demo_active(&display));
            }
            if (IsKeyPressed(KEY_K)) {
                layout.case_theme = (MarkeeCaseTheme)((layout.case_theme + 1) % MARKEE_CASE_COUNT);
            }
            if (IsKeyPressed(KEY_M)) {
                bool en = false;
                uint8_t mr = 0, mg = 0, mb = 0;
                display_get_monochrome(&display, &en, &mr, &mg, &mb);
                if (en) {
                    display_set_monochrome(&display, false, 0, 0, 0);
                } else {
                    if (mr == 0 && mg == 0 && mb == 0) {
                        mr = mono_color.r;
                        mg = mono_color.g;
                        mb = mono_color.b;
                    }
                    display_set_monochrome(&display, true, mr, mg, mb);
                }
            }
            if (IsKeyPressed(KEY_P)) {
                layout.pixel_shape = (layout.pixel_shape == MARKEE_PIXEL_SQUARE)
                                         ? MARKEE_PIXEL_CIRCLE
                                         : MARKEE_PIXEL_SQUARE;
            }
            if (IsKeyPressed(KEY_R)) {
                bool rec_active = false;
                display_get_record_status(&display, &rec_active, NULL, NULL, NULL);
                if (!rec_active) {
                    display_record_start(&display, 15, 300);
                    printf("Recording started (press R again to save recording.gif)\n");
                    fflush(stdout);
                } else {
                    uint8_t *gif = NULL;
                    size_t glen = 0;
                    if (display_record_stop(&display, &gif, &glen)) {
                        FILE *fp = fopen("recording.gif", "wb");
                        if (fp) {
                            fwrite(gif, 1, glen, fp);
                            fclose(fp);
                            printf("Saved recording.gif (%zu bytes)\n", glen);
                        }
                        free(gif);
                    }
                    fflush(stdout);
                }
            }
            if (IsKeyPressed(KEY_S)) {
                save_snapshot_png(&display, "snapshot.png");
            }
            if (IsKeyPressed(KEY_T)) {
                always_on_top = !always_on_top;
                renderer_set_always_on_top(always_on_top);
            }
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
                uint8_t b = display_get_brightness(&display);
                display_set_brightness(&display, b > 90 ? 100 : (uint8_t)(b + 10));
            }
            if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
                uint8_t b = display_get_brightness(&display);
                display_set_brightness(&display, b < 10 ? 0 : (uint8_t)(b - 10));
            }

            display_tick(&display, GetTime());
            {
                int pending_snd = display_take_pending_alert(&display);
                if (pending_snd > 0) audio_trigger(audio, pending_snd, 100);
            }
            audio_tick(audio);

            uint8_t brightness = 100;
            display_snapshot(&display, frame_buf, display.frame_bytes, &brightness);

            BeginDrawing();
            renderer_draw(&layout, frame_buf, brightness);
            EndDrawing();
        }

        if (save_position && active_config_path[0]) {
            Vector2 wp = GetWindowPosition();
            int sx = (int)wp.x + (borderless ? (int)(layout.body_x + 0.5f) : 0);
            int sy = (int)wp.y + (borderless ? (int)(layout.body_y + 0.5f) : 0);
            if (!markee_config_save_position(active_config_path, sx, sy)) {
                fprintf(stderr, "Warning: failed to save window position to %s\n", active_config_path);
            }
        }

        CloseWindow();
        free(frame_buf);
    }

    server_stop(srv);
    audio_destroy(audio);
    display_destroy(&display);
    return 0;
}
