#define _POSIX_C_SOURCE 200809L

#include "draw.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gif.h"
#include "json.h"

/* --- Named + Hex Color Parsing --- */

typedef struct {
    const char *name;
    uint8_t r, g, b;
} NamedColor;

static const NamedColor NAMED_COLORS[] = {
    {"black",       0,   0,   0},
    {"white",     255, 255, 255},
    {"red",       255,   0,   0},
    {"green",       0, 255,   0},
    {"lime",        0, 255,   0},
    {"darkgreen",   0, 128,   0},
    {"blue",        0,   0, 255},
    {"yellow",    255, 255,   0},
    {"cyan",        0, 255, 255},
    {"aqua",        0, 255, 255},
    {"magenta",   255,   0, 255},
    {"fuchsia",   255,   0, 255},
    {"orange",    255, 128,   0},
    {"amber",     255, 191,   0},
    {"gold",      255, 215,   0},
    {"purple",    160,  32, 240},
    {"violet",    238, 130, 238},
    {"indigo",     75,   0, 130},
    {"pink",      255, 105, 180},
    {"coral",     255, 127,  80},
    {"crimson",   220,  20,  60},
    {"brown",     150,  75,   0},
    {"maroon",    128,   0,   0},
    {"olive",     128, 128,   0},
    {"navy",        0,   0, 128},
    {"teal",        0, 128, 128},
    {"turquoise",  64, 224, 208},
    {"gray",      128, 128, 128},
    {"grey",      128, 128, 128},
    {"silver",    192, 192, 192},
    {"warmwhite", 255, 244, 224},
};

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool draw_parse_color_string(const char *s, MarkeeColor *out) {
    if (!s || !*s || !out) return false;

    /* Accept "#rrggbb", "0xrrggbb", or bare "rrggbb" (and 3-digit forms). */
    const char *hex = s;
    if (*s == '#') {
        hex = s + 1;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = s + 2;
    }
    size_t hlen = strlen(hex);
    if (hlen == 3) {
        int r = hex_nibble(hex[0]);
        int g = hex_nibble(hex[1]);
        int b = hex_nibble(hex[2]);
        if (r >= 0 && g >= 0 && b >= 0) {
            out->r = (uint8_t)(r * 17);
            out->g = (uint8_t)(g * 17);
            out->b = (uint8_t)(b * 17);
            return true;
        }
    } else if (hlen == 6) {
        int r1 = hex_nibble(hex[0]), r2 = hex_nibble(hex[1]);
        int g1 = hex_nibble(hex[2]), g2 = hex_nibble(hex[3]);
        int b1 = hex_nibble(hex[4]), b2 = hex_nibble(hex[5]);
        if (r1 >= 0 && r2 >= 0 && g1 >= 0 && g2 >= 0 && b1 >= 0 && b2 >= 0) {
            out->r = (uint8_t)((r1 << 4) | r2);
            out->g = (uint8_t)((g1 << 4) | g2);
            out->b = (uint8_t)((b1 << 4) | b2);
            return true;
        }
    }

    const size_t count = sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(s, NAMED_COLORS[i].name) == 0) {
            out->r = NAMED_COLORS[i].r;
            out->g = NAMED_COLORS[i].g;
            out->b = NAMED_COLORS[i].b;
            return true;
        }
    }
    return false;
}

/* Parse a JSON color value: string ("#FFF", "#00FF00", "red"), [r,g,b] array,
 * or "none"/"transparent"/null (sets *has_color = false). Returns false on invalid value. */
static bool parse_json_color(const JsonValue *val, bool *has_color, MarkeeColor *out) {
    if (!val || val->type == JSON_NULL) {
        *has_color = false;
        return true;
    }
    if (val->type == JSON_STRING) {
        const char *s = val->u.sval;
        if (strcasecmp(s, "none") == 0 || strcasecmp(s, "transparent") == 0) {
            *has_color = false;
            return true;
        }
        if (draw_parse_color_string(s, out)) {
            *has_color = true;
            return true;
        }
        return false;
    }
    if (val->type == JSON_ARRAY && val->u.arr.count == 3) {
        int r, g, b;
        if (json_as_int(val->u.arr.items[0], &r) &&
            json_as_int(val->u.arr.items[1], &g) &&
            json_as_int(val->u.arr.items[2], &b) &&
            r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
            out->r = (uint8_t)r;
            out->g = (uint8_t)g;
            out->b = (uint8_t)b;
            *has_color = true;
            return true;
        }
        return false;
    }
    return false;
}

/* --- Built-in Standard 5x7 Printable ASCII Bitmap Font (0x20..0x7E) ---
 * Each glyph is 5 columns (left to right), 7 bits per column (LSB = top row 0, bit 6 = bottom row 6). */
static const uint8_t FONT5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* 0x20 ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* 0x21 '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* 0x22 '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* 0x23 '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* 0x24 '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* 0x25 '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* 0x26 '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* 0x27 ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* 0x28 '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* 0x29 ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* 0x2A '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* 0x2B '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* 0x2C ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* 0x2D '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* 0x2E '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* 0x2F '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0x30 '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 0x31 '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 0x32 '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 0x33 '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 0x34 '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 0x35 '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 0x36 '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 0x37 '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 0x38 '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 0x39 '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* 0x3A ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* 0x3B ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* 0x3C '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* 0x3D '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* 0x3E '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* 0x3F '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* 0x40 '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 0x41 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 0x42 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 0x43 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 0x44 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 0x45 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 0x46 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 0x47 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 0x48 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 0x49 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 0x4A 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 0x4B 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 0x4C 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 0x4D 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 0x4E 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 0x4F 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 0x50 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 0x51 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 0x52 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 0x53 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 0x54 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 0x55 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 0x56 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 0x57 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 0x58 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 0x59 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 0x5A 'Z' */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* 0x5B '[' */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* 0x5C '\' */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* 0x5D ']' */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* 0x5E '^' */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* 0x5F '_' */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* 0x60 '`' */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* 0x61 'a' */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* 0x62 'b' */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* 0x63 'c' */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* 0x64 'd' */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* 0x65 'e' */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* 0x66 'f' */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* 0x67 'g' */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* 0x68 'h' */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* 0x69 'i' */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* 0x6A 'j' */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* 0x6B 'k' */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* 0x6C 'l' */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* 0x6D 'm' */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* 0x6E 'n' */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* 0x6F 'o' */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* 0x70 'p' */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* 0x71 'q' */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* 0x72 'r' */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* 0x73 's' */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* 0x74 't' */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* 0x75 'u' */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* 0x76 'v' */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* 0x77 'w' */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* 0x78 'x' */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* 0x79 'y' */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* 0x7A 'z' */
    {0x00, 0x08, 0x36, 0x41, 0x00}, /* 0x7B '{' */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* 0x7C '|' */
    {0x00, 0x41, 0x36, 0x08, 0x00}, /* 0x7D '}' */
    {0x10, 0x08, 0x08, 0x10, 0x08}, /* 0x7E '~' */
};

/* Compact 3x5 uppercase/digit/symbol font for "4x6" / "small" mode (3 cols, LSB = row 0) */
static bool lookup_font3x5(char ch, uint8_t cols[3]) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
        case ' ': cols[0]=0x00; cols[1]=0x00; cols[2]=0x00; return true;
        case '!': cols[0]=0x00; cols[1]=0x17; cols[2]=0x00; return true;
        case '.': cols[0]=0x00; cols[1]=0x10; cols[2]=0x00; return true;
        case ':': cols[0]=0x00; cols[1]=0x0A; cols[2]=0x00; return true;
        case '-': cols[0]=0x04; cols[1]=0x04; cols[2]=0x04; return true;
        case '+': cols[0]=0x04; cols[1]=0x0E; cols[2]=0x04; return true;
        case '/': cols[0]=0x18; cols[1]=0x04; cols[2]=0x03; return true;
        case '0': cols[0]=0x1F; cols[1]=0x11; cols[2]=0x1F; return true;
        case '1': cols[0]=0x12; cols[1]=0x1F; cols[2]=0x10; return true;
        case '2': cols[0]=0x1D; cols[1]=0x15; cols[2]=0x17; return true;
        case '3': cols[0]=0x15; cols[1]=0x15; cols[2]=0x1F; return true;
        case '4': cols[0]=0x07; cols[1]=0x04; cols[2]=0x1F; return true;
        case '5': cols[0]=0x17; cols[1]=0x15; cols[2]=0x1D; return true;
        case '6': cols[0]=0x1F; cols[1]=0x15; cols[2]=0x1D; return true;
        case '7': cols[0]=0x01; cols[1]=0x1D; cols[2]=0x03; return true;
        case '8': cols[0]=0x1F; cols[1]=0x15; cols[2]=0x1F; return true;
        case '9': cols[0]=0x17; cols[1]=0x15; cols[2]=0x1F; return true;
        case 'A': cols[0]=0x1E; cols[1]=0x05; cols[2]=0x1E; return true;
        case 'B': cols[0]=0x1F; cols[1]=0x15; cols[2]=0x0A; return true;
        case 'C': cols[0]=0x0E; cols[1]=0x11; cols[2]=0x11; return true;
        case 'D': cols[0]=0x1F; cols[1]=0x11; cols[2]=0x0E; return true;
        case 'E': cols[0]=0x1F; cols[1]=0x15; cols[2]=0x11; return true;
        case 'F': cols[0]=0x1F; cols[1]=0x05; cols[2]=0x01; return true;
        case 'G': cols[0]=0x0E; cols[1]=0x15; cols[2]=0x1D; return true;
        case 'H': cols[0]=0x1F; cols[1]=0x04; cols[2]=0x1F; return true;
        case 'I': cols[0]=0x11; cols[1]=0x1F; cols[2]=0x11; return true;
        case 'J': cols[0]=0x08; cols[1]=0x10; cols[2]=0x0F; return true;
        case 'K': cols[0]=0x1F; cols[1]=0x04; cols[2]=0x1B; return true;
        case 'L': cols[0]=0x1F; cols[1]=0x10; cols[2]=0x10; return true;
        case 'M': cols[0]=0x1F; cols[1]=0x02; cols[2]=0x1F; return true;
        case 'N': cols[0]=0x1F; cols[1]=0x0E; cols[2]=0x1F; return true;
        case 'O': cols[0]=0x0E; cols[1]=0x11; cols[2]=0x0E; return true;
        case 'P': cols[0]=0x1F; cols[1]=0x05; cols[2]=0x07; return true;
        case 'Q': cols[0]=0x0E; cols[1]=0x19; cols[2]=0x1E; return true;
        case 'R': cols[0]=0x1F; cols[1]=0x05; cols[2]=0x1A; return true;
        case 'S': cols[0]=0x12; cols[1]=0x15; cols[2]=0x09; return true;
        case 'T': cols[0]=0x01; cols[1]=0x1F; cols[2]=0x01; return true;
        case 'U': cols[0]=0x1F; cols[1]=0x10; cols[2]=0x1F; return true;
        case 'V': cols[0]=0x0F; cols[1]=0x10; cols[2]=0x0F; return true;
        case 'W': cols[0]=0x1F; cols[1]=0x08; cols[2]=0x1F; return true;
        case 'X': cols[0]=0x1B; cols[1]=0x04; cols[2]=0x1B; return true;
        case 'Y': cols[0]=0x03; cols[1]=0x1C; cols[2]=0x03; return true;
        case 'Z': cols[0]=0x19; cols[1]=0x15; cols[2]=0x13; return true;
        default: return false;
    }
}

typedef enum {
    FONT_4X6 = 0,
    FONT_6X8,
    FONT_8X8
} FontStyle;

static FontStyle resolve_font(const char *font) {
    if (!font || !*font) return FONT_6X8;
    if (strcasecmp(font, "4x6") == 0 || strcasecmp(font, "3x5") == 0 ||
        strcasecmp(font, "small") == 0) {
        return FONT_4X6;
    }
    if (strcasecmp(font, "8x8") == 0 || strcasecmp(font, "large") == 0 ||
        strcasecmp(font, "bold") == 0) {
        return FONT_8X8;
    }
    return FONT_6X8; /* "6x8", "5x7", "medium", "default" */
}

static void put_px_buf(uint8_t *buf, int buf_w, int buf_h, int x, int y, MarkeeColor c) {
    if (x < 0 || x >= buf_w || y < 0 || y >= buf_h) return;
    size_t idx = ((size_t)y * (size_t)buf_w + (size_t)x) * 3;
    buf[idx + 0] = c.r;
    buf[idx + 1] = c.g;
    buf[idx + 2] = c.b;
}

int draw_measure_text(const char *text, const char *font) {
    if (!text || !*text) return 0;
    FontStyle fs = resolve_font(font);
    int advance = (fs == FONT_4X6) ? 4 : (fs == FONT_8X8 ? 7 : 6);
    int len = (int)strlen(text);
    return len * advance;
}

void draw_render_text_buf(uint8_t *buf, int buf_w, int buf_h,
                          int x, int y, const char *text, const char *font,
                          MarkeeColor color) {
    if (!buf || !text) return;
    FontStyle fs = resolve_font(font);
    int cursor_x = x;

    for (const char *p = text; *p; p++) {
        unsigned char uc = (unsigned char)*p;
        if (fs == FONT_4X6) {
            uint8_t cols[3];
            if (lookup_font3x5((char)uc, cols)) {
                for (int cx = 0; cx < 3; cx++) {
                    for (int cy = 0; cy < 5; cy++) {
                        if (cols[cx] & (1U << cy)) {
                            put_px_buf(buf, buf_w, buf_h, cursor_x + cx, y + cy, color);
                        }
                    }
                }
            }
            cursor_x += 4;
        } else {
            if (uc < 0x20 || uc > 0x7E) uc = '?';
            const uint8_t *cols = FONT5X7[uc - 0x20];
            for (int cx = 0; cx < 5; cx++) {
                uint8_t bits = cols[cx];
                for (int cy = 0; cy < 7; cy++) {
                    if (bits & (1U << cy)) {
                        put_px_buf(buf, buf_w, buf_h, cursor_x + cx, y + cy, color);
                        if (fs == FONT_8X8) {
                            /* Faux-bold 1px horizontal thickening */
                            put_px_buf(buf, buf_w, buf_h, cursor_x + cx + 1, y + cy, color);
                        }
                    }
                }
            }
            cursor_x += (fs == FONT_8X8) ? 7 : 6;
        }
    }
}

/* --- Shape Rasterizers (called with disp->lock held) --- */

static void draw_line_buf(uint8_t *buf, int bw, int bh,
                          int x1, int y1, int x2, int y2, MarkeeColor c) {
    int dx = abs(x2 - x1);
    int sx = (x1 < x2) ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        put_px_buf(buf, bw, bh, x1, y1, c);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

static bool rect_contains(int px, int py, int x, int y, int w, int h, int radius) {
    if (px < x || px >= x + w || py < y || py >= y + h) return false;
    if (radius <= 0) return true;

    int x1 = x + w - 1;
    int y1 = y + h - 1;
    double r2 = (double)radius * (double)radius + 0.5;

    if (px < x + radius && py < y + radius) {
        double dx = (double)(px - (x + radius));
        double dy = (double)(py - (y + radius));
        return (dx * dx + dy * dy) <= r2;
    }
    if (px > x1 - radius && py < y + radius) {
        double dx = (double)(px - (x1 - radius));
        double dy = (double)(py - (y + radius));
        return (dx * dx + dy * dy) <= r2;
    }
    if (px < x + radius && py > y1 - radius) {
        double dx = (double)(px - (x + radius));
        double dy = (double)(py - (y1 - radius));
        return (dx * dx + dy * dy) <= r2;
    }
    if (px > x1 - radius && py > y1 - radius) {
        double dx = (double)(px - (x1 - radius));
        double dy = (double)(py - (y1 - radius));
        return (dx * dx + dy * dy) <= r2;
    }
    return true;
}

static void draw_rect_buf(uint8_t *buf, int bw, int bh,
                          int x, int y, int w, int h, int radius,
                          bool has_fill, MarkeeColor fill_c,
                          bool has_stroke, MarkeeColor stroke_c) {
    if (w <= 0 || h <= 0) return;
    int max_r = (w < h ? w : h) / 2;
    if (radius < 0) radius = 0;
    if (radius > max_r) radius = max_r;

    int cx0 = x < 0 ? 0 : x;
    int cy0 = y < 0 ? 0 : y;
    int cx1 = (x + w - 1) >= bw ? bw - 1 : (x + w - 1);
    int cy1 = (y + h - 1) >= bh ? bh - 1 : (y + h - 1);

    for (int py = cy0; py <= cy1; py++) {
        for (int px = cx0; px <= cx1; px++) {
            if (!rect_contains(px, py, x, y, w, h, radius)) continue;

            bool on_edge = !rect_contains(px - 1, py, x, y, w, h, radius) ||
                           !rect_contains(px + 1, py, x, y, w, h, radius) ||
                           !rect_contains(px, py - 1, x, y, w, h, radius) ||
                           !rect_contains(px, py + 1, x, y, w, h, radius);

            if (has_stroke && on_edge) {
                put_px_buf(buf, bw, bh, px, py, stroke_c);
            } else if (has_fill) {
                put_px_buf(buf, bw, bh, px, py, fill_c);
            }
        }
    }
}

static bool oval_contains(int px, int py, double cx, double cy, double rx, double ry) {
    double nx = ((double)px - cx) / rx;
    double ny = ((double)py - cy) / ry;
    return (nx * nx + ny * ny) <= 1.0 + 1e-9;
}

static void draw_oval_buf(uint8_t *buf, int bw, int bh,
                          int x, int y, int w, int h,
                          bool has_fill, MarkeeColor fill_c,
                          bool has_stroke, MarkeeColor stroke_c) {
    if (w <= 0 || h <= 0) return;

    int cx0 = x < 0 ? 0 : x;
    int cy0 = y < 0 ? 0 : y;
    int cx1 = (x + w - 1) >= bw ? bw - 1 : (x + w - 1);
    int cy1 = (y + h - 1) >= bh ? bh - 1 : (y + h - 1);

    double cx = (double)x + ((double)w - 1.0) * 0.5;
    double cy = (double)y + ((double)h - 1.0) * 0.5;
    double rx = (double)w * 0.5;
    double ry = (double)h * 0.5;

    for (int py = cy0; py <= cy1; py++) {
        for (int px = cx0; px <= cx1; px++) {
            if (!oval_contains(px, py, cx, cy, rx, ry)) continue;

            bool on_edge = !oval_contains(px - 1, py, cx, cy, rx, ry) ||
                           !oval_contains(px + 1, py, cx, cy, rx, ry) ||
                           !oval_contains(px, py - 1, cx, cy, rx, ry) ||
                           !oval_contains(px, py + 1, cx, cy, rx, ry);

            if (has_stroke && on_edge) {
                put_px_buf(buf, bw, bh, px, py, stroke_c);
            } else if (has_fill) {
                put_px_buf(buf, bw, bh, px, py, fill_c);
            }
        }
    }
}

typedef struct {
    int x;
    int y;
} IntPoint;

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void draw_polygon_buf(uint8_t *buf, int bw, int bh,
                             const IntPoint *pts, size_t n,
                             bool has_fill, MarkeeColor fill_c,
                             bool has_stroke, MarkeeColor stroke_c) {
    if (!pts || n < 2) return;

    /* 1. Scanline polygon fill (even-odd rule at pixel centers) */
    if (has_fill && n >= 3) {
        int min_y = pts[0].y, max_y = pts[0].y;
        for (size_t i = 1; i < n; i++) {
            if (pts[i].y < min_y) min_y = pts[i].y;
            if (pts[i].y > max_y) max_y = pts[i].y;
        }
        if (min_y < 0) min_y = 0;
        if (max_y >= bh) max_y = bh - 1;

        double *nodes = malloc(n * sizeof(double));
        if (nodes) {
            for (int py = min_y; py <= max_y; py++) {
                double sy = (double)py + 0.5;
                size_t node_count = 0;
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    double y1 = (double)pts[i].y + 0.5;
                    double y2 = (double)pts[j].y + 0.5;
                    if ((y1 <= sy && y2 > sy) || (y2 <= sy && y1 > sy)) {
                        double x1 = (double)pts[i].x + 0.5;
                        double x2 = (double)pts[j].x + 0.5;
                        nodes[node_count++] = x1 + ((sy - y1) / (y2 - y1)) * (x2 - x1);
                    }
                }
                qsort(nodes, node_count, sizeof(double), cmp_double);
                for (size_t i = 0; i + 1 < node_count; i += 2) {
                    int x_start = (int)ceil(nodes[i] - 0.5);
                    int x_end = (int)floor(nodes[i + 1] - 0.5);
                    if (x_start < 0) x_start = 0;
                    if (x_end >= bw) x_end = bw - 1;
                    for (int px = x_start; px <= x_end; px++) {
                        put_px_buf(buf, bw, bh, px, py, fill_c);
                    }
                }
            }
            free(nodes);
        }
    }

    /* 2. Crisp polygon edges */
    if (has_stroke) {
        for (size_t i = 0; i < n; i++) {
            size_t next = (i + 1) % n;
            draw_line_buf(buf, bw, bh, pts[i].x, pts[i].y, pts[next].x, pts[next].y, stroke_c);
        }
    }
}

/* --- JSON Operation Executor --- */

static int opt_int(const JsonValue *obj, const char *key, int def) {
    int v = def;
    const JsonValue *jv = json_obj_get(obj, key);
    if (jv) json_as_int(jv, &v);
    return v;
}

static bool extract_stroke_and_fill(const JsonValue *obj,
                                    bool *has_stroke, MarkeeColor *stroke_c,
                                    bool *has_fill, MarkeeColor *fill_c,
                                    char *err_buf, size_t err_size) {
    *has_stroke = false;
    *has_fill = false;
    *stroke_c = (MarkeeColor){0, 0, 0};
    *fill_c = (MarkeeColor){0, 0, 0};

    const JsonValue *jcol = json_obj_get(obj, "color");
    if (jcol) {
        if (!parse_json_color(jcol, has_stroke, stroke_c)) {
            snprintf(err_buf, err_size, "invalid 'color' value");
            return false;
        }
    }

    const JsonValue *jfill = json_obj_get(obj, "fill");
    if (jfill) {
        if (jfill->type == JSON_BOOL) {
            if (jfill->u.bval) {
                *has_fill = true;
                *fill_c = *has_stroke ? *stroke_c : (MarkeeColor){255, 255, 255};
            } else {
                *has_fill = false;
            }
        } else {
            if (!parse_json_color(jfill, has_fill, fill_c)) {
                snprintf(err_buf, err_size, "invalid 'fill' value");
                return false;
            }
        }
    }

    /* Support optional standalone r,g,b numeric channels if color/fill not provided */
    const JsonValue *jr = json_obj_get(obj, "r");
    const JsonValue *jg = json_obj_get(obj, "g");
    const JsonValue *jb = json_obj_get(obj, "b");
    if (!*has_stroke && !*has_fill && (jr || jg || jb)) {
        int r = opt_int(obj, "r", 0);
        int g = opt_int(obj, "g", 0);
        int b = opt_int(obj, "b", 0);
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            snprintf(err_buf, err_size, "r, g, b must be in 0..255");
            return false;
        }
        *has_stroke = true;
        *stroke_c = (MarkeeColor){(uint8_t)r, (uint8_t)g, (uint8_t)b};
    }

    return true;
}

/* True if the op names any color at all (color, fill, or r/g/b), even an explicit "none". */
static bool op_specifies_color(const JsonValue *obj) {
    return json_obj_get(obj, "color") || json_obj_get(obj, "fill") ||
           json_obj_get(obj, "r") || json_obj_get(obj, "g") || json_obj_get(obj, "b");
}

/* For outline shapes: when no color keys are given at all, default to a white outline
 * (matching line/text), instead of silently drawing nothing. */
static void default_white_stroke(const JsonValue *obj, bool *has_stroke, MarkeeColor *stroke_c) {
    if (!op_specifies_color(obj)) {
        *has_stroke = true;
        *stroke_c = (MarkeeColor){255, 255, 255};
    }
}

/* Base64 decoder for embedded GIF payloads in /draw.
 * Accepts optional "data:...;base64," URI prefix, standard (+/) or URL-safe (-_) alphabet,
 * and ignores ASCII whitespace. Caller frees *out_buf. */
static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static bool decode_base64(const char *s, uint8_t **out_buf, size_t *out_len) {
    *out_buf = NULL;
    *out_len = 0;
    if (!s || !*s) return false;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (strncasecmp(s, "data:", 5) == 0) {
        const char *comma = strchr(s, ',');
        if (!comma) return false;
        s = comma + 1;
    }

    size_t slen = strlen(s);
    size_t max_out = (slen / 4 + 1) * 3;
    if (max_out == 0) return false;
    uint8_t *buf = malloc(max_out);
    if (!buf) return false;

    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    bool seen_pad = false;

    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            seen_pad = true;
            continue;
        }
        if (seen_pad) {
            free(buf);
            return false;
        }
        int v = b64_val(c);
        if (v < 0) {
            free(buf);
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= MARKEE_MAX_BODY_BYTES) {
                free(buf);
                return false;
            }
            buf[n++] = (uint8_t)((acc >> bits) & 0xFFU);
        }
    }

    /* A single dangling base64 sextet (6 bits) is never valid. */
    if (n == 0 || bits == 6) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    *out_len = n;
    return true;
}

static bool has_png_magic(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data && len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

/* Composite an RGBA32 image onto an RGB24 buffer at (place_x, place_y), clipped to (bw, bh).
 * If has_fill is true, alpha is blended against fill_c; otherwise against the existing pixel. */
static void blit_rgba_buf(uint8_t *buf, int bw, int bh,
                          const uint8_t *rgba, int iw, int ih,
                          int place_x, int place_y,
                          bool has_fill, MarkeeColor fill_c) {
    int clip_x0 = place_x > 0 ? place_x : 0;
    int clip_y0 = place_y > 0 ? place_y : 0;
    int clip_x1 = (place_x + iw) < bw ? (place_x + iw) : bw;
    int clip_y1 = (place_y + ih) < bh ? (place_y + ih) : bh;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;

    for (int dy = clip_y0; dy < clip_y1; dy++) {
        int sy = dy - place_y;
        const uint8_t *src_row = rgba + (size_t)sy * (size_t)iw * 4;
        uint8_t *dst_row = buf + (size_t)dy * (size_t)bw * 3;
        for (int dx = clip_x0; dx < clip_x1; dx++) {
            int sx = dx - place_x;
            uint32_t sr = src_row[(size_t)sx * 4 + 0];
            uint32_t sg = src_row[(size_t)sx * 4 + 1];
            uint32_t sb = src_row[(size_t)sx * 4 + 2];
            uint32_t sa = src_row[(size_t)sx * 4 + 3];
            uint8_t *dp = dst_row + (size_t)dx * 3;
            if (sa == 255U) {
                dp[0] = (uint8_t)sr;
                dp[1] = (uint8_t)sg;
                dp[2] = (uint8_t)sb;
            } else if (has_fill) {
                uint32_t inv = 255U - sa;
                dp[0] = (uint8_t)((sr * sa + (uint32_t)fill_c.r * inv + 127U) / 255U);
                dp[1] = (uint8_t)((sg * sa + (uint32_t)fill_c.g * inv + 127U) / 255U);
                dp[2] = (uint8_t)((sb * sa + (uint32_t)fill_c.b * inv + 127U) / 255U);
            } else if (sa > 0U) {
                uint32_t inv = 255U - sa;
                dp[0] = (uint8_t)((sr * sa + (uint32_t)dp[0] * inv + 127U) / 255U);
                dp[1] = (uint8_t)((sg * sa + (uint32_t)dp[1] * inv + 127U) / 255U);
                dp[2] = (uint8_t)((sb * sa + (uint32_t)dp[2] * inv + 127U) / 255U);
            }
        }
    }
}

typedef struct {
    MarkeeGif *gifs[MARKEE_MAX_GIF_LAYERS];
    size_t count;
} StagedGifs;

static void staged_gifs_clear(StagedGifs *sg) {
    for (size_t i = 0; i < sg->count; i++) {
        gif_free(sg->gifs[i]);
        sg->gifs[i] = NULL;
    }
    sg->count = 0;
}

static bool exec_single_op_buf(MarkeeDisplay *disp,
                               uint8_t *buf, uint8_t *static_bg,
                               int bw, int bh, size_t frame_bytes,
                               MarkeeScrollState *scroll_state,
                               StagedGifs *staged_gifs,
                               const JsonValue *obj,
                               char *err_buf, size_t err_size) {
    if (!obj || obj->type != JSON_OBJECT) {
        snprintf(err_buf, err_size, "each draw operation must be a JSON object");
        return false;
    }

    const char *op = json_as_string(json_obj_get(obj, "op"));
    if (!op || !*op) {
        snprintf(err_buf, err_size, "missing required string field 'op'");
        return false;
    }

    if (strcasecmp(op, "clear") == 0) {
        bool has_c = false;
        MarkeeColor c = {0, 0, 0};
        const JsonValue *jcol = json_obj_get(obj, "color");
        if (!jcol) jcol = json_obj_get(obj, "fill");
        if (jcol && !parse_json_color(jcol, &has_c, &c)) {
            snprintf(err_buf, err_size, "invalid color for 'clear'");
            return false;
        }
        scroll_state->active = false;
        scroll_state->has_start_time = false;
        staged_gifs_clear(staged_gifs);
        for (size_t i = 0; i < frame_bytes; i += 3) {
            buf[i + 0] = c.r;
            buf[i + 1] = c.g;
            buf[i + 2] = c.b;
            static_bg[i + 0] = c.r;
            static_bg[i + 1] = c.g;
            static_bg[i + 2] = c.b;
        }
        return true;
    }

    if (strcasecmp(op, "pixel") == 0) {
        int x = opt_int(obj, "x", 0);
        int y = opt_int(obj, "y", 0);
        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) return false;
        if (hs) {
            put_px_buf(buf, bw, bh, x, y, sc);
            put_px_buf(static_bg, bw, bh, x, y, sc);
        } else if (hf) {
            put_px_buf(buf, bw, bh, x, y, fc);
            put_px_buf(static_bg, bw, bh, x, y, fc);
        } else if (!op_specifies_color(obj)) {
            MarkeeColor w = {255, 255, 255};
            put_px_buf(buf, bw, bh, x, y, w);
            put_px_buf(static_bg, bw, bh, x, y, w);
        } /* else: explicit "none"/null color -> nothing to draw */
        return true;
    }

    if (strcasecmp(op, "line") == 0) {
        int x1 = opt_int(obj, "x1", 0);
        int y1 = opt_int(obj, "y1", 0);
        int x2 = opt_int(obj, "x2", 0);
        int y2 = opt_int(obj, "y2", 0);
        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) return false;
        MarkeeColor c = hs ? sc : (hf ? fc : (MarkeeColor){255, 255, 255});
        draw_line_buf(buf, bw, bh, x1, y1, x2, y2, c);
        draw_line_buf(static_bg, bw, bh, x1, y1, x2, y2, c);
        return true;
    }

    if (strcasecmp(op, "rect") == 0) {
        int x = opt_int(obj, "x", 0);
        int y = opt_int(obj, "y", 0);
        int w = opt_int(obj, "w", 0);
        int h = opt_int(obj, "h", 0);
        int radius = opt_int(obj, "radius", 0);
        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) return false;
        default_white_stroke(obj, &hs, &sc);
        draw_rect_buf(buf, bw, bh, x, y, w, h, radius, hf, fc, hs, sc);
        draw_rect_buf(static_bg, bw, bh, x, y, w, h, radius, hf, fc, hs, sc);
        return true;
    }

    if (strcasecmp(op, "oval") == 0) {
        int x = opt_int(obj, "x", 0);
        int y = opt_int(obj, "y", 0);
        int w = opt_int(obj, "w", 0);
        int h = opt_int(obj, "h", 0);
        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) return false;
        default_white_stroke(obj, &hs, &sc);
        draw_oval_buf(buf, bw, bh, x, y, w, h, hf, fc, hs, sc);
        draw_oval_buf(static_bg, bw, bh, x, y, w, h, hf, fc, hs, sc);
        return true;
    }

    if (strcasecmp(op, "polygon") == 0) {
        const JsonValue *jpts = json_obj_get(obj, "points");
        if (!jpts || jpts->type != JSON_ARRAY || jpts->u.arr.count < 2) {
            snprintf(err_buf, err_size, "'polygon' requires 'points' array with at least 2 [x,y] pairs");
            return false;
        }
        size_t n = jpts->u.arr.count;
        if (n > 256) {
            snprintf(err_buf, err_size, "'polygon' supports at most 256 points");
            return false;
        }

        IntPoint *pts = malloc(n * sizeof(IntPoint));
        if (!pts) {
            snprintf(err_buf, err_size, "out of memory");
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            const JsonValue *pair = jpts->u.arr.items[i];
            if (!pair || pair->type != JSON_ARRAY || pair->u.arr.count != 2 ||
                !json_as_int(pair->u.arr.items[0], &pts[i].x) ||
                !json_as_int(pair->u.arr.items[1], &pts[i].y)) {
                free(pts);
                snprintf(err_buf, err_size, "each polygon point must be a [x, y] number pair");
                return false;
            }
        }

        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) {
            free(pts);
            return false;
        }
        default_white_stroke(obj, &hs, &sc);
        draw_polygon_buf(buf, bw, bh, pts, n, hf, fc, hs, sc);
        draw_polygon_buf(static_bg, bw, bh, pts, n, hf, fc, hs, sc);
        free(pts);
        return true;
    }

    if (strcasecmp(op, "text") == 0) {
        const char *text = json_as_string(json_obj_get(obj, "text"));
        if (!text) {
            snprintf(err_buf, err_size, "'text' op requires string field 'text'");
            return false;
        }
        int x = opt_int(obj, "x", 0);
        int y = opt_int(obj, "y", 0);
        const char *font = json_as_string(json_obj_get(obj, "font"));
        if (!font || !*font) font = "6x8";

        bool scroll = false;
        const JsonValue *jscroll = json_obj_get(obj, "scroll");
        if (jscroll) json_as_bool(jscroll, &scroll);

        double speed = 24.0;
        const JsonValue *jspeed = json_obj_get(obj, "speed");
        if (jspeed) {
            json_as_double(jspeed, &speed);
            if (speed <= 0.0) speed = 24.0;
        }

        bool hs = false, hf = false;
        MarkeeColor sc, fc;
        if (!extract_stroke_and_fill(obj, &hs, &sc, &hf, &fc, err_buf, err_size)) return false;
        MarkeeColor c = hs ? sc : (hf ? fc : (MarkeeColor){255, 255, 255});

        if (scroll) {
            /* Snapshot current staged framebuffer so prior shapes stay underneath scrolling text */
            if (scroll_state->bg_pixels) {
                memcpy(scroll_state->bg_pixels, buf, frame_bytes);
            }
            scroll_state->active = true;
            scroll_state->has_start_time = false;
            snprintf(scroll_state->text, sizeof(scroll_state->text), "%s", text);
            snprintf(scroll_state->font, sizeof(scroll_state->font), "%s", font);
            scroll_state->start_x = x;
            scroll_state->y = y;
            scroll_state->text_w = draw_measure_text(text, font);
            scroll_state->r = c.r;
            scroll_state->g = c.g;
            scroll_state->b = c.b;
            scroll_state->speed = speed;
        } else {
            scroll_state->active = false;
            scroll_state->has_start_time = false;
            draw_render_text_buf(static_bg, bw, bh, x, y, text, font, c);
        }

        draw_render_text_buf(buf, bw, bh, x, y, text, font, c);
        return true;
    }

    if (strcasecmp(op, "image") == 0) {
        const char *b64 = json_as_string(json_obj_get(obj, "data"));
        if (!b64 || !*b64) {
            snprintf(err_buf, err_size, "'image' op requires base64 string field 'data'");
            return false;
        }

        int x = opt_int(obj, "x", 0);
        int y = opt_int(obj, "y", 0);

        bool has_fill = false;
        MarkeeColor fill_c = {0, 0, 0};
        const JsonValue *jfill = json_obj_get(obj, "fill");
        if (jfill && !parse_json_color(jfill, &has_fill, &fill_c)) {
            snprintf(err_buf, err_size, "invalid 'fill' value for 'image' op");
            return false;
        }

        uint8_t *img_bytes = NULL;
        size_t img_len = 0;
        if (!decode_base64(b64, &img_bytes, &img_len)) {
            snprintf(err_buf, err_size, "'image' op contains invalid base64 in 'data'");
            return false;
        }

        if (has_png_magic(img_bytes, img_len)) {
            /* Static PNG: 'loop' is safely ignored if specified. */
            uint8_t *rgba = NULL;
            int iw = 0, ih = 0;
            bool ok_png = display_decode_png_rgba(disp, img_bytes, img_len, &rgba, &iw, &ih);
            free(img_bytes);
            if (!ok_png) {
                snprintf(err_buf, err_size, "invalid PNG data in 'image' op");
                return false;
            }
            blit_rgba_buf(buf, bw, bh, rgba, iw, ih, x, y, has_fill, fill_c);
            blit_rgba_buf(static_bg, bw, bh, rgba, iw, ih, x, y, has_fill, fill_c);
            free(rgba);
            return true;
        }

        if (gif_has_magic(img_bytes, img_len)) {
            int loop_override = -1;
            const JsonValue *jloop = json_obj_get(obj, "loop");
            if (jloop && jloop->type != JSON_NULL) {
                if (jloop->type == JSON_BOOL) {
                    loop_override = jloop->u.bval ? 0 : 1;
                } else if (jloop->type == JSON_NUMBER) {
                    double d = jloop->u.nval;
                    if (d != (double)(long)d || d < 0.0 || d > 2147483647.0) {
                        free(img_bytes);
                        snprintf(err_buf, err_size,
                                 "'image' op 'loop' must be a non-negative integer (0 = infinite)");
                        return false;
                    }
                    loop_override = (int)d;
                } else {
                    free(img_bytes);
                    snprintf(err_buf, err_size,
                             "'image' op 'loop' must be a non-negative integer (0 = infinite)");
                    return false;
                }
            }

            MarkeeGif *gif = NULL;
            bool decoded = gif_decode_placed(img_bytes, img_len, bw, bh, x, y,
                                             has_fill, fill_c.r, fill_c.g, fill_c.b,
                                             &gif, err_buf, err_size);
            free(img_bytes);
            if (!decoded) {
                return false;
            }

            if (loop_override >= 0) {
                gif->loop_count = (gif->frame_count <= 1) ? 1 : loop_override;
            }

            gif_blit_frame(gif, 0, buf, bw, bh);

            if (gif->frame_count > 1 && gif->width > 0 && gif->height > 0) {
                if (staged_gifs->count >= MARKEE_MAX_GIF_LAYERS) {
                    gif_free(gif);
                    snprintf(err_buf, err_size,
                             "at most %d animated 'image' ops are supported per /draw batch",
                             MARKEE_MAX_GIF_LAYERS);
                    return false;
                }
                staged_gifs->gifs[staged_gifs->count++] = gif;
            } else {
                /* Static 1-frame GIF (or completely off-screen): bake into static background. */
                gif_blit_frame(gif, 0, static_bg, bw, bh);
                gif_free(gif);
            }
            return true;
        }

        free(img_bytes);
        snprintf(err_buf, err_size, "'image' op 'data' must be a valid PNG or GIF image");
        return false;
    }

    snprintf(err_buf, err_size,
             "unknown op '%s' (supported: line, rect, oval, polygon, text, pixel, clear, image)",
             op);
    return false;
}

bool draw_exec_json(MarkeeDisplay *disp, const char *json_body, size_t json_len,
                    int *out_ops_executed, char *err_buf, size_t err_size) {
    if (out_ops_executed) *out_ops_executed = 0;
    if (err_buf && err_size > 0) err_buf[0] = '\0';
    if (!disp || !disp->pixels) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "display not initialized");
        return false;
    }

    JsonValue *root = json_parse(json_body, json_len, err_buf, err_size);
    if (!root) return false;

    if (root->type != JSON_ARRAY || root->u.arr.count == 0) {
        snprintf(err_buf, err_size, "JSON body must be an array of one or more draw operations");
        json_free(root);
        return false;
    }

    const size_t frame_bytes = disp->frame_bytes;
    uint8_t *stage = malloc(frame_bytes);
    uint8_t *static_bg = malloc(frame_bytes);
    uint8_t *stage_bg = malloc(frame_bytes);
    if (!stage || !static_bg || !stage_bg) {
        free(stage);
        free(static_bg);
        free(stage_bg);
        json_free(root);
        snprintf(err_buf, err_size, "out of memory");
        return false;
    }

    MarkeeScrollState next_scroll;
    memset(&next_scroll, 0, sizeof(next_scroll));
    next_scroll.bg_pixels = stage_bg;

    StagedGifs staged_gifs;
    memset(&staged_gifs, 0, sizeof(staged_gifs));

    pthread_mutex_lock(&disp->lock);
    memcpy(stage, disp->pixels, frame_bytes);
    memcpy(static_bg, disp->pixels, frame_bytes);

    bool ok = true;
    int executed = 0;
    for (size_t i = 0; i < root->u.arr.count; i++) {
        if (!exec_single_op_buf(disp, stage, static_bg, disp->width, disp->height, frame_bytes,
                                &next_scroll, &staged_gifs,
                                root->u.arr.items[i], err_buf, err_size)) {
            ok = false;
            break;
        }
        executed++;
    }

    if (ok) {
        display_stop_gif_unlocked(disp);
        memcpy(disp->pixels, stage, frame_bytes);
        uint8_t *existing_bg = disp->scroll.bg_pixels;
        disp->scroll = next_scroll;
        disp->scroll.bg_pixels = existing_bg;
        if (next_scroll.active && existing_bg) {
            memcpy(existing_bg, stage_bg, frame_bytes);
        }
        if (staged_gifs.count > 0) {
            if (disp->gif.bg_pixels) {
                memcpy(disp->gif.bg_pixels, static_bg, frame_bytes);
            }
            for (size_t i = 0; i < staged_gifs.count; i++) {
                disp->gif.layers[i].gif = staged_gifs.gifs[i];
                disp->gif.layers[i].current_frame = 0;
                disp->gif.layers[i].loops_done = 0;
                disp->gif.layers[i].finished = false;
                disp->gif.layers[i].accum_ms = 0.0;
                staged_gifs.gifs[i] = NULL;
            }
            disp->gif.count = staged_gifs.count;
            disp->gif.active = true;
            disp->gif.has_last_time = false;
            disp->gif.last_time = 0.0;
            staged_gifs.count = 0;
        }
        disp->demo_active = false;
        disp->frames_received++;
        disp->frame_seq++;
        display_push_event_unlocked(disp, MARKEE_EVT_FRAME, -1, -1, "draw");
    } else {
        staged_gifs_clear(&staged_gifs);
    }
    pthread_mutex_unlock(&disp->lock);

    free(stage);
    free(static_bg);
    free(stage_bg);
    json_free(root);
    if (out_ops_executed) *out_ops_executed = ok ? executed : 0;
    return ok;
}
