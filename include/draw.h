#ifndef MARKEE_DRAW_H
#define MARKEE_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} MarkeeColor;

/* Parse a color string: hex with optional "#" or "0x" prefix in 3- or 6-digit form
 * ("#FFF", "#00FF00", "0x00ff00", "00ff00") or a named color ("white", "amber", ...). */
bool draw_parse_color_string(const char *s, MarkeeColor *out);

/* Render bitmap text directly into an RGB pixel buffer (returns pixel width of rendered text).
 * Supported fonts: "4x6" ("small"), "5x7", "6x8" ("medium"/"default"), "8x8" ("large"). */
int draw_measure_text(const char *text, const char *font);
void draw_render_text_buf(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, const char *text, const char *font,
                          MarkeeColor color);

/* Execute a /draw JSON payload against the display.
 * The JSON body must be a non-empty array of one or more draw operation objects:
 *   [ { "op": "rect", ... }, { "op": "text", ... } ]
 * Supported ops: line, rect, oval, polygon, text, pixel, clear, image.
 * Execution is atomic: if any op in the array is invalid, the display is unchanged.
 * On success, returns true and sets *out_ops_executed (if non-NULL).
 * On error, returns false and writes a human-readable message into err_buf. */
bool draw_exec_json(MarkeeDisplay *disp, const char *json_body, size_t json_len,
                    int *out_ops_executed, char *err_buf, size_t err_size);

#endif /* MARKEE_DRAW_H */
