#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "audio.h"
#include "config.h"
#include "draw.h"
#include "json.h"

struct MarkeeServer {
    MarkeeDisplay *disp;
    MarkeeAudio *audio;
    int listen_fd;
    uint16_t port;
    char host[64];
    atomic_bool running;
    atomic_int active_clients;
    pthread_t accept_thread;
};

typedef struct {
    MarkeeServer *srv;
    int client_fd;
} ClientCtx;

typedef struct {
    char method[16];
    char path[256];
    char query[512];
    char content_type[128];
    char accept[128];
    size_t content_length;
    bool has_content_length;
    bool chunked;
    bool expect_continue;
    bool keep_alive;
    uint8_t *body;
    size_t body_len;
} HttpRequest;

static bool send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_response_ex(int fd, int status, const char *status_text,
                             const char *content_type, const void *body, size_t body_len,
                             bool keep_alive, const char *extra_headers) {
    char header[768];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type, Accept\r\n"
                        "Connection: %s\r\n"
                        "%s"
                        "\r\n",
                        status, status_text, content_type, body_len,
                        keep_alive ? "keep-alive" : "close",
                        extra_headers ? extra_headers : "");
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) return false;
    if (!send_all(fd, header, (size_t)hlen)) return false;
    if (body && body_len > 0) {
        if (!send_all(fd, body, body_len)) return false;
    }
    return true;
}

static bool send_response(int fd, int status, const char *status_text,
                          const char *content_type, const void *body, size_t body_len,
                          bool keep_alive) {
    return send_response_ex(fd, status, status_text, content_type, body, body_len,
                            keep_alive, NULL);
}

static bool send_json(int fd, int status, const char *status_text,
                      const char *json, bool keep_alive) {
    return send_response(fd, status, status_text, "application/json",
                         json, strlen(json), keep_alive);
}

/* Escape a string for safe embedding inside a JSON string literal.
 * Output is always NUL-terminated and truncated (on a character boundary) to fit. */
static void json_escape(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        char tmp[8];
        const char *rep = NULL;
        size_t rlen;
        switch (*p) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    rep = tmp;
                }
                break;
        }
        if (rep) {
            rlen = strlen(rep);
        } else {
            tmp[0] = (char)*p;
            tmp[1] = '\0';
            rep = tmp;
            rlen = 1;
        }
        if (o + rlen >= out_size) break;
        memcpy(out + o, rep, rlen);
        o += rlen;
    }
    out[o] = '\0';
}

/* Send {"error":"<msg>"} with msg JSON-escaped. */
static bool send_error(int fd, int status, const char *status_text, const char *msg,
                       bool keep_alive) {
    char esc[512];
    char json[600];
    json_escape(msg, esc, sizeof(esc));
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", esc);
    return send_json(fd, status, status_text, json, keep_alive);
}

/* Case-insensitive substring check */
static bool contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return true;
    }
    return false;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-decode src[0..len) into out ('+' -> space, %XX -> byte). Invalid escapes are kept
 * literally. Returns false if the decoded value does not fit in out. */
static bool url_decode(const char *src, size_t len, char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < len) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        if (o + 1 >= out_size) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

/* Extract and URL-decode a query parameter value from "a=1&b=2".
 * Returns false if the key is absent or its decoded value does not fit in out. */
static bool query_get(const char *query, const char *key, char *out, size_t out_size) {
    if (!query || !key || !out || out_size == 0) return false;
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            return url_decode(p + klen + 1, seg_len - klen - 1, out, out_size);
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static bool parse_int_param(const char *s, int *out) {
    if (!s || !*s || !out) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = (int)v;
    return true;
}

/* Parse a color from a (URL-decoded) query parameter or JSON string using the same rules
 * as /draw: hex with optional '#' or '0x' prefix, 3 or 6 digits, or a named color. */
static bool parse_color_param(const char *s, MarkeeColor *out) {
    if (!s || !out) return false;
    return draw_parse_color_string(s, out);
}

static bool parse_color_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    MarkeeColor c;
    if (!parse_color_param(s, &c)) return false;
    *r = c.r;
    *g = c.g;
    *b = c.b;
    return true;
}

/* Parse a request body as JSON. Returns NULL if there is no body or it is not valid JSON.
 * Caller frees with json_free(). */
static JsonValue *parse_body_json(const uint8_t *body, size_t len) {
    if (!body || len == 0) return NULL;
    return json_parse((const char *)body, len, NULL, 0);
}

/* Read an integer member from a JSON object. Accepts only whole numbers. */
static bool json_member_int(const JsonValue *obj, const char *key, int *out) {
    if (!obj || obj->type != JSON_OBJECT) return false;
    const JsonValue *v = json_obj_get(obj, key);
    double d = 0.0;
    if (!v || !json_as_double(v, &d)) return false;
    if (d != (double)(long)d || d < -2147483648.0 || d > 2147483647.0) return false;
    *out = (int)d;
    return true;
}

/* Buffered reader for persistent HTTP connections */
typedef struct {
    int fd;
    uint8_t buf[65536];
    size_t len;
    size_t pos;
    bool overflow; /* set when a line exceeded the caller's buffer */
} SockReader;

static bool reader_fill(SockReader *r) {
    if (r->pos > 0) {
        if (r->pos < r->len) {
            memmove(r->buf, r->buf + r->pos, r->len - r->pos);
            r->len -= r->pos;
        } else {
            r->len = 0;
        }
        r->pos = 0;
    }
    if (r->len >= sizeof(r->buf)) return false;
    for (;;) {
        ssize_t n = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        r->len += (size_t)n;
        return true;
    }
}

static bool reader_read_exact(SockReader *r, uint8_t *dst, size_t count) {
    size_t got = 0;
    while (got < count) {
        size_t avail = r->len - r->pos;
        if (avail > 0) {
            size_t take = count - got;
            if (take > avail) take = avail;
            memcpy(dst + got, r->buf + r->pos, take);
            r->pos += take;
            got += take;
        } else {
            if (!reader_fill(r)) return false;
        }
    }
    return true;
}

static bool reader_read_line(SockReader *r, char *out, size_t max_len) {
    size_t n = 0;
    for (;;) {
        while (r->pos < r->len) {
            char c = (char)r->buf[r->pos++];
            if (c == '\n') {
                if (n > 0 && out[n - 1] == '\r') n--;
                out[n] = '\0';
                return true;
            }
            if (n + 1 >= max_len) {
                r->overflow = true;
                return false;
            }
            out[n++] = c;
        }
        if (!reader_fill(r)) return false;
    }
}

static bool read_chunked_body(SockReader *r, uint8_t **out_buf, size_t *out_len) {
    size_t cap = 4096;
    size_t total = 0;
    uint8_t *buf = malloc(cap + 1);
    if (!buf) return false;

    char line[128];
    for (;;) {
        if (!reader_read_line(r, line, sizeof(line))) {
            free(buf);
            return false;
        }
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';

        char *end = NULL;
        unsigned long chunk_size = strtoul(line, &end, 16);
        if (end == line) {
            free(buf);
            return false;
        }
        if (chunk_size == 0) {
            while (reader_read_line(r, line, sizeof(line)) && line[0] != '\0') {}
            buf[total] = '\0';
            *out_buf = buf;
            *out_len = total;
            return true;
        }
        if (total + chunk_size > MARKEE_MAX_BODY_BYTES) {
            free(buf);
            return false;
        }
        if (total + chunk_size > cap) {
            while (cap < total + chunk_size) cap *= 2;
            if (cap > MARKEE_MAX_BODY_BYTES) cap = MARKEE_MAX_BODY_BYTES;
            uint8_t *nbuf = realloc(buf, cap + 1);
            if (!nbuf) {
                free(buf);
                return false;
            }
            buf = nbuf;
        }
        if (!reader_read_exact(r, buf + total, (size_t)chunk_size)) {
            free(buf);
            return false;
        }
        total += (size_t)chunk_size;
        if (!reader_read_line(r, line, sizeof(line))) {
            free(buf);
            return false;
        }
    }
}

#define READ_OK 1
#define READ_EOF 0
#define READ_BAD -1
#define READ_URI_TOO_LONG -2

/* Read one HTTP request. Returns READ_OK, READ_EOF (clean close / timeout),
 * READ_BAD (protocol or size error), or READ_URI_TOO_LONG. */
static int read_http_request(SockReader *r, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    r->overflow = false;

    char line[1024];
    if (!reader_read_line(r, line, sizeof(line))) {
        return r->overflow ? READ_URI_TOO_LONG : READ_EOF;
    }
    if (line[0] == '\0') {
        if (!reader_read_line(r, line, sizeof(line))) {
            return r->overflow ? READ_URI_TOO_LONG : READ_EOF;
        }
    }

    char method[32] = {0};
    char target[1024] = {0};
    char version[32] = {0};
    if (sscanf(line, "%31s %1023s %31s", method, target, version) != 3 ||
        strncmp(version, "HTTP/1.", 7) != 0 || strlen(method) >= sizeof(req->method)) {
        return READ_BAD;
    }
    snprintf(req->method, sizeof(req->method), "%s", method);

    req->keep_alive = (strcmp(version, "HTTP/1.1") == 0);

    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        if (strlen(q + 1) >= sizeof(req->query)) return READ_URI_TOO_LONG;
        snprintf(req->query, sizeof(req->query), "%s", q + 1);
    }
    if (strlen(target) >= sizeof(req->path)) return READ_URI_TOO_LONG;
    snprintf(req->path, sizeof(req->path), "%s", target);

    size_t header_bytes = strlen(line);
    for (;;) {
        if (!reader_read_line(r, line, sizeof(line))) return READ_BAD;
        header_bytes += strlen(line) + 2;
        if (header_bytes > MARKEE_MAX_HEADER_BYTES) return -1;
        if (line[0] == '\0') break;

        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcasecmp(line, "Content-Length") == 0) {
            char *end = NULL;
            unsigned long cl = strtoul(val, &end, 10);
            if (end == val || cl > MARKEE_MAX_BODY_BYTES) return -1;
            req->content_length = (size_t)cl;
            req->has_content_length = true;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            snprintf(req->content_type, sizeof(req->content_type), "%s", val);
        } else if (strcasecmp(line, "Accept") == 0) {
            snprintf(req->accept, sizeof(req->accept), "%s", val);
        } else if (strcasecmp(line, "Connection") == 0) {
            if (contains_ci(val, "close")) req->keep_alive = false;
            else if (contains_ci(val, "keep-alive")) req->keep_alive = true;
        } else if (strcasecmp(line, "Expect") == 0) {
            if (contains_ci(val, "100-continue")) req->expect_continue = true;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (contains_ci(val, "chunked")) req->chunked = true;
        }
    }

    if (req->expect_continue) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        if (!send_all(r->fd, cont, strlen(cont))) return -1;
    }

    if (req->chunked) {
        if (!read_chunked_body(r, &req->body, &req->body_len)) return -1;
    } else if (req->has_content_length && req->content_length > 0) {
        req->body = malloc(req->content_length + 1);
        if (!req->body) return -1;
        if (!reader_read_exact(r, req->body, req->content_length)) {
            free(req->body);
            req->body = NULL;
            return -1;
        }
        req->body[req->content_length] = '\0';
        req->body_len = req->content_length;
    }

    return 1;
}

static bool has_png_magic(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return data && len >= 8 && memcmp(data, sig, 8) == 0;
}

/* Color for /fill and /pixel, from (in order): ?color=, ?r=&g=&b=, JSON "color", JSON r/g/b.
 * body_json may be NULL. */
static bool extract_rgb_params(const HttpRequest *req, const JsonValue *body_json,
                               uint8_t *r, uint8_t *g, uint8_t *b) {
    char val[64];
    if (query_get(req->query, "color", val, sizeof(val))) {
        return parse_color_rgb(val, r, g, b);
    }
    int ri = 0, gi = 0, bi = 0;
    bool have = false;
    if (query_get(req->query, "r", val, sizeof(val)) && parse_int_param(val, &ri) &&
        query_get(req->query, "g", val, sizeof(val)) && parse_int_param(val, &gi) &&
        query_get(req->query, "b", val, sizeof(val)) && parse_int_param(val, &bi)) {
        have = true;
    } else if (body_json && body_json->type == JSON_OBJECT) {
        const JsonValue *jcol = json_obj_get(body_json, "color");
        if (jcol) {
            const char *s = json_as_string(jcol);
            return s ? parse_color_rgb(s, r, g, b) : false;
        }
        have = json_member_int(body_json, "r", &ri) &&
               json_member_int(body_json, "g", &gi) &&
               json_member_int(body_json, "b", &bi);
    }
    if (!have) return false;
    if (ri < 0 || ri > 255 || gi < 0 || gi > 255 || bi < 0 || bi > 255) return false;
    *r = (uint8_t)ri;
    *g = (uint8_t)gi;
    *b = (uint8_t)bi;
    return true;
}

/* Known paths and the methods each supports (used for 405 + Allow). */
typedef struct {
    const char *path;
    const char *allow;
} RouteMethods;

static const RouteMethods KNOWN_ROUTES[] = {
    {"/",           "GET, OPTIONS"},
    {"/info",       "GET, OPTIONS"},
    {"/api/info",   "GET, OPTIONS"},
    {"/frame",      "GET, POST, PUT, DELETE, OPTIONS"},
    {"/api/frame",  "GET, POST, PUT, DELETE, OPTIONS"},
    {"/frame.png",  "GET, POST, PUT, OPTIONS"},
    {"/frame.rgb",  "GET, POST, PUT, OPTIONS"},
    {"/clear",      "POST, OPTIONS"},
    {"/fill",       "POST, OPTIONS"},
    {"/pixel",      "GET, POST, PUT, OPTIONS"},
    {"/draw",       "POST, PUT, OPTIONS"},
    {"/api/draw",   "POST, PUT, OPTIONS"},
    {"/monochrome", "GET, POST, PUT, DELETE, OPTIONS"},
    {"/mono",       "GET, POST, PUT, DELETE, OPTIONS"},
    {"/brightness", "GET, POST, PUT, OPTIONS"},
    {"/demo",       "POST, OPTIONS"},
    {"/alert",      "GET, POST, OPTIONS"},
    {"/api/alert",  "GET, POST, OPTIONS"},
    {"/alerts",     "GET, OPTIONS"},
    {"/api/alerts", "GET, OPTIONS"},
};

/* If path is "/alert/<seg>" or "/api/alert/<seg>" (single segment), return pointer to <seg>. */
static const char *alert_path_segment(const char *path) {
    const char *seg = NULL;
    if (strncmp(path, "/alert/", 7) == 0) {
        seg = path + 7;
    } else if (strncmp(path, "/api/alert/", 11) == 0) {
        seg = path + 11;
    }
    if (!seg || !*seg || strchr(seg, '/') != NULL) return NULL;
    return seg;
}

static const char *route_allow(const char *path) {
    for (size_t i = 0; i < sizeof(KNOWN_ROUTES) / sizeof(KNOWN_ROUTES[0]); i++) {
        if (strcmp(path, KNOWN_ROUTES[i].path) == 0) return KNOWN_ROUTES[i].allow;
    }
    const char *seg = alert_path_segment(path);
    if (seg && audio_parse_alert_id(seg) > 0) {
        return "POST, OPTIONS";
    }
    return NULL;
}

/* Parse optional volume (0..100) from query ?volume= or JSON object "volume".
 * Sets *out_vol (defaults to 100 when omitted). Returns false if present but invalid. */
static bool extract_alert_volume(const HttpRequest *req, const JsonValue *body_json, int *out_vol) {
    *out_vol = 100;
    char val[32];
    if (query_get(req->query, "volume", val, sizeof(val))) {
        int v = -1;
        if (!parse_int_param(val, &v) || v < 0 || v > 100) return false;
        *out_vol = v;
        return true;
    }
    if (body_json && body_json->type == JSON_OBJECT) {
        const JsonValue *jv = json_obj_get(body_json, "volume");
        if (jv) {
            int v = -1;
            if (!json_member_int(body_json, "volume", &v) || v < 0 || v > 100) return false;
            *out_vol = v;
            return true;
        }
    }
    return true;
}

/* Parse an alert sound ID from a JSON value (integer 1..N or string "1".."N" / name). */
static int parse_alert_json_value(const JsonValue *v) {
    if (!v) return 0;
    if (v->type == JSON_NUMBER) {
        double d = v->u.nval;
        if (d == (double)(long)d && d >= 1.0 && d <= (double)MARKEE_ALERT_COUNT) {
            return (int)d;
        }
        return 0;
    }
    if (v->type == JSON_STRING) {
        return audio_parse_alert_id(v->u.sval);
    }
    return 0;
}

static void handle_request(MarkeeServer *srv, int fd, const HttpRequest *req) {
    MarkeeDisplay *disp = srv->disp;
    bool ka = req->keep_alive;

    if (strcmp(req->method, "OPTIONS") == 0) {
        send_response(fd, 204, "No Content", "text/plain", "", 0, ka);
        return;
    }

    /* GET / or GET /info */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/") == 0 || strcmp(req->path, "/info") == 0 ||
         strcmp(req->path, "/api/info") == 0)) {
        double phys_w_in = (double)MARKEE_MATRIX_WIDTH_INCHES *
                           ((double)disp->width / (double)MARKEE_DEFAULT_WIDTH);
        bool mono = false;
        uint8_t mr = 0, mg = 0, mb = 0;
        display_get_monochrome(disp, &mono, &mr, &mg, &mb);
        char mono_json[16];
        if (mono) {
            snprintf(mono_json, sizeof(mono_json), "\"#%02x%02x%02x\"", mr, mg, mb);
        } else {
            snprintf(mono_json, sizeof(mono_json), "null");
        }
        char json[640];
        snprintf(json, sizeof(json),
                 "{\"name\":\"markee\","
                 "\"width\":%d,"
                 "\"height\":%d,"
                 "\"bits_per_pixel\":24,"
                 "\"raw_frame_bytes\":%zu,"
                 "\"refresh_hz\":%d,"
                 "\"physical_width_inches\":%.2f,"
                 "\"brightness\":%u,"
                 "\"monochrome\":%s,"
                 "\"demo_active\":%s,"
                 "\"frames_received\":%llu,"
                 "\"audio_ready\":%s,"
                 "\"alert_sounds\":%d}",
                 disp->width, disp->height, disp->frame_bytes,
                 MARKEE_REFRESH_HZ, phys_w_in,
                 (unsigned)display_get_brightness(disp),
                 mono_json,
                 display_is_demo_active(disp) ? "true" : "false",
                 (unsigned long long)display_get_frames_received(disp),
                 audio_is_ready(srv->audio) ? "true" : "false",
                 MARKEE_ALERT_COUNT);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* GET /frame, /frame.png, /frame.rgb, /api/frame */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/frame") == 0 || strcmp(req->path, "/frame.png") == 0 ||
         strcmp(req->path, "/frame.rgb") == 0 || strcmp(req->path, "/api/frame") == 0)) {
        char fmt[32] = {0};
        bool want_raw = (strcmp(req->path, "/frame.rgb") == 0) ||
                        (query_get(req->query, "format", fmt, sizeof(fmt)) &&
                         (strcasecmp(fmt, "raw") == 0 || strcasecmp(fmt, "rgb") == 0)) ||
                        contains_ci(req->accept, "application/octet-stream");

        if (want_raw) {
            uint8_t *raw = malloc(disp->frame_bytes);
            if (!raw) {
                send_json(fd, 500, "Internal Server Error",
                          "{\"error\":\"out of memory\"}", ka);
                return;
            }
            display_get_raw_rgb(disp, raw, disp->frame_bytes);
            send_response(fd, 200, "OK", "application/octet-stream", raw, disp->frame_bytes, ka);
            free(raw);
            return;
        }

        uint8_t *png = NULL;
        size_t png_len = 0;
        if (display_encode_png(disp, &png, &png_len) != MARKEE_OK) {
            send_json(fd, 500, "Internal Server Error",
                      "{\"error\":\"failed to encode PNG\"}", ka);
            return;
        }
        send_response(fd, 200, "OK", "image/png", png, png_len, ka);
        display_free_buffer(png);
        return;
    }

    /* POST or PUT /frame, /frame.png, /frame.rgb, /api/frame */
    if ((strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) &&
        (strcmp(req->path, "/frame") == 0 || strcmp(req->path, "/frame.png") == 0 ||
         strcmp(req->path, "/frame.rgb") == 0 || strcmp(req->path, "/api/frame") == 0)) {
        if (!req->body || req->body_len == 0) {
            char err[192];
            snprintf(err, sizeof(err),
                     "{\"error\":\"empty body; send a %dx%d PNG or %zu raw RGB bytes\"}",
                     disp->width, disp->height, disp->frame_bytes);
            send_json(fd, 400, "Bad Request", err, ka);
            return;
        }

        bool is_raw_path = (strcmp(req->path, "/frame.rgb") == 0);
        bool is_png_payload = has_png_magic(req->body, req->body_len);

        if (!is_raw_path && (is_png_payload || contains_ci(req->content_type, "image/png") ||
                             strcmp(req->path, "/frame.png") == 0)) {
            int aw = 0, ah = 0;
            MarkeeResult res = display_load_png(disp, req->body, req->body_len, &aw, &ah);
            if (res == MARKEE_OK) {
                char ok[128];
                snprintf(ok, sizeof(ok),
                         "{\"ok\":true,\"format\":\"png\",\"width\":%d,\"height\":%d}",
                         disp->width, disp->height);
                send_json(fd, 200, "OK", ok, ka);
                return;
            }
            char err[192];
            if (res == MARKEE_ERR_WRONG_DIMENSIONS) {
                snprintf(err, sizeof(err),
                         "{\"error\":\"expected %dx%d PNG, got %dx%d\"}",
                         disp->width, disp->height, aw, ah);
            } else {
                snprintf(err, sizeof(err), "{\"error\":\"invalid PNG data\"}");
            }
            send_json(fd, 400, "Bad Request", err, ka);
            return;
        }

        /* Raw 24-bit RGB payload (W * H * 3 bytes) */
        if (req->body_len == disp->frame_bytes) {
            display_set_raw_rgb(disp, req->body, req->body_len, true);
            char ok[128];
            snprintf(ok, sizeof(ok),
                     "{\"ok\":true,\"format\":\"rgb24\",\"width\":%d,\"height\":%d}",
                     disp->width, disp->height);
            send_json(fd, 200, "OK", ok, ka);
            return;
        }

        char err[192];
        snprintf(err, sizeof(err),
                 "{\"error\":\"expected %dx%d PNG or %zu raw RGB bytes, got %zu bytes\"}",
                 disp->width, disp->height, disp->frame_bytes, req->body_len);
        send_json(fd, 400, "Bad Request", err, ka);
        return;
    }

    /* DELETE /frame or POST /clear */
    if ((strcmp(req->method, "DELETE") == 0 &&
         (strcmp(req->path, "/frame") == 0 || strcmp(req->path, "/api/frame") == 0)) ||
        (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/clear") == 0)) {
        display_clear(disp, true);
        send_json(fd, 200, "OK", "{\"ok\":true}", ka);
        return;
    }

    /* POST /fill */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/fill") == 0) {
        uint8_t r = 0, g = 0, b = 0;
        JsonValue *bj = parse_body_json(req->body, req->body_len);
        bool got = extract_rgb_params(req, bj, &r, &g, &b);
        json_free(bj);
        if (!got) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide ?color=(RRGGBB, 0xRRGGBB, RGB, or name), ?r=..&g=..&b=.., or JSON {color} / {r,g,b}\"}",
                      ka);
            return;
        }
        display_fill(disp, r, g, b, true);
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"r\":%u,\"g\":%u,\"b\":%u,\"hex\":\"#%02x%02x%02x\"}",
                 r, g, b, r, g, b);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* GET /pixel?x=..&y=.. */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/pixel") == 0) {
        char xv[32], yv[32];
        int x = -1, y = -1;
        if (!query_get(req->query, "x", xv, sizeof(xv)) || !parse_int_param(xv, &x) ||
            !query_get(req->query, "y", yv, sizeof(yv)) || !parse_int_param(yv, &y)) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide integer query parameters x and y\"}", ka);
            return;
        }
        uint8_t r = 0, g = 0, b = 0;
        if (display_get_pixel(disp, x, y, &r, &g, &b) != MARKEE_OK) {
            char err[128];
            snprintf(err, sizeof(err),
                     "{\"error\":\"pixel coordinates out of range (0..%d, 0..%d)\"}",
                     disp->width - 1, disp->height - 1);
            send_json(fd, 400, "Bad Request", err, ka);
            return;
        }
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"x\":%d,\"y\":%d,\"r\":%u,\"g\":%u,\"b\":%u,\"hex\":\"#%02x%02x%02x\"}",
                 x, y, r, g, b, r, g, b);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* POST or PUT /pixel */
    if ((strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) &&
        strcmp(req->path, "/pixel") == 0) {
        char xv[32], yv[32];
        int x = -1, y = -1;
        bool has_xy = (query_get(req->query, "x", xv, sizeof(xv)) && parse_int_param(xv, &x) &&
                       query_get(req->query, "y", yv, sizeof(yv)) && parse_int_param(yv, &y));
        JsonValue *bj = parse_body_json(req->body, req->body_len);
        if (!has_xy) {
            has_xy = json_member_int(bj, "x", &x) && json_member_int(bj, "y", &y);
        }
        uint8_t r = 0, g = 0, b = 0;
        bool got = has_xy && extract_rgb_params(req, bj, &r, &g, &b);
        json_free(bj);
        if (!got) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide x, y and color (RRGGBB, 0xRRGGBB, RGB, name, or r,g,b)\"}", ka);
            return;
        }
        if (display_set_pixel(disp, x, y, r, g, b, true) != MARKEE_OK) {
            char err[128];
            snprintf(err, sizeof(err),
                     "{\"error\":\"pixel coordinates out of range (0..%d, 0..%d)\"}",
                     disp->width - 1, disp->height - 1);
            send_json(fd, 400, "Bad Request", err, ka);
            return;
        }
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"x\":%d,\"y\":%d,\"r\":%u,\"g\":%u,\"b\":%u}",
                 x, y, r, g, b);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* POST or PUT /draw (and /api/draw): JSON array of one or more draw ops */
    if ((strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) &&
        (strcmp(req->path, "/draw") == 0 || strcmp(req->path, "/api/draw") == 0)) {
        if (!req->body || req->body_len == 0) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"empty body; send a JSON array of one or more draw operations\"}", ka);
            return;
        }
        int executed = 0;
        char err[256] = {0};
        if (!draw_exec_json(disp, (const char *)req->body, req->body_len,
                            &executed, err, sizeof(err))) {
            send_error(fd, 400, "Bad Request", err[0] ? err : "invalid draw request", ka);
            return;
        }
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":true,\"ops\":%d}", executed);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* GET /monochrome */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/monochrome") == 0 || strcmp(req->path, "/mono") == 0)) {
        bool enabled = false;
        uint8_t mr = 0, mg = 0, mb = 0;
        display_get_monochrome(disp, &enabled, &mr, &mg, &mb);
        char json[128];
        if (enabled) {
            snprintf(json, sizeof(json),
                     "{\"enabled\":true,\"r\":%u,\"g\":%u,\"b\":%u,\"hex\":\"#%02x%02x%02x\"}",
                     mr, mg, mb, mr, mg, mb);
        } else {
            snprintf(json, sizeof(json), "{\"enabled\":false,\"color\":null}");
        }
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* DELETE /monochrome */
    if (strcmp(req->method, "DELETE") == 0 &&
        (strcmp(req->path, "/monochrome") == 0 || strcmp(req->path, "/mono") == 0)) {
        display_set_monochrome(disp, false, 0, 0, 0);
        send_json(fd, 200, "OK", "{\"ok\":true,\"enabled\":false,\"color\":null}", ka);
        return;
    }

    /* POST or PUT /monochrome (also /mono):
     * Accepts ?color=#rrggbb, JSON {"color":"#rrggbb"} / {"color":null}, or plain body "#rrggbb" / "off" */
    if ((strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) &&
        (strcmp(req->path, "/monochrome") == 0 || strcmp(req->path, "/mono") == 0)) {
        bool disable = false;
        bool has_color = false;
        MarkeeColor mc = {0, 0, 0};
        char val[64];

        if (query_get(req->query, "color", val, sizeof(val))) {
            if (strcasecmp(val, "none") == 0 || strcasecmp(val, "off") == 0 ||
                strcasecmp(val, "false") == 0 || strcasecmp(val, "null") == 0) {
                disable = true;
            } else if (parse_color_param(val, &mc)) {
                has_color = true;
            }
        } else if (query_get(req->query, "enabled", val, sizeof(val)) &&
                   (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0)) {
            disable = true;
        } else if (req->body && req->body_len > 0) {
            /* Try JSON first, then trimmed plain text color string */
            JsonValue *root = json_parse((const char *)req->body, req->body_len, NULL, 0);
            if (root) {
                if (root->type == JSON_NULL) {
                    disable = true;
                } else if (root->type == JSON_STRING) {
                    const char *s = root->u.sval;
                    if (strcasecmp(s, "none") == 0 || strcasecmp(s, "off") == 0) {
                        disable = true;
                    } else if (draw_parse_color_string(s, &mc)) {
                        has_color = true;
                    }
                } else if (root->type == JSON_OBJECT) {
                    const JsonValue *jen = json_obj_get(root, "enabled");
                    bool en = true;
                    if (jen && json_as_bool(jen, &en) && !en) {
                        disable = true;
                    } else {
                        const JsonValue *jcol = json_obj_get(root, "color");
                        if (jcol) {
                            if (jcol->type == JSON_NULL) {
                                disable = true;
                            } else if (jcol->type == JSON_STRING) {
                                const char *s = jcol->u.sval;
                                if (strcasecmp(s, "none") == 0 || strcasecmp(s, "off") == 0) {
                                    disable = true;
                                } else if (draw_parse_color_string(s, &mc)) {
                                    has_color = true;
                                }
                            }
                        }
                    }
                }
                json_free(root);
            } else {
                const char *s = (const char *)req->body;
                while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
                size_t len = strlen(s);
                while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                                   s[len - 1] == '\r' || s[len - 1] == '\n')) {
                    len--;
                }
                if (len > 0 && len < sizeof(val)) {
                    memcpy(val, s, len);
                    val[len] = '\0';
                    if (strcasecmp(val, "none") == 0 || strcasecmp(val, "off") == 0 ||
                        strcasecmp(val, "false") == 0 || strcasecmp(val, "null") == 0) {
                        disable = true;
                    } else if (draw_parse_color_string(val, &mc)) {
                        has_color = true;
                    }
                }
            }
        }

        if (!disable && !has_color) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide a 24-bit color (e.g. #00ff00, #ffbf00, amber) or 'off'/'none' to disable\"}",
                      ka);
            return;
        }

        if (disable) {
            display_set_monochrome(disp, false, 0, 0, 0);
            send_json(fd, 200, "OK", "{\"ok\":true,\"enabled\":false,\"color\":null}", ka);
            return;
        }

        display_set_monochrome(disp, true, mc.r, mc.g, mc.b);
        char json[128];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"enabled\":true,\"r\":%u,\"g\":%u,\"b\":%u,\"hex\":\"#%02x%02x%02x\"}",
                 mc.r, mc.g, mc.b, mc.r, mc.g, mc.b);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* GET /brightness */
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/brightness") == 0) {
        char json[64];
        snprintf(json, sizeof(json), "{\"brightness\":%u}",
                 (unsigned)display_get_brightness(disp));
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* POST or PUT /brightness */
    if ((strcmp(req->method, "POST") == 0 || strcmp(req->method, "PUT") == 0) &&
        strcmp(req->path, "/brightness") == 0) {
        char val[32];
        int b = -1;
        bool ok = false;
        if (query_get(req->query, "value", val, sizeof(val)) && parse_int_param(val, &b)) {
            ok = true;
        } else if (req->body && req->body_len > 0) {
            /* JSON object {"brightness":N} / {"value":N}, or a bare number such as "75" */
            JsonValue *bj = parse_body_json(req->body, req->body_len);
            if (bj && bj->type == JSON_OBJECT) {
                ok = json_member_int(bj, "brightness", &b) || json_member_int(bj, "value", &b);
            } else if (bj && bj->type == JSON_NUMBER) {
                double d = bj->u.nval;
                if (d == (double)(long)d && d >= -1000.0 && d <= 1000.0) {
                    b = (int)d;
                    ok = true;
                }
            }
            json_free(bj);
        }
        if (!ok || b < 0 || b > 100) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"brightness must be an integer 0..100\"}", ka);
            return;
        }
        display_set_brightness(disp, (uint8_t)b);
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":true,\"brightness\":%d}", b);
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* POST /demo */
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/demo") == 0) {
        char val[16];
        bool enabled = true;
        if (query_get(req->query, "enabled", val, sizeof(val))) {
            enabled = !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0);
        }
        display_set_demo(disp, enabled);
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":true,\"demo_active\":%s}",
                 enabled ? "true" : "false");
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* GET /alert, /alerts, /api/alert, /api/alerts */
    if (strcmp(req->method, "GET") == 0 &&
        (strcmp(req->path, "/alert") == 0 || strcmp(req->path, "/alerts") == 0 ||
         strcmp(req->path, "/api/alert") == 0 || strcmp(req->path, "/api/alerts") == 0)) {
        char json[1024];
        int off = snprintf(json, sizeof(json),
                           "{\"audio_ready\":%s,\"alerts_played\":%llu,\"sounds\":[",
                           audio_is_ready(srv->audio) ? "true" : "false",
                           (unsigned long long)audio_get_trigger_count(srv->audio));
        for (int i = 1; i <= MARKEE_ALERT_COUNT && off > 0 && (size_t)off < sizeof(json); i++) {
            const MarkeeAlertInfo *info = audio_get_alert_info(i);
            off += snprintf(json + off, sizeof(json) - (size_t)off,
                            "%s{\"id\":%d,\"name\":\"%s\",\"description\":\"%s\",\"duration_ms\":%d}",
                            i > 1 ? "," : "",
                            info->id, info->name, info->description, info->duration_ms);
        }
        if (off > 0 && (size_t)off + 3 < sizeof(json)) {
            snprintf(json + off, sizeof(json) - (size_t)off, "]}");
        }
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    /* POST /alert/<id|name> or POST /api/alert/<id|name> */
    if (strcmp(req->method, "POST") == 0) {
        const char *seg = alert_path_segment(req->path);
        if (seg) {
            int sound_id = audio_parse_alert_id(seg);
            if (sound_id <= 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "unknown alert sound '%s' (valid: 1/ding, 2/chime, 3/bell)", seg);
                send_error(fd, 404, "Not Found", msg, ka);
                return;
            }
            JsonValue *bj = NULL;
            if (req->body && req->body_len > 0) {
                const char *s = (const char *)req->body;
                while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
                if (*s != '\0') {
                    bj = parse_body_json(req->body, req->body_len);
                    if (!bj || bj->type != JSON_OBJECT) {
                        json_free(bj);
                        send_json(fd, 400, "Bad Request",
                                  "{\"error\":\"optional body must be a JSON object (e.g. {\\\"volume\\\":80})\"}",
                                  ka);
                        return;
                    }
                }
            }
            int volume = 100;
            bool vol_ok = extract_alert_volume(req, bj, &volume);
            json_free(bj);
            if (!vol_ok) {
                send_json(fd, 400, "Bad Request",
                          "{\"error\":\"volume must be an integer 0..100\"}", ka);
                return;
            }
            const MarkeeAlertInfo *info = audio_get_alert_info(sound_id);
            bool played = audio_trigger(srv->audio, sound_id, (uint8_t)volume);
            char json[160];
            snprintf(json, sizeof(json),
                     "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"volume\":%d,\"played\":%s}",
                     info->id, info->name, volume, played ? "true" : "false");
            send_json(fd, 200, "OK", json, ka);
            return;
        }
    }

    /* POST /alert or POST /api/alert */
    if (strcmp(req->method, "POST") == 0 &&
        (strcmp(req->path, "/alert") == 0 || strcmp(req->path, "/api/alert") == 0)) {
        int sound_id = 1; /* default: 1 (ding) */
        bool sound_ok = true;
        char val[64];

        JsonValue *bj = parse_body_json(req->body, req->body_len);
        int volume = 100;
        if (!extract_alert_volume(req, bj, &volume)) {
            json_free(bj);
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"volume must be an integer 0..100\"}", ka);
            return;
        }

        if (query_get(req->query, "sound", val, sizeof(val)) ||
            query_get(req->query, "id", val, sizeof(val))) {
            sound_id = audio_parse_alert_id(val);
            sound_ok = (sound_id > 0);
        } else if (req->body && req->body_len > 0) {
            const char *s = (const char *)req->body;
            while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
            if (*s != '\0') {
                if (bj) {
                    if (bj->type == JSON_OBJECT) {
                        const JsonValue *js = json_obj_get(bj, "sound");
                        if (!js) js = json_obj_get(bj, "id");
                        if (js) {
                            sound_id = parse_alert_json_value(js);
                            sound_ok = (sound_id > 0);
                        }
                    } else if (bj->type == JSON_NUMBER || bj->type == JSON_STRING) {
                        sound_id = parse_alert_json_value(bj);
                        sound_ok = (sound_id > 0);
                    } else {
                        sound_ok = false;
                    }
                } else {
                    sound_id = audio_parse_alert_id(s);
                    sound_ok = (sound_id > 0);
                }
            }
        }
        json_free(bj);

        if (!sound_ok) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"sound must be 1..3 or one of: ding, chime, bell\"}", ka);
            return;
        }

        const MarkeeAlertInfo *info = audio_get_alert_info(sound_id);
        bool played = audio_trigger(srv->audio, sound_id, (uint8_t)volume);
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"volume\":%d,\"played\":%s}",
                 info->id, info->name, volume, played ? "true" : "false");
        send_json(fd, 200, "OK", json, ka);
        return;
    }

    const char *allow = route_allow(req->path);
    if (allow) {
        char hdr[128];
        char body[192];
        snprintf(hdr, sizeof(hdr), "Allow: %s\r\n", allow);
        /* Method came from the client: escape it rather than embedding raw. */
        char esc_method[64];
        json_escape(req->method, esc_method, sizeof(esc_method));
        snprintf(body, sizeof(body), "{\"error\":\"method %s not allowed; allowed: %s\"}",
                 esc_method, allow);
        send_response_ex(fd, 405, "Method Not Allowed", "application/json",
                         body, strlen(body), ka, hdr);
        return;
    }
    send_json(fd, 404, "Not Found", "{\"error\":\"endpoint not found\"}", ka);
}

static void *client_thread_main(void *arg) {
    ClientCtx *ctx = (ClientCtx *)arg;
    MarkeeServer *srv = ctx->srv;
    int fd = ctx->client_fd;
    free(ctx);

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    SockReader reader;
    reader.fd = fd;
    reader.len = 0;
    reader.pos = 0;
    reader.overflow = false;

    while (atomic_load(&srv->running)) {
        HttpRequest req;
        int rc = read_http_request(&reader, &req);
        if (rc == READ_EOF) {
            free(req.body);
            break;
        }
        if (rc == READ_URI_TOO_LONG) {
            send_json(fd, 414, "URI Too Long",
                      "{\"error\":\"request target too long (max 255-byte path, 511-byte query)\"}",
                      false);
            free(req.body);
            break;
        }
        if (rc != READ_OK) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"malformed HTTP request\"}", false);
            free(req.body);
            break;
        }

        handle_request(srv, fd, &req);
        free(req.body);

        if (!req.keep_alive) {
            break;
        }
    }

    close(fd);
    atomic_fetch_sub(&srv->active_clients, 1);
    return NULL;
}

static void *accept_thread_main(void *arg) {
    MarkeeServer *srv = (MarkeeServer *)arg;

    while (atomic_load(&srv->running)) {
        struct pollfd pfd = {.fd = srv->listen_fd, .events = POLLIN, .revents = 0};
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;

        if (pfd.revents & POLLIN) {
            struct sockaddr_storage caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(srv->listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd < 0) continue;

            if (atomic_load(&srv->active_clients) >= MARKEE_MAX_CLIENTS) {
                send_json(cfd, 503, "Service Unavailable",
                          "{\"error\":\"too many concurrent clients\"}", false);
                close(cfd);
                continue;
            }

            ClientCtx *ctx = malloc(sizeof(*ctx));
            if (!ctx) {
                close(cfd);
                continue;
            }
            ctx->srv = srv;
            ctx->client_fd = cfd;

            atomic_fetch_add(&srv->active_clients, 1);
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&tid, &attr, client_thread_main, ctx) != 0) {
                atomic_fetch_sub(&srv->active_clients, 1);
                free(ctx);
                close(cfd);
            }
            pthread_attr_destroy(&attr);
        }
    }

    return NULL;
}

MarkeeServer *server_start_ex(MarkeeDisplay *disp, MarkeeAudio *audio,
                              const char *host, uint16_t port) {
    if (!disp) return NULL;
    if (!host || !*host) host = MARKEE_DEFAULT_HOST;

    /* Resolve host (IPv4 literal, IPv6 literal, or hostname) and bind the first usable address. */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "Invalid bind host address '%s': %s\n", host, gai_strerror(gai));
        return NULL;
    }

    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 32) == 0) {
            break;
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "Failed to bind %s:%u: %s\n", host, (unsigned)port, strerror(last_errno));
        return NULL;
    }

    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0) {
        if (addr.ss_family == AF_INET) {
            port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
        }
    }

    MarkeeServer *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        close(fd);
        return NULL;
    }

    srv->disp = disp;
    srv->audio = audio;
    srv->listen_fd = fd;
    srv->port = port;
    snprintf(srv->host, sizeof(srv->host), "%s", host);
    atomic_store(&srv->running, true);
    atomic_store(&srv->active_clients, 0);

    if (pthread_create(&srv->accept_thread, NULL, accept_thread_main, srv) != 0) {
        perror("pthread_create");
        close(fd);
        free(srv);
        return NULL;
    }

    return srv;
}

MarkeeServer *server_start(MarkeeDisplay *disp, const char *host, uint16_t port) {
    return server_start_ex(disp, NULL, host, port);
}

uint16_t server_get_port(const MarkeeServer *srv) {
    return srv ? srv->port : 0;
}

void server_stop(MarkeeServer *srv) {
    if (!srv) return;
    atomic_store(&srv->running, false);
    if (srv->listen_fd >= 0) {
        shutdown(srv->listen_fd, SHUT_RDWR);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    pthread_join(srv->accept_thread, NULL);
    free(srv);
}
