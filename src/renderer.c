#include "renderer.h"

#include <math.h>
#include <stdio.h>

#include "raylib.h"

MarkeeLayout renderer_compute_layout(float override_dpi, int monitor_w, int monitor_phys_w_mm) {
    MarkeeLayout L = {0};

    float dpi = MARKEE_DEFAULT_DPI;
    if (override_dpi > 0.0f) {
        dpi = override_dpi;
    } else if (monitor_w > 0 && monitor_phys_w_mm > 0) {
        float detected = (float)monitor_w / ((float)monitor_phys_w_mm / MARKEE_MM_PER_INCH);
        if (detected >= MARKEE_MIN_VALID_DPI && detected <= MARKEE_MAX_VALID_DPI) {
            dpi = detected;
        }
    }

    L.dpi = dpi;

    /* LED matrix active area calibrated to ~6.35 inches wide */
    L.matrix_w = MARKEE_MATRIX_WIDTH_INCHES * dpi;
    L.cell_pitch = L.matrix_w / (float)MARKEE_WIDTH;
    L.matrix_h = L.cell_pitch * (float)MARKEE_HEIGHT;

    /* Plastic body proportions modeled after the BUSY Bar (168.6 x 55.2 mm around 158.4 x 35.2 mm)
     * with a slightly generous outer shell rim so the plastic casing reads clearly on screen. */
    float body_scale_x = MARKEE_REF_BODY_W_MM / MARKEE_REF_MATRIX_W_MM;
    float body_scale_y = MARKEE_REF_BODY_H_MM / MARKEE_REF_MATRIX_H_MM;

    L.body_w = L.matrix_w * body_scale_x + dpi * 0.18f;
    L.body_h = L.matrix_h * body_scale_y + dpi * 0.10f;

    /* Dark desk margin around the plastic enclosure for drop shadow & rounded silhouette */
    float margin_x = roundf(dpi * 0.24f);
    float margin_y = roundf(dpi * 0.24f);

    L.window_w = (int)roundf(L.body_w + margin_x * 2.0f);
    L.window_h = (int)roundf(L.body_h + margin_y * 2.0f);

    L.body_x = ((float)L.window_w - L.body_w) * 0.5f;
    L.body_y = ((float)L.window_h - L.body_h) * 0.5f;

    /* Inset black polycarbonate faceplate inside the molded plastic outer housing */
    float shell_side = roundf(dpi * 0.095f);
    float shell_vert = roundf(dpi * 0.095f);
    L.face_x = L.body_x + shell_side;
    L.face_y = L.body_y + shell_vert;
    L.face_w = L.body_w - shell_side * 2.0f;
    L.face_h = L.body_h - shell_vert * 2.0f;

    /* Center the 72x16 LED matrix inside the black front panel */
    L.matrix_x = L.face_x + (L.face_w - L.matrix_w) * 0.5f;
    L.matrix_y = L.face_y + (L.face_h - L.matrix_h) * 0.5f;

    return L;
}

MarkeeLayout renderer_init_window(const char *host, uint16_t port, float override_dpi) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);

    /* Initial window using fallback DPI so GLFW initializes monitor metrics cleanly */
    MarkeeLayout initial = renderer_compute_layout(override_dpi, 0, 0);

    char title[128];
    snprintf(title, sizeof(title), "markee — 72x16 LED Display (http://%s:%u)",
             host ? host : MARKEE_DEFAULT_HOST, (unsigned)port);

    InitWindow(initial.window_w, initial.window_h, title);
    SetTargetFPS(MARKEE_REFRESH_HZ);

    int monitor = GetCurrentMonitor();
    int mon_w = GetMonitorWidth(monitor);
    int mon_h = GetMonitorHeight(monitor);
    int phys_w_mm = GetMonitorPhysicalWidth(monitor);

    MarkeeLayout layout = renderer_compute_layout(override_dpi, mon_w, phys_w_mm);
    if (layout.window_w != initial.window_w || layout.window_h != initial.window_h) {
        SetWindowSize(layout.window_w, layout.window_h);
    }

    if (mon_w > layout.window_w && mon_h > layout.window_h) {
        SetWindowPosition((mon_w - layout.window_w) / 2, (mon_h - layout.window_h) / 2);
    }

    printf("Display calibrated: monitor=%dx%d (%d mm physical), logical DPI=%.1f, "
           "LED matrix=%.1fx%.1f pts (%.2f\" wide), refresh=%d Hz\n",
           mon_w, mon_h, phys_w_mm, layout.dpi,
           layout.matrix_w, layout.matrix_h, layout.matrix_w / layout.dpi,
           MARKEE_REFRESH_HZ);
    fflush(stdout);

    return layout;
}

static uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

void renderer_draw(const MarkeeLayout *L,
                   const uint8_t pixels[MARKEE_FRAME_BYTES],
                   uint8_t brightness) {
    if (!L || !pixels) return;

    const float bright = (float)brightness / 100.0f;

    /* 1. Dark matte desk backdrop */
    ClearBackground((Color){26, 28, 33, 255});

    /* 2. Soft layered drop shadow beneath the plastic casing */
    for (int s = 4; s >= 1; s--) {
        float spread = (float)s * 3.5f;
        Rectangle shadow = {
            L->body_x - spread * 0.45f,
            L->body_y + (float)s * 3.2f,
            L->body_w + spread * 0.9f,
            L->body_h + spread * 0.45f
        };
        unsigned char alpha = (unsigned char)(38 - s * 7);
        DrawRectangleRounded(shadow, 0.24f, 16, (Color){0, 0, 0, alpha});
    }

    /* 3. Molded off-white PC/ABS plastic outer enclosure (BUSY Bar aesthetic) */
    Rectangle body = {L->body_x, L->body_y, L->body_w, L->body_h};
    /* Base shadow bevel along bottom rim */
    Rectangle body_lower = {L->body_x, L->body_y + 2.0f, L->body_w, L->body_h};
    DrawRectangleRounded(body_lower, 0.22f, 16, (Color){168, 164, 156, 255});

    /* Main molded plastic shell */
    DrawRectangleRounded(body, 0.22f, 16, (Color){230, 227, 220, 255});

    /* Top specular molded highlight rim */
    Rectangle body_hi = {L->body_x + 1.5f, L->body_y + 1.5f, L->body_w - 3.0f, L->body_h - 3.5f};
    DrawRectangleRoundedLinesEx(body_hi, 0.21f, 16, 1.5f, (Color){248, 246, 242, 200});

    /* Outer subtle contour line */
    DrawRectangleRoundedLinesEx(body, 0.22f, 16, 1.2f, (Color){132, 129, 122, 220});

    /* Signature subtle orange accent notches on left & right shell edges */
    float accent_h = L->body_h * 0.24f;
    float accent_y = L->body_y + (L->body_h - accent_h) * 0.5f;
    DrawRectangleRounded((Rectangle){L->body_x + 1.5f, accent_y, 3.0f, accent_h},
                         1.0f, 6, (Color){242, 92, 25, 225});
    DrawRectangleRounded((Rectangle){L->body_x + L->body_w - 4.5f, accent_y, 3.0f, accent_h},
                         1.0f, 6, (Color){242, 92, 25, 225});

    /* 4. Inset black polycarbonate front faceplate with recessed bezel */
    Rectangle bezel_outer = {
        L->face_x - 2.0f, L->face_y - 2.0f,
        L->face_w + 4.0f, L->face_h + 4.0f
    };
    DrawRectangleRounded(bezel_outer, 0.18f, 14, (Color){180, 176, 168, 255});

    Rectangle face = {L->face_x, L->face_y, L->face_w, L->face_h};
    DrawRectangleRounded(face, 0.16f, 14, (Color){18, 19, 23, 255});

    /* Subtle inner chamfer rim around the smoked polycarbonate panel */
    DrawRectangleRoundedLinesEx(face, 0.16f, 14, 1.5f, (Color){44, 47, 54, 255});

    /* Deep black recessed LED cavity */
    float cavity_pad = fmaxf(3.0f, L->cell_pitch * 0.45f);
    Rectangle cavity = {
        L->matrix_x - cavity_pad,
        L->matrix_y - cavity_pad,
        L->matrix_w + cavity_pad * 2.0f,
        L->matrix_h + cavity_pad * 2.0f
    };
    DrawRectangleRounded(cavity, 0.08f, 8, (Color){8, 9, 11, 255});
    DrawRectangleRoundedLinesEx(cavity, 0.08f, 8, 1.0f, (Color){28, 30, 36, 255});

    /* 5. 72x16 LED Matrix rendering
     *    Pass A: Unlit light-isolating diffuser cells (visible faintly when LEDs are off)
     *    Pass B: Soft diffusion glow halos behind lit LEDs
     *    Pass C: Crisp illuminated 24-bit LED elements + subtle hot-spot die core */
    const float pitch = L->cell_pitch;
    const float led_size = pitch * 0.72f;
    const float led_inset = (pitch - led_size) * 0.5f;
    const float core_size = led_size * 0.46f;
    const float core_inset = (pitch - core_size) * 0.5f;

    /* Pass A: Unlit diffuser cells */
    const Color unlit = {20, 22, 26, 255};
    for (int y = 0; y < MARKEE_HEIGHT; y++) {
        float py = L->matrix_y + (float)y * pitch + led_inset;
        for (int x = 0; x < MARKEE_WIDTH; x++) {
            float px = L->matrix_x + (float)x * pitch + led_inset;
            DrawRectangleRounded((Rectangle){px, py, led_size, led_size}, 0.28f, 4, unlit);
        }
    }

    /* Pass B: Soft polycarbonate diffuser halos for lit pixels */
    if (brightness > 0) {
        for (int y = 0; y < MARKEE_HEIGHT; y++) {
            float cy = L->matrix_y + ((float)y + 0.5f) * pitch;
            for (int x = 0; x < MARKEE_WIDTH; x++) {
                size_t idx = (size_t)(y * MARKEE_WIDTH + x) * 3;
                uint8_t r = (uint8_t)roundf((float)pixels[idx + 0] * bright);
                uint8_t g = (uint8_t)roundf((float)pixels[idx + 1] * bright);
                uint8_t b = (uint8_t)roundf((float)pixels[idx + 2] * bright);

                int maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
                if (maxc < 10) continue;

                float cx = L->matrix_x + ((float)x + 0.5f) * pitch;
                unsigned char halo_alpha = (unsigned char)(18 + (maxc * 52) / 255);
                DrawCircleV((Vector2){cx, cy}, pitch * 0.88f, (Color){r, g, b, halo_alpha});
            }
        }
    }

    /* Pass C: Illuminated LED dies */
    if (brightness > 0) {
        for (int y = 0; y < MARKEE_HEIGHT; y++) {
            float cell_y = L->matrix_y + (float)y * pitch;
            for (int x = 0; x < MARKEE_WIDTH; x++) {
                size_t idx = (size_t)(y * MARKEE_WIDTH + x) * 3;
                uint8_t r = (uint8_t)roundf((float)pixels[idx + 0] * bright);
                uint8_t g = (uint8_t)roundf((float)pixels[idx + 1] * bright);
                uint8_t b = (uint8_t)roundf((float)pixels[idx + 2] * bright);

                if (r == 0 && g == 0 && b == 0) continue;

                float cell_x = L->matrix_x + (float)x * pitch;

                /* Slightly blend dark pixels with unlit diffuser lens so very low 24-bit values
                 * still transition smoothly from the dark plastic housing. */
                Color led_col = {
                    clamp_u8(r + (20 * (255 - r)) / 2550),
                    clamp_u8(g + (22 * (255 - g)) / 2550),
                    clamp_u8(b + (26 * (255 - b)) / 2550),
                    255
                };
                DrawRectangleRounded(
                    (Rectangle){cell_x + led_inset, cell_y + led_inset, led_size, led_size},
                    0.28f, 4, led_col);

                /* Subtle brighter center core on high-intensity pixels */
                int lum = (r * 3 + g * 6 + b) / 10;
                if (lum > 70) {
                    int boost = (lum - 70) / 5;
                    Color core_col = {
                        clamp_u8(r + boost),
                        clamp_u8(g + boost),
                        clamp_u8(b + boost),
                        160
                    };
                    DrawRectangleRounded(
                        (Rectangle){cell_x + core_inset, cell_y + core_inset, core_size, core_size},
                        0.45f, 4, core_col);
                }
            }
        }
    }

    /* 6. Subtle smoked polycarbonate top-surface sheen */
    Rectangle sheen = {L->face_x + 4.0f, L->face_y + 2.0f, L->face_w - 8.0f, L->face_h * 0.28f};
    DrawRectangleGradientV((int)sheen.x, (int)sheen.y, (int)sheen.width, (int)sheen.height,
                           (Color){255, 255, 255, 13}, (Color){255, 255, 255, 0});
}
