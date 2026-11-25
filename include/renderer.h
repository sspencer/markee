#ifndef MARKEE_RENDERER_H
#define MARKEE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    MARKEE_PIXEL_SQUARE = 0,
    MARKEE_PIXEL_CIRCLE
} MarkeePixelShape;

typedef enum {
    MARKEE_CASE_WHITE = 0,
    MARKEE_CASE_CHARCOAL,
    MARKEE_CASE_ORANGE,
    MARKEE_CASE_RETRO,
    MARKEE_CASE_COUNT
} MarkeeCaseTheme;

typedef struct {
    int width;
    int height;
    float zoom;
    MarkeePixelShape pixel_shape;
    MarkeeCaseTheme case_theme;
    bool borderless;

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

typedef enum {
    MARKEE_CORNER_CENTER = 0,
    MARKEE_CORNER_TOP_LEFT,
    MARKEE_CORNER_TOP_RIGHT,
    MARKEE_CORNER_BOTTOM_LEFT,
    MARKEE_CORNER_BOTTOM_RIGHT,
    MARKEE_CORNER_TOP,
    MARKEE_CORNER_BOTTOM
} MarkeeWindowCorner;

typedef struct {
    bool always_on_top;
    bool borderless;
    bool has_position;
    int pos_x;
    int pos_y;
    MarkeeWindowCorner corner;
    MarkeeCaseTheme case_theme;
} MarkeeWindowOpts;

/* Parse an enclosure color theme ("white", "charcoal"/"black"/"dark", "orange", "retro"/"beige"). */
bool renderer_parse_case_theme(const char *s, MarkeeCaseTheme *out_theme);
const char *renderer_case_theme_name(MarkeeCaseTheme theme);

/* Parse a corner placement name ("center", "top-left"/"tl", "top-right"/"tr",
 * "bottom-left"/"bl", "bottom-right"/"br", "top", "bottom"). */
bool renderer_parse_corner(const char *s, MarkeeWindowCorner *out_corner);

/* Compute initial window (x, y) on a mon_w x mon_h monitor from opts. */
void renderer_compute_window_position(int mon_w, int mon_h, int win_w, int win_h,
                                      const MarkeeWindowOpts *opts,
                                      int *out_x, int *out_y);

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

/* Handle click-and-drag window repositioning when running in --borderless mode.
 * Returns true while a drag gesture is actively moving the window. */
bool renderer_handle_drag(const MarkeeLayout *layout);

/* Draw one frame of the plastic-encased LED display. */
void renderer_draw(const MarkeeLayout *layout,
                   const uint8_t *pixels,
                   uint8_t brightness);

#endif /* MARKEE_RENDERER_H */
