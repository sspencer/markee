#define _POSIX_C_SOURCE 200809L

#include "display.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"
#include "raylib.h"

static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

/* --- sRGB <-> Linear Light BT.709 Monochrome Phosphor Filter --- */

static float srgb_u8_to_linear(uint8_t c) {
    float s = (float)c / 255.0f;
    if (s <= 0.04045f) return s / 12.92f;
    return powf((s + 0.055f) / 1.055f, 2.4f);
}

static uint8_t linear_to_srgb_u8(float lin) {
    if (lin <= 0.0f) return 0;
    if (lin >= 1.0f) return 255;
    float s = (lin <= 0.0031308f) ? (12.92f * lin) : (1.055f * powf(lin, 1.0f / 2.4f) - 0.055f);
    int v = (int)lroundf(s * 255.0f);
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static float g_srgb_lut[256];
static pthread_once_t g_lut_once = PTHREAD_ONCE_INIT;

static void init_srgb_lut(void) {
    for (int i = 0; i < 256; i++) {
        g_srgb_lut[i] = srgb_u8_to_linear((uint8_t)i);
    }
}

/* Transform a single 24-bit RGB pixel into a monochrome shade of (tr, tg, tb)
 * preserving true perceptual relative luminance (ITU-R BT.709 in linear light)
 * and constant tint chromaticity across all brightness levels. */
static void apply_monochrome_pixel(uint8_t in_r, uint8_t in_g, uint8_t in_b,
                                   float tint_r_lin, float tint_g_lin, float tint_b_lin,
                                   uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    float y_lin = 0.2126f * g_srgb_lut[in_r] +
                  0.7152f * g_srgb_lut[in_g] +
                  0.0722f * g_srgb_lut[in_b];
    *out_r = linear_to_srgb_u8(y_lin * tint_r_lin);
    *out_g = linear_to_srgb_u8(y_lin * tint_g_lin);
    *out_b = linear_to_srgb_u8(y_lin * tint_b_lin);
}

static void apply_monochrome_buffer(uint8_t *buf, size_t frame_bytes,
                                    uint8_t tr, uint8_t tg, uint8_t tb) {
    pthread_once(&g_lut_once, init_srgb_lut);
    float tr_lin = g_srgb_lut[tr];
    float tg_lin = g_srgb_lut[tg];
    float tb_lin = g_srgb_lut[tb];
    for (size_t i = 0; i < frame_bytes; i += 3) {
        apply_monochrome_pixel(buf[i + 0], buf[i + 1], buf[i + 2],
                               tr_lin, tg_lin, tb_lin,
                               &buf[i + 0], &buf[i + 1], &buf[i + 2]);
    }
}

static void stop_scroll_unlocked(MarkeeDisplay *disp) {
    disp->scroll.active = false;
    disp->scroll.has_start_time = false;
}

bool display_init(MarkeeDisplay *disp, int width, int height, bool start_demo) {
    if (!disp) return false;
    if (width < MARKEE_MIN_DIM || width > MARKEE_MAX_DIM ||
        height < MARKEE_MIN_DIM || height > MARKEE_MAX_DIM) {
        return false;
    }

    memset(disp, 0, sizeof(*disp));
    size_t frame_bytes = (size_t)width * (size_t)height * MARKEE_CHANNELS;
    uint8_t *pixels = calloc(frame_bytes, 1);
    uint8_t *bg_pixels = calloc(frame_bytes, 1);
    if (!pixels || !bg_pixels) {
        free(pixels);
        free(bg_pixels);
        return false;
    }

    if (pthread_mutex_init(&disp->lock, NULL) != 0) {
        free(pixels);
        free(bg_pixels);
        return false;
    }
    if (pthread_mutex_init(&disp->png_lock, NULL) != 0) {
        pthread_mutex_destroy(&disp->lock);
        free(pixels);
        free(bg_pixels);
        return false;
    }

    disp->width = width;
    disp->height = height;
    disp->frame_bytes = frame_bytes;
    disp->pixels = pixels;
    disp->scroll.bg_pixels = bg_pixels;
    disp->brightness = 100;
    disp->demo_active = start_demo;
    return true;
}

void display_destroy(MarkeeDisplay *disp) {
    if (!disp) return;
    pthread_mutex_destroy(&disp->png_lock);
    pthread_mutex_destroy(&disp->lock);
    free(disp->pixels);
    free(disp->scroll.bg_pixels);
    disp->pixels = NULL;
    disp->scroll.bg_pixels = NULL;
}

void display_clear(MarkeeDisplay *disp, bool stop_demo) {
    if (!disp || !disp->pixels) return;
    pthread_mutex_lock(&disp->lock);
    stop_scroll_unlocked(disp);
    memset(disp->pixels, 0, disp->frame_bytes);
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
}

void display_fill(MarkeeDisplay *disp, uint8_t r, uint8_t g, uint8_t b, bool stop_demo) {
    if (!disp || !disp->pixels) return;
    pthread_mutex_lock(&disp->lock);
    stop_scroll_unlocked(disp);
    for (size_t i = 0; i < disp->frame_bytes; i += 3) {
        disp->pixels[i + 0] = r;
        disp->pixels[i + 1] = g;
        disp->pixels[i + 2] = b;
    }
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
}

MarkeeResult display_set_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b, bool stop_demo) {
    if (!disp || !disp->pixels) return MARKEE_ERR_INVALID_ARG;
    if (x < 0 || x >= disp->width || y < 0 || y >= disp->height) {
        return MARKEE_ERR_OUT_OF_RANGE;
    }
    size_t idx = ((size_t)y * (size_t)disp->width + (size_t)x) * 3;
    pthread_mutex_lock(&disp->lock);
    stop_scroll_unlocked(disp);
    disp->pixels[idx + 0] = r;
    disp->pixels[idx + 1] = g;
    disp->pixels[idx + 2] = b;
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
    return MARKEE_OK;
}

MarkeeResult display_get_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!disp || !disp->pixels || !r || !g || !b) return MARKEE_ERR_INVALID_ARG;
    if (x < 0 || x >= disp->width || y < 0 || y >= disp->height) {
        return MARKEE_ERR_OUT_OF_RANGE;
    }
    size_t idx = ((size_t)y * (size_t)disp->width + (size_t)x) * 3;
    pthread_mutex_lock(&disp->lock);
    uint8_t pr = disp->pixels[idx + 0];
    uint8_t pg = disp->pixels[idx + 1];
    uint8_t pb = disp->pixels[idx + 2];
    bool mono = disp->mono_enabled;
    uint8_t tr = disp->mono_r, tg = disp->mono_g, tb = disp->mono_b;
    pthread_mutex_unlock(&disp->lock);

    if (mono) {
        pthread_once(&g_lut_once, init_srgb_lut);
        apply_monochrome_pixel(pr, pg, pb,
                               g_srgb_lut[tr], g_srgb_lut[tg], g_srgb_lut[tb],
                               r, g, b);
    } else {
        *r = pr;
        *g = pg;
        *b = pb;
    }
    return MARKEE_OK;
}

MarkeeResult display_set_raw_rgb(MarkeeDisplay *disp, const uint8_t *rgb, size_t len,
                                 bool stop_demo) {
    if (!disp || !disp->pixels || !rgb) return MARKEE_ERR_INVALID_ARG;
    if (len != disp->frame_bytes) return MARKEE_ERR_WRONG_DIMENSIONS;

    pthread_mutex_lock(&disp->lock);
    stop_scroll_unlocked(disp);
    memcpy(disp->pixels, rgb, disp->frame_bytes);
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
    return MARKEE_OK;
}

bool display_get_raw_rgb(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap) {
    if (!disp || !disp->pixels || !out_rgb || out_cap < disp->frame_bytes) return false;
    pthread_mutex_lock(&disp->lock);
    memcpy(out_rgb, disp->pixels, disp->frame_bytes);
    bool mono = disp->mono_enabled;
    uint8_t tr = disp->mono_r, tg = disp->mono_g, tb = disp->mono_b;
    pthread_mutex_unlock(&disp->lock);

    if (mono) {
        apply_monochrome_buffer(out_rgb, disp->frame_bytes, tr, tg, tb);
    }
    return true;
}

bool display_snapshot(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap,
                      uint8_t *out_brightness) {
    if (!disp || !disp->pixels || !out_rgb || out_cap < disp->frame_bytes) return false;
    pthread_mutex_lock(&disp->lock);
    memcpy(out_rgb, disp->pixels, disp->frame_bytes);
    if (out_brightness) {
        *out_brightness = disp->brightness;
    }
    bool mono = disp->mono_enabled;
    uint8_t tr = disp->mono_r, tg = disp->mono_g, tb = disp->mono_b;
    pthread_mutex_unlock(&disp->lock);

    if (mono) {
        apply_monochrome_buffer(out_rgb, disp->frame_bytes, tr, tg, tb);
    }
    return true;
}

void display_set_brightness(MarkeeDisplay *disp, uint8_t brightness) {
    if (!disp) return;
    if (brightness > 100) brightness = 100;
    pthread_mutex_lock(&disp->lock);
    disp->brightness = brightness;
    pthread_mutex_unlock(&disp->lock);
}

uint8_t display_get_brightness(MarkeeDisplay *disp) {
    if (!disp) return 100;
    pthread_mutex_lock(&disp->lock);
    uint8_t b = disp->brightness;
    pthread_mutex_unlock(&disp->lock);
    return b;
}

void display_set_monochrome(MarkeeDisplay *disp, bool enabled,
                            uint8_t r, uint8_t g, uint8_t b) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    disp->mono_enabled = enabled;
    if (enabled) {
        disp->mono_r = r;
        disp->mono_g = g;
        disp->mono_b = b;
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
}

void display_get_monochrome(MarkeeDisplay *disp, bool *enabled,
                            uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    if (enabled) *enabled = disp->mono_enabled;
    if (r) *r = disp->mono_r;
    if (g) *g = disp->mono_g;
    if (b) *b = disp->mono_b;
    pthread_mutex_unlock(&disp->lock);
}

void display_set_demo(MarkeeDisplay *disp, bool active) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    if (active) stop_scroll_unlocked(disp);
    disp->demo_active = active;
    pthread_mutex_unlock(&disp->lock);
}

bool display_is_demo_active(MarkeeDisplay *disp) {
    if (!disp) return false;
    pthread_mutex_lock(&disp->lock);
    bool active = disp->demo_active;
    pthread_mutex_unlock(&disp->lock);
    return active;
}

uint64_t display_get_frames_received(MarkeeDisplay *disp) {
    if (!disp) return 0;
    pthread_mutex_lock(&disp->lock);
    uint64_t count = disp->frames_received;
    pthread_mutex_unlock(&disp->lock);
    return count;
}

MarkeeResult display_load_png(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                              int *actual_w, int *actual_h) {
    if (actual_w) *actual_w = 0;
    if (actual_h) *actual_h = 0;
    if (!disp || !disp->pixels || !png_data ||
        png_len < sizeof(PNG_SIG) || png_len > MARKEE_MAX_BODY_BYTES) {
        return MARKEE_ERR_INVALID_PNG;
    }
    if (memcmp(png_data, PNG_SIG, sizeof(PNG_SIG)) != 0) {
        return MARKEE_ERR_INVALID_PNG;
    }

    uint8_t *decoded = malloc(disp->frame_bytes);
    if (!decoded) {
        return MARKEE_ERR_ENCODE_FAILED;
    }

    pthread_mutex_lock(&disp->png_lock);
    Image img = LoadImageFromMemory(".png", png_data, (int)png_len);
    if (img.data == NULL) {
        pthread_mutex_unlock(&disp->png_lock);
        free(decoded);
        return MARKEE_ERR_INVALID_PNG;
    }

    if (actual_w) *actual_w = img.width;
    if (actual_h) *actual_h = img.height;

    if (img.width != disp->width || img.height != disp->height) {
        UnloadImage(img);
        pthread_mutex_unlock(&disp->png_lock);
        free(decoded);
        return MARKEE_ERR_WRONG_DIMENSIONS;
    }

    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const uint8_t *rgba = (const uint8_t *)img.data;
    size_t pixel_count = (size_t)disp->width * (size_t)disp->height;
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t r = rgba[i * 4 + 0];
        uint32_t g = rgba[i * 4 + 1];
        uint32_t b = rgba[i * 4 + 2];
        uint32_t a = rgba[i * 4 + 3];
        decoded[i * 3 + 0] = (uint8_t)((r * a + 127U) / 255U);
        decoded[i * 3 + 1] = (uint8_t)((g * a + 127U) / 255U);
        decoded[i * 3 + 2] = (uint8_t)((b * a + 127U) / 255U);
    }

    UnloadImage(img);
    pthread_mutex_unlock(&disp->png_lock);

    MarkeeResult res = display_set_raw_rgb(disp, decoded, disp->frame_bytes, true);
    free(decoded);
    return res;
}

MarkeeResult display_encode_png(MarkeeDisplay *disp, uint8_t **out_png, size_t *out_len) {
    if (!disp || !disp->pixels || !out_png || !out_len) return MARKEE_ERR_INVALID_ARG;
    *out_png = NULL;
    *out_len = 0;

    uint8_t *snap = malloc(disp->frame_bytes);
    if (!snap) return MARKEE_ERR_ENCODE_FAILED;
    display_get_raw_rgb(disp, snap, disp->frame_bytes);

    Image img = {
        .data = snap,
        .width = disp->width,
        .height = disp->height,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8
    };

    int file_size = 0;
    pthread_mutex_lock(&disp->png_lock);
    unsigned char *png = ExportImageToMemory(img, ".png", &file_size);
    pthread_mutex_unlock(&disp->png_lock);
    free(snap);

    if (!png || file_size <= 0) {
        if (png) MemFree(png);
        return MARKEE_ERR_ENCODE_FAILED;
    }

    *out_png = png;
    *out_len = (size_t)file_size;
    return MARKEE_OK;
}

void display_free_buffer(uint8_t *buf) {
    if (buf) {
        MemFree(buf);
    }
}

/* --- Built-in 60 Hz startup demo + scrolling text tick --- */

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0.0f, gf = 0.0f, bf = 0.0f;

    if (h < 60.0f)       { rf = c; gf = x; bf = 0; }
    else if (h < 120.0f) { rf = x; gf = c; bf = 0; }
    else if (h < 180.0f) { rf = 0; gf = c; bf = x; }
    else if (h < 240.0f) { rf = 0; gf = x; bf = c; }
    else if (h < 300.0f) { rf = x; gf = 0; bf = c; }
    else                 { rf = c; gf = 0; bf = x; }

    *r = (uint8_t)((rf + m) * 255.0f + 0.5f);
    *g = (uint8_t)((gf + m) * 255.0f + 0.5f);
    *b = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

static void put_px(uint8_t *buf, int w, int h, int x, int y,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 3;
    buf[idx + 0] = r;
    buf[idx + 1] = g;
    buf[idx + 2] = b;
}

static void tick_scroll(MarkeeDisplay *disp, double time_sec) {
    pthread_mutex_lock(&disp->lock);
    if (!disp->scroll.active || !disp->scroll.bg_pixels) {
        pthread_mutex_unlock(&disp->lock);
        return;
    }

    if (!disp->scroll.has_start_time) {
        disp->scroll.start_time = time_sec;
        disp->scroll.has_start_time = true;
    }

    double elapsed = time_sec - disp->scroll.start_time;
    if (elapsed < 0.0) elapsed = 0.0;

    int offset = (int)floor(elapsed * disp->scroll.speed);
    int text_w = disp->scroll.text_w > 0 ? disp->scroll.text_w : 1;
    int span = disp->width + text_w;
    int draw_x = ((disp->scroll.start_x - offset + text_w) % span + span) % span - text_w;

    memcpy(disp->pixels, disp->scroll.bg_pixels, disp->frame_bytes);
    MarkeeColor col = {disp->scroll.r, disp->scroll.g, disp->scroll.b};
    draw_render_text_buf(disp->pixels, disp->width, disp->height,
                         draw_x, disp->scroll.y,
                         disp->scroll.text, disp->scroll.font, col);
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
}

void display_tick(MarkeeDisplay *disp, double time_sec) {
    if (!disp || !disp->pixels) return;

    if (!display_is_demo_active(disp)) {
        tick_scroll(disp, time_sec);
        return;
    }

    const int w = disp->width;
    const int h = disp->height;
    uint8_t *buf = calloc(disp->frame_bytes, 1);
    if (!buf) return;

    if (w >= MARKEE_DEFAULT_WIDTH && h >= MARKEE_DEFAULT_HEIGHT) {
        for (int x = 0; x < w; x++) {
            float hue = (float)x * 4.5f - (float)time_sec * 45.0f;
            uint8_t r, g, b;
            hsv_to_rgb(hue, 0.90f, 0.22f, &r, &g, &b);
            put_px(buf, w, h, x, 0, r, g, b);
            put_px(buf, w, h, x, h - 1, r, g, b);
        }

        int ox = (w - MARKEE_DEFAULT_WIDTH) / 2;
        int oy = (h - MARKEE_DEFAULT_HEIGHT) / 2;

        float pulse = 0.60f + 0.40f * sinf((float)time_sec * 3.5f);
        uint8_t or_r = (uint8_t)(255.0f * pulse);
        uint8_t or_g = (uint8_t)(92.0f * pulse);
        uint8_t or_b = (uint8_t)(16.0f * pulse);
        for (int y = 6; y <= 9; y++) {
            for (int x = 4; x <= 7; x++) {
                bool corner = (x == 4 || x == 7) && (y == 6 || y == 9);
                if (!corner) {
                    put_px(buf, w, h, ox + x, oy + y, or_r, or_g, or_b);
                }
            }
        }

        MarkeeColor white = {248, 244, 236};
        draw_render_text_buf(buf, w, h, ox + 12, oy + 4, "MARKEE", "6x8", white);

        for (int i = 0; i < 6; i++) {
            int bx = ox + 52 + i * 3;
            float wave = 0.5f + 0.5f * sinf((float)time_sec * (3.2f + (float)i * 0.55f) + (float)i * 1.1f);
            int bar_h = 2 + (int)(wave * 9.0f);
            for (int k = 0; k < bar_h; k++) {
                int y = oy + 12 - k;
                float hue = 18.0f + (float)i * 42.0f + (float)k * 10.0f + (float)time_sec * 30.0f;
                uint8_t r, g, b;
                hsv_to_rgb(hue, 0.92f, 0.92f, &r, &g, &b);
                put_px(buf, w, h, bx, y, r, g, b);
                put_px(buf, w, h, bx + 1, y, r, g, b);
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float hue = (float)x * (240.0f / (float)(w > 1 ? w : 1)) +
                            (float)y * (120.0f / (float)(h > 1 ? h : 1)) -
                            (float)time_sec * 60.0f;
                float val = 0.35f + 0.55f * (0.5f + 0.5f * sinf((float)x * 0.35f - (float)y * 0.35f + (float)time_sec * 3.0f));
                uint8_t r, g, b;
                hsv_to_rgb(hue, 0.90f, val, &r, &g, &b);
                put_px(buf, w, h, x, y, r, g, b);
            }
        }
    }

    pthread_mutex_lock(&disp->lock);
    if (disp->demo_active) {
        memcpy(disp->pixels, buf, disp->frame_bytes);
        disp->frame_seq++;
    }
    pthread_mutex_unlock(&disp->lock);
    free(buf);
}

void display_tick_demo(MarkeeDisplay *disp, double time_sec) {
    display_tick(disp, time_sec);
}
