#ifndef MARKEE_RENDERER_H
#define MARKEE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    int window_w;
    int window_h;
    float dpi;

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

    /* Recessed 72x16 LED matrix active area */
    float matrix_x;
    float matrix_y;
    float matrix_w;
    float matrix_h;

    float cell_pitch;
} MarkeeLayout;

/* Compute layout for a given logical DPI (or auto-detect when override_dpi <= 0). */
MarkeeLayout renderer_compute_layout(float override_dpi, int monitor_w, int monitor_phys_w_mm);

/* Initialize raylib window calibrated so the LED matrix appears ~6.35 inches wide. */
MarkeeLayout renderer_init_window(const char *host, uint16_t port, float override_dpi);

/* Draw one frame of the plastic-encased 72x16 LED display. */
void renderer_draw(const MarkeeLayout *layout,
                   const uint8_t pixels[MARKEE_FRAME_BYTES],
                   uint8_t brightness);

#endif /* MARKEE_RENDERER_H */
