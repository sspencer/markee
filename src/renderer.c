#include "renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "raylib.h"

bool renderer_parse_case_theme(const char *s, MarkeeCaseTheme *out_theme) {
    if (!s || !*s || !out_theme) return false;
    if (strcasecmp(s, "white") == 0 || strcasecmp(s, "default") == 0) {
        *out_theme = MARKEE_CASE_WHITE;
        return true;
    }
    if (strcasecmp(s, "charcoal") == 0 || strcasecmp(s, "black") == 0 || strcasecmp(s, "dark") == 0) {
        *out_theme = MARKEE_CASE_CHARCOAL;
        return true;
    }
    if (strcasecmp(s, "orange") == 0) {
        *out_theme = MARKEE_CASE_ORANGE;
        return true;
    }
    if (strcasecmp(s, "retro") == 0 || strcasecmp(s, "beige") == 0) {
        *out_theme = MARKEE_CASE_RETRO;
        return true;
    }
    return false;
}

const char *renderer_case_theme_name(MarkeeCaseTheme theme) {
    switch (theme) {
        case MARKEE_CASE_CHARCOAL: return "charcoal";
        case MARKEE_CASE_ORANGE:   return "orange";
        case MARKEE_CASE_RETRO:    return "retro";
        case MARKEE_CASE_WHITE:
        default:                   return "white";
    }
}

bool renderer_parse_corner(const char *s, MarkeeWindowCorner *out_corner) {
    if (!s || !*s || !out_corner) return false;
    if (strcasecmp(s, "center") == 0 || strcasecmp(s, "middle") == 0 || strcasecmp(s, "c") == 0) {
        *out_corner = MARKEE_CORNER_CENTER;
        return true;
    }
    if (strcasecmp(s, "nw") == 0 || strcasecmp(s, "northwest") == 0 ||
        strcasecmp(s, "top-left") == 0 || strcasecmp(s, "topleft") == 0 || strcasecmp(s, "tl") == 0) {
        *out_corner = MARKEE_CORNER_TOP_LEFT;
        return true;
    }
    if (strcasecmp(s, "ne") == 0 || strcasecmp(s, "northeast") == 0 ||
        strcasecmp(s, "top-right") == 0 || strcasecmp(s, "topright") == 0 || strcasecmp(s, "tr") == 0) {
        *out_corner = MARKEE_CORNER_TOP_RIGHT;
        return true;
    }
    if (strcasecmp(s, "sw") == 0 || strcasecmp(s, "southwest") == 0 ||
        strcasecmp(s, "bottom-left") == 0 || strcasecmp(s, "bottomleft") == 0 || strcasecmp(s, "bl") == 0) {
        *out_corner = MARKEE_CORNER_BOTTOM_LEFT;
        return true;
    }
    if (strcasecmp(s, "se") == 0 || strcasecmp(s, "southeast") == 0 ||
        strcasecmp(s, "bottom-right") == 0 || strcasecmp(s, "bottomright") == 0 || strcasecmp(s, "br") == 0) {
        *out_corner = MARKEE_CORNER_BOTTOM_RIGHT;
        return true;
    }
    if (strcasecmp(s, "n") == 0 || strcasecmp(s, "north") == 0 ||
        strcasecmp(s, "top") == 0 || strcasecmp(s, "top-center") == 0 || strcasecmp(s, "tc") == 0) {
        *out_corner = MARKEE_CORNER_TOP;
        return true;
    }
    if (strcasecmp(s, "s") == 0 || strcasecmp(s, "south") == 0 ||
        strcasecmp(s, "bottom") == 0 || strcasecmp(s, "bottom-center") == 0 || strcasecmp(s, "bc") == 0) {
        *out_corner = MARKEE_CORNER_BOTTOM;
        return true;
    }
    if (strcasecmp(s, "w") == 0 || strcasecmp(s, "west") == 0 ||
        strcasecmp(s, "left") == 0 || strcasecmp(s, "middle-left") == 0 ||
        strcasecmp(s, "center-left") == 0 || strcasecmp(s, "ml") == 0 || strcasecmp(s, "cl") == 0) {
        *out_corner = MARKEE_CORNER_LEFT;
        return true;
    }
    if (strcasecmp(s, "e") == 0 || strcasecmp(s, "east") == 0 ||
        strcasecmp(s, "right") == 0 || strcasecmp(s, "middle-right") == 0 ||
        strcasecmp(s, "center-right") == 0 || strcasecmp(s, "mr") == 0 || strcasecmp(s, "cr") == 0) {
        *out_corner = MARKEE_CORNER_RIGHT;
        return true;
    }
    return false;
}

static bool parse_axis_coord(const char *s, size_t len,
                             bool *out_centered, bool *out_from_end, int *out_val) {
    if (!s || len == 0 || len >= 32) return false;
    char buf[32];
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (strcasecmp(buf, "center") == 0 || strcasecmp(buf, "middle") == 0 || strcasecmp(buf, "c") == 0) {
        *out_centered = true;
        *out_from_end = false;
        *out_val = 0;
        return true;
    }
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || v < -16384 || v > 16384) return false;
    *out_centered = false;
    *out_from_end = (buf[0] == '-');
    *out_val = (int)v;
    return true;
}

bool renderer_parse_position(const char *s, MarkeeWindowOpts *out_opts) {
    if (!s || !*s || !out_opts) return false;

    MarkeeWindowCorner corner;
    if (renderer_parse_corner(s, &corner)) {
        out_opts->has_position = false;
        out_opts->center_x = false;
        out_opts->center_y = false;
        out_opts->from_right = false;
        out_opts->from_bottom = false;
        out_opts->corner = corner;
        return true;
    }

    const char *comma = strchr(s, ',');
    if (!comma || strchr(comma + 1, ',') != NULL) return false;

    bool cx = false, cy = false;
    bool fx = false, fy = false;
    int px = 0, py = 0;
    if (!parse_axis_coord(s, (size_t)(comma - s), &cx, &fx, &px)) return false;
    if (!parse_axis_coord(comma + 1, strlen(comma + 1), &cy, &fy, &py)) return false;

    if (cx && cy) {
        out_opts->has_position = false;
        out_opts->center_x = false;
        out_opts->center_y = false;
        out_opts->from_right = false;
        out_opts->from_bottom = false;
        out_opts->corner = MARKEE_CORNER_CENTER;
        return true;
    }

    out_opts->has_position = true;
    out_opts->center_x = cx;
    out_opts->center_y = cy;
    out_opts->from_right = fx;
    out_opts->from_bottom = fy;
    out_opts->pos_x = px;
    out_opts->pos_y = py;
    return true;
}

void renderer_compute_window_position(int mon_w, int mon_h, int win_w, int win_h,
                                      const MarkeeWindowOpts *opts,
                                      int *out_x, int *out_y) {
    int x = 0, y = 0;
    int cx = (mon_w > win_w) ? (mon_w - win_w) / 2 : 0;
    int cy = (mon_h > win_h) ? (mon_h - win_h) / 2 : 0;

    if (opts && opts->has_position) {
        if (opts->center_x) {
            x = cx;
        } else if ((opts->from_right || opts->pos_x < 0) && mon_w > 0) {
            x = mon_w - win_w + opts->pos_x;
        } else {
            x = opts->pos_x;
        }

        if (opts->center_y) {
            y = cy;
        } else if ((opts->from_bottom || opts->pos_y < 0) && mon_h > 0) {
            y = mon_h - win_h + opts->pos_y;
        } else {
            y = opts->pos_y;
        }
    } else if (mon_w > 0 && mon_h > 0) {
        const int pad_x = 24;
        const int pad_top = 48;    /* clearance for OS menu bar */
        const int pad_bottom = 64; /* clearance for dock / taskbar */
        int rx = mon_w > win_w + pad_x ? mon_w - win_w - pad_x : 0;
        int by = mon_h > win_h + pad_bottom ? mon_h - win_h - pad_bottom : 0;
        int ty = mon_h > win_h + pad_top ? pad_top : 0;
        int lx = mon_w > win_w + pad_x ? pad_x : 0;

        MarkeeWindowCorner corner = opts ? opts->corner : MARKEE_CORNER_CENTER;
        switch (corner) {
            case MARKEE_CORNER_TOP_LEFT:     x = lx; y = ty; break;
            case MARKEE_CORNER_TOP_RIGHT:    x = rx; y = ty; break;
            case MARKEE_CORNER_BOTTOM_LEFT:  x = lx; y = by; break;
            case MARKEE_CORNER_BOTTOM_RIGHT: x = rx; y = by; break;
            case MARKEE_CORNER_TOP:          x = cx; y = ty; break;
            case MARKEE_CORNER_BOTTOM:       x = cx; y = by; break;
            case MARKEE_CORNER_LEFT:         x = lx; y = cy; break;
            case MARKEE_CORNER_RIGHT:        x = rx; y = cy; break;
            case MARKEE_CORNER_CENTER:
            default:                         x = cx; y = cy; break;
        }
    }
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

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

MarkeeLayout renderer_init_window_ex(int width, int height, float zoom,
                                     MarkeePixelShape pixel_shape,
                                     const char *host, uint16_t port,
                                     float override_dpi,
                                     const MarkeeWindowOpts *opts) {
    MarkeeWindowOpts def = {0};
    if (!opts) opts = &def;

    SetTraceLogLevel(LOG_WARNING);
    unsigned int flags = FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT;
    if (opts->always_on_top) {
        flags |= FLAG_WINDOW_TOPMOST;
    }
    if (opts->borderless) {
        flags |= FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TRANSPARENT;
    }
    SetConfigFlags(flags);

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
    layout.borderless = opts->borderless;
    layout.case_theme = opts->case_theme;
    if (layout.window_w != initial.window_w || layout.window_h != initial.window_h) {
        SetWindowSize(layout.window_w, layout.window_h);
    }

    if (opts->has_position || (mon_w > 0 && mon_h > 0)) {
        int px = 0, py = 0;
        renderer_compute_window_position(mon_w, mon_h, layout.window_w, layout.window_h,
                                         opts, &px, &py);
        SetWindowPosition(px, py);
    }

    printf("Display calibrated: %dx%d LEDs, zoom=%.2fx, pixels=%s, case=%s, monitor=%dx%d (%d mm physical), "
           "logical DPI=%.1f, LED matrix=%.1fx%.1f pts (%.2f\" wide), refresh=%d Hz%s%s\n",
           layout.width, layout.height, layout.zoom,
           layout.pixel_shape == MARKEE_PIXEL_CIRCLE ? "circle" : "square",
           renderer_case_theme_name(layout.case_theme),
           mon_w, mon_h, phys_w_mm, layout.dpi,
           layout.matrix_w, layout.matrix_h, layout.matrix_w / layout.dpi,
           MARKEE_REFRESH_HZ,
           opts->always_on_top ? ", always-on-top" : "",
           opts->borderless ? ", borderless" : "");
    fflush(stdout);

    return layout;
}

MarkeeLayout renderer_init_window(int width, int height, float zoom,
                                  MarkeePixelShape pixel_shape,
                                  const char *host, uint16_t port,
                                  float override_dpi) {
    return renderer_init_window_ex(width, height, zoom, pixel_shape,
                                   host, port, override_dpi, NULL);
}

void renderer_set_always_on_top(bool enabled) {
    if (enabled) {
        SetWindowState(FLAG_WINDOW_TOPMOST);
    } else {
        ClearWindowState(FLAG_WINDOW_TOPMOST);
    }
}

bool renderer_handle_drag(const MarkeeLayout *L) {
    static bool dragging = false;
    static Vector2 grab_offset = {0.0f, 0.0f};

    if (!L || !L->borderless) {
        dragging = false;
        return false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 mp = GetMousePosition();
        Rectangle body = {L->body_x, L->body_y, L->body_w, L->body_h};
        if (CheckCollisionPointRec(mp, body)) {
            dragging = true;
            grab_offset = mp;
        }
    }

    if (dragging) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 wp = GetWindowPosition();
            Vector2 mp = GetMousePosition();
            int nx = (int)roundf(wp.x + mp.x - grab_offset.x);
            int ny = (int)roundf(wp.y + mp.y - grab_offset.y);
            if (nx != (int)roundf(wp.x) || ny != (int)roundf(wp.y)) {
                SetWindowPosition(nx, ny);
            }
        } else {
            dragging = false;
        }
    }

    return dragging;
}

int renderer_poll_click(const MarkeeLayout *L, int *out_x, int *out_y, const char **out_btn) {
    static bool left_down = false;
    static Vector2 left_start = {0.0f, 0.0f};
    static bool right_down = false;
    static Vector2 right_start = {0.0f, 0.0f};

    if (out_x) *out_x = -1;
    if (out_y) *out_y = -1;
    if (out_btn) *out_btn = "";
    if (!L) return 0;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        left_down = true;
        left_start = GetMousePosition();
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        right_down = true;
        right_start = GetMousePosition();
    }

    bool clicked = false;
    Vector2 mp = {0.0f, 0.0f};
    const char *btn = "left";

    if (left_down && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        left_down = false;
        mp = GetMousePosition();
        float dx = mp.x - left_start.x;
        float dy = mp.y - left_start.y;
        if (dx * dx + dy * dy <= 36.0f) {
            clicked = true;
            btn = "left";
        }
    } else if (right_down && IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
        right_down = false;
        mp = GetMousePosition();
        float dx = mp.x - right_start.x;
        float dy = mp.y - right_start.y;
        if (dx * dx + dy * dy <= 36.0f) {
            clicked = true;
            btn = "right";
        }
    }

    if (!clicked) return 0;
    if (out_btn) *out_btn = btn;

    Rectangle matrix = {L->matrix_x, L->matrix_y, L->matrix_w, L->matrix_h};
    if (CheckCollisionPointRec(mp, matrix) && L->cell_pitch > 0.0f) {
        int gx = (int)floorf((mp.x - L->matrix_x) / L->cell_pitch);
        int gy = (int)floorf((mp.y - L->matrix_y) / L->cell_pitch);
        if (gx < 0) gx = 0;
        if (gx >= L->width) gx = L->width - 1;
        if (gy < 0) gy = 0;
        if (gy >= L->height) gy = L->height - 1;
        if (out_x) *out_x = gx;
        if (out_y) *out_y = gy;
        return 1;
    }

    Rectangle body = {L->body_x, L->body_y, L->body_w, L->body_h};
    if (CheckCollisionPointRec(mp, body)) {
        return 2;
    }
    return 0;
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

    /* 1. Backdrop: transparent in --borderless floating widget mode, dark matte desk otherwise */
    if (L->borderless) {
        ClearBackground(BLANK);
    } else {
        ClearBackground((Color){26, 28, 33, 255});
    }

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

    /* 3. Molded PC/ABS plastic outer enclosure (themed palette) */
    Color shell_lower, shell_main, shell_hi, shell_contour, shell_bezel;
    switch (L->case_theme) {
        case MARKEE_CASE_CHARCOAL:
            shell_lower   = (Color){36, 38, 44, 255};
            shell_main    = (Color){56, 59, 66, 255};
            shell_hi      = (Color){88, 92, 102, 200};
            shell_contour = (Color){24, 26, 30, 220};
            shell_bezel   = (Color){42, 45, 51, 255};
            break;
        case MARKEE_CASE_ORANGE:
            shell_lower   = (Color){178, 70, 20, 255};
            shell_main    = (Color){236, 104, 36, 255};
            shell_hi      = (Color){252, 152, 92, 200};
            shell_contour = (Color){132, 50, 14, 220};
            shell_bezel   = (Color){192, 80, 26, 255};
            break;
        case MARKEE_CASE_RETRO:
            shell_lower   = (Color){158, 148, 128, 255};
            shell_main    = (Color){216, 206, 184, 255};
            shell_hi      = (Color){238, 230, 212, 200};
            shell_contour = (Color){122, 114, 98, 220};
            shell_bezel   = (Color){172, 162, 140, 255};
            break;
        case MARKEE_CASE_WHITE:
        default:
            shell_lower   = (Color){168, 164, 156, 255};
            shell_main    = (Color){230, 227, 220, 255};
            shell_hi      = (Color){248, 246, 242, 200};
            shell_contour = (Color){132, 129, 122, 220};
            shell_bezel   = (Color){180, 176, 168, 255};
            break;
    }

    Rectangle body = {L->body_x, L->body_y, L->body_w, L->body_h};
    Rectangle body_lower = {L->body_x, L->body_y + 2.0f * z, L->body_w, L->body_h};
    DrawRectangleRounded(body_lower, body_r, 16, shell_lower);

    /* Main molded plastic shell */
    DrawRectangleRounded(body, body_r, 16, shell_main);

    /* Top specular molded highlight rim */
    Rectangle body_hi = {L->body_x + 1.5f * z, L->body_y + 1.5f * z,
                         L->body_w - 3.0f * z, L->body_h - 3.5f * z};
    DrawRectangleRoundedLinesEx(body_hi, body_hi_r, 16, fmaxf(1.0f, 1.5f * z), shell_hi);

    /* Outer subtle contour line */
    DrawRectangleRoundedLinesEx(body, body_r, 16, fmaxf(1.0f, 1.2f * z), shell_contour);

    /* 4. Inset black polycarbonate front faceplate with recessed bezel */
    float bpad = 2.0f * z;
    Rectangle bezel_outer = {
        L->face_x - bpad, L->face_y - bpad,
        L->face_w + bpad * 2.0f, L->face_h + bpad * 2.0f
    };
    DrawRectangleRounded(bezel_outer, bezel_r, 14, shell_bezel);

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
