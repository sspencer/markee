#ifndef MARKEE_GIF_H
#define MARKEE_GIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Safety bounds on decoded GIF animations */
#define MARKEE_GIF_MAX_SRC_DIM   4096
#define MARKEE_GIF_MAX_SRC_PIXELS (16UL * 1024UL * 1024UL)
#define MARKEE_GIF_MAX_FRAMES    4096
#define MARKEE_GIF_MAX_BYTES     (32UL * 1024UL * 1024UL)

typedef struct {
    int src_width;           /* Original GIF logical screen width */
    int src_height;          /* Original GIF logical screen height */
    int dst_x;               /* Destination X on display (0 for centered full-screen decode) */
    int dst_y;               /* Destination Y on display (0 for centered full-screen decode) */
    int width;               /* Stored frame width (dst_w for centered, clipped vis_w for placed) */
    int height;              /* Stored frame height (dst_h for centered, clipped vis_h for placed) */
    bool has_alpha;          /* false = RGB24 (3 Bpp), true = RGBA32 (4 Bpp, A=0 transparent) */
    size_t frame_count;      /* Number of decoded frames (>= 1) */
    size_t frame_bytes;      /* width * height * (has_alpha ? 4 : 3) */
    uint8_t *frames;         /* Contiguous frame_count * frame_bytes pixels */
    uint32_t *delays_ms;     /* Per-frame display duration in ms (>= 20 ms) */
    uint64_t total_duration_ms; /* Sum of delays_ms across all frames */
    int loop_count;          /* 0 = infinite loop; >0 = total number of plays */
    bool has_fill;           /* true if transparent pixels were composited over bg color */
    uint8_t bg_r, bg_g, bg_b; /* Fill color composited beneath transparent pixels & margins */
} MarkeeGif;

/* Check for the 6-byte GIF87a or GIF89a header signature. */
bool gif_has_magic(const uint8_t *data, size_t len);

/* Decode an animated or static GIF87a/GIF89a buffer into display-sized RGB24 frames,
 * compositing transparent pixels and centered margins over (bg_r, bg_g, bg_b).
 * On each axis independently:
 *   - If the GIF is larger than the display, it is center-cropped.
 *   - If the GIF is smaller than the display, it is centered on the fill color.
 * Supports Global and per-frame Local Color Tables, interlacing, 1-bit transparency,
 * all 4 GIF89a disposal methods, per-frame Graphic Control Extension delays (clamping
 * < 20 ms to 100 ms per standard browser behavior), and NETSCAPE2.0 loop counts.
 *
 * On success, returns true and stores a heap-allocated MarkeeGif in *out_gif
 * (caller frees with gif_free()). On failure, returns false and writes a
 * human-readable reason into err_buf (if non-NULL). */
typedef enum {
    MARKEE_GIF_FIT_CROP = 0,   /* Center-crop if larger, center if smaller (default) */
    MARKEE_GIF_FIT_CONTAIN,    /* Scale preserving aspect ratio to fit inside display */
    MARKEE_GIF_FIT_STRETCH     /* Scale to fill exact display dimensions */
} MarkeeGifFitMode;

bool gif_parse_fit_mode(const char *s, MarkeeGifFitMode *out);

bool gif_decode_fit(const uint8_t *data, size_t len, int dst_w, int dst_h,
                    MarkeeGifFitMode fit,
                    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                    MarkeeGif **out_gif, char *err_buf, size_t err_size);

bool gif_decode_ex(const uint8_t *data, size_t len, int dst_w, int dst_h,
                   uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                   MarkeeGif **out_gif, char *err_buf, size_t err_size);

/* Convenience wrapper for gif_decode_ex() with black (0, 0, 0) fill background. */
bool gif_decode(const uint8_t *data, size_t len, int dst_w, int dst_h,
                MarkeeGif **out_gif, char *err_buf, size_t err_size);

/* Decode a GIF placed at (place_x, place_y) on a dst_w x dst_h display, clipped to the display
 * bounds. If has_fill is true, transparent pixels inside the GIF rectangle are filled with
 * (bg_r, bg_g, bg_b) (RGB24 output); if has_fill is false, transparent pixels preserve alpha=0
 * (RGBA32 output) so gif_blit_frame() reveals underlying display pixels. */
bool gif_decode_placed(const uint8_t *data, size_t len, int dst_w, int dst_h,
                       int place_x, int place_y,
                       bool has_fill, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                       MarkeeGif **out_gif, char *err_buf, size_t err_size);

/* Composite frame `frame_idx` of a decoded MarkeeGif onto an RGB24 framebuffer. */
void gif_blit_frame(const MarkeeGif *gif, size_t frame_idx,
                    uint8_t *dst_rgb, int dst_w, int dst_h);

/* Release a decoded MarkeeGif. Safe to call with NULL. */
void gif_free(MarkeeGif *gif);

#endif /* MARKEE_GIF_H */
