#include "renderer.h"

#include <math.h>
#include <stdio.h>

#include "raylib.h"

MarkeeLayout renderer_compute_layout(int width, int height, float zoom,
                                     MarkeePixelShape pixel_shape,
                                     float override_dpi,
                                     int monitor_w, int monitor_phys_w_mm) {
    MarkeeLayout L = {0};

    if (width < MARKEE_MIN_DIM) width = MARKEE_DEFAULT_WIDTH;
    if (height < MARKEE_MIN_DIM) height = MARKEE_DEFAULT_HEIGHT;
    if (zoom <= 0.0f) zoom = MARKEE_DEFAULT_ZOOM;

    float dpi = MARKEE_DEFAULT_DPI;
    if (override_dpi > 0.0f) {
        dpi = override_dpi;
    } else if (monitor_w > 0 && monitor_phys_w_mm > 0) {
        float detected = (float)monitor_w / ((float)monitor_phys_w_mm / MARKEE_MM_PER_INCH);
        if (detected >= MARKEE_MIN_VALID_DPI && detected <= MARKEE_MAX_VALID_DPI) {
            dpi = detected;
        }
    }

    L.width = width;
    L.height = height;
    L.zoom = zoom;
    L.pixel_shape = pixel_shape;
    L.dpi = dpi;

    /* Effective logical DPI scaled by zoom.
     * At default 72x16 and zoom=1.0, this reproduces the exact 6.35-inch LED matrix
     * and plastic enclosure geometry. */
    float eff_dpi = dpi * zoom;
    float ref_matrix_w = MARKEE_MATRIX_WIDTH_INCHES * eff_dpi;

    L.cell_pitch = ref_matrix_w / (float)MARKEE_DEFAULT_WIDTH;
    L.matrix_w = L.cell_pitch * (float)width;
    L.matrix_h = L.cell_pitch * (float)height;

    /* Bezel padding around the LED matrix, sized from the reference body width
     * (168.6 mm around a 158.4 mm matrix) and kept constant in physical units so any custom
     * resolution keeps the same frame thickness. The same padding is used vertically so the
     * gap between the LEDs and the frame is even on all four sides. */
    float ref_matrix_h = L.cell_pitch * (float)MARKEE_DEFAULT_HEIGHT;
    float bezel_w_total = ref_matrix_w * (MARKEE_REF_BODY_W_MM / MARKEE_REF_MATRIX_W_MM - 1.0f) + eff_dpi * 0.18f;
    float bezel_h_total = bezel_w_total;

    L.ref_body_h = ref_matrix_h + bezel_h_total;
    L.body_w = L.matrix_w + bezel_w_total;
    L.body_h = L.matrix_h + bezel_h_total;

    /* Dark desk margin around the plastic enclosure for drop shadow & rounded silhouette */
    float margin_x = roundf(eff_dpi * 0.24f);
    float margin_y = roundf(eff_dpi * 0.24f);

    L.window_w = (int)roundf(L.body_w + margin_x * 2.0f);
    L.window_h = (int)roundf(L.body_h + margin_y * 2.0f);

    L.body_x = ((float)L.window_w - L.body_w) * 0.5f;
    L.body_y = ((float)L.window_h - L.body_h) * 0.5f;

    /* Inset black polycarbonate faceplate inside the molded plastic outer housing */
    float shell_side = roundf(eff_dpi * 0.095f);
    float shell_vert = roundf(eff_dpi * 0.095f);
    L.face_x = L.body_x + shell_side;
    L.face_y = L.body_y + shell_vert;
    L.face_w = L.body_w - shell_side * 2.0f;
    L.face_h = L.body_h - shell_vert * 2.0f;

    /* Center the LED matrix inside the black front panel */
    L.matrix_x = L.face_x + (L.face_w - L.matrix_w) * 0.5f;
    L.matrix_y = L.face_y + (L.face_h - L.matrix_h) * 0.5f;

    return L;
}

MarkeeLayout renderer_init_window(int width, int height, float zoom,
                                  MarkeePixelShape pixel_shape,
                                  const char *host, uint16_t port,
                                  float override_dpi) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);

    /* Initial window using fallback DPI so GLFW initializes monitor metrics cleanly */
    MarkeeLayout initial = renderer_compute_layout(width, height, zoom, pixel_shape, override_dpi, 0, 0);

    char title[160];
    snprintf(title, sizeof(title), "markee — %dx%d LED Display (http://%s:%u)",
             width, height, host ? host : MARKEE_DEFAULT_HOST, (unsigned)port);

    InitWindow(initial.window_w, initial.window_h, title);
    SetTargetFPS(MARKEE_REFRESH_HZ);

    int monitor = GetCurrentMonitor();
    int mon_w = GetMonitorWidth(monitor);
    int mon_h = GetMonitorHeight(monitor);
    int phys_w_mm = GetMonitorPhysicalWidth(monitor);

    MarkeeLayout layout = renderer_compute_layout(width, height, zoom, pixel_shape, override_dpi, mon_w, phys_w_mm);
    if (layout.window_w != initial.window_w || layout.window_h != initial.window_h) {
        SetWindowSize(layout.window_w, layout.window_h);
    }

    if (mon_w > 0 && mon_h > 0) {
        int px = mon_w > layout.window_w ? (mon_w - layout.window_w) / 2 : 0;
        int py = mon_h > layout.window_h ? (mon_h - layout.window_h) / 2 : 0;
        SetWindowPosition(px, py);
    }

    printf("Display calibrated: %dx%d LEDs, zoom=%.2fx, pixels=%s, monitor=%dx%d (%d mm physical), "
           "logical DPI=%.1f, LED matrix=%.1fx%.1f pts (%.2f\" wide), refresh=%d Hz\n",
           layout.width, layout.height, layout.zoom,
           layout.pixel_shape == MARKEE_PIXEL_CIRCLE ? "circle" : "square",
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

static float scale_roundness(float base_roundness, float ref_short_side, float actual_w, float actual_h) {
    float short_side = actual_w < actual_h ? actual_w : actual_h;
    if (short_side <= 0.0f) return base_roundness;
    float r = base_roundness * (ref_short_side / short_side);
    if (r > 0.5f) r = 0.5f;
    if (r < 0.01f) r = 0.01f;
    return r;
}

void renderer_draw(const MarkeeLayout *L,
                   const uint8_t *pixels,
                   uint8_t brightness) {
    if (!L || !pixels) return;

    const float bright = (float)brightness / 100.0f;
    const float z = L->zoom;

    float body_r   = scale_roundness(0.22f, L->ref_body_h, L->body_w, L->body_h);
    float body_hi_r = scale_roundness(0.21f, L->ref_body_h, L->body_w - 3.0f, L->body_h - 3.5f);
    float shadow_r = scale_roundness(0.24f, L->ref_body_h, L->body_w, L->body_h);
    float ref_face_h = L->ref_body_h - roundf(L->dpi * z * 0.095f) * 2.0f;
    float bezel_r  = scale_roundness(0.18f, ref_face_h + 4.0f, L->face_w + 4.0f, L->face_h + 4.0f);
    float face_r   = scale_roundness(0.16f, ref_face_h, L->face_w, L->face_h);

    /* 1. Dark matte desk backdrop */
    ClearBackground((Color){26, 28, 33, 255});

    /* 2. Soft layered drop shadow beneath the plastic casing */
    for (int s = 4; s >= 1; s--) {
        float spread = (float)s * 3.5f * z;
        Rectangle shadow = {
            L->body_x - spread * 0.45f,
            L->body_y + (float)s * 3.2f * z,
            L->body_w + spread * 0.9f,
            L->body_h + spread * 0.45f
        };
        unsigned char alpha = (unsigned char)(38 - s * 7);
        DrawRectangleRounded(shadow, shadow_r, 16, (Color){0, 0, 0, alpha});
    }

    /* 3. Molded off-white PC/ABS plastic outer enclosure (BUSY Bar aesthetic) */
    Rectangle body = {L->body_x, L->body_y, L->body_w, L->body_h};
    Rectangle body_lower = {L->body_x, L->body_y + 2.0f * z, L->body_w, L->body_h};
    DrawRectangleRounded(body_lower, body_r, 16, (Color){168, 164, 156, 255});

    /* Main molded plastic shell */
    DrawRectangleRounded(body, body_r, 16, (Color){230, 227, 220, 255});

    /* Top specular molded highlight rim */
    Rectangle body_hi = {L->body_x + 1.5f * z, L->body_y + 1.5f * z,
                         L->body_w - 3.0f * z, L->body_h - 3.5f * z};
    DrawRectangleRoundedLinesEx(body_hi, body_hi_r, 16, fmaxf(1.0f, 1.5f * z), (Color){248, 246, 242, 200});

    /* Outer subtle contour line */
    DrawRectangleRoundedLinesEx(body, body_r, 16, fmaxf(1.0f, 1.2f * z), (Color){132, 129, 122, 220});

    /* 4. Inset black polycarbonate front faceplate with recessed bezel */
    float bpad = 2.0f * z;
    Rectangle bezel_outer = {
        L->face_x - bpad, L->face_y - bpad,
        L->face_w + bpad * 2.0f, L->face_h + bpad * 2.0f
    };
    DrawRectangleRounded(bezel_outer, bezel_r, 14, (Color){180, 176, 168, 255});

    Rectangle face = {L->face_x, L->face_y, L->face_w, L->face_h};
    DrawRectangleRounded(face, face_r, 14, (Color){18, 19, 23, 255});

    /* Subtle inner chamfer rim around the smoked polycarbonate panel */
    DrawRectangleRoundedLinesEx(face, face_r, 14, fmaxf(1.0f, 1.5f * z), (Color){44, 47, 54, 255});

    /* Deep black recessed LED cavity */
    float cavity_pad = fmaxf(3.0f * z, L->cell_pitch * 0.45f);
    Rectangle cavity = {
        L->matrix_x - cavity_pad,
        L->matrix_y - cavity_pad,
        L->matrix_w + cavity_pad * 2.0f,
        L->matrix_h + cavity_pad * 2.0f
    };
    float ref_cavity_h = L->cell_pitch * (float)MARKEE_DEFAULT_HEIGHT + cavity_pad * 2.0f;
    float cavity_r = scale_roundness(0.08f, ref_cavity_h, cavity.width, cavity.height);
    DrawRectangleRounded(cavity, cavity_r, 8, (Color){8, 9, 11, 255});
    DrawRectangleRoundedLinesEx(cavity, cavity_r, 8, fmaxf(1.0f, 1.0f * z), (Color){28, 30, 36, 255});

    /* 5. WxH LED Matrix rendering */
    const int w = L->width;
    const int h = L->height;
    const float pitch = L->cell_pitch;
    const float led_size = pitch * 0.72f;
    const float led_inset = (pitch - led_size) * 0.5f;
    const float core_size = led_size * 0.46f;
    const float core_inset = (pitch - core_size) * 0.5f;
    const float circle_r = fmaxf(0.5f, pitch * 0.38f);
    const float circle_core_r = circle_r * 0.46f;
    const bool is_circle = (L->pixel_shape == MARKEE_PIXEL_CIRCLE);
    const bool detailed = (pitch >= 3.0f);

    /* Pass A: Unlit diffuser cells */
    const Color unlit = {20, 22, 26, 255};
    for (int y = 0; y < h; y++) {
        float cell_y = L->matrix_y + (float)y * pitch;
        float cy = cell_y + pitch * 0.5f;
        float py = cell_y + led_inset;
        for (int x = 0; x < w; x++) {
            float cell_x = L->matrix_x + (float)x * pitch;
            if (is_circle) {
                DrawCircleV((Vector2){cell_x + pitch * 0.5f, cy}, circle_r, unlit);
            } else if (detailed) {
                DrawRectangleRounded((Rectangle){cell_x + led_inset, py, led_size, led_size}, 0.28f, 4, unlit);
            } else {
                DrawRectangleRec((Rectangle){cell_x + led_inset, py, fmaxf(1.0f, led_size), fmaxf(1.0f, led_size)}, unlit);
            }
        }
    }

    /* Pass B: Soft polycarbonate diffuser halos for lit pixels */
    if (brightness > 0 && detailed) {
        for (int y = 0; y < h; y++) {
            float cy = L->matrix_y + ((float)y + 0.5f) * pitch;
            for (int x = 0; x < w; x++) {
                size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3;
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
        for (int y = 0; y < h; y++) {
            float cell_y = L->matrix_y + (float)y * pitch;
            float cy = cell_y + pitch * 0.5f;
            for (int x = 0; x < w; x++) {
                size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3;
                uint8_t r = (uint8_t)roundf((float)pixels[idx + 0] * bright);
                uint8_t g = (uint8_t)roundf((float)pixels[idx + 1] * bright);
                uint8_t b = (uint8_t)roundf((float)pixels[idx + 2] * bright);

                if (r == 0 && g == 0 && b == 0) continue;

                float cell_x = L->matrix_x + (float)x * pitch;

                Color led_col = {
                    clamp_u8(r + (20 * (255 - r)) / 2550),
                    clamp_u8(g + (22 * (255 - g)) / 2550),
                    clamp_u8(b + (26 * (255 - b)) / 2550),
                    255
                };

                if (is_circle) {
                    Vector2 center = {cell_x + pitch * 0.5f, cy};
                    DrawCircleV(center, circle_r, led_col);

                    if (detailed) {
                        int lum = (r * 3 + g * 6 + b) / 10;
                        if (lum > 70) {
                            int boost = (lum - 70) / 5;
                            Color core_col = {
                                clamp_u8(r + boost),
                                clamp_u8(g + boost),
                                clamp_u8(b + boost),
                                160
                            };
                            DrawCircleV(center, circle_core_r, core_col);
                        }
                    }
                } else if (detailed) {
                    DrawRectangleRounded(
                        (Rectangle){cell_x + led_inset, cell_y + led_inset, led_size, led_size},
                        0.28f, 4, led_col);

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
                } else {
                    DrawRectangleRec(
                        (Rectangle){cell_x + led_inset, cell_y + led_inset,
                                    fmaxf(1.0f, led_size), fmaxf(1.0f, led_size)},
                        led_col);
                }
            }
        }
    }

    /* 6. Subtle smoked polycarbonate top-surface sheen */
    Rectangle sheen = {L->face_x + 4.0f * z, L->face_y + 2.0f * z,
                       L->face_w - 8.0f * z, L->face_h * 0.28f};
    DrawRectangleGradientV((int)sheen.x, (int)sheen.y, (int)sheen.width, (int)sheen.height,
                           (Color){255, 255, 255, 13}, (Color){255, 255, 255, 0});
}
