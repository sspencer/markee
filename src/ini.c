#define _POSIX_C_SOURCE 200809L

#include "ini.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"

void markee_config_defaults(MarkeeConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->width = MARKEE_DEFAULT_WIDTH;
    cfg->height = MARKEE_DEFAULT_HEIGHT;
    cfg->zoom = MARKEE_DEFAULT_ZOOM;
    cfg->pixel_shape = MARKEE_PIXEL_SQUARE;
    cfg->mono_enabled = false;
    cfg->mono_color.r = 255;
    cfg->mono_color.g = 191;
    cfg->mono_color.b = 0;
    snprintf(cfg->host, sizeof(cfg->host), "%s", MARKEE_DEFAULT_HOST);
    cfg->port = MARKEE_DEFAULT_PORT;
    cfg->token[0] = '\0';
    cfg->override_dpi = 0.0f;
    cfg->always_on_top = false;
    cfg->borderless = false;
    cfg->has_position = false;
    cfg->center_x = false;
    cfg->center_y = false;
    cfg->pos_x = 0;
    cfg->pos_y = 0;
    cfg->corner = MARKEE_CORNER_CENTER;
    cfg->case_theme = MARKEE_CASE_WHITE;
    cfg->idle_dim_sec = 0.0;
    cfg->idle_brightness = 20;
    cfg->start_demo = false;
    cfg->mute = false;
    cfg->headless = false;
    cfg->save_position = false;
}

bool markee_config_default_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        int n = snprintf(out, out_size, "%s/markee/config.ini", xdg);
        return n > 0 && (size_t)n < out_size;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        int n = snprintf(out, out_size, "%s/.config/markee/config.ini", home);
        return n > 0 && (size_t)n < out_size;
    }
    return false;
}

static char *trim_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s;
}

static bool parse_bool_val(const char *s, bool *out) {
    if (!s || !out) return false;
    if (strcasecmp(s, "true") == 0 || strcasecmp(s, "1") == 0 ||
        strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(s, "false") == 0 || strcasecmp(s, "0") == 0 ||
        strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_res(const char *s, int *out_w, int *out_h) {
    char *sep = NULL;
    long w = strtol(s, &sep, 10);
    if (sep == s || (*sep != 'x' && *sep != 'X')) return false;
    if (!isdigit((unsigned char)sep[1])) return false;
    char *end = NULL;
    long h = strtol(sep + 1, &end, 10);
    if (end == sep + 1 || *end != '\0') return false;
    if (w < MARKEE_MIN_DIM || w > MARKEE_MAX_DIM ||
        h < MARKEE_MIN_DIM || h > MARKEE_MAX_DIM) return false;
    *out_w = (int)w;
    *out_h = (int)h;
    return true;
}

static void strip_inline_comment(char *s) {
    bool in_quote = false;
    char qchar = '\0';
    for (char *p = s; *p; p++) {
        if (!in_quote && (*p == '"' || *p == '\'')) {
            in_quote = true;
            qchar = *p;
        } else if (in_quote && *p == qchar) {
            in_quote = false;
        } else if (!in_quote && (*p == '#' || *p == ';')) {
            /* Only treat '#' as comment if preceded by whitespace (so monochrome = #ffbf00 works!) */
            if (*p == ';' || p == s || isspace((unsigned char)p[-1])) {
                if (*p == '#' && p > s && p[-1] == '=') continue;
                /* If '#' is immediately followed by 3 or 6 hex digits and end/ws, keep as color */
                if (*p == '#') {
                    size_t h = 0;
                    while (isxdigit((unsigned char)p[1 + h])) h++;
                    if ((h == 3 || h == 6) && (p[1 + h] == '\0' || isspace((unsigned char)p[1 + h]))) {
                        continue;
                    }
                }
                *p = '\0';
                break;
            }
        }
    }
}

bool markee_config_parse_string(const char *text, MarkeeConfig *cfg,
                                char *err_buf, size_t err_size) {
    if (!text || !cfg) return false;

    int line_no = 0;
    const char *p = text;
    while (*p) {
        line_no++;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0 && p[len - 1] == '\r') len--;

        char line[512];
        if (len >= sizeof(line)) {
            if (err_buf && err_size > 0) {
                snprintf(err_buf, err_size, "line %d: line too long", line_no);
            }
            return false;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        char *s = trim_ws(line);
        if (*s == '\0' || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *rb = strchr(s, ']');
            if (!rb) {
                if (err_buf && err_size > 0) {
                    snprintf(err_buf, err_size, "line %d: unterminated section header", line_no);
                }
                return false;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            if (err_buf && err_size > 0) {
                snprintf(err_buf, err_size, "line %d: expected 'key = value'", line_no);
            }
            return false;
        }
        *eq = '\0';
        char *key = trim_ws(s);
        char *val = eq + 1;
        strip_inline_comment(val);
        val = trim_ws(val);

        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen - 1] == '"') ||
                          (val[0] == '\'' && val[vlen - 1] == '\''))) {
            val[vlen - 1] = '\0';
            val++;
        }

        if (strcasecmp(key, "resolution") == 0) {
            if (!parse_res(val, &cfg->width, &cfg->height)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid resolution '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "zoom") == 0) {
            char *end = NULL;
            float z = strtof(val, &end);
            if (end == val || *end != '\0' || z < MARKEE_MIN_ZOOM || z > MARKEE_MAX_ZOOM) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid zoom '%s'", line_no, val);
                return false;
            }
            cfg->zoom = z;
        } else if (strcasecmp(key, "shape") == 0 || strcasecmp(key, "pixel_shape") == 0) {
            if (strcasecmp(val, "square") == 0) cfg->pixel_shape = MARKEE_PIXEL_SQUARE;
            else if (strcasecmp(val, "circle") == 0) cfg->pixel_shape = MARKEE_PIXEL_CIRCLE;
            else {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid shape '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "monochrome") == 0 || strcasecmp(key, "mono") == 0) {
            if (*val == '\0' || strcasecmp(val, "off") == 0 || strcasecmp(val, "false") == 0 ||
                strcasecmp(val, "none") == 0) {
                cfg->mono_enabled = false;
            } else if (draw_parse_color_string(val, &cfg->mono_color)) {
                cfg->mono_enabled = true;
            } else {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid monochrome color '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "host") == 0) {
            if (*val) snprintf(cfg->host, sizeof(cfg->host), "%s", val);
        } else if (strcasecmp(key, "port") == 0) {
            char *end = NULL;
            long port = strtol(val, &end, 10);
            if (end == val || *end != '\0' || port < 0 || port > 65535) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid port '%s'", line_no, val);
                return false;
            }
            cfg->port = (uint16_t)port;
        } else if (strcasecmp(key, "token") == 0) {
            snprintf(cfg->token, sizeof(cfg->token), "%s", val);
        } else if (strcasecmp(key, "dpi") == 0) {
            char *end = NULL;
            float d = strtof(val, &end);
            if (end == val || *end != '\0' || d < 0.0f || d > 10000.0f) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid dpi '%s'", line_no, val);
                return false;
            }
            cfg->override_dpi = d;
        } else if (strcasecmp(key, "always_on_top") == 0 || strcasecmp(key, "topmost") == 0) {
            if (!parse_bool_val(val, &cfg->always_on_top)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "borderless") == 0 || strcasecmp(key, "frameless") == 0) {
            if (!parse_bool_val(val, &cfg->borderless)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "position") == 0 || strcasecmp(key, "pos") == 0) {
            if (*val == '\0' || strcasecmp(val, "auto") == 0 || strcasecmp(val, "none") == 0) {
                cfg->has_position = false;
                cfg->center_x = false;
                cfg->center_y = false;
            } else {
                MarkeeWindowOpts tmp = {0};
                if (renderer_parse_position(val, &tmp)) {
                    cfg->has_position = tmp.has_position;
                    cfg->center_x = tmp.center_x;
                    cfg->center_y = tmp.center_y;
                    cfg->pos_x = tmp.pos_x;
                    cfg->pos_y = tmp.pos_y;
                    if (!tmp.has_position) cfg->corner = tmp.corner;
                } else {
                    if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid position '%s'", line_no, val);
                    return false;
                }
            }
        } else if (strcasecmp(key, "corner") == 0) {
            if (!renderer_parse_corner(val, &cfg->corner)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid corner '%s'", line_no, val);
                return false;
            }
            cfg->has_position = false;
            cfg->center_x = false;
            cfg->center_y = false;
        } else if (strcasecmp(key, "case") == 0 || strcasecmp(key, "case_theme") == 0) {
            if (!renderer_parse_case_theme(val, &cfg->case_theme)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid case theme '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "idle_dim") == 0) {
            char *end = NULL;
            double sec = strtod(val, &end);
            if (end == val || *end != '\0' || sec < 0.0 || sec > 86400.0) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid idle_dim '%s'", line_no, val);
                return false;
            }
            cfg->idle_dim_sec = sec;
        } else if (strcasecmp(key, "idle_brightness") == 0) {
            char *end = NULL;
            long b = strtol(val, &end, 10);
            if (end == val || *end != '\0' || b < 0 || b > 100) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid idle_brightness '%s'", line_no, val);
                return false;
            }
            cfg->idle_brightness = (uint8_t)b;
        } else if (strcasecmp(key, "demo") == 0) {
            if (!parse_bool_val(val, &cfg->start_demo)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "mute") == 0) {
            if (!parse_bool_val(val, &cfg->mute)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "headless") == 0) {
            if (!parse_bool_val(val, &cfg->headless)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else if (strcasecmp(key, "save_position") == 0) {
            if (!parse_bool_val(val, &cfg->save_position)) {
                if (err_buf && err_size > 0) snprintf(err_buf, err_size, "line %d: invalid boolean '%s'", line_no, val);
                return false;
            }
        } else {
            if (err_buf && err_size > 0) {
                snprintf(err_buf, err_size, "line %d: unknown config key '%s'", line_no, key);
            }
            return false;
        }
    }

    return true;
}

bool markee_config_load_file(const char *path, MarkeeConfig *cfg,
                             bool *out_not_found, char *err_buf, size_t err_size) {
    if (out_not_found) *out_not_found = false;
    if (!path || !*path || !cfg) return false;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errno == ENOENT && out_not_found) *out_not_found = true;
        if (err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "cannot open '%s': %s", path, strerror(errno));
        }
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 0 || sz > 256 * 1024) {
        fclose(fp);
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "config file too large");
        return false;
    }
    rewind(fp);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    bool ok = markee_config_parse_string(buf, cfg, err_buf, err_size);
    free(buf);
    return ok;
}

static void mkdir_p_parent(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0755);
}

bool markee_config_save_position(const char *path, int pos_x, int pos_y) {
    if (!path || !*path) return false;
    mkdir_p_parent(path);

    char *existing = NULL;
    FILE *in = fopen(path, "rb");
    if (in) {
        if (fseek(in, 0, SEEK_END) == 0) {
            long sz = ftell(in);
            if (sz >= 0 && sz <= 256 * 1024) {
                rewind(in);
                existing = malloc((size_t)sz + 1);
                if (existing) {
                    size_t got = fread(existing, 1, (size_t)sz, in);
                    existing[got] = '\0';
                }
            }
        }
        fclose(in);
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        free(existing);
        return false;
    }

    bool replaced = false;
    if (existing) {
        const char *p = existing;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            size_t raw_len = len;
            if (len > 0 && p[len - 1] == '\r') len--;

            char line[512];
            bool is_pos = false;
            if (len < sizeof(line)) {
                memcpy(line, p, len);
                line[len] = '\0';
                char *s = trim_ws(line);
                char *eq = strchr(s, '=');
                if (eq && *s != '#' && *s != ';') {
                    *eq = '\0';
                    char *k = trim_ws(s);
                    if (strcasecmp(k, "position") == 0 || strcasecmp(k, "pos") == 0) {
                        is_pos = true;
                    }
                }
            }

            if (is_pos) {
                if (!replaced) {
                    fprintf(out, "position = %d,%d\n", pos_x, pos_y);
                    replaced = true;
                }
            } else {
                fwrite(p, 1, raw_len, out);
                fputc('\n', out);
            }
            p = nl ? nl + 1 : p + strlen(p);
        }
        free(existing);
    }

    if (!replaced) {
        fprintf(out, "position = %d,%d\n", pos_x, pos_y);
    }

    fclose(out);
    return true;
}
