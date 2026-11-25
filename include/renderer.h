#ifndef MARKEE_RENDERER_H
#define MARKEE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    MARKEE_PIXEL_SQUARE = 0,
    MARKEE_PIXEL_CIRCLE
} MarkeePixelShape;

typedef struct {
    int width;
    int height;
    float zoom;
    MarkeePixelShape pixel_shape;

    int window_w;
    int window_h;
    float dpi;

    /* Reference 72x16 body height at current DPI*zoom (for consistent corner radii) */
    float ref_body_h;

    /* Outer plastic casing rect */
    float body_x;
    float body_y;
    float body_w;
    float body_h;

    /* Black front polycarbonate faceplate rect */
    float face_x;
    float face_y;
    float face_w;
    float face_h;

    /* Recessed WxH LED matrix active area */
    float matrix_x;
    float matrix_y;
    float matrix_w;
    float matrix_h;

    float cell_pitch;
} MarkeeLayout;

typedef struct {
    bool always_on_top;
} MarkeeWindowOpts;

/* Compute layout for resolution (width x height), zoom factor, pixel shape, and logical DPI
 * (or auto-detect DPI when override_dpi <= 0). */
MarkeeLayout renderer_compute_layout(int width, int height, float zoom,
                                     MarkeePixelShape pixel_shape,
                                     float override_dpi,
                                     int monitor_w, int monitor_phys_w_mm);

/* Initialize raylib window calibrated for the given resolution, zoom level, and pixel shape. */
MarkeeLayout renderer_init_window(int width, int height, float zoom,
                                  MarkeePixelShape pixel_shape,
                                  const char *host, uint16_t port,
                                  float override_dpi);

/* Extended window initializer accepting window behavior options (always-on-top, etc.). */
MarkeeLayout renderer_init_window_ex(int width, int height, float zoom,
                                     MarkeePixelShape pixel_shape,
                                     const char *host, uint16_t port,
                                     float override_dpi,
                                     const MarkeeWindowOpts *opts);

/* Toggle the OS always-on-top window flag at runtime. */
void renderer_set_always_on_top(bool enabled);

/* Draw one frame of the plastic-encased LED display. */
void renderer_draw(const MarkeeLayout *layout,
                   const uint8_t *pixels,
                   uint8_t brightness);

#endif /* MARKEE_RENDERER_H */
