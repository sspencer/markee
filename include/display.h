#ifndef MARKEE_DISPLAY_H
#define MARKEE_DISPLAY_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    MARKEE_OK = 0,
    MARKEE_ERR_INVALID_ARG,
    MARKEE_ERR_INVALID_PNG,
    MARKEE_ERR_WRONG_DIMENSIONS,
    MARKEE_ERR_ENCODE_FAILED,
    MARKEE_ERR_OUT_OF_RANGE
} MarkeeResult;

typedef struct {
    pthread_mutex_t lock;
    pthread_mutex_t png_lock;
    int width;
    int height;
    size_t frame_bytes;                 /* width * height * 3 */
    uint8_t *pixels;                    /* Row-major WxH 24-bit RGB */
    uint8_t brightness;                 /* 0..100 */
    bool demo_active;                   /* Startup animation enabled until API write */
    uint64_t frames_received;
    uint64_t frame_seq;
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

/* Brightness (0..100) and demo controls. */
void display_set_brightness(MarkeeDisplay *disp, uint8_t brightness);
uint8_t display_get_brightness(MarkeeDisplay *disp);
void display_set_demo(MarkeeDisplay *disp, bool active);
bool display_is_demo_active(MarkeeDisplay *disp);
uint64_t display_get_frames_received(MarkeeDisplay *disp);

/* PNG I/O: decode an exact WxH PNG into the framebuffer, or encode current pixels as a WxH 24-bit PNG.
 * On success for display_encode_png(), caller must free *out_png with display_free_buffer(). */
MarkeeResult display_load_png(MarkeeDisplay *disp, const uint8_t *png_data, size_t png_len,
                              int *actual_w, int *actual_h);
MarkeeResult display_encode_png(MarkeeDisplay *disp, uint8_t **out_png, size_t *out_len);
void display_free_buffer(uint8_t *buf);

/* Update the built-in startup demo animation for time_sec (no-op if demo_active is false). */
void display_tick_demo(MarkeeDisplay *disp, double time_sec);

#endif /* MARKEE_DISPLAY_H */
