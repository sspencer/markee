#define _POSIX_C_SOURCE 200809L

#include "display.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

void display_stop_gif_unlocked(MarkeeDisplay *disp) {
    if (!disp) return;
    for (size_t i = 0; i < disp->gif.count && i < MARKEE_MAX_GIF_LAYERS; i++) {
        gif_free(disp->gif.layers[i].gif);
        disp->gif.layers[i].gif = NULL;
    }
    memset(disp->gif.layers, 0, sizeof(disp->gif.layers));
    disp->gif.count = 0;
    disp->gif.active = false;
    disp->gif.has_last_time = false;
    disp->gif.last_time = 0.0;
}

void display_stop_gif(MarkeeDisplay *disp) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    display_stop_gif_unlocked(disp);
    pthread_mutex_unlock(&disp->lock);
}

void display_notify_cancel_unlocked(MarkeeDisplay *disp) {
    if (!disp) return;
    for (size_t i = 0; i < disp->notify.saved_gif.count && i < MARKEE_MAX_GIF_LAYERS; i++) {
        gif_free(disp->notify.saved_gif.layers[i].gif);
        disp->notify.saved_gif.layers[i].gif = NULL;
    }
    memset(disp->notify.saved_gif.layers, 0, sizeof(disp->notify.saved_gif.layers));
    disp->notify.saved_gif.count = 0;
    disp->notify.saved_gif.active = false;
    disp->notify.active = false;
    disp->notify.has_start_time = false;
    disp->notify.duration_sec = 0.0;
}

void display_notify_begin_unlocked(MarkeeDisplay *disp, double ttl_sec) {
    if (!disp || ttl_sec <= 0.0) return;
    if (!disp->notify.active) {
        if (disp->notify.saved_pixels && disp->pixels) {
            memcpy(disp->notify.saved_pixels, disp->pixels, disp->frame_bytes);
        }
        disp->notify.saved_demo_active = disp->demo_active;

        disp->notify.saved_scroll = disp->scroll;
        disp->notify.saved_scroll.bg_pixels = disp->notify.saved_scroll_bg;
        if (disp->notify.saved_scroll_bg && disp->scroll.bg_pixels) {
            memcpy(disp->notify.saved_scroll_bg, disp->scroll.bg_pixels, disp->frame_bytes);
        }

        /* Move active GIF layer ownership into notify.saved_gif */
        disp->notify.saved_gif = disp->gif;
        disp->notify.saved_gif.bg_pixels = disp->notify.saved_gif_bg;
        if (disp->notify.saved_gif_bg && disp->gif.bg_pixels) {
            memcpy(disp->notify.saved_gif_bg, disp->gif.bg_pixels, disp->frame_bytes);
        }
        memset(disp->gif.layers, 0, sizeof(disp->gif.layers));
        disp->gif.count = 0;
        disp->gif.active = false;
        disp->gif.has_last_time = false;
    }
    disp->notify.active = true;
    disp->notify.duration_sec = ttl_sec;
    disp->notify.has_start_time = disp->has_tick_time;
    disp->notify.start_time = disp->has_tick_time ? disp->last_tick_time : 0.0;
}

static void display_notify_restore_unlocked(MarkeeDisplay *disp) {
    if (!disp || !disp->notify.active) return;

    display_stop_gif_unlocked(disp);
    if (disp->pixels && disp->notify.saved_pixels) {
        memcpy(disp->pixels, disp->notify.saved_pixels, disp->frame_bytes);
    }
    disp->demo_active = disp->notify.saved_demo_active;

    uint8_t *scroll_bg = disp->scroll.bg_pixels;
    disp->scroll = disp->notify.saved_scroll;
    disp->scroll.bg_pixels = scroll_bg;
    if (scroll_bg && disp->notify.saved_scroll_bg) {
        memcpy(scroll_bg, disp->notify.saved_scroll_bg, disp->frame_bytes);
    }

    uint8_t *gif_bg = disp->gif.bg_pixels;
    disp->gif = disp->notify.saved_gif;
    disp->gif.bg_pixels = gif_bg;
    disp->gif.has_last_time = false;
    if (gif_bg && disp->notify.saved_gif_bg) {
        memcpy(gif_bg, disp->notify.saved_gif_bg, disp->frame_bytes);
    }

    memset(disp->notify.saved_gif.layers, 0, sizeof(disp->notify.saved_gif.layers));
    disp->notify.saved_gif.count = 0;
    disp->notify.saved_gif.active = false;
    disp->notify.active = false;
    disp->notify.has_start_time = false;
    disp->notify.duration_sec = 0.0;
    disp->frame_seq++;
}

bool display_notify_dismiss(MarkeeDisplay *disp) {
    if (!disp) return false;
    pthread_mutex_lock(&disp->lock);
    bool was_active = disp->notify.active;
    if (was_active) {
        display_notify_restore_unlocked(disp);
    }
    pthread_mutex_unlock(&disp->lock);
    return was_active;
}

bool display_get_notify_status(MarkeeDisplay *disp, double *out_ttl, double *out_remaining) {
    if (!disp) return false;
    pthread_mutex_lock(&disp->lock);
    bool active = disp->notify.active;
    if (active) {
        if (out_ttl) *out_ttl = disp->notify.duration_sec;
        double rem = disp->notify.duration_sec;
        if (disp->notify.has_start_time && disp->has_tick_time) {
            double elapsed = disp->last_tick_time - disp->notify.start_time;
            if (elapsed > 0.0) rem -= elapsed;
            if (rem < 0.0) rem = 0.0;
        }
        if (out_remaining) *out_remaining = rem;
    }
    pthread_mutex_unlock(&disp->lock);
    return active;
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
    uint8_t *gif_bg_pixels = calloc(frame_bytes, 1);
    uint8_t *notify_px = calloc(frame_bytes, 1);
    uint8_t *notify_scroll_bg = calloc(frame_bytes, 1);
    uint8_t *notify_gif_bg = calloc(frame_bytes, 1);
    if (!pixels || !bg_pixels || !gif_bg_pixels ||
        !notify_px || !notify_scroll_bg || !notify_gif_bg) {
        free(pixels);
        free(bg_pixels);
        free(gif_bg_pixels);
        free(notify_px);
        free(notify_scroll_bg);
        free(notify_gif_bg);
        return false;
    }

    if (pthread_mutex_init(&disp->lock, NULL) != 0) {
        free(pixels);
        free(bg_pixels);
        free(gif_bg_pixels);
        free(notify_px);
        free(notify_scroll_bg);
        free(notify_gif_bg);
        return false;
    }
    if (pthread_cond_init(&disp->event_cond, NULL) != 0) {
        pthread_mutex_destroy(&disp->lock);
        free(pixels);
        free(bg_pixels);
        free(gif_bg_pixels);
        free(notify_px);
        free(notify_scroll_bg);
        free(notify_gif_bg);
        return false;
    }
    if (pthread_mutex_init(&disp->png_lock, NULL) != 0) {
        pthread_cond_destroy(&disp->event_cond);
        pthread_mutex_destroy(&disp->lock);
        free(pixels);
        free(bg_pixels);
        free(gif_bg_pixels);
        free(notify_px);
        free(notify_scroll_bg);
        free(notify_gif_bg);
        return false;
    }

    disp->width = width;
    disp->height = height;
    disp->frame_bytes = frame_bytes;
    disp->pixels = pixels;
    disp->scroll.bg_pixels = bg_pixels;
    disp->gif.bg_pixels = gif_bg_pixels;
    disp->notify.saved_pixels = notify_px;
    disp->notify.saved_scroll_bg = notify_scroll_bg;
    disp->notify.saved_gif_bg = notify_gif_bg;
    disp->brightness = 100;
    disp->idle_brightness = 20;
    disp->demo_active = start_demo;
    disp->next_event_id = 1;
    return true;
}

void display_destroy(MarkeeDisplay *disp) {
    if (!disp) return;
    display_signal_shutdown(disp);
    display_notify_cancel_unlocked(disp);
    display_stop_gif_unlocked(disp);
    pthread_mutex_destroy(&disp->png_lock);
    pthread_cond_destroy(&disp->event_cond);
    pthread_mutex_destroy(&disp->lock);
    free(disp->pixels);
    free(disp->scroll.bg_pixels);
    free(disp->gif.bg_pixels);
    free(disp->notify.saved_pixels);
    free(disp->notify.saved_scroll_bg);
    free(disp->notify.saved_gif_bg);
    disp->pixels = NULL;
    disp->scroll.bg_pixels = NULL;
    disp->gif.bg_pixels = NULL;
    disp->notify.saved_pixels = NULL;
    disp->notify.saved_scroll_bg = NULL;
    disp->notify.saved_gif_bg = NULL;
}

const char *display_event_type_name(MarkeeEventType type) {
    switch (type) {
        case MARKEE_EVT_CLICK:  return "click";
        case MARKEE_EVT_BUTTON: return "button";
        case MARKEE_EVT_FRAME:  return "frame";
        default:                return "unknown";
    }
}

void display_push_event_unlocked(MarkeeDisplay *disp, MarkeeEventType type,
                                 int x, int y, const char *detail) {
    if (!disp) return;
    size_t slot;
    if (disp->event_count < MARKEE_EVENT_RING_CAP) {
        slot = (disp->event_head + disp->event_count) % MARKEE_EVENT_RING_CAP;
        disp->event_count++;
    } else {
        slot = disp->event_head;
        disp->event_head = (disp->event_head + 1) % MARKEE_EVENT_RING_CAP;
    }
    MarkeeEvent *ev = &disp->events[slot];
    ev->id = disp->next_event_id++;
    ev->type = type;
    ev->x = x;
    ev->y = y;
    snprintf(ev->detail, sizeof(ev->detail), "%s", detail ? detail : "");
    if (disp->has_tick_time) {
        disp->last_activity_time = disp->last_tick_time;
    }
    pthread_cond_broadcast(&disp->event_cond);
}

void display_push_event(MarkeeDisplay *disp, MarkeeEventType type,
                        int x, int y, const char *detail) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    display_push_event_unlocked(disp, type, x, y, detail);
    pthread_mutex_unlock(&disp->lock);
}

uint64_t display_get_last_event_id(MarkeeDisplay *disp) {
    if (!disp) return 0;
    pthread_mutex_lock(&disp->lock);
    uint64_t id = disp->next_event_id > 0 ? disp->next_event_id - 1 : 0;
    pthread_mutex_unlock(&disp->lock);
    return id;
}

static size_t collect_events_unlocked(const MarkeeDisplay *disp, uint64_t since_id,
                                      MarkeeEvent *out, size_t max_out) {
    size_t n = 0;
    for (size_t i = 0; i < disp->event_count && n < max_out; i++) {
        size_t idx = (disp->event_head + i) % MARKEE_EVENT_RING_CAP;
        if (disp->events[idx].id > since_id) {
            out[n++] = disp->events[idx];
        }
    }
    return n;
}

size_t display_get_events_since(MarkeeDisplay *disp, uint64_t since_id,
                                MarkeeEvent *out, size_t max_out) {
    if (!disp || !out || max_out == 0) return 0;
    pthread_mutex_lock(&disp->lock);
    size_t n = collect_events_unlocked(disp, since_id, out, max_out);
    pthread_mutex_unlock(&disp->lock);
    return n;
}

size_t display_wait_for_events(MarkeeDisplay *disp, uint64_t since_id, int timeout_ms,
                               MarkeeEvent *out, size_t max_out) {
    if (!disp || !out || max_out == 0) return 0;
    pthread_mutex_lock(&disp->lock);
    size_t n = collect_events_unlocked(disp, since_id, out, max_out);
    if (n == 0 && timeout_ms > 0 && !disp->shutting_down) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        while (!disp->shutting_down) {
            n = collect_events_unlocked(disp, since_id, out, max_out);
            if (n > 0) break;
            int rc = pthread_cond_timedwait(&disp->event_cond, &disp->lock, &ts);
            if (rc == ETIMEDOUT) {
                n = collect_events_unlocked(disp, since_id, out, max_out);
                break;
            }
        }
    }
    pthread_mutex_unlock(&disp->lock);
    return n;
}

void display_signal_shutdown(MarkeeDisplay *disp) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    disp->shutting_down = true;
    pthread_cond_broadcast(&disp->event_cond);
    pthread_mutex_unlock(&disp->lock);
}

void display_clear(MarkeeDisplay *disp, bool stop_demo) {
    if (!disp || !disp->pixels) return;
    pthread_mutex_lock(&disp->lock);
    display_notify_cancel_unlocked(disp);
    stop_scroll_unlocked(disp);
    display_stop_gif_unlocked(disp);
    memset(disp->pixels, 0, disp->frame_bytes);
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
        display_push_event_unlocked(disp, MARKEE_EVT_FRAME, -1, -1, "clear");
    }
    disp->frame_seq++;
    pthread_mutex_unlock(&disp->lock);
}

void display_fill(MarkeeDisplay *disp, uint8_t r, uint8_t g, uint8_t b, bool stop_demo) {
    if (!disp || !disp->pixels) return;
    pthread_mutex_lock(&disp->lock);
    display_notify_cancel_unlocked(disp);
    stop_scroll_unlocked(disp);
    display_stop_gif_unlocked(disp);
    for (size_t i = 0; i < disp->frame_bytes; i += 3) {
        disp->pixels[i + 0] = r;
        disp->pixels[i + 1] = g;
        disp->pixels[i + 2] = b;
    }
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
        display_push_event_unlocked(disp, MARKEE_EVT_FRAME, -1, -1, "fill");
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
    display_notify_cancel_unlocked(disp);
    stop_scroll_unlocked(disp);
    display_stop_gif_unlocked(disp);
    disp->pixels[idx + 0] = r;
    disp->pixels[idx + 1] = g;
    disp->pixels[idx + 2] = b;
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
        display_push_event_unlocked(disp, MARKEE_EVT_FRAME, x, y, "pixel");
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
    display_notify_cancel_unlocked(disp);
    stop_scroll_unlocked(disp);
    display_stop_gif_unlocked(disp);
    memcpy(disp->pixels, rgb, disp->frame_bytes);
    if (stop_demo) {
        disp->demo_active = false;
        disp->frames_received++;
        display_push_event_unlocked(disp, MARKEE_EVT_FRAME, -1, -1, "frame");
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

static uint8_t compute_effective_brightness_unlocked(const MarkeeDisplay *disp) {
    uint8_t b = disp->brightness;
    if (disp->idle_dim_sec <= 0.0 || !disp->has_tick_time) return b;
    double idle = disp->last_tick_time - disp->last_activity_time;
    if (idle < disp->idle_dim_sec) return b;
    uint8_t target = disp->idle_brightness < b ? disp->idle_brightness : b;
    double fade = (idle - disp->idle_dim_sec); /* 1-second smooth ramp */
    if (fade >= 1.0) return target;
    double cur = (double)b + ((double)target - (double)b) * fade;
    if (cur < 0.0) cur = 0.0;
    if (cur > 100.0) cur = 100.0;
    return (uint8_t)lround(cur);
}

bool display_snapshot(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap,
                      uint8_t *out_brightness) {
    if (!disp || !disp->pixels || !out_rgb || out_cap < disp->frame_bytes) return false;
    pthread_mutex_lock(&disp->lock);
    memcpy(out_rgb, disp->pixels, disp->frame_bytes);
    if (out_brightness) {
        *out_brightness = compute_effective_brightness_unlocked(disp);
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
    if (disp->has_tick_time) {
        disp->last_activity_time = disp->last_tick_time;
    }
    pthread_mutex_unlock(&disp->lock);
}

void display_set_idle_dim(MarkeeDisplay *disp, double timeout_sec, uint8_t idle_brightness) {
    if (!disp) return;
    if (timeout_sec < 0.0) timeout_sec = 0.0;
    if (idle_brightness > 100) idle_brightness = 100;
    pthread_mutex_lock(&disp->lock);
    disp->idle_dim_sec = timeout_sec;
    disp->idle_brightness = idle_brightness;
    if (disp->has_tick_time) {
        disp->last_activity_time = disp->last_tick_time;
    }
    pthread_mutex_unlock(&disp->lock);
}

void display_get_idle_dim(MarkeeDisplay *disp, double *timeout_sec,
                          uint8_t *idle_brightness, bool *is_dimmed) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    if (timeout_sec) *timeout_sec = disp->idle_dim_sec;
    if (idle_brightness) *idle_brightness = disp->idle_brightness;
    if (is_dimmed) {
        *is_dimmed = (disp->idle_dim_sec > 0.0 && disp->has_tick_time &&
                      (disp->last_tick_time - disp->last_activity_time) >= disp->idle_dim_sec);
    }
    pthread_mutex_unlock(&disp->lock);
}

void display_wake_activity(MarkeeDisplay *disp) {
    if (!disp) return;
    pthread_mutex_lock(&disp->lock);
    if (disp->has_tick_time) {
        disp->last_activity_time = disp->last_tick_time;
    }
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
    if (active) {
        display_notify_cancel_unlocked(disp);
        stop_scroll_unlocked(disp);
        display_stop_gif_unlocked(disp);
    }
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

bool display_decode_png_rgba(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                             uint8_t **out_rgba, int *out_w, int *out_h) {
    if (out_rgba) *out_rgba = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!disp || !png_data || !out_rgba || !out_w || !out_h ||
        png_len < sizeof(PNG_SIG) || png_len > MARKEE_MAX_BODY_BYTES) {
        return false;
    }
    if (memcmp(png_data, PNG_SIG, sizeof(PNG_SIG)) != 0) {
        return false;
    }

    pthread_mutex_lock(&disp->png_lock);
    Image img = LoadImageFromMemory(".png", png_data, (int)png_len);
    if (img.data == NULL || img.width <= 0 || img.height <= 0 ||
        img.width > MARKEE_GIF_MAX_SRC_DIM || img.height > MARKEE_GIF_MAX_SRC_DIM) {
        if (img.data) UnloadImage(img);
        pthread_mutex_unlock(&disp->png_lock);
        return false;
    }

    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    size_t nbytes = (size_t)img.width * (size_t)img.height * 4;
    uint8_t *copy = malloc(nbytes);
    if (!copy) {
        UnloadImage(img);
        pthread_mutex_unlock(&disp->png_lock);
        return false;
    }
    memcpy(copy, img.data, nbytes);
    *out_w = img.width;
    *out_h = img.height;
    *out_rgba = copy;

    UnloadImage(img);
    pthread_mutex_unlock(&disp->png_lock);
    return true;
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

MarkeeResult display_load_gif(MarkeeDisplay *disp, const uint8_t *gif_data, size_t gif_len,
                              int loop_override,
                              uint8_t fill_r, uint8_t fill_g, uint8_t fill_b,
                              MarkeeGifStatus *out_status,
                              char *err_buf, size_t err_size) {
    if (out_status) memset(out_status, 0, sizeof(*out_status));
    if (err_buf && err_size > 0) err_buf[0] = '\0';

    if (!disp || !disp->pixels || !gif_data || gif_len == 0 ||
        gif_len > MARKEE_MAX_BODY_BYTES) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "invalid GIF data");
        return MARKEE_ERR_INVALID_GIF;
    }

    MarkeeGif *gif = NULL;
    if (!gif_decode_ex(gif_data, gif_len, disp->width, disp->height,
                       fill_r, fill_g, fill_b, &gif, err_buf, err_size)) {
        return MARKEE_ERR_INVALID_GIF;
    }

    if (loop_override >= 0) {
        gif->loop_count = (gif->frame_count <= 1) ? 1 : loop_override;
    }

    MarkeeGifStatus status;
    memset(&status, 0, sizeof(status));
    status.active = (gif->frame_count > 1);
    status.src_width = gif->src_width;
    status.src_height = gif->src_height;
    status.frame_count = gif->frame_count;
    status.current_frame = 0;
    status.total_duration_ms = gif->total_duration_ms;
    status.loop_count = gif->loop_count;
    status.fill_r = gif->bg_r;
    status.fill_g = gif->bg_g;
    status.fill_b = gif->bg_b;

    pthread_mutex_lock(&disp->lock);
    display_notify_cancel_unlocked(disp);
    stop_scroll_unlocked(disp);
    display_stop_gif_unlocked(disp);

    gif_blit_frame(gif, 0, disp->pixels, disp->width, disp->height);
    if (gif->frame_count > 1) {
        if (disp->gif.bg_pixels) {
            for (size_t i = 0; i < disp->frame_bytes; i += 3) {
                disp->gif.bg_pixels[i + 0] = fill_r;
                disp->gif.bg_pixels[i + 1] = fill_g;
                disp->gif.bg_pixels[i + 2] = fill_b;
            }
        }
        disp->gif.layers[0].gif = gif;
        disp->gif.layers[0].current_frame = 0;
        disp->gif.layers[0].loops_done = 0;
        disp->gif.layers[0].finished = false;
        disp->gif.layers[0].accum_ms = 0.0;
        disp->gif.count = 1;
        disp->gif.active = true;
        disp->gif.has_last_time = false;
        disp->gif.last_time = 0.0;
    } else {
        gif_free(gif);
    }

    disp->demo_active = false;
    disp->frames_received++;
    disp->frame_seq++;
    display_push_event_unlocked(disp, MARKEE_EVT_FRAME, -1, -1, "animate");
    pthread_mutex_unlock(&disp->lock);

    if (out_status) *out_status = status;
    return MARKEE_OK;
}

bool display_get_gif_status(MarkeeDisplay *disp, MarkeeGifStatus *out_status) {
    if (!disp || !out_status) return false;
    memset(out_status, 0, sizeof(*out_status));
    pthread_mutex_lock(&disp->lock);
    bool active = disp->gif.active && disp->gif.count > 0 && disp->gif.layers[0].gif != NULL;
    if (active) {
        size_t idx = 0;
        for (size_t i = 0; i < disp->gif.count; i++) {
            if (!disp->gif.layers[i].finished && disp->gif.layers[i].gif) {
                idx = i;
                break;
            }
        }
        const MarkeeGifLayer *L = &disp->gif.layers[idx];
        out_status->active = true;
        out_status->src_width = L->gif->src_width;
        out_status->src_height = L->gif->src_height;
        out_status->frame_count = L->gif->frame_count;
        out_status->current_frame = L->current_frame;
        out_status->total_duration_ms = L->gif->total_duration_ms;
        out_status->loop_count = L->gif->loop_count;
        out_status->fill_r = L->gif->bg_r;
        out_status->fill_g = L->gif->bg_g;
        out_status->fill_b = L->gif->bg_b;
    }
    pthread_mutex_unlock(&disp->lock);
    return active;
}

bool display_is_gif_active(MarkeeDisplay *disp) {
    if (!disp) return false;
    pthread_mutex_lock(&disp->lock);
    bool active = disp->gif.active && disp->gif.count > 0;
    pthread_mutex_unlock(&disp->lock);
    return active;
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

static void tick_animations(MarkeeDisplay *disp, double time_sec) {
    pthread_mutex_lock(&disp->lock);
    bool gif_active = disp->gif.active && disp->gif.count > 0;
    bool scroll_active = disp->scroll.active && disp->scroll.bg_pixels != NULL;
    if (!gif_active && !scroll_active) {
        pthread_mutex_unlock(&disp->lock);
        return;
    }

    bool gif_changed = false;
    bool all_gifs_done = false;

    if (gif_active) {
        if (!disp->gif.has_last_time) {
            disp->gif.last_time = time_sec;
            disp->gif.has_last_time = true;
        } else {
            double dt_ms = (time_sec - disp->gif.last_time) * 1000.0;
            disp->gif.last_time = time_sec;
            if (dt_ms > 0.0) {
                size_t still_running = 0;
                for (size_t i = 0; i < disp->gif.count; i++) {
                    MarkeeGifLayer *L = &disp->gif.layers[i];
                    if (L->finished || !L->gif || L->gif->frame_count <= 1) {
                        continue;
                    }
                    MarkeeGif *g = L->gif;
                    L->accum_ms += dt_ms;
                    if (g->loop_count == 0 && g->total_duration_ms > 0 &&
                        L->accum_ms >= (double)g->total_duration_ms) {
                        L->accum_ms = fmod(L->accum_ms, (double)g->total_duration_ms);
                    }
                    while (!L->finished) {
                        double delay = (double)g->delays_ms[L->current_frame];
                        if (delay < 1.0) delay = 100.0;
                        if (L->accum_ms < delay) break;

                        L->accum_ms -= delay;
                        if (L->current_frame + 1 < g->frame_count) {
                            L->current_frame++;
                            gif_changed = true;
                        } else {
                            L->loops_done++;
                            if (g->loop_count > 0 && L->loops_done >= g->loop_count) {
                                L->finished = true;
                                break;
                            }
                            L->current_frame = 0;
                            gif_changed = true;
                        }
                    }
                    if (!L->finished) still_running++;
                }
                if (still_running == 0) {
                    all_gifs_done = true;
                }
            }
        }
    }

    if (gif_changed || all_gifs_done || scroll_active) {
        if (gif_active && disp->gif.bg_pixels) {
            memcpy(disp->pixels, disp->gif.bg_pixels, disp->frame_bytes);
            for (size_t i = 0; i < disp->gif.count; i++) {
                MarkeeGifLayer *L = &disp->gif.layers[i];
                if (L->gif) {
                    gif_blit_frame(L->gif, L->current_frame,
                                   disp->pixels, disp->width, disp->height);
                }
            }
        } else if (scroll_active) {
            memcpy(disp->pixels, disp->scroll.bg_pixels, disp->frame_bytes);
        }

        if (all_gifs_done) {
            /* Bake final frames into scroll background (if scrolling continues) and free GIFs. */
            if (scroll_active && disp->scroll.bg_pixels) {
                memcpy(disp->scroll.bg_pixels, disp->pixels, disp->frame_bytes);
            }
            display_stop_gif_unlocked(disp);
        }

        if (scroll_active) {
            if (!disp->scroll.has_start_time) {
                disp->scroll.start_time = time_sec;
                disp->scroll.has_start_time = true;
            }
            double elapsed = time_sec - disp->scroll.start_time;
            if (elapsed < 0.0) elapsed = 0.0;

            int offset = (int)floor(elapsed * disp->scroll.speed);
            int text_w = disp->scroll.text_w > 0 ? disp->scroll.text_w : 1;
            int region_x = disp->scroll.has_clip ? disp->scroll.clip_x : 0;
            int region_y = disp->scroll.has_clip ? disp->scroll.clip_y : 0;
            int region_w = disp->scroll.has_clip ? disp->scroll.clip_w : disp->width;
            int region_h = disp->scroll.has_clip ? disp->scroll.clip_h : disp->height;
            int span = region_w + text_w;
            int rel_start = disp->scroll.start_x - region_x;
            int draw_x = region_x + (((rel_start - offset + text_w) % span + span) % span - text_w);

            MarkeeColor col = {disp->scroll.r, disp->scroll.g, disp->scroll.b};
            draw_render_text_buf_clipped(disp->pixels, disp->width, disp->height,
                                         draw_x, disp->scroll.y,
                                         disp->scroll.text, disp->scroll.font, col,
                                         region_x, region_y, region_w, region_h);
        }

        disp->frame_seq++;
    }

    pthread_mutex_unlock(&disp->lock);
}

void display_tick(MarkeeDisplay *disp, double time_sec) {
    if (!disp || !disp->pixels) return;

    pthread_mutex_lock(&disp->lock);
    if (!disp->has_tick_time) {
        disp->has_tick_time = true;
        disp->last_activity_time = time_sec;
    }
    disp->last_tick_time = time_sec;

    if (disp->notify.active) {
        if (!disp->notify.has_start_time) {
            disp->notify.start_time = time_sec;
            disp->notify.has_start_time = true;
        } else if ((time_sec - disp->notify.start_time) >= disp->notify.duration_sec) {
            display_notify_restore_unlocked(disp);
        }
    }
    pthread_mutex_unlock(&disp->lock);

    if (!display_is_demo_active(disp)) {
        tick_animations(disp, time_sec);
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
