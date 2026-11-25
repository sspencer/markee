#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
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

#include "config.h"
#include "draw.h"
#include "json.h"

struct MarkeeServer {
    MarkeeDisplay *disp;
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

static bool send_response(int fd, int status, const char *status_text,
                          const char *content_type, const void *body, size_t body_len,
                          bool keep_alive) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
                        "Access-Control-Allow-Headers: Content-Type, Accept\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        status, status_text, content_type, body_len,
                        keep_alive ? "keep-alive" : "close");
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) return false;
    if (!send_all(fd, header, (size_t)hlen)) return false;
    if (body && body_len > 0) {
        if (!send_all(fd, body, body_len)) return false;
    }
    return true;
}

static bool send_json(int fd, int status, const char *status_text,
                      const char *json, bool keep_alive) {
    return send_response(fd, status, status_text, "application/json",
                         json, strlen(json), keep_alive);
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

/* Extract a query parameter value from "a=1&b=2" */
static bool query_get(const char *query, const char *key, char *out, size_t out_size) {
    if (!query || !key || !out || out_size == 0) return false;
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t vlen = seg_len - klen - 1;
            if (vlen >= out_size) vlen = out_size - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            return true;
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

static bool parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!s) return false;
    if (*s == '#') s++;
    /* Also handle URL-encoded %23 prefix */
    if (s[0] == '%' && s[1] == '2' && s[2] == '3') s += 3;
    if (strlen(s) != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    unsigned int val = 0;
    if (sscanf(s, "%06x", &val) != 1) return false;
    *r = (uint8_t)((val >> 16) & 0xFFU);
    *g = (uint8_t)((val >> 8) & 0xFFU);
    *b = (uint8_t)(val & 0xFFU);
    return true;
}

/* Minimal JSON integer field extractor: finds "key": <int> */
static bool json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

/* Minimal JSON string field extractor: finds "key": "value" */
static bool json_get_str(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != '"') return false;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* Buffered reader for persistent HTTP connections */
typedef struct {
    int fd;
    uint8_t buf[65536];
    size_t len;
    size_t pos;
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
            if (n + 1 >= max_len) return false;
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

/* Read one HTTP request. Returns: 1 = ok, 0 = clean EOF, -1 = protocol/size error */
static int read_http_request(SockReader *r, HttpRequest *req) {
    memset(req, 0, sizeof(*req));

    char line[1024];
    if (!reader_read_line(r, line, sizeof(line))) {
        return 0;
    }
    if (line[0] == '\0') {
        if (!reader_read_line(r, line, sizeof(line))) return 0;
    }

    char target[768] = {0};
    char version[16] = {0};
    if (sscanf(line, "%15s %767s %15s", req->method, target, version) != 3) {
        return -1;
    }

    req->keep_alive = (strcmp(version, "HTTP/1.1") == 0);

    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        snprintf(req->query, sizeof(req->query), "%s", q + 1);
    }
    snprintf(req->path, sizeof(req->path), "%s", target);

    size_t header_bytes = strlen(line);
    while (reader_read_line(r, line, sizeof(line))) {
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

static bool extract_rgb_params(const HttpRequest *req, uint8_t *r, uint8_t *g, uint8_t *b) {
    char val[64];
    if (query_get(req->query, "color", val, sizeof(val))) {
        return parse_hex_color(val, r, g, b);
    }
    int ri = 0, gi = 0, bi = 0;
    if (query_get(req->query, "r", val, sizeof(val)) && parse_int_param(val, &ri) &&
        query_get(req->query, "g", val, sizeof(val)) && parse_int_param(val, &gi) &&
        query_get(req->query, "b", val, sizeof(val)) && parse_int_param(val, &bi)) {
        if (ri < 0 || ri > 255 || gi < 0 || gi > 255 || bi < 0 || bi > 255) return false;
        *r = (uint8_t)ri;
        *g = (uint8_t)gi;
        *b = (uint8_t)bi;
        return true;
    }
    if (req->body && req->body_len > 0) {
        const char *jb = (const char *)req->body;
        if (json_get_str(jb, "color", val, sizeof(val))) {
            return parse_hex_color(val, r, g, b);
        }
        if (json_get_int(jb, "r", &ri) &&
            json_get_int(jb, "g", &gi) &&
            json_get_int(jb, "b", &bi)) {
            if (ri < 0 || ri > 255 || gi < 0 || gi > 255 || bi < 0 || bi > 255) return false;
            *r = (uint8_t)ri;
            *g = (uint8_t)gi;
            *b = (uint8_t)bi;
            return true;
        }
    }
    return false;
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
        char json[576];
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
                 "\"frames_received\":%llu}",
                 disp->width, disp->height, disp->frame_bytes,
                 MARKEE_REFRESH_HZ, phys_w_in,
                 (unsigned)display_get_brightness(disp),
                 mono_json,
                 display_is_demo_active(disp) ? "true" : "false",
                 (unsigned long long)display_get_frames_received(disp));
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
        if (!extract_rgb_params(req, &r, &g, &b)) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide ?color=RRGGBB, ?r=..&g=..&b=.., or JSON {r,g,b}\"}",
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
        if (!has_xy && req->body && req->body_len > 0) {
            has_xy = json_get_int((const char *)req->body, "x", &x) &&
                     json_get_int((const char *)req->body, "y", &y);
        }
        uint8_t r = 0, g = 0, b = 0;
        if (!has_xy || !extract_rgb_params(req, &r, &g, &b)) {
            send_json(fd, 400, "Bad Request",
                      "{\"error\":\"provide x, y and color (RRGGBB or r,g,b)\"}", ka);
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
            char resp[320];
            snprintf(resp, sizeof(resp), "{\"error\":\"%s\"}",
                     err[0] ? err : "invalid draw request");
            send_json(fd, 400, "Bad Request", resp, ka);
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
            /* Decode leading %23 for URL-encoded '#' */
            const char *s = val;
            char unesc[64];
            if (s[0] == '%' && s[1] == '2' && s[2] == '3') {
                snprintf(unesc, sizeof(unesc), "#%s", s + 3);
                s = unesc;
            }
            if (strcasecmp(s, "none") == 0 || strcasecmp(s, "off") == 0 ||
                strcasecmp(s, "false") == 0 || strcasecmp(s, "null") == 0) {
                disable = true;
            } else if (draw_parse_color_string(s, &mc)) {
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
            const char *s = (const char *)req->body;
            if (json_get_int(s, "brightness", &b) || json_get_int(s, "value", &b)) {
                ok = true;
            } else {
                while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
                char *end = NULL;
                long v = strtol(s, &end, 10);
                if (end != s) {
                    b = (int)v;
                    ok = true;
                }
            }
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

    while (atomic_load(&srv->running)) {
        HttpRequest req;
        int rc = read_http_request(&reader, &req);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"malformed HTTP request\"}", false);
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
            struct sockaddr_in caddr;
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

MarkeeServer *server_start(MarkeeDisplay *disp, const char *host, uint16_t port) {
    if (!disp) return NULL;
    if (!host || !*host) host = MARKEE_DEFAULT_HOST;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind host address: %s\n", host);
        close(fd);
        return NULL;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return NULL;
    }

    if (listen(fd, 32) < 0) {
        perror("listen");
        close(fd);
        return NULL;
    }

    socklen_t alen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0) {
        port = ntohs(addr.sin_port);
    }

    MarkeeServer *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        close(fd);
        return NULL;
    }

    srv->disp = disp;
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
