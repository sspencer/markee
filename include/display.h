#ifndef MARKEE_DISPLAY_H
#define MARKEE_DISPLAY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "gif.h"

typedef enum {
    MARKEE_OK = 0,
    MARKEE_ERR_INVALID_ARG,
    MARKEE_ERR_INVALID_PNG,
    MARKEE_ERR_INVALID_GIF,
    MARKEE_ERR_WRONG_DIMENSIONS,
    MARKEE_ERR_ENCODE_FAILED,
    MARKEE_ERR_OUT_OF_RANGE
} MarkeeResult;

typedef struct {
    bool active;
    char text[512];
    char font[16];
    int start_x;
    int y;
    int text_w;
    uint8_t r, g, b;
    double speed;       /* pixels per second */
    bool has_start_time;
    double start_time;
    uint8_t *bg_pixels; /* background snapshot underneath scrolling text */
} MarkeeScrollState;

#define MARKEE_MAX_GIF_LAYERS 8

typedef struct {
    MarkeeGif *gif;
    size_t current_frame;
    int loops_done;
    bool finished;
    double accum_ms;
} MarkeeGifLayer;

typedef struct {
    bool active;
    size_t count;
    MarkeeGifLayer layers[MARKEE_MAX_GIF_LAYERS];
    bool has_last_time;
    double last_time;
    uint8_t *bg_pixels; /* static background snapshot beneath animated GIF layers */
} MarkeeGifAnimState;

typedef struct {
    bool active;
    int src_width;
    int src_height;
    size_t frame_count;
    size_t current_frame;
    uint64_t total_duration_ms;
    int loop_count;
    uint8_t fill_r;
    uint8_t fill_g;
    uint8_t fill_b;
} MarkeeGifStatus;

typedef struct {
    pthread_mutex_t lock;
    pthread_mutex_t png_lock;
    int width;
    int height;
    size_t frame_bytes;                 /* width * height * 3 */
    uint8_t *pixels;                    /* Row-major WxH 24-bit RGB */
    uint8_t brightness;                 /* 0..100 */
    bool mono_enabled;                  /* Non-destructive BT.709 monochrome tint filter */
    uint8_t mono_r;
    uint8_t mono_g;
    uint8_t mono_b;
    bool demo_active;                   /* Startup animation enabled until API write */
    uint64_t frames_received;
    uint64_t frame_seq;
    MarkeeScrollState scroll;
    MarkeeGifAnimState gif;
} MarkeeDisplay;

/* Initialize and destroy display state for the given resolution (width x height). */
bool display_init(MarkeeDisplay *disp, int width, int height, bool start_demo);
void display_destroy(MarkeeDisplay *disp);

/* Thread-safe raw 24-bit RGB framebuffer operations. */
void display_clear(MarkeeDisplay *disp, bool stop_demo);
void display_fill(MarkeeDisplay *disp, uint8_t r, uint8_t g, uint8_t b, bool stop_demo);
MarkeeResult display_set_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b, bool stop_demo);
MarkeeResult display_get_pixel(MarkeeDisplay *disp, int x, int y,
                               uint8_t *r, uint8_t *g, uint8_t *b);
MarkeeResult display_set_raw_rgb(MarkeeDisplay *disp, const uint8_t *rgb, size_t len,
                                 bool stop_demo);
bool display_get_raw_rgb(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap);

/* Fast snapshot for the 60 Hz renderer. */
bool display_snapshot(MarkeeDisplay *disp, uint8_t *out_rgb, size_t out_cap,
                      uint8_t *out_brightness);

/* Brightness (0..100), monochrome filter, and demo controls. */
void display_set_brightness(MarkeeDisplay *disp, uint8_t brightness);
uint8_t display_get_brightness(MarkeeDisplay *disp);
void display_set_monochrome(MarkeeDisplay *disp, bool enabled,
                            uint8_t r, uint8_t g, uint8_t b);
void display_get_monochrome(MarkeeDisplay *disp, bool *enabled,
                            uint8_t *r, uint8_t *g, uint8_t *b);
void display_set_demo(MarkeeDisplay *disp, bool active);
bool display_is_demo_active(MarkeeDisplay *disp);
uint64_t display_get_frames_received(MarkeeDisplay *disp);

/* PNG I/O: decode an exact WxH PNG into the framebuffer, decode any PNG to RGBA32 (caller frees
 * *out_rgba with free()), or encode current pixels as a WxH 24-bit PNG (caller frees *out_png
 * with display_free_buffer()). */
MarkeeResult display_load_png(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                              int *actual_w, int *actual_h);
bool display_decode_png_rgba(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                             uint8_t **out_rgba, int *out_w, int *out_h);
MarkeeResult display_encode_png(MarkeeDisplay *disp, uint8_t **out_png, size_t *out_len);
void display_free_buffer(uint8_t *buf);

/* GIF animation I/O: decode an animated/static GIF (center-cropped if too big, centered if too
 * small), compositing transparent pixels and margins over (fill_r, fill_g, fill_b).
 * Displays frame 0 immediately and advances frames in display_tick().
 * Pass loop_override < 0 to honor the GIF's own loop setting (0 = infinite, >0 = total plays). */
MarkeeResult display_load_gif(MarkeeDisplay *disp, const uint8_t *gif_data, size_t gif_len,
                              int loop_override,
                              uint8_t fill_r, uint8_t fill_g, uint8_t fill_b,
                              MarkeeGifStatus *out_status,
                              char *err_buf, size_t err_size);
void display_stop_gif(MarkeeDisplay *disp);
void display_stop_gif_unlocked(MarkeeDisplay *disp);
bool display_get_gif_status(MarkeeDisplay *disp, MarkeeGifStatus *out_status);
bool display_is_gif_active(MarkeeDisplay *disp);

/* Advance 60 Hz animations (startup demo, active GIF animation, or active scrolling text). */
void display_tick(MarkeeDisplay *disp, double time_sec);

/* Backward-compatible alias for display_tick(). */
void display_tick_demo(MarkeeDisplay *disp, double time_sec);

#endif /* MARKEE_DISPLAY_H */
