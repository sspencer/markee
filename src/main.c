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
#include "config.h"
#include "display.h"
#include "draw.h"
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

static bool parse_position(const char *s, int *out_x, int *out_y) {
    if (!s || !*s || !out_x || !out_y) return false;
    char *sep = NULL;
    long x = strtol(s, &sep, 10);
    if (sep == s || *sep != ',') return false;
    const char *y_start = sep + 1;
    if (*y_start == '\0') return false;
    char *end = NULL;
    long y = strtol(y_start, &end, 10);
    if (end == y_start || *end != '\0') return false;
    if (x < -16384 || x > 16384 || y < -16384 || y > 16384) return false;
    *out_x = (int)x;
    *out_y = (int)y;
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
           "      --dpi <value>       Override monitor logical DPI for physical sizing\n"
           "      --always-on-top     Keep the GUI window above other windows (alias: --topmost)\n"
           "      --borderless        Floating widget mode: hide titlebar, transparent background,\n"
           "                          click-and-drag enclosure to move (alias: --frameless)\n"
           "      --position <X,Y>    Initial window position in screen coordinates (alias: --pos)\n"
           "      --corner <place>    Snap initial window to monitor corner/edge: center,\n"
           "                          top-left (tl), top-right (tr), bottom-left (bl),\n"
           "                          bottom-right (br), top, bottom\n"
           "      --case <theme>      Enclosure color theme: white, charcoal, orange, retro\n"
           "                          (default: white)\n"
           "      --idle-dim <sec>    Dim GUI brightness after N seconds with no API writes\n"
           "      --idle-brightness <0..100>  Dimmed brightness level for --idle-dim (default: 20)\n"
           "      --demo              Start with the built-in demo animation (default: blank)\n"
           "      --mute              Disable alert sound playback (alias: --no-audio)\n"
           "      --headless          Run API server only without opening a GUI window\n"
           "  -h, --help              Show this help message and exit\n\n"
           "Keyboard shortcuts (GUI mode):\n"
           "  1 / 2 / 3               Preview alert sounds (ding, chime, bell)\n"
           "  C                       Clear display\n"
           "  D                       Toggle built-in demo animation\n"
           "  K                       Cycle enclosure case color theme\n"
           "  M                       Toggle monochrome filter on/off (default amber #ffbf00)\n"
           "  P                       Toggle pixel shape (square / circle)\n"
           "  S                       Save current frame to snapshot.png\n"
           "  T                       Toggle always-on-top window mode\n"
           "  SPACE / ENTER           Emit an 'action' button event on GET /events\n"
           "  + / -                   Adjust brightness (+/- 10%%)\n"
           "  Q / ESC                 Quit\n\n"
           "Examples:\n"
           "  %s                           # Default 72x16 square LEDs at 1.0x zoom\n"
           "  %s --circle --zoom 2         # 72x16 circular LEDs at 2x size\n"
           "  %s -m \"#ffbf00\"              # Monochrome amber phosphor filter\n"
           "  %s -r 128x32 -z 0.75         # Custom 128x32 LED matrix at 0.75x zoom\n",
           prog,
           MARKEE_DEFAULT_WIDTH, MARKEE_DEFAULT_HEIGHT,
           MARKEE_DEFAULT_WIDTH, MARKEE_DEFAULT_HEIGHT,
           MARKEE_MIN_DIM, MARKEE_MAX_DIM,
           MARKEE_DEFAULT_ZOOM, MARKEE_MIN_ZOOM, MARKEE_MAX_ZOOM,
           MARKEE_DEFAULT_HOST, MARKEE_DEFAULT_PORT,
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
    int width = MARKEE_DEFAULT_WIDTH;
    int height = MARKEE_DEFAULT_HEIGHT;
    float zoom = MARKEE_DEFAULT_ZOOM;
    MarkeePixelShape pixel_shape = MARKEE_PIXEL_SQUARE;
    bool mono_enabled = false;
    MarkeeColor mono_color = {255, 191, 0}; /* Default amber if toggled with 'M' */
    const char *host = MARKEE_DEFAULT_HOST;
    uint16_t port = MARKEE_DEFAULT_PORT;
    float override_dpi = 0.0f;
    bool always_on_top = false;
    bool borderless = false;
    bool has_position = false;
    int pos_x = 0, pos_y = 0;
    MarkeeWindowCorner corner = MARKEE_CORNER_CENTER;
    MarkeeCaseTheme case_theme = MARKEE_CASE_WHITE;
    double idle_dim_sec = 0.0;
    uint8_t idle_brightness = 20;
    bool start_demo = false;
    bool mute = false;
    bool headless = false;

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
        } else if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc) {
            if (!parse_dpi(argv[++i], &override_dpi)) {
                fprintf(stderr, "Invalid DPI value '%s' (expected a positive number)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--always-on-top") == 0 || strcmp(argv[i], "--topmost") == 0) {
            always_on_top = true;
        } else if (strcmp(argv[i], "--borderless") == 0 || strcmp(argv[i], "--frameless") == 0) {
            borderless = true;
        } else if ((strcmp(argv[i], "--position") == 0 || strcmp(argv[i], "--pos") == 0) && i + 1 < argc) {
            if (!parse_position(argv[++i], &pos_x, &pos_y)) {
                fprintf(stderr, "Invalid position '%s' (expected X,Y integers)\n", argv[i]);
                return 1;
            }
            has_position = true;
        } else if (strcmp(argv[i], "--corner") == 0 && i + 1 < argc) {
            if (!renderer_parse_corner(argv[++i], &corner)) {
                fprintf(stderr, "Invalid corner '%s' (expected center, top-left, top-right, bottom-left, bottom-right, top, or bottom)\n",
                        argv[i]);
                return 1;
            }
            has_position = false;
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

        CloseWindow();
        free(frame_buf);
    }

    server_stop(srv);
    audio_destroy(audio);
    display_destroy(&display);
    return 0;
}
