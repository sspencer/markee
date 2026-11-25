#define _POSIX_C_SOURCE 200809L

#include "gif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"

bool gif_parse_fit_mode(const char *s, MarkeeGifFitMode *out) {
    if (!s || !*s || !out) return false;
    if (strcasecmp(s, "crop") == 0 || strcasecmp(s, "center") == 0) {
        *out = MARKEE_GIF_FIT_CROP;
        return true;
    }
    if (strcasecmp(s, "contain") == 0 || strcasecmp(s, "fit") == 0) {
        *out = MARKEE_GIF_FIT_CONTAIN;
        return true;
    }
    if (strcasecmp(s, "stretch") == 0 || strcasecmp(s, "fill") == 0) {
        *out = MARKEE_GIF_FIT_STRETCH;
        return true;
    }
    return false;
}

#define GIF_LZW_MAX_BITS   12
#define GIF_LZW_TABLE_SIZE (1U << GIF_LZW_MAX_BITS) /* 4096 */

static void set_err(char *err_buf, size_t err_size, const char *msg) {
    if (err_buf && err_size > 0) {
        snprintf(err_buf, err_size, "%s", msg ? msg : "invalid GIF data");
    }
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool gif_has_magic(const uint8_t *data, size_t len) {
    if (!data || len < 6) return false;
    return memcmp(data, "GIF89a", 6) == 0 || memcmp(data, "GIF87a", 6) == 0;
}

/* Skip a sequence of GIF data sub-blocks ending in a 0-length terminator. */
static bool skip_sub_blocks(const uint8_t *data, size_t len, size_t *pos) {
    for (;;) {
        if (*pos >= len) return false;
        uint8_t sz = data[(*pos)++];
        if (sz == 0) return true;
        if (sz > len - *pos) return false;
        *pos += sz;
    }
}

/* Streaming bit reader across GIF LZW sub-blocks (LSB-first). */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint8_t block_rem;
    bool hit_terminator;
    uint32_t bit_buf;
    int bit_count;
} GifBitReader;

static bool bit_reader_read_code(GifBitReader *br, int code_size, int *out_code) {
    while (br->bit_count < code_size) {
        if (br->block_rem == 0) {
            if (br->hit_terminator || br->pos >= br->len) {
                return false;
            }
            uint8_t sz = br->data[br->pos++];
            if (sz == 0) {
                br->hit_terminator = true;
                return false;
            }
            if (sz > br->len - br->pos) {
                return false;
            }
            br->block_rem = sz;
        }
        br->bit_buf |= (uint32_t)br->data[br->pos++] << br->bit_count;
        br->bit_count += 8;
        br->block_rem--;
    }

    *out_code = (int)(br->bit_buf & ((1U << code_size) - 1U));
    br->bit_buf >>= code_size;
    br->bit_count -= code_size;
    return true;
}

static bool bit_reader_finish(GifBitReader *br) {
    /* Advance past remaining bytes in current sub-block and any trailing sub-blocks. */
    if (br->block_rem > 0) {
        if (br->block_rem > br->len - br->pos) return false;
        br->pos += br->block_rem;
        br->block_rem = 0;
    }
    if (br->hit_terminator) return true;
    return skip_sub_blocks(br->data, br->len, &br->pos);
}

/* Decode one LZW-compressed GIF image frame into `out_indices` (fw * fh bytes).
 * Advances *pos past the image's sub-block terminator. */
static bool decode_lzw_frame(const uint8_t *data, size_t len, size_t *pos,
                             uint8_t min_code_size,
                             uint8_t *out_indices, size_t pixel_count,
                             char *err_buf, size_t err_size) {
    if (min_code_size < 2 || min_code_size > 11) {
        set_err(err_buf, err_size, "invalid GIF LZW minimum code size");
        return false;
    }

    const int clear_code = 1 << min_code_size;
    const int eoi_code = clear_code + 1;
    int code_size = min_code_size + 1;
    int next_code = eoi_code + 1;
    int prev_code = -1;
    uint8_t first_char = 0;

    uint16_t prefix[GIF_LZW_TABLE_SIZE];
    uint8_t suffix[GIF_LZW_TABLE_SIZE];
    uint8_t stack[GIF_LZW_TABLE_SIZE];

    memset(prefix, 0, sizeof(prefix));
    for (int i = 0; i < clear_code; i++) {
        suffix[i] = (uint8_t)i;
    }

    GifBitReader br;
    memset(&br, 0, sizeof(br));
    br.data = data;
    br.len = len;
    br.pos = *pos;

    size_t out_pos = 0;

    for (;;) {
        int code = 0;
        if (!bit_reader_read_code(&br, code_size, &code)) {
            /* Some encoders omit EOI once all pixels are emitted. */
            if (out_pos >= pixel_count) break;
            set_err(err_buf, err_size, "truncated GIF LZW stream");
            return false;
        }

        if (code == clear_code) {
            code_size = min_code_size + 1;
            next_code = eoi_code + 1;
            prev_code = -1;
            continue;
        }

        if (code == eoi_code) {
            break;
        }

        if (prev_code == -1) {
            if (code >= clear_code) {
                set_err(err_buf, err_size, "corrupt GIF LZW initial code");
                return false;
            }
            if (out_pos < pixel_count) {
                out_indices[out_pos++] = (uint8_t)code;
            }
            prev_code = code;
            first_char = (uint8_t)code;
            continue;
        }

        int in_code = code;
        size_t sp = 0;

        if (code == next_code) {
            stack[sp++] = first_char;
            code = prev_code;
        } else if (code > next_code) {
            set_err(err_buf, err_size, "corrupt GIF LZW code out of range");
            return false;
        }

        while (code >= clear_code) {
            if (sp >= GIF_LZW_TABLE_SIZE) {
                set_err(err_buf, err_size, "corrupt GIF LZW table cycle");
                return false;
            }
            stack[sp++] = suffix[code];
            code = prefix[code];
        }

        first_char = (uint8_t)code;
        if (sp >= GIF_LZW_TABLE_SIZE) {
            set_err(err_buf, err_size, "corrupt GIF LZW stack overflow");
            return false;
        }
        stack[sp++] = first_char;

        while (sp > 0) {
            uint8_t px = stack[--sp];
            if (out_pos < pixel_count) {
                out_indices[out_pos++] = px;
            }
        }

        if (next_code < (int)GIF_LZW_TABLE_SIZE) {
            prefix[next_code] = (uint16_t)prev_code;
            suffix[next_code] = first_char;
            next_code++;
            if (next_code == (1 << code_size) && code_size < GIF_LZW_MAX_BITS) {
                code_size++;
            }
        }

        prev_code = in_code;
    }

    if (!bit_reader_finish(&br)) {
        set_err(err_buf, err_size, "unterminated GIF image data sub-blocks");
        return false;
    }

    *pos = br.pos;
    return true;
}

/* Map interlaced scanline index `row` (0..fh-1) to actual y offset within the frame. */
static int deinterlace_row(int row, int fh) {
    static const int pass_start[4] = {0, 4, 2, 1};
    static const int pass_step[4] = {8, 8, 4, 2};
    int rem = row;
    for (int p = 0; p < 4; p++) {
        if (fh <= pass_start[p]) continue;
        int count = (fh - 1 - pass_start[p]) / pass_step[p] + 1;
        if (rem < count) {
            return pass_start[p] + rem * pass_step[p];
        }
        rem -= count;
    }
    return row;
}

/* Extract the visible region of the RGBA logical screen canvas into one output frame.
 * - Full-screen centered mode (!placed): outputs dst_w x dst_h RGB24 with margins + transparent
 *   pixels filled with (bg_r, bg_g, bg_b).
 * - Placed mode (placed): outputs out_w x out_h RGB24 (if has_fill) or RGBA32 (if !has_fill)
 *   for the clipped source rectangle starting at (src_x0, src_y0). */
static void extract_frame(const uint8_t *canvas_rgba, int src_w,
                          uint8_t *out_buf, int out_w, int out_h,
                          int src_x0, int src_y0, int dst_x0, int dst_y0,
                          int copy_w, int copy_h,
                          bool has_alpha, bool has_fill,
                          uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    if (out_w <= 0 || out_h <= 0 || !out_buf) return;

    if (has_alpha) {
        /* RGBA32 output for transparent placed sprites */
        for (int y = 0; y < copy_h; y++) {
            const uint8_t *src_row =
                canvas_rgba + ((size_t)(src_y0 + y) * (size_t)src_w + (size_t)src_x0) * 4;
            uint8_t *dst_row = out_buf + (size_t)y * (size_t)out_w * 4;
            memcpy(dst_row, src_row, (size_t)copy_w * 4);
        }
        return;
    }

    size_t dst_bytes = (size_t)out_w * (size_t)out_h * MARKEE_CHANNELS;
    if (!has_fill || (bg_r == 0 && bg_g == 0 && bg_b == 0)) {
        memset(out_buf, 0, dst_bytes);
    } else {
        for (size_t i = 0; i < dst_bytes; i += 3) {
            out_buf[i + 0] = bg_r;
            out_buf[i + 1] = bg_g;
            out_buf[i + 2] = bg_b;
        }
    }

    for (int y = 0; y < copy_h; y++) {
        const uint8_t *src_row =
            canvas_rgba + ((size_t)(src_y0 + y) * (size_t)src_w + (size_t)src_x0) * 4;
        uint8_t *dst_row =
            out_buf + ((size_t)(dst_y0 + y) * (size_t)out_w + (size_t)dst_x0) * 3;
        for (int x = 0; x < copy_w; x++) {
            uint8_t a = src_row[x * 4 + 3];
            if (a != 0) {
                dst_row[x * 3 + 0] = src_row[x * 4 + 0];
                dst_row[x * 3 + 1] = src_row[x * 4 + 1];
                dst_row[x * 3 + 2] = src_row[x * 4 + 2];
            }
        }
    }
}

static void extract_frame_scaled(const uint8_t *canvas_rgba, int src_w, int src_h,
                                 uint8_t *out_buf, int dst_w, int dst_h,
                                 MarkeeGifFitMode fit,
                                 uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    size_t dst_bytes = (size_t)dst_w * (size_t)dst_h * MARKEE_CHANNELS;
    if (bg_r == 0 && bg_g == 0 && bg_b == 0) {
        memset(out_buf, 0, dst_bytes);
    } else {
        for (size_t i = 0; i < dst_bytes; i += 3) {
            out_buf[i + 0] = bg_r;
            out_buf[i + 1] = bg_g;
            out_buf[i + 2] = bg_b;
        }
    }

    int target_w = dst_w;
    int target_h = dst_h;
    int off_x = 0, off_y = 0;

    if (fit == MARKEE_GIF_FIT_CONTAIN) {
        double sx = (double)dst_w / (double)src_w;
        double sy = (double)dst_h / (double)src_h;
        double scale = sx < sy ? sx : sy;
        target_w = (int)((double)src_w * scale + 0.5);
        target_h = (int)((double)src_h * scale + 0.5);
        if (target_w < 1) target_w = 1;
        if (target_h < 1) target_h = 1;
        if (target_w > dst_w) target_w = dst_w;
        if (target_h > dst_h) target_h = dst_h;
        off_x = (dst_w - target_w) / 2;
        off_y = (dst_h - target_h) / 2;
    }

    for (int y = 0; y < target_h; y++) {
        int sy = (int)(((int64_t)y * (int64_t)src_h) / (int64_t)target_h);
        if (sy >= src_h) sy = src_h - 1;
        const uint8_t *src_row = canvas_rgba + (size_t)sy * (size_t)src_w * 4;
        uint8_t *dst_row = out_buf + ((size_t)(off_y + y) * (size_t)dst_w + (size_t)off_x) * 3;
        for (int x = 0; x < target_w; x++) {
            int sx = (int)(((int64_t)x * (int64_t)src_w) / (int64_t)target_w);
            if (sx >= src_w) sx = src_w - 1;
            if (src_row[sx * 4 + 3] != 0) {
                dst_row[x * 3 + 0] = src_row[sx * 4 + 0];
                dst_row[x * 3 + 1] = src_row[sx * 4 + 1];
                dst_row[x * 3 + 2] = src_row[sx * 4 + 2];
            }
        }
    }
}

static bool gif_decode_internal(const uint8_t *data, size_t len, int dst_w, int dst_h,
                                bool placed, int place_x, int place_y,
                                MarkeeGifFitMode fit,
                                bool has_fill, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                                MarkeeGif **out_gif, char *err_buf, size_t err_size) {
    if (out_gif) *out_gif = NULL;
    if (err_buf && err_size > 0) err_buf[0] = '\0';

    if (!data || !out_gif || dst_w < MARKEE_MIN_DIM || dst_w > MARKEE_MAX_DIM ||
        dst_h < MARKEE_MIN_DIM || dst_h > MARKEE_MAX_DIM) {
        set_err(err_buf, err_size, "invalid GIF decode arguments");
        return false;
    }

    if (len < 13 || !gif_has_magic(data, len)) {
        set_err(err_buf, err_size, "invalid GIF header (expected GIF87a or GIF89a)");
        return false;
    }

    int src_w = (int)read_u16_le(data + 6);
    int src_h = (int)read_u16_le(data + 8);
    uint8_t lsd_packed = data[10];
    size_t pos = 13;

    if (src_w <= 0 || src_h <= 0 ||
        src_w > MARKEE_GIF_MAX_SRC_DIM || src_h > MARKEE_GIF_MAX_SRC_DIM ||
        (size_t)src_w * (size_t)src_h > MARKEE_GIF_MAX_SRC_PIXELS) {
        set_err(err_buf, err_size, "GIF dimensions invalid or exceed 4096x4096 limit");
        return false;
    }

    bool has_gct = (lsd_packed & 0x80U) != 0;
    size_t gct_size = 0;
    uint8_t gct[256 * 3];
    memset(gct, 0, sizeof(gct));

    if (has_gct) {
        gct_size = (size_t)1U << ((lsd_packed & 0x07U) + 1U);
        size_t gct_bytes = gct_size * 3;
        if (gct_bytes > len - pos) {
            set_err(err_buf, err_size, "truncated GIF global color table");
            return false;
        }
        memcpy(gct, data + pos, gct_bytes);
        pos += gct_bytes;
    }

    size_t canvas_bytes = (size_t)src_w * (size_t)src_h * 4;
    uint8_t *canvas = calloc(canvas_bytes, 1);
    uint8_t *prev_canvas = NULL;
    if (!canvas) {
        set_err(err_buf, err_size, "out of memory allocating GIF canvas");
        return false;
    }

    /* Compute output geometry and clipping. */
    int out_dst_x = 0, out_dst_y = 0;
    int out_w = dst_w, out_h = dst_h;
    int src_x0 = 0, src_y0 = 0;
    int blit_dst_x0 = 0, blit_dst_y0 = 0;
    int copy_w = 0, copy_h = 0;
    bool has_alpha = false;

    if (placed) {
        int clip_x0 = place_x > 0 ? place_x : 0;
        int clip_y0 = place_y > 0 ? place_y : 0;
        int end_x = place_x + src_w;
        int end_y = place_y + src_h;
        int clip_x1 = end_x < dst_w ? end_x : dst_w;
        int clip_y1 = end_y < dst_h ? end_y : dst_h;
        if (clip_x1 > clip_x0 && clip_y1 > clip_y0) {
            out_dst_x = clip_x0;
            out_dst_y = clip_y0;
            out_w = clip_x1 - clip_x0;
            out_h = clip_y1 - clip_y0;
            src_x0 = clip_x0 - place_x;
            src_y0 = clip_y0 - place_y;
            copy_w = out_w;
            copy_h = out_h;
        } else {
            out_dst_x = 0;
            out_dst_y = 0;
            out_w = 0;
            out_h = 0;
            copy_w = 0;
            copy_h = 0;
        }
        has_alpha = !has_fill;
    } else {
        src_x0 = (src_w > dst_w) ? (src_w - dst_w) / 2 : 0;
        blit_dst_x0 = (dst_w > src_w) ? (dst_w - src_w) / 2 : 0;
        copy_w = (src_w < dst_w) ? src_w : dst_w;

        src_y0 = (src_h > dst_h) ? (src_h - dst_h) / 2 : 0;
        blit_dst_y0 = (dst_h > src_h) ? (dst_h - src_h) / 2 : 0;
        copy_h = (src_h < dst_h) ? src_h : dst_h;
        has_alpha = false;
    }

    const size_t bpp = has_alpha ? 4U : (size_t)MARKEE_CHANNELS;
    const size_t frame_bytes = (size_t)out_w * (size_t)out_h * bpp;
    size_t max_frames_by_mem = (frame_bytes > 0) ? (MARKEE_GIF_MAX_BYTES / frame_bytes)
                                                 : MARKEE_GIF_MAX_FRAMES;
    if (max_frames_by_mem == 0) max_frames_by_mem = 1;
    size_t max_frames = max_frames_by_mem < MARKEE_GIF_MAX_FRAMES
                            ? max_frames_by_mem
                            : MARKEE_GIF_MAX_FRAMES;

    size_t cap_frames = 8;
    if (cap_frames > max_frames) cap_frames = max_frames;
    uint8_t *frames = (frame_bytes > 0) ? malloc(cap_frames * frame_bytes) : NULL;
    uint32_t *delays_ms = malloc(cap_frames * sizeof(uint32_t));
    if ((frame_bytes > 0 && !frames) || !delays_ms) {
        free(canvas);
        free(frames);
        free(delays_ms);
        set_err(err_buf, err_size, "out of memory allocating GIF frames");
        return false;
    }

    size_t frame_count = 0;
    uint64_t total_duration_ms = 0;

    /* Per-frame Graphic Control Extension state (reset after each image block). */
    bool has_gce = false;
    uint8_t disposal = 0;
    bool has_transparency = false;
    uint8_t transparent_index = 0;
    uint16_t delay_cs = 0;

    /* NETSCAPE2.0 looping extension state. */
    bool has_netscape = false;
    uint16_t netscape_iters = 0;

    bool ok = true;
    while (pos < len) {
        uint8_t introducer = data[pos++];

        if (introducer == 0x3B) {
            /* GIF Trailer: normal end of file. */
            break;
        }

        if (introducer == 0x21) {
            /* Extension block */
            if (pos >= len) {
                set_err(err_buf, err_size, "truncated GIF extension");
                ok = false;
                break;
            }
            uint8_t label = data[pos++];

            if (label == 0xF9) {
                /* Graphic Control Extension */
                if (pos >= len) {
                    set_err(err_buf, err_size, "truncated GIF graphic control extension");
                    ok = false;
                    break;
                }
                uint8_t bsz = data[pos++];
                if (bsz > len - pos) {
                    set_err(err_buf, err_size, "truncated GIF graphic control block");
                    ok = false;
                    break;
                }
                if (bsz >= 4) {
                    uint8_t gce_packed = data[pos];
                    disposal = (uint8_t)((gce_packed >> 2) & 0x07U);
                    has_transparency = (gce_packed & 0x01U) != 0;
                    delay_cs = read_u16_le(data + pos + 1);
                    transparent_index = data[pos + 3];
                    has_gce = true;
                }
                pos += bsz;
                if (!skip_sub_blocks(data, len, &pos)) {
                    set_err(err_buf, err_size, "unterminated GIF graphic control extension");
                    ok = false;
                    break;
                }
            } else if (label == 0xFF) {
                /* Application Extension (check for NETSCAPE2.0 / ANIMEXTS1.0 loop count) */
                if (pos >= len) {
                    set_err(err_buf, err_size, "truncated GIF application extension");
                    ok = false;
                    break;
                }
                uint8_t bsz = data[pos++];
                if (bsz > len - pos) {
                    set_err(err_buf, err_size, "truncated GIF application block");
                    ok = false;
                    break;
                }
                bool is_netscape = (bsz == 11) &&
                                   (memcmp(data + pos, "NETSCAPE2.0", 11) == 0 ||
                                    memcmp(data + pos, "ANIMEXTS1.0", 11) == 0);
                pos += bsz;

                if (is_netscape) {
                    if (pos < len && data[pos] > 0) {
                        uint8_t sub_sz = data[pos++];
                        if (sub_sz > len - pos) {
                            set_err(err_buf, err_size, "truncated NETSCAPE2.0 sub-block");
                            ok = false;
                            break;
                        }
                        if (sub_sz >= 3 && data[pos] == 0x01) {
                            has_netscape = true;
                            netscape_iters = read_u16_le(data + pos + 1);
                        }
                        pos += sub_sz;
                    }
                }
                if (!skip_sub_blocks(data, len, &pos)) {
                    set_err(err_buf, err_size, "unterminated GIF application extension");
                    ok = false;
                    break;
                }
            } else {
                /* Comment, Plain Text, or unknown extension: skip all sub-blocks. */
                if (!skip_sub_blocks(data, len, &pos)) {
                    set_err(err_buf, err_size, "unterminated GIF extension");
                    ok = false;
                    break;
                }
            }
            continue;
        }

        if (introducer == 0x2C) {
            /* Image Descriptor */
            if (len - pos < 9) {
                set_err(err_buf, err_size, "truncated GIF image descriptor");
                ok = false;
                break;
            }
            int fx = (int)read_u16_le(data + pos + 0);
            int fy = (int)read_u16_le(data + pos + 2);
            int fw = (int)read_u16_le(data + pos + 4);
            int fh = (int)read_u16_le(data + pos + 6);
            uint8_t img_packed = data[pos + 8];
            pos += 9;

            bool has_lct = (img_packed & 0x80U) != 0;
            bool interlaced = (img_packed & 0x40U) != 0;
            size_t lct_size = 0;
            uint8_t lct[256 * 3];

            const uint8_t *palette = has_gct ? gct : NULL;
            size_t palette_size = has_gct ? gct_size : 0;

            if (has_lct) {
                lct_size = (size_t)1U << ((img_packed & 0x07U) + 1U);
                size_t lct_bytes = lct_size * 3;
                if (lct_bytes > len - pos) {
                    set_err(err_buf, err_size, "truncated GIF local color table");
                    ok = false;
                    break;
                }
                memcpy(lct, data + pos, lct_bytes);
                pos += lct_bytes;
                palette = lct;
                palette_size = lct_size;
            }

            if (pos >= len) {
                set_err(err_buf, err_size, "truncated GIF LZW header");
                ok = false;
                break;
            }
            uint8_t min_code_size = data[pos++];

            if (fw <= 0 || fh <= 0 ||
                fw > MARKEE_GIF_MAX_SRC_DIM || fh > MARKEE_GIF_MAX_SRC_DIM ||
                (size_t)fw * (size_t)fh > MARKEE_GIF_MAX_SRC_PIXELS) {
                set_err(err_buf, err_size, "invalid GIF frame dimensions");
                ok = false;
                break;
            }

            if (frame_count >= max_frames) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "GIF exceeds maximum frame/memory limit (%zu frames)", max_frames);
                set_err(err_buf, err_size, msg);
                ok = false;
                break;
            }

            size_t pixel_count = (size_t)fw * (size_t)fh;
            uint8_t *indices = calloc(pixel_count, 1);
            if (!indices) {
                set_err(err_buf, err_size, "out of memory decoding GIF frame");
                ok = false;
                break;
            }

            if (!decode_lzw_frame(data, len, &pos, min_code_size,
                                  indices, pixel_count, err_buf, err_size)) {
                free(indices);
                ok = false;
                break;
            }

            /* Save previous canvas before drawing if disposal method 3 (restore to previous). */
            if (disposal == 3) {
                if (!prev_canvas) {
                    prev_canvas = malloc(canvas_bytes);
                    if (!prev_canvas) {
                        free(indices);
                        set_err(err_buf, err_size, "out of memory saving GIF previous frame");
                        ok = false;
                        break;
                    }
                }
                memcpy(prev_canvas, canvas, canvas_bytes);
            }

            /* Composite decoded frame onto the logical screen canvas. */
            for (int row = 0; row < fh; row++) {
                int dy = fy + (interlaced ? deinterlace_row(row, fh) : row);
                if (dy < 0 || dy >= src_h) continue;
                const uint8_t *idx_row = indices + (size_t)row * (size_t)fw;
                uint8_t *dst_row = canvas + (size_t)dy * (size_t)src_w * 4;
                for (int col = 0; col < fw; col++) {
                    int dx = fx + col;
                    if (dx < 0 || dx >= src_w) continue;
                    uint8_t ci = idx_row[col];
                    if (has_transparency && ci == transparent_index) {
                        continue;
                    }
                    uint8_t r = 0, g = 0, b = 0;
                    if (palette && (size_t)ci < palette_size) {
                        r = palette[(size_t)ci * 3 + 0];
                        g = palette[(size_t)ci * 3 + 1];
                        b = palette[(size_t)ci * 3 + 2];
                    }
                    dst_row[(size_t)dx * 4 + 0] = r;
                    dst_row[(size_t)dx * 4 + 1] = g;
                    dst_row[(size_t)dx * 4 + 2] = b;
                    dst_row[(size_t)dx * 4 + 3] = 255;
                }
            }
            free(indices);

            /* Grow output frame storage if needed. */
            if (frame_count == cap_frames) {
                size_t new_cap = cap_frames * 2;
                if (new_cap > max_frames) new_cap = max_frames;
                uint8_t *nframes = (frame_bytes > 0) ? realloc(frames, new_cap * frame_bytes) : NULL;
                uint32_t *ndelays = realloc(delays_ms, new_cap * sizeof(uint32_t));
                if ((frame_bytes > 0 && !nframes) || !ndelays) {
                    if (nframes) frames = nframes;
                    if (ndelays) delays_ms = ndelays;
                    set_err(err_buf, err_size, "out of memory growing GIF frame buffer");
                    ok = false;
                    break;
                }
                frames = nframes;
                delays_ms = ndelays;
                cap_frames = new_cap;
            }

            /* Extract this display-clipped or scaled frame from the composited canvas. */
            if (frame_bytes > 0) {
                if (!placed && fit != MARKEE_GIF_FIT_CROP) {
                    extract_frame_scaled(canvas, src_w, src_h,
                                         frames + frame_count * frame_bytes, out_w, out_h,
                                         fit, bg_r, bg_g, bg_b);
                } else {
                    extract_frame(canvas, src_w,
                                  frames + frame_count * frame_bytes, out_w, out_h,
                                  src_x0, src_y0, blit_dst_x0, blit_dst_y0,
                                  copy_w, copy_h,
                                  has_alpha, has_fill, bg_r, bg_g, bg_b);
                }
            }

            /* Standard browser delay rule: clamp < 20 ms (0 or 1 cs) to 100 ms. */
            uint32_t delay_ms = 100;
            if (has_gce && delay_cs >= 2) {
                delay_ms = (uint32_t)delay_cs * 10U;
            }
            delays_ms[frame_count] = delay_ms;
            total_duration_ms += delay_ms;
            frame_count++;

            /* Apply disposal method to prepare canvas for the next frame. */
            if (disposal == 2) {
                /* Restore frame rectangle to transparent/black background. */
                int y0 = fy < 0 ? 0 : fy;
                int y1 = (fy + fh) > src_h ? src_h : (fy + fh);
                int x0 = fx < 0 ? 0 : fx;
                int x1 = (fx + fw) > src_w ? src_w : (fx + fw);
                if (x0 < x1 && y0 < y1) {
                    size_t row_bytes = (size_t)(x1 - x0) * 4;
                    for (int y = y0; y < y1; y++) {
                        memset(canvas + ((size_t)y * (size_t)src_w + (size_t)x0) * 4,
                               0, row_bytes);
                    }
                }
            } else if (disposal == 3 && prev_canvas) {
                memcpy(canvas, prev_canvas, canvas_bytes);
            }

            /* Reset GCE state for the next frame. */
            has_gce = false;
            disposal = 0;
            has_transparency = false;
            transparent_index = 0;
            delay_cs = 0;
            continue;
        }

        if (introducer == 0x00) {
            /* Ignore stray zero padding bytes occasionally emitted before trailer. */
            continue;
        }

        set_err(err_buf, err_size, "corrupt GIF block introducer");
        ok = false;
        break;
    }

    free(canvas);
    free(prev_canvas);

    if (!ok) {
        free(frames);
        free(delays_ms);
        return false;
    }

    if (frame_count == 0) {
        free(frames);
        free(delays_ms);
        set_err(err_buf, err_size, "GIF contains no image frames");
        return false;
    }

    MarkeeGif *gif = calloc(1, sizeof(*gif));
    if (!gif) {
        free(frames);
        free(delays_ms);
        set_err(err_buf, err_size, "out of memory");
        return false;
    }

    /* Shrink buffers to exact decoded frame count to minimize resident memory. */
    if (frame_count < cap_frames) {
        if (frame_bytes > 0) {
            uint8_t *sf = realloc(frames, frame_count * frame_bytes);
            if (sf) frames = sf;
        }
        uint32_t *sd = realloc(delays_ms, frame_count * sizeof(uint32_t));
        if (sd) delays_ms = sd;
    }

    gif->src_width = src_w;
    gif->src_height = src_h;
    gif->dst_x = out_dst_x;
    gif->dst_y = out_dst_y;
    gif->width = out_w;
    gif->height = out_h;
    gif->has_alpha = has_alpha;
    gif->frame_count = frame_count;
    gif->frame_bytes = frame_bytes;
    gif->frames = frames;
    gif->delays_ms = delays_ms;
    gif->total_duration_ms = total_duration_ms;
    gif->has_fill = has_fill;
    gif->bg_r = bg_r;
    gif->bg_g = bg_g;
    gif->bg_b = bg_b;

    if (frame_count == 1) {
        gif->loop_count = 1;
    } else if (has_netscape) {
        /* NETSCAPE2.0: 0 = infinite; N > 0 = N additional repeats (N + 1 total plays). */
        gif->loop_count = (netscape_iters == 0) ? 0 : (int)netscape_iters + 1;
    } else {
        /* Default multi-frame animations without NETSCAPE2.0 to infinite loop on the marquee. */
        gif->loop_count = 0;
    }

    *out_gif = gif;
    return true;
}

bool gif_decode_fit(const uint8_t *data, size_t len, int dst_w, int dst_h,
                    MarkeeGifFitMode fit,
                    uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                    MarkeeGif **out_gif, char *err_buf, size_t err_size) {
    return gif_decode_internal(data, len, dst_w, dst_h, false, 0, 0, fit,
                               true, bg_r, bg_g, bg_b, out_gif, err_buf, err_size);
}

bool gif_decode_ex(const uint8_t *data, size_t len, int dst_w, int dst_h,
                   uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                   MarkeeGif **out_gif, char *err_buf, size_t err_size) {
    return gif_decode_fit(data, len, dst_w, dst_h, MARKEE_GIF_FIT_CROP,
                          bg_r, bg_g, bg_b, out_gif, err_buf, err_size);
}

bool gif_decode(const uint8_t *data, size_t len, int dst_w, int dst_h,
                MarkeeGif **out_gif, char *err_buf, size_t err_size) {
    return gif_decode_ex(data, len, dst_w, dst_h, 0, 0, 0, out_gif, err_buf, err_size);
}

bool gif_decode_placed(const uint8_t *data, size_t len, int dst_w, int dst_h,
                       int place_x, int place_y,
                       bool has_fill, uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                       MarkeeGif **out_gif, char *err_buf, size_t err_size) {
    return gif_decode_internal(data, len, dst_w, dst_h, true, place_x, place_y,
                               MARKEE_GIF_FIT_CROP,
                               has_fill, bg_r, bg_g, bg_b, out_gif, err_buf, err_size);
}

void gif_blit_frame(const MarkeeGif *gif, size_t frame_idx,
                    uint8_t *dst_rgb, int dst_w, int dst_h) {
    if (!gif || !dst_rgb || !gif->frames || gif->width <= 0 || gif->height <= 0) return;
    if (frame_idx >= gif->frame_count) return;

    const uint8_t *src = gif->frames + frame_idx * gif->frame_bytes;
    int gw = gif->width;
    int gh = gif->height;
    int ox = gif->dst_x;
    int oy = gif->dst_y;

    if (!gif->has_alpha && ox == 0 && oy == 0 && gw == dst_w && gh == dst_h) {
        memcpy(dst_rgb, src, gif->frame_bytes);
        return;
    }

    for (int y = 0; y < gh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst_h) continue;
        uint8_t *dst_row = dst_rgb + ((size_t)dy * (size_t)dst_w + (size_t)ox) * 3;
        if (gif->has_alpha) {
            const uint8_t *src_row = src + (size_t)y * (size_t)gw * 4;
            for (int x = 0; x < gw; x++) {
                if (ox + x < 0 || ox + x >= dst_w) continue;
                if (src_row[x * 4 + 3] != 0) {
                    dst_row[x * 3 + 0] = src_row[x * 4 + 0];
                    dst_row[x * 3 + 1] = src_row[x * 4 + 1];
                    dst_row[x * 3 + 2] = src_row[x * 4 + 2];
                }
            }
        } else {
            const uint8_t *src_row = src + (size_t)y * (size_t)gw * 3;
            int x0 = ox < 0 ? -ox : 0;
            int x1 = (ox + gw) > dst_w ? (dst_w - ox) : gw;
            if (x0 < x1) {
                memcpy(dst_row + (size_t)x0 * 3, src_row + (size_t)x0 * 3,
                       (size_t)(x1 - x0) * 3);
            }
        }
    }
}

void gif_free(MarkeeGif *gif) {
    if (!gif) return;
    free(gif->frames);
    free(gif->delays_ms);
    free(gif);
}

/* --- Compact GIF89a Animated Encoder (for recording) --- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool oom;
} GifEncBuf;

static void enc_push(GifEncBuf *b, uint8_t v) {
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 4096;
        uint8_t *nb = realloc(b->buf, ncap);
        if (!nb) { b->oom = true; return; }
        b->buf = nb;
        b->cap = ncap;
    }
    b->buf[b->len++] = v;
}

static void enc_push_bytes(GifEncBuf *b, const void *src, size_t n) {
    if (b->oom || n == 0) return;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        while (ncap < b->len + n) ncap *= 2;
        uint8_t *nb = realloc(b->buf, ncap);
        if (!nb) { b->oom = true; return; }
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
}

static void enc_push_u16(GifEncBuf *b, uint16_t v) {
    enc_push(b, (uint8_t)(v & 0xFF));
    enc_push(b, (uint8_t)((v >> 8) & 0xFF));
}

typedef struct {
    GifEncBuf *out;
    uint8_t sub[255];
    size_t sub_len;
    uint32_t bit_buf;
    int bit_count;
} GifEncBits;

static void enc_flush_sub(GifEncBits *w) {
    if (w->sub_len > 0) {
        enc_push(w->out, (uint8_t)w->sub_len);
        enc_push_bytes(w->out, w->sub, w->sub_len);
        w->sub_len = 0;
    }
}

static void enc_write_code(GifEncBits *w, int code, int bits) {
    w->bit_buf |= ((uint32_t)code << w->bit_count);
    w->bit_count += bits;
    while (w->bit_count >= 8) {
        w->sub[w->sub_len++] = (uint8_t)(w->bit_buf & 0xFF);
        w->bit_buf >>= 8;
        w->bit_count -= 8;
        if (w->sub_len == 255) enc_flush_sub(w);
    }
}

static void enc_finish_bits(GifEncBits *w) {
    if (w->bit_count > 0) {
        w->sub[w->sub_len++] = (uint8_t)(w->bit_buf & 0xFF);
        w->bit_buf = 0;
        w->bit_count = 0;
    }
    enc_flush_sub(w);
    enc_push(w->out, 0x00);
}

/* Build an exact palette if <=256 unique colors exist in the frame; otherwise use a 6x6x6 cube + 40-step gray ramp. */
static void quantize_frame_rgb(const uint8_t *rgb, size_t px_count,
                               uint8_t palette[256 * 3], uint8_t *indices) {
    uint32_t uniq[256];
    size_t ucount = 0;
    bool exact = true;

    for (size_t i = 0; i < px_count; i++) {
        uint32_t c = ((uint32_t)rgb[i * 3 + 0] << 16) |
                     ((uint32_t)rgb[i * 3 + 1] << 8) |
                     (uint32_t)rgb[i * 3 + 2];
        size_t found = ucount;
        for (size_t j = 0; j < ucount; j++) {
            if (uniq[j] == c) { found = j; break; }
        }
        if (found == ucount) {
            if (ucount >= 256) {
                exact = false;
                break;
            }
            uniq[ucount++] = c;
        }
        indices[i] = (uint8_t)found;
    }

    if (exact) {
        memset(palette, 0, 256 * 3);
        for (size_t j = 0; j < ucount; j++) {
            palette[j * 3 + 0] = (uint8_t)((uniq[j] >> 16) & 0xFF);
            palette[j * 3 + 1] = (uint8_t)((uniq[j] >> 8) & 0xFF);
            palette[j * 3 + 2] = (uint8_t)(uniq[j] & 0xFF);
        }
        return;
    }

    /* Fallback 6x6x6 uniform RGB cube (0..215) + 40-step grayscale ramp (216..255) */
    size_t idx = 0;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                palette[idx * 3 + 0] = (uint8_t)(r * 51);
                palette[idx * 3 + 1] = (uint8_t)(g * 51);
                palette[idx * 3 + 2] = (uint8_t)(b * 51);
                idx++;
            }
        }
    }
    for (int k = 0; k < 40; k++) {
        uint8_t v = (uint8_t)((k * 255) / 39);
        palette[idx * 3 + 0] = v;
        palette[idx * 3 + 1] = v;
        palette[idx * 3 + 2] = v;
        idx++;
    }
    for (size_t i = 0; i < px_count; i++) {
        int r = (rgb[i * 3 + 0] * 5 + 127) / 255;
        int g = (rgb[i * 3 + 1] * 5 + 127) / 255;
        int b = (rgb[i * 3 + 2] * 5 + 127) / 255;
        indices[i] = (uint8_t)(r * 36 + g * 6 + b);
    }
}

bool gif_encode_frames(const uint8_t *rgb_frames, size_t frame_count,
                       int width, int height, uint16_t delay_cs,
                       uint8_t **out_gif, size_t *out_len) {
    if (out_gif) *out_gif = NULL;
    if (out_len) *out_len = 0;
    if (!rgb_frames || frame_count == 0 || !out_gif || !out_len ||
        width < 1 || width > 65535 || height < 1 || height > 65535) {
        return false;
    }
    if (delay_cs < 2) delay_cs = 2;

    size_t px_count = (size_t)width * (size_t)height;
    size_t frame_bytes = px_count * 3;
    uint8_t *indices = malloc(px_count);
    if (!indices) return false;

    GifEncBuf out = {0};
    /* Header + Logical Screen Descriptor (no GCT; each frame carries a 256-entry LCT) */
    enc_push_bytes(&out, "GIF89a", 6);
    enc_push_u16(&out, (uint16_t)width);
    enc_push_u16(&out, (uint16_t)height);
    enc_push(&out, 0x70); /* 8-bit color resolution, no GCT */
    enc_push(&out, 0x00);
    enc_push(&out, 0x00);

    /* NETSCAPE2.0 infinite loop extension */
    if (frame_count > 1) {
        enc_push(&out, 0x21);
        enc_push(&out, 0xFF);
        enc_push(&out, 11);
        enc_push_bytes(&out, "NETSCAPE2.0", 11);
        enc_push(&out, 3);
        enc_push(&out, 0x01);
        enc_push_u16(&out, 0);
        enc_push(&out, 0x00);
    }

    uint8_t palette[256 * 3];
    for (size_t f = 0; f < frame_count && !out.oom; f++) {
        quantize_frame_rgb(rgb_frames + f * frame_bytes, px_count, palette, indices);

        /* Graphic Control Extension */
        enc_push(&out, 0x21);
        enc_push(&out, 0xF9);
        enc_push(&out, 4);
        enc_push(&out, 0x04); /* disposal=1 (do not dispose), no transparency */
        enc_push_u16(&out, delay_cs);
        enc_push(&out, 0x00);
        enc_push(&out, 0x00);

        /* Image Descriptor with 256-color LCT */
        enc_push(&out, 0x2C);
        enc_push_u16(&out, 0);
        enc_push_u16(&out, 0);
        enc_push_u16(&out, (uint16_t)width);
        enc_push_u16(&out, (uint16_t)height);
        enc_push(&out, 0x87); /* LCT flag + 256 entries */
        enc_push_bytes(&out, palette, sizeof(palette));

        /* Valid 9-bit LZW stream with periodic Clear codes */
        enc_push(&out, 8); /* min_code_size */
        GifEncBits bw = {0};
        bw.out = &out;
        enc_write_code(&bw, 256, 9); /* Clear */
        int emitted = 0;
        for (size_t i = 0; i < px_count; i++) {
            enc_write_code(&bw, indices[i], 9);
            emitted++;
            if (emitted >= 200) {
                enc_write_code(&bw, 256, 9);
                emitted = 0;
            }
        }
        enc_write_code(&bw, 257, 9); /* EOI */
        enc_finish_bits(&bw);
    }

    enc_push(&out, 0x3B); /* GIF Trailer */
    free(indices);

    if (out.oom || !out.buf) {
        free(out.buf);
        return false;
    }

    *out_gif = out.buf;
    *out_len = out.len;
    return true;
}
