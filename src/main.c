#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "display.h"
#include "raylib.h"
#include "renderer.h"
#include "server.h"

static atomic_bool g_quit = false;

static void on_signal(int sig) {
    (void)sig;
    atomic_store(&g_quit, true);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n"
           "Simulated 72x16 24-bit RGB LED matrix display (6.35\" wide, 60 Hz)\n"
           "with an HTTP/TCP socket API for PNG and raw RGB frame I/O.\n\n"
           "Options:\n"
           "  -H, --host <ip>     Bind IP address (default: %s)\n"
           "  -p, --port <port>   TCP port to listen on (default: %d, or MARKEE_PORT)\n"
           "      --dpi <value>   Override monitor logical DPI for 6.35\" physical sizing\n"
           "      --blank         Start with a blank display instead of the startup demo\n"
           "      --headless      Run API server only without opening a GUI window\n"
           "  -h, --help          Show this help message\n\n"
           "Keyboard shortcuts (GUI mode):\n"
           "  C                   Clear display\n"
           "  D                   Toggle startup demo animation\n"
           "  S                   Save current 72x16 frame to snapshot.png\n"
           "  + / -               Adjust brightness (+/- 10%%)\n"
           "  Q / ESC             Quit\n",
           prog, MARKEE_DEFAULT_HOST, MARKEE_DEFAULT_PORT);
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
    const char *host = MARKEE_DEFAULT_HOST;
    uint16_t port = MARKEE_DEFAULT_PORT;
    float override_dpi = 0.0f;
    bool start_demo = true;
    bool headless = false;

    const char *env_port = getenv("MARKEE_PORT");
    if (env_port && *env_port) {
        long p = strtol(env_port, NULL, 10);
        if (p >= 0 && p <= 65535) port = (uint16_t)p;
    }

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            long p = strtol(argv[++i], NULL, 10);
            if (p < 0 || p > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (uint16_t)p;
        } else if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc) {
            override_dpi = strtof(argv[++i], NULL);
            if (override_dpi <= 0.0f) {
                fprintf(stderr, "Invalid DPI value: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--blank") == 0) {
            start_demo = false;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
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

    MarkeeDisplay display;
    if (!display_init(&display, start_demo)) {
        fprintf(stderr, "Failed to initialize display state\n");
        return 1;
    }

    MarkeeServer *srv = server_start(&display, host, port);
    if (!srv) {
        fprintf(stderr, "Failed to start API server on %s:%u\n", host, (unsigned)port);
        display_destroy(&display);
        return 1;
    }

    uint16_t actual_port = server_get_port(srv);
    printf("markee API listening on http://%s:%u\n", host, (unsigned)actual_port);
    printf("  POST /frame   Upload 72x16 PNG (or 3456-byte raw RGB24)\n");
    printf("  GET  /frame   Download displayed 72x16 pixels as PNG (?format=raw for RGB24)\n");
    printf("  GET  /info    Device status & capabilities\n");
    fflush(stdout);

    if (headless) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 16666667L}; /* ~60 Hz */
        double t = 0.0;
        while (!atomic_load(&g_quit)) {
            display_tick_demo(&display, t);
            nanosleep(&ts, NULL);
            t += 1.0 / (double)MARKEE_REFRESH_HZ;
        }
    } else {
        MarkeeLayout layout = renderer_init_window(host, actual_port, override_dpi);
        uint8_t frame_buf[MARKEE_FRAME_BYTES];

        while (!WindowShouldClose() && !atomic_load(&g_quit)) {
            if (IsKeyPressed(KEY_Q)) break;
            if (IsKeyPressed(KEY_C)) {
                display_clear(&display, true);
            }
            if (IsKeyPressed(KEY_D)) {
                display_set_demo(&display, !display_is_demo_active(&display));
            }
            if (IsKeyPressed(KEY_S)) {
                save_snapshot_png(&display, "snapshot.png");
            }
            if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
                uint8_t b = display_get_brightness(&display);
                display_set_brightness(&display, b > 90 ? 100 : (uint8_t)(b + 10));
            }
            if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
                uint8_t b = display_get_brightness(&display);
                display_set_brightness(&display, b < 10 ? 0 : (uint8_t)(b - 10));
            }

            display_tick_demo(&display, GetTime());

            uint8_t brightness = 100;
            display_snapshot(&display, frame_buf, &brightness);

            BeginDrawing();
            renderer_draw(&layout, frame_buf, brightness);
            EndDrawing();
        }

        CloseWindow();
    }

    server_stop(srv);
    display_destroy(&display);
    return 0;
}
