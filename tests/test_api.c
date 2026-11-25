#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <assert.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.h"
#include "config.h"
#include "display.h"
#include "draw.h"
#include "gif.h"
#include "json.h"
#include "raylib.h"
#include "renderer.h"
#include "server.h"

typedef struct {
    int status;
    char content_type[128];
    uint8_t *body;
    size_t body_len;
} HttpResp;

static int connect_local(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

static void send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        assert(n > 0);
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static HttpResp do_request_on_fd(int fd, const char *method, const char *path,
                                 const char *content_type, const void *body, size_t body_len,
                                 bool keep_alive) {
    char req_hdr[512];
    int hlen = snprintf(req_hdr, sizeof(req_hdr),
                        "%s %s HTTP/1.1\r\n"
                        "Host: 127.0.0.1\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        method, path,
                        content_type ? content_type : "application/octet-stream",
                        body_len,
                        keep_alive ? "keep-alive" : "close");
    assert(hlen > 0 && (size_t)hlen < sizeof(req_hdr));
    send_all(fd, req_hdr, (size_t)hlen);
    if (body && body_len > 0) {
        send_all(fd, body, body_len);
    }

    size_t cap = 65536;
    uint8_t *raw = malloc(cap);
    assert(raw != NULL);
    size_t got = 0;
    uint8_t *hdr_end = NULL;

    while (!hdr_end) {
        if (got == cap) {
            cap *= 2;
            raw = realloc(raw, cap);
            assert(raw != NULL);
        }
        ssize_t n = recv(fd, raw + got, cap - got, 0);
        assert(n > 0);
        got += (size_t)n;
        for (size_t i = 0; i + 3 < got; i++) {
            if (raw[i] == '\r' && raw[i + 1] == '\n' &&
                raw[i + 2] == '\r' && raw[i + 3] == '\n') {
                hdr_end = raw + i + 4;
                break;
            }
        }
    }

    size_t header_len = (size_t)(hdr_end - raw);
    char *hdr_str = malloc(header_len + 1);
    assert(hdr_str != NULL);
    memcpy(hdr_str, raw, header_len);
    hdr_str[header_len] = '\0';

    HttpResp resp;
    memset(&resp, 0, sizeof(resp));
    assert(sscanf(hdr_str, "HTTP/1.1 %d", &resp.status) == 1);

    size_t clen = 0;
    const char *cl = strstr(hdr_str, "Content-Length:");
    if (cl) {
        clen = (size_t)strtoul(cl + 15, NULL, 10);
    }
    const char *ct = strstr(hdr_str, "Content-Type:");
    if (ct) {
        ct += 13;
        while (*ct == ' ') ct++;
        const char *eol = strstr(ct, "\r\n");
        size_t ctlen = eol ? (size_t)(eol - ct) : strlen(ct);
        if (ctlen >= sizeof(resp.content_type)) ctlen = sizeof(resp.content_type) - 1;
        memcpy(resp.content_type, ct, ctlen);
        resp.content_type[ctlen] = '\0';
    }
    free(hdr_str);

    size_t already = got - header_len;
    resp.body = malloc(clen + 1);
    assert(resp.body != NULL);
    if (already > 0) {
        size_t copy = already > clen ? clen : already;
        memcpy(resp.body, hdr_end, copy);
        already = copy;
    }
    while (already < clen) {
        ssize_t n = recv(fd, resp.body + already, clen - already, 0);
        assert(n > 0);
        already += (size_t)n;
    }
    resp.body[clen] = '\0';
    resp.body_len = clen;
    free(raw);
    return resp;
}

static HttpResp do_request(uint16_t port, const char *method, const char *path,
                           const char *content_type, const void *body, size_t body_len) {
    int fd = connect_local(port);
    HttpResp r = do_request_on_fd(fd, method, path, content_type, body, body_len, false);
    close(fd);
    return r;
}

static void test_layout_sizing(void) {
    /* Default 72x16 @ zoom=1.0, square pixels matches 6.35" physical matrix width */
    MarkeeLayout L = renderer_compute_layout(72, 16, 1.0f, MARKEE_PIXEL_SQUARE, 0.0f, 1512, 302);
    float inches = L.matrix_w / L.dpi;
    assert(fabsf(inches - MARKEE_MATRIX_WIDTH_INCHES) < 0.01f);
    assert(L.body_w > L.matrix_w);
    assert(L.body_h > L.matrix_h);
    assert(L.pixel_shape == MARKEE_PIXEL_SQUARE);

    /* Circle pixel mode preserves identical layout geometry */
    MarkeeLayout Lc = renderer_compute_layout(72, 16, 1.0f, MARKEE_PIXEL_CIRCLE, 0.0f, 1512, 302);
    assert(Lc.pixel_shape == MARKEE_PIXEL_CIRCLE);
    assert(fabsf(Lc.matrix_w - L.matrix_w) < 0.001f);
    assert(fabsf(Lc.matrix_h - L.matrix_h) < 0.001f);

    /* Zoom scaling: 2.0x doubles matrix, cell pitch, and body dimensions */
    MarkeeLayout Lz2 = renderer_compute_layout(72, 16, 2.0f, MARKEE_PIXEL_SQUARE, 0.0f, 1512, 302);
    assert(fabsf(Lz2.matrix_w - L.matrix_w * 2.0f) < 0.01f);
    assert(fabsf(Lz2.matrix_h - L.matrix_h * 2.0f) < 0.01f);
    assert(fabsf(Lz2.cell_pitch - L.cell_pitch * 2.0f) < 0.01f);
    assert(fabsf(Lz2.body_w - L.body_w * 2.0f) < 0.01f);
    assert(fabsf(Lz2.body_h - L.body_h * 2.0f) < 0.01f);

    /* Custom resolution preserves LED pitch and scales active matrix proportionally */
    MarkeeLayout L144 = renderer_compute_layout(144, 32, 1.0f, MARKEE_PIXEL_SQUARE, 0.0f, 1512, 302);
    assert(fabsf(L144.cell_pitch - L.cell_pitch) < 0.001f);
    assert(fabsf(L144.matrix_w - L.matrix_w * 2.0f) < 0.01f);
    assert(fabsf(L144.matrix_h - L.matrix_h * 2.0f) < 0.01f);

    /* 144x32 @ zoom=0.5 fits back to 6.35" wide */
    MarkeeLayout L144_half = renderer_compute_layout(144, 32, 0.5f, MARKEE_PIXEL_CIRCLE, 0.0f, 1512, 302);
    assert(fabsf(L144_half.matrix_w - L.matrix_w) < 0.01f);

    /* Fallback when physical width is unavailable */
    MarkeeLayout L2 = renderer_compute_layout(72, 16, 1.0f, MARKEE_PIXEL_SQUARE, 0.0f, 1920, 0);
    assert(fabsf(L2.dpi - MARKEE_DEFAULT_DPI) < 0.01f);
    assert(fabsf(L2.matrix_w / L2.dpi - MARKEE_MATRIX_WIDTH_INCHES) < 0.01f);

    /* Manual DPI override */
    MarkeeLayout L3 = renderer_compute_layout(72, 16, 1.0f, MARKEE_PIXEL_SQUARE, 144.0f, 1920, 500);
    assert(fabsf(L3.dpi - 144.0f) < 0.01f);
    assert(fabsf(L3.matrix_w - 6.35f * 144.0f) < 0.01f);

    /* Corner parsing and window placement */
    MarkeeWindowCorner c;
    assert(renderer_parse_corner("center", &c) && c == MARKEE_CORNER_CENTER);
    assert(renderer_parse_corner("top-right", &c) && c == MARKEE_CORNER_TOP_RIGHT);
    assert(renderer_parse_corner("TR", &c) && c == MARKEE_CORNER_TOP_RIGHT);
    assert(renderer_parse_corner("bottom-left", &c) && c == MARKEE_CORNER_BOTTOM_LEFT);
    assert(renderer_parse_corner("br", &c) && c == MARKEE_CORNER_BOTTOM_RIGHT);
    assert(renderer_parse_corner("top", &c) && c == MARKEE_CORNER_TOP);
    assert(renderer_parse_corner("bottom", &c) && c == MARKEE_CORNER_BOTTOM);
    assert(!renderer_parse_corner("middle", &c));

    int wx = -1, wy = -1;
    MarkeeWindowOpts opts = {0};
    opts.corner = MARKEE_CORNER_TOP_RIGHT;
    renderer_compute_window_position(1920, 1080, 600, 160, &opts, &wx, &wy);
    assert(wx == 1920 - 600 - 24 && wy == 48);

    opts.corner = MARKEE_CORNER_BOTTOM_LEFT;
    renderer_compute_window_position(1920, 1080, 600, 160, &opts, &wx, &wy);
    assert(wx == 24 && wy == 1080 - 160 - 64);

    opts.has_position = true;
    opts.pos_x = 120;
    opts.pos_y = 340;
    renderer_compute_window_position(1920, 1080, 600, 160, &opts, &wx, &wy);
    assert(wx == 120 && wy == 340);

    /* Enclosure case themes */
    MarkeeCaseTheme th;
    assert(renderer_parse_case_theme("white", &th) && th == MARKEE_CASE_WHITE);
    assert(renderer_parse_case_theme("charcoal", &th) && th == MARKEE_CASE_CHARCOAL);
    assert(renderer_parse_case_theme("dark", &th) && th == MARKEE_CASE_CHARCOAL);
    assert(renderer_parse_case_theme("orange", &th) && th == MARKEE_CASE_ORANGE);
    assert(renderer_parse_case_theme("retro", &th) && th == MARKEE_CASE_RETRO);
    assert(renderer_parse_case_theme("beige", &th) && th == MARKEE_CASE_RETRO);
    assert(!renderer_parse_case_theme("neon", &th));
    assert(strcmp(renderer_case_theme_name(MARKEE_CASE_ORANGE), "orange") == 0);

    printf("[PASS] layout physical sizing, zoom, pixel shape, custom resolution, placement, and case themes\n");
}

static void test_display_and_png_roundtrip(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));

    uint8_t pattern[MARKEE_FRAME_BYTES];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 72; x++) {
            size_t idx = (size_t)(y * 72 + x) * 3;
            pattern[idx + 0] = (uint8_t)((x * 3 + y * 5) & 0xFF);
            pattern[idx + 1] = (uint8_t)((y * 16 + x) & 0xFF);
            pattern[idx + 2] = (uint8_t)((255 - x * 3) & 0xFF);
        }
    }
    assert(display_set_raw_rgb(&disp, pattern, sizeof(pattern), true) == MARKEE_OK);

    uint8_t *png = NULL;
    size_t png_len = 0;
    assert(display_encode_png(&disp, &png, &png_len) == MARKEE_OK);
    assert(png != NULL && png_len > 32);

    display_clear(&disp, true);
    int aw = 0, ah = 0;
    assert(display_load_png(&disp, png, png_len, &aw, &ah) == MARKEE_OK);
    assert(aw == 72 && ah == 16);

    uint8_t readback[MARKEE_FRAME_BYTES];
    assert(display_get_raw_rgb(&disp, readback, sizeof(readback)));
    assert(memcmp(pattern, readback, MARKEE_FRAME_BYTES) == 0);
    display_free_buffer(png);

    /* Reject a wrong-sized PNG (16x16 on 72x16 display) */
    uint8_t small_px[16 * 16 * 3];
    memset(small_px, 0x77, sizeof(small_px));
    Image small_img = {
        .data = small_px,
        .width = 16,
        .height = 16,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8
    };
    int small_len = 0;
    unsigned char *small_png = ExportImageToMemory(small_img, ".png", &small_len);
    assert(small_png != NULL && small_len > 0);
    assert(display_load_png(&disp, small_png, (size_t)small_len, &aw, &ah) == MARKEE_ERR_WRONG_DIMENSIONS);
    assert(aw == 16 && ah == 16);
    MemFree(small_png);

    display_destroy(&disp);
    printf("[PASS] display framebuffer & lossless 24-bit PNG roundtrip\n");
}

static void test_http_socket_api(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, true));

    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);
    assert(port > 0);

    /* 1. GET /info */
    HttpResp r = do_request(port, "GET", "/info", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"width\":72") != NULL);
    assert(strstr((char *)r.body, "\"height\":16") != NULL);
    assert(strstr((char *)r.body, "\"refresh_hz\":60") != NULL);
    free(r.body);

    /* 2. POST /fill?color=12abef then GET /pixel */
    r = do_request(port, "POST", "/fill?color=12abef", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    r = do_request(port, "GET", "/pixel?x=35&y=7", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"hex\":\"#12abef\"") != NULL);
    free(r.body);

    /* 3. POST /pixel single pixel override */
    r = do_request(port, "POST", "/pixel?x=0&y=0&r=255&g=64&b=1", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    /* 4. GET /frame -> PNG download */
    r = do_request(port, "GET", "/frame", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strcmp(r.content_type, "image/png") == 0);
    assert(r.body_len > 32);
    uint8_t *saved_png = r.body;
    size_t saved_png_len = r.body_len;

    /* 5. POST /clear */
    r = do_request(port, "POST", "/clear", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    uint8_t pr = 1, pg = 1, pb = 1;
    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);

    /* 6. POST /frame with PNG -> verify pixel restoration */
    r = do_request(port, "POST", "/frame", "image/png", saved_png, saved_png_len);
    assert(r.status == 200);
    free(r.body);
    free(saved_png);

    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 64 && pb == 1);
    assert(display_get_pixel(&disp, 35, 7, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0x12 && pg == 0xab && pb == 0xef);

    /* 7. Keep-alive persistent socket: raw RGB upload + readback + brightness */
    int kfd = connect_local(port);
    uint8_t raw_in[MARKEE_FRAME_BYTES];
    for (size_t i = 0; i < MARKEE_FRAME_BYTES; i++) {
        raw_in[i] = (uint8_t)((i * 37) & 0xFF);
    }

    r = do_request_on_fd(kfd, "PUT", "/frame.rgb", "application/octet-stream",
                         raw_in, sizeof(raw_in), true);
    assert(r.status == 200);
    free(r.body);

    r = do_request_on_fd(kfd, "GET", "/frame?format=raw", NULL, NULL, 0, true);
    assert(r.status == 200);
    assert(r.body_len == MARKEE_FRAME_BYTES);
    assert(memcmp(raw_in, r.body, MARKEE_FRAME_BYTES) == 0);
    free(r.body);

    r = do_request_on_fd(kfd, "POST", "/brightness?value=65", NULL, NULL, 0, false);
    assert(r.status == 200);
    assert(display_get_brightness(&disp) == 65);
    free(r.body);
    close(kfd);

    /* 8. Invalid payload rejected cleanly with 400 */
    const char *bad = "not a png";
    r = do_request(port, "POST", "/frame", "image/png", bad, strlen(bad));
    assert(r.status == 400);
    free(r.body);

    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] HTTP/TCP socket API (72x16 PNG I/O, raw RGB24, keep-alive)\n");
}

static void test_custom_resolution_api(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 32, 8, true));
    display_tick_demo(&disp, 0.5);

    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);

    /* GET /info reports 32x8 and 768 raw bytes */
    HttpResp r = do_request(port, "GET", "/info", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"width\":32") != NULL);
    assert(strstr((char *)r.body, "\"height\":8") != NULL);
    assert(strstr((char *)r.body, "\"raw_frame_bytes\":768") != NULL);
    free(r.body);

    /* Out-of-bounds pixel rejected (32 >= 32) */
    r = do_request(port, "POST", "/pixel?x=32&y=0&color=ffffff", NULL, NULL, 0);
    assert(r.status == 400);
    free(r.body);

    /* Raw 32x8 RGB upload + PNG download + PNG re-upload roundtrip */
    uint8_t raw[32 * 8 * 3];
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)((i * 19 + 7) & 0xFF);

    r = do_request(port, "PUT", "/frame.rgb", "application/octet-stream", raw, sizeof(raw));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"width\":32,\"height\":8") != NULL);
    free(r.body);

    r = do_request(port, "GET", "/frame", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strcmp(r.content_type, "image/png") == 0);
    uint8_t *png32x8 = r.body;
    size_t png32x8_len = r.body_len;

    r = do_request(port, "POST", "/clear", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    r = do_request(port, "POST", "/frame", "image/png", png32x8, png32x8_len);
    assert(r.status == 200);
    free(r.body);
    free(png32x8);

    uint8_t readback[32 * 8 * 3];
    assert(display_get_raw_rgb(&disp, readback, sizeof(readback)));
    assert(memcmp(raw, readback, sizeof(raw)) == 0);

    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] custom resolution (32x8) API & PNG roundtrip\n");
}

static void test_draw_api(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));

    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);
    uint8_t pr, pg, pb;

    /* 1. Single-element array with line op */
    const char *line_json = "[{ \"op\": \"line\", \"x1\": 0, \"y1\": 15, \"x2\": 71, \"y2\": 0, \"color\": \"#00FF00\" }]";
    HttpResp r = do_request(port, "POST", "/draw", "application/json", line_json, strlen(line_json));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"ops\":1") != NULL);
    free(r.body);

    assert(display_get_pixel(&disp, 0, 15, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 255 && pb == 0);
    assert(display_get_pixel(&disp, 71, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 255 && pb == 0);

    /* 2. Multi-shape scene array: clear + rounded rect + oval + polygon */
    const char *scene_json = "["
        "{ \"op\": \"clear\" },"
        "{ \"op\": \"rect\", \"x\": 2, \"y\": 2, \"w\": 30, \"h\": 12, \"color\": \"white\", \"fill\": \"#002040\", \"radius\": 2 },"
        "{ \"op\": \"oval\", \"x\": 40, \"y\": 0, \"w\": 16, \"h\": 16, \"color\": \"yellow\", \"fill\": \"yellow\" },"
        "{ \"op\": \"polygon\", \"points\": [[60,14],[66,2],[71,14]], \"color\": \"red\", \"fill\": \"red\" }"
    "]";
    r = do_request(port, "POST", "/draw", "application/json", scene_json, strlen(scene_json));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"ops\":4") != NULL);
    free(r.body);

    /* Rounded rect corner clipped off -> black; top edge -> white; interior -> #002040 */
    assert(display_get_pixel(&disp, 2, 2, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);
    assert(display_get_pixel(&disp, 5, 2, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 255 && pb == 255);
    assert(display_get_pixel(&disp, 10, 7, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0x00 && pg == 0x20 && pb == 0x40);

    /* Oval corner outside -> black; center -> yellow */
    assert(display_get_pixel(&disp, 40, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);
    assert(display_get_pixel(&disp, 47, 7, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 255 && pb == 0);

    /* Polygon vertex & interior -> red; outside -> black */
    assert(display_get_pixel(&disp, 66, 2, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);
    assert(display_get_pixel(&disp, 66, 10, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);
    assert(display_get_pixel(&disp, 60, 2, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);

    /* 3. Text op with 3-digit hex #FFF and scroll:true inside an array */
    const char *text_json = "["
        "{ \"op\": \"clear\" },"
        "{ \"op\": \"text\", \"x\": 4, \"y\": 4, \"text\": \"OK\", \"font\": \"6x8\", \"color\": \"#FFF\", \"scroll\": true }"
    "]";
    r = do_request(port, "POST", "/draw", "application/json", text_json, strlen(text_json));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"ops\":2") != NULL);
    free(r.body);

    int lit_before = 0;
    for (int y = 4; y < 11; y++) {
        for (int x = 4; x < 16; x++) {
            assert(display_get_pixel(&disp, x, y, &pr, &pg, &pb) == MARKEE_OK);
            if (pr == 255 && pg == 255 && pb == 255) lit_before++;
        }
    }
    assert(lit_before > 10);

    display_tick(&disp, 1.0);
    display_tick(&disp, 1.5);
    int lit_after_same_box = 0;
    for (int y = 4; y < 11; y++) {
        for (int x = 4; x < 16; x++) {
            assert(display_get_pixel(&disp, x, y, &pr, &pg, &pb) == MARKEE_OK);
            if (pr == 255 && pg == 255 && pb == 255) lit_after_same_box++;
        }
    }
    assert(lit_after_same_box < lit_before);

    /* 4. Bare JSON object (not an array) and empty array [] are rejected with 400 */
    const char *bare_obj = "{ \"op\": \"line\", \"x1\": 0, \"y1\": 0, \"x2\": 5, \"y2\": 5, \"color\": \"white\" }";
    r = do_request(port, "POST", "/draw", "application/json", bare_obj, strlen(bare_obj));
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/draw", "application/json", "[]", 2);
    assert(r.status == 400);
    free(r.body);

    /* 5. Atomic rollback: if a later op in the array fails, earlier ops are not committed */
    const char *setup = "[{\"op\":\"clear\"},{\"op\":\"pixel\",\"x\":0,\"y\":0,\"color\":\"blue\"}]";
    r = do_request(port, "POST", "/draw", "application/json", setup, strlen(setup));
    assert(r.status == 200);
    free(r.body);

    const char *bad_batch = "[{\"op\":\"pixel\",\"x\":0,\"y\":0,\"color\":\"red\"},{\"op\":\"star\"}]";
    r = do_request(port, "POST", "/draw", "application/json", bad_batch, strlen(bad_batch));
    assert(r.status == 400);
    free(r.body);

    /* Pixel (0,0) must still be blue (not overwritten by red) */
    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 255);

    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] POST /draw JSON array API (line, rect+radius, oval, polygon, text+scroll, atomic)\n");
}

static void test_monochrome_filter(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));

    /* Set distinct pure primary + white + black pixels */
    assert(display_set_pixel(&disp, 0, 0, 255, 255, 255, true) == MARKEE_OK); /* white */
    assert(display_set_pixel(&disp, 1, 0,   0, 255,   0, true) == MARKEE_OK); /* pure green */
    assert(display_set_pixel(&disp, 2, 0, 255,   0,   0, true) == MARKEE_OK); /* pure red */
    assert(display_set_pixel(&disp, 3, 0,   0,   0, 255, true) == MARKEE_OK); /* pure blue */
    assert(display_set_pixel(&disp, 4, 0,   0,   0,   0, true) == MARKEE_OK); /* black */

    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);

    /* 1. Enable green monochrome filter via JSON body */
    const char *green_req = "{\"color\":\"#00ff00\"}";
    HttpResp r = do_request(port, "POST", "/monochrome", "application/json",
                            green_req, strlen(green_req));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"hex\":\"#00ff00\"") != NULL);
    free(r.body);

    /* Check GET /info reports monochrome */
    r = do_request(port, "GET", "/info", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"monochrome\":\"#00ff00\"") != NULL);
    free(r.body);

    /* Verify BT.709 perceptual luminance ordering: white (255) > green (~220) > red (~127) > blue (~76) > black (0) */
    uint8_t wr, wg, wb, gr, gg, gb, rr, rg, rb, br, bg, bb, kr, kg, kb;
    assert(display_get_pixel(&disp, 0, 0, &wr, &wg, &wb) == MARKEE_OK);
    assert(display_get_pixel(&disp, 1, 0, &gr, &gg, &gb) == MARKEE_OK);
    assert(display_get_pixel(&disp, 2, 0, &rr, &rg, &rb) == MARKEE_OK);
    assert(display_get_pixel(&disp, 3, 0, &br, &bg, &bb) == MARKEE_OK);
    assert(display_get_pixel(&disp, 4, 0, &kr, &kg, &kb) == MARKEE_OK);

    assert(wr == 0 && wg == 255 && wb == 0);
    assert(gr == 0 && gb == 0 && rr == 0 && rb == 0 && br == 0 && bb == 0);
    assert(kr == 0 && kg == 0 && kb == 0);
    assert(wg > gg && gg > rg && rg > bg && bg > kg);

    /* 2. Switch to amber (#ffbf00) via plain text body — non-destructive from original colors */
    const char *amber_req = "#ffbf00";
    r = do_request(port, "POST", "/monochrome", "text/plain", amber_req, strlen(amber_req));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"hex\":\"#ffbf00\"") != NULL);
    free(r.body);

    assert(display_get_pixel(&disp, 0, 0, &wr, &wg, &wb) == MARKEE_OK);
    assert(wr == 0xff && wg == 0xbf && wb == 0x00);

    /* GET /frame PNG readback reflects active amber monochrome pixels */
    r = do_request(port, "GET", "/frame", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strcmp(r.content_type, "image/png") == 0);
    Image img = LoadImageFromMemory(".png", r.body, (int)r.body_len);
    assert(img.data != NULL && img.width == 72 && img.height == 16);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    const uint8_t *px = (const uint8_t *)img.data;
    assert(px[0] == 0xff && px[1] == 0xbf && px[2] == 0x00); /* pixel (0,0) white -> amber */
    assert(px[3] > px[6] && px[6] > px[9]);                  /* green > red > blue */
    UnloadImage(img);
    free(r.body);

    /* 3. Disable monochrome via POST {"color":null} — original full RGB colors restored */
    const char *off_req = "{\"color\":null}";
    r = do_request(port, "POST", "/monochrome", "application/json", off_req, strlen(off_req));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"enabled\":false") != NULL);
    free(r.body);

    assert(display_get_pixel(&disp, 0, 0, &wr, &wg, &wb) == MARKEE_OK);
    assert(wr == 255 && wg == 255 && wb == 255);
    assert(display_get_pixel(&disp, 2, 0, &rr, &rg, &rb) == MARKEE_OK);
    assert(rr == 255 && rg == 0 && rb == 0);
    assert(display_get_pixel(&disp, 3, 0, &br, &bg, &bb) == MARKEE_OK);
    assert(br == 0 && bg == 0 && bb == 255);

    /* 4. Invalid monochrome color rejected with 400 */
    const char *bad = "{\"color\":\"notacolor\"}";
    r = do_request(port, "POST", "/monochrome", "application/json", bad, strlen(bad));
    assert(r.status == 400);
    free(r.body);

    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] BT.709 monochrome filter (perceptual luminance, PNG readback, non-destructive toggle)\n");
}

static void test_color_formats(void) {
    /* Shared parser: '#', '0x', bare hex, 3-digit, names; rejects junk */
    MarkeeColor c;
    assert(draw_parse_color_string("#ff5500", &c) && c.r == 0xff && c.g == 0x55 && c.b == 0x00);
    assert(draw_parse_color_string("ff5500", &c) && c.r == 0xff && c.g == 0x55 && c.b == 0x00);
    assert(draw_parse_color_string("0x11aabb", &c) && c.r == 0x11 && c.g == 0xaa && c.b == 0xbb);
    assert(draw_parse_color_string("0X11AABB", &c) && c.r == 0x11 && c.g == 0xaa && c.b == 0xbb);
    assert(draw_parse_color_string("0xf50", &c) && c.r == 0xff && c.g == 0x55 && c.b == 0x00);
    assert(draw_parse_color_string("f50", &c) && c.r == 0xff && c.g == 0x55 && c.b == 0x00);
    assert(draw_parse_color_string("amber", &c) && c.r == 255 && c.g == 191 && c.b == 0);
    assert(!draw_parse_color_string("0x", &c));
    assert(!draw_parse_color_string("0x12345", &c));
    assert(!draw_parse_color_string("#0x112233", &c));
    assert(!draw_parse_color_string("notacolor", &c));

    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));
    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);

    struct {
        const char *method;
        const char *path;
        const char *body;
        uint8_t r, g, b;
    } ok_cases[] = {
        {"POST", "/fill?color=0x11aabb", NULL, 0x11, 0xaa, 0xbb},
        {"POST", "/fill?color=%2322ccdd", NULL, 0x22, 0xcc, 0xdd},
        {"POST", "/fill?color=f50", NULL, 0xff, 0x55, 0x00},
        {"POST", "/fill?color=amber", NULL, 255, 191, 0},
        {"POST", "/fill", "{\"color\":\"teal\"}", 0, 128, 128},
        {"POST", "/fill", "{\"color\":\"0x010203\"}", 1, 2, 3},
        {"POST", "/pixel?x=3&y=4&color=0xabcdef", NULL, 0xab, 0xcd, 0xef},
        {"POST", "/pixel?x=3&y=4&color=red", NULL, 255, 0, 0},
    };
    for (size_t i = 0; i < sizeof(ok_cases) / sizeof(ok_cases[0]); i++) {
        const char *body = ok_cases[i].body;
        HttpResp r = do_request(port, ok_cases[i].method, ok_cases[i].path,
                                body ? "application/json" : NULL,
                                (const uint8_t *)body, body ? strlen(body) : 0);
        assert(r.status == 200);
        free(r.body);
        uint8_t pr = 0, pg = 0, pb = 0;
        assert(display_get_pixel(&disp, 3, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == ok_cases[i].r && pg == ok_cases[i].g && pb == ok_cases[i].b);
    }

    /* Invalid colors are rejected */
    HttpResp r = do_request(port, "POST", "/fill?color=0xzzzzzz", NULL, NULL, 0);
    assert(r.status == 400);
    free(r.body);

    /* /monochrome and /draw accept 0x too */
    r = do_request(port, "POST", "/monochrome?color=0x00ff00", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"hex\":\"#00ff00\"") != NULL);
    free(r.body);
    r = do_request(port, "DELETE", "/monochrome", NULL, NULL, 0);
    free(r.body);

    const char *draw = "[{\"op\":\"pixel\",\"x\":3,\"y\":4,\"color\":\"0x0a0b0c\"}]";
    r = do_request(port, "POST", "/draw", "application/json", (const uint8_t *)draw, strlen(draw));
    assert(r.status == 200);
    free(r.body);
    uint8_t pr = 0, pg = 0, pb = 0;
    assert(display_get_pixel(&disp, 3, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0x0a && pg == 0x0b && pb == 0x0c);

    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] unified color parsing (#hex, 0xhex, bare hex, 3-digit, names) across endpoints\n");
}


/* Send a raw request on a fresh connection and read until the server closes. */
static char *raw_exchange(uint16_t port, const char *req, size_t req_len) {
    int fd = connect_local(port);
    send_all(fd, req, req_len);
    size_t cap = 4096, got = 0;
    char *buf = malloc(cap);
    assert(buf != NULL);
    for (;;) {
        if (got + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            assert(buf != NULL);
        }
        ssize_t n = recv(fd, buf + got, cap - got - 1, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(fd);
    return buf;
}

static void test_audit_fixes(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));
    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);
    uint8_t pr, pg, pb;

    /* Draw error messages that echo user input are valid, escaped JSON */
    {
        const char *body = "[{\"op\":\"bad\\\"op\"}]";
        HttpResp r = do_request(port, "POST", "/draw", "application/json",
                                (const uint8_t *)body, strlen(body));
        assert(r.status == 400);
        char perr[128];
        JsonValue *j = json_parse((const char *)r.body, r.body_len, perr, sizeof(perr));
        assert(j != NULL && j->type == JSON_OBJECT);
        const char *msg = json_as_string(json_obj_get(j, "error"));
        assert(msg && strstr(msg, "bad\"op"));
        json_free(j);
        free(r.body);
    }

    /* Wrong method on a known path -> 405 with Allow; unknown path still 404 */
    {
        const char *req = "GET /fill HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        char *resp = raw_exchange(port, req, strlen(req));
        assert(strncmp(resp, "HTTP/1.1 405", 12) == 0);
        assert(strstr(resp, "\r\nAllow: POST, OPTIONS\r\n"));
        free(resp);
        HttpResp r = do_request(port, "GET", "/nope", NULL, NULL, 0);
        assert(r.status == 404);
        free(r.body);
    }

    /* Query values are URL-decoded (%XX and '+') */
    {
        HttpResp r = do_request(port, "POST", "/fill?color=%30x%31%32%33%34%35%36", NULL, NULL, 0);
        assert(r.status == 200);
        free(r.body);
        assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0x12 && pg == 0x34 && pb == 0x56);
    }

    /* Real JSON parsing: key names inside string values don't confuse lookup */
    {
        const char *body = "{\"note\":\"\\\"color\\\":\\\"red\\\"\",\"r\":1,\"g\":2,\"b\":3}";
        HttpResp r = do_request(port, "POST", "/fill", "application/json",
                                (const uint8_t *)body, strlen(body));
        assert(r.status == 200);
        free(r.body);
        assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 1 && pg == 2 && pb == 3);
    }

    /* Brightness body must be a whole number or JSON; trailing junk is rejected */
    {
        HttpResp r = do_request(port, "POST", "/brightness", "text/plain",
                                (const uint8_t *)"75abc", 5);
        assert(r.status == 400);
        free(r.body);
        r = do_request(port, "POST", "/brightness", "text/plain", (const uint8_t *)" 60\n", 4);
        assert(r.status == 200);
        free(r.body);
        r = do_request(port, "POST", "/brightness", "application/json",
                       (const uint8_t *)"{\"brightness\":70}", strlen("{\"brightness\":70}"));
        assert(r.status == 200);
        free(r.body);
    }

    /* Over-long request target -> 414 instead of silent truncation */
    {
        char req[900];
        int n = snprintf(req, sizeof(req), "GET /info?x=");
        memset(req + n, 'a', 600);
        n += 600;
        n += snprintf(req + n, sizeof(req) - (size_t)n, " HTTP/1.1\r\nHost: x\r\n\r\n");
        char *resp = raw_exchange(port, req, (size_t)n);
        assert(strncmp(resp, "HTTP/1.1 414", 12) == 0);
        free(resp);

        char big[1400];
        memcpy(big, "GET /", 5);
        memset(big + 5, 'a', 1200);
        n = 1205 + snprintf(big + 1205, sizeof(big) - 1205, " HTTP/1.1\r\nHost: x\r\n\r\n");
        resp = raw_exchange(port, big, (size_t)n);
        assert(strncmp(resp, "HTTP/1.1 414", 12) == 0);
        free(resp);
    }

    /* Shapes with no color keys default to a white outline; pixel defaults to white */
    {
        HttpResp r = do_request(port, "POST", "/clear", NULL, NULL, 0);
        free(r.body);
        const char *body = "[{\"op\":\"rect\",\"x\":0,\"y\":0,\"w\":4,\"h\":4},"
                           "{\"op\":\"pixel\",\"x\":10,\"y\":10},"
                           "{\"op\":\"pixel\",\"x\":11,\"y\":10,\"color\":\"none\"}]";
        r = do_request(port, "POST", "/draw", "application/json",
                       (const uint8_t *)body, strlen(body));
        assert(r.status == 200);
        free(r.body);
        assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        assert(display_get_pixel(&disp, 1, 1, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 0);
        assert(display_get_pixel(&disp, 10, 10, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        assert(display_get_pixel(&disp, 11, 10, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 0);
    }

    server_stop(srv);

    /* Binding by hostname and IPv6 literal */
    const char *hosts[] = {"localhost", "::1"};
    for (size_t i = 0; i < 2; i++) {
        MarkeeServer *s2 = server_start(&disp, hosts[i], 0);
        if (!s2 && i == 1) {
            printf("  (IPv6 loopback unavailable; skipped ::1 bind)\n");
            continue;
        }
        assert(s2 != NULL);
        assert(server_get_port(s2) != 0);
        server_stop(s2);
    }

    display_destroy(&disp);
    printf("[PASS] audit fixes (escaping, 405, URL decode, JSON, strict brightness, 414, defaults, bind)\n");
}

static void test_alert_audio_synthesis(void) {
    /* Identifier parsing: 1..3 and case-insensitive names; invalid values rejected */
    assert(audio_parse_alert_id("1") == 1);
    assert(audio_parse_alert_id("2") == 2);
    assert(audio_parse_alert_id("3") == 3);
    assert(audio_parse_alert_id("ding") == 1);
    assert(audio_parse_alert_id("CHIME") == 2);
    assert(audio_parse_alert_id(" Bell ") == 3);
    assert(audio_parse_alert_id("0") == 0);
    assert(audio_parse_alert_id("4") == 0);
    assert(audio_parse_alert_id("siren") == 0);
    assert(audio_parse_alert_id("") == 0);
    assert(audio_parse_alert_id(NULL) == 0);

    int16_t *pcms[MARKEE_ALERT_COUNT] = {0};
    size_t counts[MARKEE_ALERT_COUNT] = {0};

    for (int id = 1; id <= MARKEE_ALERT_COUNT; id++) {
        const MarkeeAlertInfo *info = audio_get_alert_info(id);
        assert(info != NULL && info->id == id && info->name != NULL && info->duration_ms > 0);

        unsigned int sr = 0;
        assert(audio_synthesize_pcm(id, &pcms[id - 1], &counts[id - 1], &sr));
        assert(sr == 44100U);
        assert(counts[id - 1] >= sr / 4); /* at least 250 ms */

        int16_t *s = pcms[id - 1];
        size_t n = counts[id - 1];

        /* Starts and ends cleanly near zero (no DC step / click) */
        assert(abs(s[0]) < 16);
        assert(abs(s[n - 1]) < 16);

        /* Compute peak, RMS, and DC mean */
        int peak = 0;
        double sum = 0.0;
        double sum_sq = 0.0;
        for (size_t i = 0; i < n; i++) {
            int a = abs(s[i]);
            if (a > peak) peak = a;
            double v = (double)s[i] / 32767.0;
            sum += v;
            sum_sq += v * v;
        }
        double rms = sqrt(sum_sq / (double)n);
        double dc = fabs(sum / (double)n);

        /* Comfortable non-clipping peak (~0.50 full scale) and audible energy */
        assert(peak >= 12000 && peak <= 20000);
        assert(rms > 0.03 && rms < 0.45);
        assert(dc < 0.01);
    }

    /* Verify all 3 sounds are distinct waveforms */
    assert(counts[0] != counts[1] && counts[1] != counts[2]);
    assert(memcmp(pcms[0], pcms[1], counts[0] < counts[1] ? counts[0] : counts[1]) != 0);
    assert(memcmp(pcms[1], pcms[2], counts[1] < counts[2] ? counts[1] : counts[2]) != 0);

    for (int i = 0; i < MARKEE_ALERT_COUNT; i++) free(pcms[i]);
    printf("[PASS] synthesized alert sounds (ding, chime, bell: clean edges, non-clipping, distinct)\n");
}

static void test_alert_http_api(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, true));
    /* Silent engine (open_device=false) so unit tests don't play audio out loud */
    MarkeeAudio *audio = audio_create(false);
    assert(audio != NULL);
    assert(!audio_is_ready(audio));

    MarkeeServer *srv = server_start_ex(&disp, audio, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);

    /* 1. GET /alert lists all 3 sounds and audio state */
    HttpResp r = do_request(port, "GET", "/alert", NULL, NULL, 0);
    assert(r.status == 200);
    JsonValue *j = json_parse((const char *)r.body, r.body_len, NULL, 0);
    assert(j != NULL && j->type == JSON_OBJECT);
    const JsonValue *sounds = json_obj_get(j, "sounds");
    assert(sounds != NULL && sounds->type == JSON_ARRAY && sounds->u.arr.count == 3);
    assert(strstr((char *)r.body, "\"name\":\"ding\"") != NULL);
    assert(strstr((char *)r.body, "\"name\":\"chime\"") != NULL);
    assert(strstr((char *)r.body, "\"name\":\"bell\"") != NULL);
    json_free(j);
    free(r.body);

    /* GET /alerts alias works too */
    r = do_request(port, "GET", "/alerts", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    /* GET /info includes audio_ready and alert_sounds */
    r = do_request(port, "GET", "/info", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"audio_ready\":false") != NULL);
    assert(strstr((char *)r.body, "\"alert_sounds\":3") != NULL);
    free(r.body);

    /* 2. POST /alert with empty body defaults to sound 1 (ding) at volume 100 */
    r = do_request(port, "POST", "/alert", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"id\":1") != NULL);
    assert(strstr((char *)r.body, "\"name\":\"ding\"") != NULL);
    assert(strstr((char *)r.body, "\"volume\":100") != NULL);
    assert(strstr((char *)r.body, "\"played\":false") != NULL);
    assert(audio_get_trigger_count(audio) == 1);
    assert(audio_get_last_alert_id(audio) == 1);
    assert(audio_get_last_volume(audio) == 100);
    free(r.body);

    /* 3. POST /alert/<id|name> with path segments and optional volume */
    r = do_request(port, "POST", "/alert/2?volume=75", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"id\":2") != NULL);
    assert(strstr((char *)r.body, "\"name\":\"chime\"") != NULL);
    assert(strstr((char *)r.body, "\"volume\":75") != NULL);
    assert(audio_get_last_alert_id(audio) == 2);
    assert(audio_get_last_volume(audio) == 75);
    free(r.body);

    const char *vol_body = "{\"volume\":40}";
    r = do_request(port, "POST", "/alert/bell", "application/json", vol_body, strlen(vol_body));
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"id\":3") != NULL);
    assert(strstr((char *)r.body, "\"name\":\"bell\"") != NULL);
    assert(strstr((char *)r.body, "\"volume\":40") != NULL);
    assert(audio_get_last_alert_id(audio) == 3);
    assert(audio_get_last_volume(audio) == 40);
    free(r.body);

    /* 4. POST /alert via query, JSON body, and plain text body */
    r = do_request(port, "POST", "/alert?sound=chime&volume=60", NULL, NULL, 0);
    assert(r.status == 200);
    assert(audio_get_last_alert_id(audio) == 2 && audio_get_last_volume(audio) == 60);
    free(r.body);

    const char *json_req = "{\"sound\":\"bell\",\"volume\":85}";
    r = do_request(port, "POST", "/alert", "application/json", json_req, strlen(json_req));
    assert(r.status == 200);
    assert(audio_get_last_alert_id(audio) == 3 && audio_get_last_volume(audio) == 85);
    free(r.body);

    const char *json_num = "{\"id\":1}";
    r = do_request(port, "POST", "/alert", "application/json", json_num, strlen(json_num));
    assert(r.status == 200);
    assert(audio_get_last_alert_id(audio) == 1 && audio_get_last_volume(audio) == 100);
    free(r.body);

    const char *plain = "chime\n";
    r = do_request(port, "POST", "/alert", "text/plain", plain, strlen(plain));
    assert(r.status == 200);
    assert(audio_get_last_alert_id(audio) == 2);
    free(r.body);

    /* Alerts must NOT stop the demo or increment frames_received */
    assert(display_is_demo_active(&disp));
    assert(display_get_frames_received(&disp) == 0);
    assert(audio_get_trigger_count(audio) == 7);

    /* 5. Error handling: 404 unknown path sound, 400 invalid query/body/volume, 405 wrong method */
    r = do_request(port, "POST", "/alert/99", NULL, NULL, 0);
    assert(r.status == 404);
    free(r.body);

    r = do_request(port, "POST", "/alert/siren", NULL, NULL, 0);
    assert(r.status == 404);
    free(r.body);

    r = do_request(port, "POST", "/alert?sound=99", NULL, NULL, 0);
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/alert?volume=150", NULL, NULL, 0);
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/alert/1?volume=-1", NULL, NULL, 0);
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/alert", "text/plain", "nope", 4);
    assert(r.status == 400);
    free(r.body);

    const char *get_alert1 = "GET /alert/1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    char *raw = raw_exchange(port, get_alert1, strlen(get_alert1));
    assert(strncmp(raw, "HTTP/1.1 405", 12) == 0);
    assert(strstr(raw, "\r\nAllow: POST, OPTIONS\r\n") != NULL);
    free(raw);

    const char *del_alert = "DELETE /alert HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    raw = raw_exchange(port, del_alert, strlen(del_alert));
    assert(strncmp(raw, "HTTP/1.1 405", 12) == 0);
    assert(strstr(raw, "\r\nAllow: GET, POST, OPTIONS\r\n") != NULL);
    free(raw);

    /* Failed requests did not increment trigger_count */
    assert(audio_get_trigger_count(audio) == 7);

    server_stop(srv);
    audio_destroy(audio);
    display_destroy(&disp);
    printf("[PASS] /alert HTTP API (GET list, POST /alert/<id|name>, query/JSON/text, volume, 400/404/405)\n");
}

/* --- Minimal GIF89a test builder (emits valid LZW streams with Clear codes) --- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} ByteBuf;

static void bb_push(ByteBuf *b, uint8_t v) {
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        b->buf = realloc(b->buf, ncap);
        assert(b->buf != NULL);
        b->cap = ncap;
    }
    b->buf[b->len++] = v;
}

static void bb_push_bytes(ByteBuf *b, const void *src, size_t n) {
    const uint8_t *p = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) bb_push(b, p[i]);
}

static void bb_push_u16(ByteBuf *b, uint16_t v) {
    bb_push(b, (uint8_t)(v & 0xFF));
    bb_push(b, (uint8_t)((v >> 8) & 0xFF));
}

typedef struct {
    ByteBuf *out;
    uint8_t sub[255];
    size_t sub_len;
    uint32_t bit_buf;
    int bit_count;
} GifBitWriter;

static void bw_flush_sub(GifBitWriter *w) {
    if (w->sub_len > 0) {
        bb_push(w->out, (uint8_t)w->sub_len);
        bb_push_bytes(w->out, w->sub, w->sub_len);
        w->sub_len = 0;
    }
}

static void bw_write_code(GifBitWriter *w, int code, int bits) {
    w->bit_buf |= ((uint32_t)code << w->bit_count);
    w->bit_count += bits;
    while (w->bit_count >= 8) {
        w->sub[w->sub_len++] = (uint8_t)(w->bit_buf & 0xFF);
        w->bit_buf >>= 8;
        w->bit_count -= 8;
        if (w->sub_len == 255) bw_flush_sub(w);
    }
}

static void bw_finish(GifBitWriter *w) {
    if (w->bit_count > 0) {
        w->sub[w->sub_len++] = (uint8_t)(w->bit_buf & 0xFF);
        w->bit_buf = 0;
        w->bit_count = 0;
    }
    bw_flush_sub(w);
    bb_push(w->out, 0x00); /* sub-block terminator */
}

static void gif_begin(ByteBuf *b, uint16_t w, uint16_t h,
                      const uint8_t *gct, int gct_exp /* 0..7 => 2^(exp+1) colors; <0 = no GCT */) {
    bb_push_bytes(b, "GIF89a", 6);
    bb_push_u16(b, w);
    bb_push_u16(b, h);
    uint8_t packed = (gct_exp >= 0) ? (uint8_t)(0x80U | (gct_exp & 0x07)) : 0x00;
    bb_push(b, packed);
    bb_push(b, 0x00); /* bg index */
    bb_push(b, 0x00); /* aspect */
    if (gct_exp >= 0 && gct) {
        size_t n = (size_t)1U << (gct_exp + 1);
        bb_push_bytes(b, gct, n * 3);
    }
}

static void gif_add_netscape(ByteBuf *b, uint16_t iters) {
    bb_push(b, 0x21);
    bb_push(b, 0xFF);
    bb_push(b, 11);
    bb_push_bytes(b, "NETSCAPE2.0", 11);
    bb_push(b, 3);
    bb_push(b, 0x01);
    bb_push_u16(b, iters);
    bb_push(b, 0x00);
}

static void gif_add_frame(ByteBuf *b, uint16_t fx, uint16_t fy, uint16_t fw, uint16_t fh,
                          uint16_t delay_cs, uint8_t disposal,
                          bool has_trans, uint8_t trans_idx,
                          bool interlaced,
                          const uint8_t *lct, int lct_exp,
                          const uint8_t *indices) {
    /* Graphic Control Extension */
    bb_push(b, 0x21);
    bb_push(b, 0xF9);
    bb_push(b, 4);
    uint8_t gce_packed = (uint8_t)(((disposal & 0x07U) << 2) | (has_trans ? 1U : 0U));
    bb_push(b, gce_packed);
    bb_push_u16(b, delay_cs);
    bb_push(b, trans_idx);
    bb_push(b, 0x00);

    /* Image Descriptor */
    bb_push(b, 0x2C);
    bb_push_u16(b, fx);
    bb_push_u16(b, fy);
    bb_push_u16(b, fw);
    bb_push_u16(b, fh);
    uint8_t img_packed = (interlaced ? 0x40U : 0x00U) |
                         ((lct_exp >= 0) ? (uint8_t)(0x80U | (lct_exp & 0x07)) : 0x00U);
    bb_push(b, img_packed);
    if (lct_exp >= 0 && lct) {
        size_t n = (size_t)1U << (lct_exp + 1);
        bb_push_bytes(b, lct, n * 3);
    }

    /* Uncompressed-style valid LZW stream (min_code_size = 8, clear every 200 pixels so code
     * size stays 9 bits while exercising real LZW table additions + clear resets). */
    const uint8_t min_cs = 8;
    const int clear_code = 256;
    const int eoi_code = 257;
    bb_push(b, min_cs);

    GifBitWriter bw;
    memset(&bw, 0, sizeof(bw));
    bw.out = b;
    bw_write_code(&bw, clear_code, 9);
    int emitted_since_clear = 0;
    size_t count = (size_t)fw * (size_t)fh;
    for (size_t i = 0; i < count; i++) {
        bw_write_code(&bw, indices[i], 9);
        emitted_since_clear++;
        if (emitted_since_clear >= 200) {
            bw_write_code(&bw, clear_code, 9);
            emitted_since_clear = 0;
        }
    }
    bw_write_code(&bw, eoi_code, 9);
    bw_finish(&bw);
}

static void gif_end(ByteBuf *b) {
    bb_push(b, 0x3B);
}

static void test_gif_decoder_crop_center_and_features(void) {
    /* 1. Too small (10x6) on 72x16 -> centered at x=31..40, y=5..10, black margins */
    {
        uint8_t gct[4 * 3] = {
            0, 0, 0,       /* 0: black */
            255, 0, 0,     /* 1: red */
            0, 255, 0,     /* 2: green */
            0, 0, 255      /* 3: blue */
        };
        uint8_t px[10 * 6];
        memset(px, 1, sizeof(px));
        px[0] = 2;              /* top-left of GIF = green */
        px[5 * 10 + 9] = 3;     /* bottom-right of GIF = blue */

        ByteBuf bb = {0};
        gif_begin(&bb, 10, 6, gct, 1);
        gif_add_frame(&bb, 0, 0, 10, 6, 5, 0, false, 0, false, NULL, -1, px);
        gif_end(&bb);

        MarkeeGif *gif = NULL;
        char err[128] = {0};
        assert(gif_decode(bb.buf, bb.len, 72, 16, &gif, err, sizeof(err)));
        assert(gif != NULL);
        assert(gif->src_width == 10 && gif->src_height == 6);
        assert(gif->width == 72 && gif->height == 16);
        assert(gif->frame_count == 1);
        assert(gif->delays_ms[0] == 50);

        /* Outside centered box -> black */
        const uint8_t *f0 = gif->frames;
        assert(f0[(0 * 72 + 0) * 3 + 0] == 0 && f0[(0 * 72 + 0) * 3 + 1] == 0);
        assert(f0[(5 * 72 + 30) * 3 + 0] == 0 && f0[(5 * 72 + 30) * 3 + 1] == 0);
        assert(f0[(4 * 72 + 31) * 3 + 0] == 0 && f0[(4 * 72 + 31) * 3 + 1] == 0);

        /* Top-left of GIF at (31, 5) -> green */
        assert(f0[(5 * 72 + 31) * 3 + 0] == 0 &&
               f0[(5 * 72 + 31) * 3 + 1] == 255 &&
               f0[(5 * 72 + 31) * 3 + 2] == 0);
        /* Interior at (35, 7) -> red */
        assert(f0[(7 * 72 + 35) * 3 + 0] == 255 &&
               f0[(7 * 72 + 35) * 3 + 1] == 0 &&
               f0[(7 * 72 + 35) * 3 + 2] == 0);
        /* Bottom-right of GIF at (40, 10) -> blue */
        assert(f0[(10 * 72 + 40) * 3 + 0] == 0 &&
               f0[(10 * 72 + 40) * 3 + 1] == 0 &&
               f0[(10 * 72 + 40) * 3 + 2] == 255);
        /* Just past bottom-right at (41, 10) -> black */
        assert(f0[(10 * 72 + 41) * 3 + 2] == 0);

        gif_free(gif);
        free(bb.buf);
    }

    /* 2. Too big (100x24) on 72x16 -> center-cropped (src_x0=14, src_y0=4) */
    {
        uint8_t gct[256 * 3];
        memset(gct, 0, sizeof(gct));
        for (int i = 0; i < 256; i++) {
            gct[i * 3 + 0] = (uint8_t)i;
            gct[i * 3 + 1] = (uint8_t)(255 - i);
            gct[i * 3 + 2] = (uint8_t)((i * 3) & 0xFF);
        }
        uint8_t px[100 * 24];
        for (int y = 0; y < 24; y++) {
            for (int x = 0; x < 100; x++) {
                px[y * 100 + x] = (uint8_t)((x + y * 5) & 0xFF);
            }
        }
        ByteBuf bb = {0};
        gif_begin(&bb, 100, 24, gct, 7);
        gif_add_frame(&bb, 0, 0, 100, 24, 4, 0, false, 0, false, NULL, -1, px);
        gif_end(&bb);

        MarkeeGif *gif = NULL;
        assert(gif_decode(bb.buf, bb.len, 72, 16, &gif, NULL, 0));
        assert(gif->src_width == 100 && gif->src_height == 24);
        /* Display (0,0) == source (14,4) */
        uint8_t idx00 = px[4 * 100 + 14];
        assert(gif->frames[0] == gct[idx00 * 3 + 0] &&
               gif->frames[1] == gct[idx00 * 3 + 1] &&
               gif->frames[2] == gct[idx00 * 3 + 2]);
        /* Display (71,15) == source (85,19) */
        uint8_t idx11 = px[19 * 100 + 85];
        size_t off11 = (15 * 72 + 71) * 3;
        assert(gif->frames[off11 + 0] == gct[idx11 * 3 + 0] &&
               gif->frames[off11 + 1] == gct[idx11 * 3 + 1] &&
               gif->frames[off11 + 2] == gct[idx11 * 3 + 2]);

        gif_free(gif);
        free(bb.buf);
    }

    /* 3. Mixed axis (100x8 on 72x16): center-cropped X (src_x0=14), centered Y (dst_y0=4) */
    {
        uint8_t gct[4 * 3] = {0, 0, 0,  255, 191, 0,  0, 200, 255,  255, 255, 255};
        uint8_t px[100 * 8];
        memset(px, 1, sizeof(px));
        px[0 * 100 + 14] = 2; /* visible top-left after X crop */

        ByteBuf bb = {0};
        gif_begin(&bb, 100, 8, gct, 1);
        gif_add_frame(&bb, 0, 0, 100, 8, 10, 0, false, 0, false, NULL, -1, px);
        gif_end(&bb);

        MarkeeGif *gif = NULL;
        assert(gif_decode(bb.buf, bb.len, 72, 16, &gif, NULL, 0));
        /* Rows 0..3 and 12..15 are black */
        assert(gif->frames[(3 * 72 + 0) * 3 + 0] == 0);
        assert(gif->frames[(12 * 72 + 0) * 3 + 0] == 0);
        /* (0, 4) matches source (14, 0) -> cyan (0, 200, 255) */
        assert(gif->frames[(4 * 72 + 0) * 3 + 0] == 0 &&
               gif->frames[(4 * 72 + 0) * 3 + 1] == 200 &&
               gif->frames[(4 * 72 + 0) * 3 + 2] == 255);
        /* (1, 4) -> amber (255, 191, 0) */
        assert(gif->frames[(4 * 72 + 1) * 3 + 0] == 255 &&
               gif->frames[(4 * 72 + 1) * 3 + 1] == 191 &&
               gif->frames[(4 * 72 + 1) * 3 + 2] == 0);

        gif_free(gif);
        free(bb.buf);
    }

    /* 4. Per-frame Local Color Table, transparency, disposal 1/2/3, interlacing, delay clamping */
    {
        uint8_t gct[4 * 3] = {
            0, 0, 0,        /* 0: black */
            200, 0, 0,      /* 1: red */
            0, 200, 0,      /* 2: green */
            0, 0, 200       /* 3: blue */
        };
        uint8_t lct[4 * 3] = {
            0, 0, 0,        /* 0: transparent */
            255, 255, 0,    /* 1: yellow (different from GCT!) */
            255, 0, 255,    /* 2: magenta */
            0, 255, 255     /* 3: cyan */
        };

        /* 8x8 GIF on 8x8 display, 4 frames:
         * Frame 0: full 8x8 red (interlaced!), disposal=1 (leave), delay=4 cs (40 ms)
         * Frame 1: 4x4 sub-rect at (2,2) with LCT yellow & index 0 transparent, disposal=3 (restore prev), delay=0 cs -> clamped 100 ms
         * Frame 2: 2x2 sub-rect at (0,0) green (GCT), disposal=2 (restore bg), delay=6 cs (60 ms)
         * Frame 3: 1x1 sub-rect at (7,7) blue (GCT), disposal=0, delay=1 cs -> clamped 100 ms */
        uint8_t f0_px[64];
        memset(f0_px, 1, sizeof(f0_px));
        /* Interlaced pass 0 starts at row 0, pass 3 has row 1 -> put index 3 at pass-0 row 0, col 0 */
        f0_px[0] = 3; /* row 0, col 0 => blue */

        uint8_t f1_px[16];
        memset(f1_px, 1, sizeof(f1_px)); /* yellow in LCT */
        f1_px[0] = 0;                    /* transparent at (2,2) => red shows through! */

        uint8_t f2_px[4] = {2, 2, 2, 2}; /* green at (0..1, 0..1) */
        uint8_t f3_px[1] = {3};          /* blue at (7,7) */

        ByteBuf bb = {0};
        gif_begin(&bb, 8, 8, gct, 1);
        gif_add_netscape(&bb, 0); /* infinite loop */
        gif_add_frame(&bb, 0, 0, 8, 8, 4, 1, false, 0, true, NULL, -1, f0_px);
        gif_add_frame(&bb, 2, 2, 4, 4, 0, 3, true, 0, false, lct, 1, f1_px);
        gif_add_frame(&bb, 0, 0, 2, 2, 6, 2, false, 0, false, NULL, -1, f2_px);
        gif_add_frame(&bb, 7, 7, 1, 1, 1, 0, false, 0, false, NULL, -1, f3_px);
        gif_end(&bb);

        MarkeeGif *gif = NULL;
        assert(gif_decode(bb.buf, bb.len, 8, 8, &gif, NULL, 0));
        assert(gif->frame_count == 4);
        assert(gif->loop_count == 0);
        assert(gif->delays_ms[0] == 40);
        assert(gif->delays_ms[1] == 100); /* 0 cs clamped to 100 ms */
        assert(gif->delays_ms[2] == 60);
        assert(gif->delays_ms[3] == 100); /* 1 cs clamped to 100 ms */
        assert(gif->total_duration_ms == 300);

        size_t fb = gif->frame_bytes;
        const uint8_t *fr0 = gif->frames + 0 * fb;
        const uint8_t *fr1 = gif->frames + 1 * fb;
        const uint8_t *fr2 = gif->frames + 2 * fb;
        const uint8_t *fr3 = gif->frames + 3 * fb;

        /* Frame 0: (0,0) blue, (3,3) red */
        assert(fr0[0] == 0 && fr0[1] == 0 && fr0[2] == 200);
        assert(fr0[(3 * 8 + 3) * 3 + 0] == 200 && fr0[(3 * 8 + 3) * 3 + 1] == 0);

        /* Frame 1: (2,2) transparent -> stays red; (3,3) LCT yellow (255,255,0) */
        assert(fr1[(2 * 8 + 2) * 3 + 0] == 200 && fr1[(2 * 8 + 2) * 3 + 1] == 0);
        assert(fr1[(3 * 8 + 3) * 3 + 0] == 255 &&
               fr1[(3 * 8 + 3) * 3 + 1] == 255 &&
               fr1[(3 * 8 + 3) * 3 + 2] == 0);

        /* Frame 2: disposal=3 from Frame 1 restored (3,3) back to red! And (0,0) is green */
        assert(fr2[(3 * 8 + 3) * 3 + 0] == 200 && fr2[(3 * 8 + 3) * 3 + 1] == 0);
        assert(fr2[0] == 0 && fr2[1] == 200 && fr2[2] == 0);

        /* Frame 3: disposal=2 from Frame 2 cleared (0,0) to black! And (7,7) is blue */
        assert(fr3[0] == 0 && fr3[1] == 0 && fr3[2] == 0);
        assert(fr3[(7 * 8 + 7) * 3 + 0] == 0 &&
               fr3[(7 * 8 + 7) * 3 + 1] == 0 &&
               fr3[(7 * 8 + 7) * 3 + 2] == 200);

        gif_free(gif);
        free(bb.buf);
    }

    printf("[PASS] GIF89a decoder (crop, center, mixed axis, LCT, transparency, disposal 1/2/3, interlacing)\n");
}

static void test_animate_http_api(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, true));
    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);
    uint8_t pr, pg, pb;

    /* Build a 3-frame 12x8 animated GIF:
     * Frame 0: red (50 ms), Frame 1: green (60 ms), Frame 2: blue (70 ms), infinite loop.
     * Centered on 72x16 => x=30..41, y=4..11. */
    uint8_t gct[4 * 3] = {0, 0, 0,  255, 0, 0,  0, 255, 0,  0, 0, 255};
    uint8_t p_red[12 * 8], p_grn[12 * 8], p_blu[12 * 8];
    memset(p_red, 1, sizeof(p_red));
    memset(p_grn, 2, sizeof(p_grn));
    memset(p_blu, 3, sizeof(p_blu));

    ByteBuf anim = {0};
    gif_begin(&anim, 12, 8, gct, 1);
    gif_add_netscape(&anim, 0);
    gif_add_frame(&anim, 0, 0, 12, 8, 5, 0, false, 0, false, NULL, -1, p_red);
    gif_add_frame(&anim, 0, 0, 12, 8, 6, 0, false, 0, false, NULL, -1, p_grn);
    gif_add_frame(&anim, 0, 0, 12, 8, 7, 0, false, 0, false, NULL, -1, p_blu);
    gif_end(&anim);

    /* 1. GET /animate before upload -> active:false */
    HttpResp r = do_request(port, "GET", "/animate", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"active\":false") != NULL);
    free(r.body);

    /* 2. POST /animate -> uploads, stops demo, immediately displays centered frame 0 (red) */
    r = do_request(port, "POST", "/animate", "image/gif", anim.buf, anim.len);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"format\":\"gif\"") != NULL);
    assert(strstr((char *)r.body, "\"source_width\":12") != NULL);
    assert(strstr((char *)r.body, "\"source_height\":8") != NULL);
    assert(strstr((char *)r.body, "\"frames\":3") != NULL);
    assert(strstr((char *)r.body, "\"duration_ms\":180") != NULL);
    assert(strstr((char *)r.body, "\"loop\":0") != NULL);
    assert(strstr((char *)r.body, "\"active\":true") != NULL);
    free(r.body);

    assert(!display_is_demo_active(&disp));
    assert(display_is_gif_active(&disp));

    /* Check GET /info reports animation_active:true */
    r = do_request(port, "GET", "/info", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"animation_active\":true") != NULL);
    free(r.body);

    /* Centered box (30,4) is red; margin (0,0) is black */
    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);

    /* 3. Advance time via display_tick: 0.0 -> anchor, +55 ms -> frame 1 (green), +60 ms -> frame 2 (blue), +70 ms -> wraps to frame 0 (red) */
    display_tick(&disp, 1.000);
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);

    display_tick(&disp, 1.055); /* +55 ms >= 50 ms => frame 1 (green) */
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 255 && pb == 0);

    r = do_request(port, "GET", "/animate", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"active\":true") != NULL);
    assert(strstr((char *)r.body, "\"current_frame\":1") != NULL);
    free(r.body);

    display_tick(&disp, 1.115); /* +60 ms (accum 65 >= 60) => frame 2 (blue) */
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 255);

    display_tick(&disp, 1.185); /* +70 ms (accum 75 >= 70) => wraps to frame 0 (red) */
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);

    /* 4. DELETE /animate stops playback while keeping current frame on screen */
    r = do_request(port, "DELETE", "/animate", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"active\":false") != NULL);
    free(r.body);
    assert(!display_is_gif_active(&disp));
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 0 && pb == 0);

    /* 5. Finite loop override (?loop=1) plays once and stops on the last frame (blue) */
    r = do_request(port, "POST", "/animate?loop=1", "image/gif", anim.buf, anim.len);
    assert(r.status == 200);
    assert(strstr((char *)r.body, "\"loop\":1") != NULL);
    free(r.body);

    display_tick(&disp, 2.000);
    display_tick(&disp, 2.500); /* > 180 ms total -> finishes loop 1, stops on frame 2 (blue) */
    assert(!display_is_gif_active(&disp));
    assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 255);

    /* 6. fill=<color> composites transparent GIF pixels and centered margins over the fill color */
    {
        uint8_t px_t[12 * 8];
        memset(px_t, 0, sizeof(px_t)); /* index 0 = transparent */
        px_t[0] = 1;                   /* (0,0) of GIF = opaque red -> centered at (30,4) */

        ByteBuf tgif = {0};
        gif_begin(&tgif, 12, 8, gct, 1);
        gif_add_netscape(&tgif, 0);
        gif_add_frame(&tgif, 0, 0, 12, 8, 5, 2, true, 0, false, NULL, -1, px_t);
        gif_add_frame(&tgif, 0, 0, 12, 8, 5, 2, true, 0, false, NULL, -1, px_t);
        gif_end(&tgif);

        r = do_request(port, "POST", "/animate?fill=white", "image/gif", tgif.buf, tgif.len);
        assert(r.status == 200);
        assert(strstr((char *)r.body, "\"fill\":\"#ffffff\"") != NULL);
        free(r.body);

        r = do_request(port, "GET", "/animate", NULL, NULL, 0);
        assert(r.status == 200);
        assert(strstr((char *)r.body, "\"fill\":\"#ffffff\"") != NULL);
        free(r.body);

        /* Centered margin (0,0) and transparent GIF pixel (31,4) -> white; opaque (30,4) -> red */
        assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        assert(display_get_pixel(&disp, 31, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        assert(display_get_pixel(&disp, 30, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 0 && pb == 0);

        /* Hex fill color (0x102030) works too */
        r = do_request(port, "POST", "/animate?fill=0x102030&loop=1", "image/gif", tgif.buf, tgif.len);
        assert(r.status == 200);
        assert(strstr((char *)r.body, "\"fill\":\"#102030\"") != NULL);
        free(r.body);
        assert(display_get_pixel(&disp, 31, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0x10 && pg == 0x20 && pb == 0x30);

        /* Invalid fill color -> 400 */
        r = do_request(port, "POST", "/animate?fill=notacolor", "image/gif", tgif.buf, tgif.len);
        assert(r.status == 400);
        free(r.body);

        free(tgif.buf);
    }

    /* 7. Other framebuffer writes (/draw, /clear) automatically stop an active animation */
    r = do_request(port, "POST", "/animate", "image/gif", anim.buf, anim.len);
    assert(r.status == 200);
    free(r.body);
    assert(display_is_gif_active(&disp));

    const char *draw_px = "[{\"op\":\"pixel\",\"x\":0,\"y\":0,\"color\":\"white\"}]";
    r = do_request(port, "POST", "/draw", "application/json", draw_px, strlen(draw_px));
    assert(r.status == 200);
    free(r.body);
    assert(!display_is_gif_active(&disp));

    /* 7. Error handling: empty body, non-GIF, truncated GIF, bad loop param, 405 */
    r = do_request(port, "POST", "/animate", "image/gif", NULL, 0);
    assert(r.status == 400);
    free(r.body);

    const char *not_gif = "not a gif file";
    r = do_request(port, "POST", "/animate", "image/gif", not_gif, strlen(not_gif));
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/animate", "image/gif", anim.buf, 20); /* truncated */
    assert(r.status == 400);
    free(r.body);

    r = do_request(port, "POST", "/animate?loop=-1", "image/gif", anim.buf, anim.len);
    assert(r.status == 400);
    free(r.body);

    const char *patch_req = "PATCH /animate HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    char *raw = raw_exchange(port, patch_req, strlen(patch_req));
    assert(strncmp(raw, "HTTP/1.1 405", 12) == 0);
    assert(strstr(raw, "\r\nAllow: GET, POST, PUT, DELETE, OPTIONS\r\n") != NULL);
    free(raw);

    free(anim.buf);
    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] /animate HTTP API (POST/GET/DELETE, timing, looping, stop-on-write, 400/405)\n");
}

static char *encode_base64(const uint8_t *in, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    assert(out != NULL);
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16);
        if (i + 1 < len) v |= ((uint32_t)in[i + 1] << 8);
        if (i + 2 < len) v |= (uint32_t)in[i + 2];
        out[o++] = T[(v >> 18) & 0x3F];
        out[o++] = T[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

static void test_draw_image_op(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, 72, 16, false));
    MarkeeServer *srv = server_start(&disp, "127.0.0.1", 0);
    assert(srv != NULL);
    uint16_t port = server_get_port(srv);
    uint8_t pr, pg, pb;

    /* 6x6 transparent 2-frame GIF sprite:
     * index 0 = transparent; (0,0) and (5,5) are opaque:
     * Frame 0: red (50 ms), Frame 1: green (60 ms), disposal=2 */
    uint8_t gct[4 * 3] = {0, 0, 0,  255, 0, 0,  0, 255, 0,  0, 0, 255};
    uint8_t f0[6 * 6], f1[6 * 6];
    memset(f0, 0, sizeof(f0));
    memset(f1, 0, sizeof(f1));
    f0[0] = 1; f0[5 * 6 + 5] = 1; /* red */
    f1[0] = 2; f1[5 * 6 + 5] = 2; /* green */

    ByteBuf sprite = {0};
    gif_begin(&sprite, 6, 6, gct, 1);
    gif_add_netscape(&sprite, 0);
    gif_add_frame(&sprite, 0, 0, 6, 6, 5, 2, true, 0, false, NULL, -1, f0);
    gif_add_frame(&sprite, 0, 0, 6, 6, 6, 2, true, 0, false, NULL, -1, f1);
    gif_end(&sprite);

    char *b64 = encode_base64(sprite.buf, sprite.len);

    /* 1. Placed animated GIF without fill over #002040 background + sibling static pixel */
    {
        char *body = malloc(strlen(b64) + 512);
        assert(body != NULL);
        sprintf(body,
                "["
                "{\"op\":\"clear\",\"color\":\"#002040\"},"
                "{\"op\":\"image\",\"x\":4,\"y\":2,\"data\":\"data:image/gif;base64,%s\",\"loop\":0},"
                "{\"op\":\"pixel\",\"x\":30,\"y\":8,\"color\":\"amber\"}"
                "]",
                b64);
        HttpResp r = do_request(port, "POST", "/draw", "application/json", body, strlen(body));
        free(body);
        assert(r.status == 200);
        assert(strstr((char *)r.body, "\"ops\":3") != NULL);
        free(r.body);

        assert(display_is_gif_active(&disp));

        /* Frame 0 immediately visible: opaque (4,2) = red, transparent (5,2) = #002040, sibling (30,8) = amber */
        assert(display_get_pixel(&disp, 4, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 0 && pb == 0);
        assert(display_get_pixel(&disp, 5, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0x00 && pg == 0x20 && pb == 0x40);
        assert(display_get_pixel(&disp, 30, 8, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 191 && pb == 0);

        /* Tick forward 55 ms -> frame 1 (green), transparent pixel & sibling pixel preserved */
        display_tick(&disp, 1.000);
        display_tick(&disp, 1.055);
        assert(display_get_pixel(&disp, 4, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 255 && pb == 0);
        assert(display_get_pixel(&disp, 5, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0x00 && pg == 0x20 && pb == 0x40);
        assert(display_get_pixel(&disp, 30, 8, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 191 && pb == 0);
    }

    /* 2. Placed GIF with fill="white" and loop=1 + negative x clipping */
    {
        char *body = malloc(strlen(b64) + 512);
        assert(body != NULL);
        sprintf(body,
                "["
                "{\"op\":\"clear\",\"color\":\"#001020\"},"
                "{\"op\":\"image\",\"x\":-2,\"y\":3,\"data\":\"%s\",\"loop\":1,\"fill\":\"white\"}"
                "]",
                b64);
        HttpResp r = do_request(port, "POST", "/draw", "application/json", body, strlen(body));
        free(body);
        assert(r.status == 200);
        free(r.body);

        /* x=-2..3, y=3..8 -> visible x=0..3, y=3..8:
         * source (2,0) -> (0,3) is transparent => white fill!
         * source (5,5) -> (3,8) is opaque => red (frame 0)!
         * outside rect (4,3) => #001020! */
        assert(display_get_pixel(&disp, 0, 3, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        assert(display_get_pixel(&disp, 3, 8, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 0 && pb == 0);
        assert(display_get_pixel(&disp, 4, 3, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0x00 && pg == 0x10 && pb == 0x20);

        /* Finish single loop (> 110 ms) -> stops on frame 1 (green at (3,8), white at (0,3)) */
        display_tick(&disp, 2.000);
        display_tick(&disp, 2.300);
        assert(!display_is_gif_active(&disp));
        assert(display_get_pixel(&disp, 3, 8, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 255 && pb == 0);
        assert(display_get_pixel(&disp, 0, 3, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
    }

    /* 3. Two simultaneous animated GIFs with different loop counts + scrolling text */
    {
        char *body = malloc(strlen(b64) * 2 + 512);
        assert(body != NULL);
        sprintf(body,
                "["
                "{\"op\":\"clear\"},"
                "{\"op\":\"image\",\"x\":2,\"y\":5,\"data\":\"%s\",\"loop\":1,\"fill\":[10,20,30]},"
                "{\"op\":\"image\",\"x\":60,\"y\":5,\"data\":\"%s\",\"loop\":0},"
                "{\"op\":\"text\",\"x\":20,\"y\":0,\"text\":\"HI\",\"color\":\"white\",\"scroll\":true,\"speed\":20}"
                "]",
                b64, b64);
        HttpResp r = do_request(port, "POST", "/draw", "application/json", body, strlen(body));
        free(body);
        assert(r.status == 200);
        free(r.body);

        display_tick(&disp, 3.000);
        /* Advance 130 ms: left GIF finishes loop 1 (holds green), right GIF wraps to frame 0 (red) */
        display_tick(&disp, 3.130);
        assert(display_is_gif_active(&disp));
        assert(display_get_pixel(&disp, 2, 5, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 255 && pb == 0); /* left GIF holds final frame (green) */
        assert(display_get_pixel(&disp, 3, 5, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 10 && pg == 20 && pb == 30); /* left GIF [10,20,30] fill */
        assert(display_get_pixel(&disp, 60, 5, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 0 && pb == 0); /* right GIF wrapped back to frame 0 (red) */
    }

    /* 4. PNG support: RGBA alpha blending over canvas & over fill, negative x clipping, loop ignored */
    {
        /* 4x2 RGBA PNG:
         * (0,0) = opaque red (255,0,0,255)
         * (1,0) = transparent (0,0,0,0)
         * (2,0) = ~50% green (0,255,0,128)
         * (3,0) = opaque blue (0,0,255,255) */
        uint8_t png_rgba[4 * 2 * 4] = {
            255, 0, 0, 255,    0, 0, 0, 0,      0, 255, 0, 128,    0, 0, 255, 255,
            0, 0, 0, 0,        255, 255, 0, 255, 0, 0, 0, 0,       255, 0, 255, 255
        };
        Image pimg = {
            .data = png_rgba,
            .width = 4,
            .height = 2,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        };
        int png_len = 0;
        unsigned char *png_raw = ExportImageToMemory(pimg, ".png", &png_len);
        assert(png_raw != NULL && png_len > 0);
        char *png_b64 = encode_base64(png_raw, (size_t)png_len);
        MemFree(png_raw);

        /* 4a. PNG without fill over #0000c8 blue background, with 'loop' accidentally specified */
        char *body = malloc(strlen(png_b64) + 512);
        assert(body != NULL);
        sprintf(body,
                "["
                "{\"op\":\"clear\",\"color\":\"#0000c8\"},"
                "{\"op\":\"image\",\"x\":10,\"y\":4,\"data\":\"data:image/png;base64,%s\",\"loop\":5}"
                "]",
                png_b64);
        HttpResp r = do_request(port, "POST", "/draw", "application/json", body, strlen(body));
        free(body);
        assert(r.status == 200);
        free(r.body);
        assert(!display_is_gif_active(&disp)); /* PNG is static */

        /* (10,4) opaque red */
        assert(display_get_pixel(&disp, 10, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 0 && pb == 0);
        /* (11,4) transparent -> underlying #0000c8 */
        assert(display_get_pixel(&disp, 11, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 0xc8);
        /* (12,4) 50% green (a=128) over #0000c8 -> g ~ 128, b ~ 100 */
        assert(display_get_pixel(&disp, 12, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg >= 127 && pg <= 129 && pb >= 99 && pb <= 101);
        /* (13,4) opaque blue */
        assert(display_get_pixel(&disp, 13, 4, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 255);

        /* 4b. PNG with fill="white" and negative x clipping; outside rect stays black */
        body = malloc(strlen(png_b64) + 512);
        assert(body != NULL);
        sprintf(body,
                "["
                "{\"op\":\"clear\"},"
                "{\"op\":\"image\",\"x\":-1,\"y\":2,\"data\":\"%s\",\"fill\":\"white\",\"loop\":\"ignored\"}"
                "]",
                png_b64);
        r = do_request(port, "POST", "/draw", "application/json", body, strlen(body));
        free(body);
        assert(r.status == 200);
        free(r.body);

        /* x=-1 clips col 0; col 1 (transparent) lands at (0,2) => white fill */
        assert(display_get_pixel(&disp, 0, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 255 && pg == 255 && pb == 255);
        /* col 2 (green a=128) lands at (1,2) => blended with white (r~127, g=255, b~127) */
        assert(display_get_pixel(&disp, 1, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr >= 126 && pr <= 128 && pg == 255 && pb >= 126 && pb <= 128);
        /* col 3 (blue) lands at (2,2) => opaque blue */
        assert(display_get_pixel(&disp, 2, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 255);
        /* (3,2) outside PNG rect => black */
        assert(display_get_pixel(&disp, 3, 2, &pr, &pg, &pb) == MARKEE_OK);
        assert(pr == 0 && pg == 0 && pb == 0);

        free(png_b64);
    }

    /* 5. Validation errors and atomic rollback */
    {
        /* Restore an active animated GIF to verify rollback preserves it */
        char *setup = malloc(strlen(b64) + 128);
        sprintf(setup, "[{\"op\":\"clear\"},{\"op\":\"image\",\"data\":\"%s\"}]", b64);
        HttpResp r = do_request(port, "POST", "/draw", "application/json", setup, strlen(setup));
        assert(r.status == 200);
        free(r.body);
        free(setup);
        assert(display_is_gif_active(&disp));

        /* Old 'gif' op name is no longer accepted */
        char *old_op = malloc(strlen(b64) + 128);
        sprintf(old_op, "[{\"op\":\"gif\",\"data\":\"%s\"}]", b64);
        r = do_request(port, "POST", "/draw", "application/json", old_op, strlen(old_op));
        assert(r.status == 400);
        free(r.body);
        free(old_op);

        r = do_request(port, "POST", "/draw", "application/json",
                       "[{\"op\":\"image\"}]", strlen("[{\"op\":\"image\"}]"));
        assert(r.status == 400);
        free(r.body);

        r = do_request(port, "POST", "/draw", "application/json",
                       "[{\"op\":\"image\",\"data\":\"!!!not-base64!!!\"}]",
                       strlen("[{\"op\":\"image\",\"data\":\"!!!not-base64!!!\"}]"));
        assert(r.status == 400);
        free(r.body);

        /* Valid base64 ("hello world") but neither PNG nor GIF -> 400 */
        r = do_request(port, "POST", "/draw", "application/json",
                       "[{\"op\":\"image\",\"data\":\"aGVsbG8gd29ybGQ=\"}]",
                       strlen("[{\"op\":\"image\",\"data\":\"aGVsbG8gd29ybGQ=\"}]"));
        assert(r.status == 400);
        free(r.body);

        char *bad_loop = malloc(strlen(b64) + 128);
        sprintf(bad_loop, "[{\"op\":\"image\",\"data\":\"%s\",\"loop\":-2}]", b64);
        r = do_request(port, "POST", "/draw", "application/json", bad_loop, strlen(bad_loop));
        assert(r.status == 400);
        free(r.body);
        free(bad_loop);

        char *bad_fill = malloc(strlen(b64) + 128);
        sprintf(bad_fill, "[{\"op\":\"image\",\"data\":\"%s\",\"fill\":\"notacolor\"}]", b64);
        r = do_request(port, "POST", "/draw", "application/json", bad_fill, strlen(bad_fill));
        assert(r.status == 400);
        free(r.body);
        free(bad_fill);

        /* Atomic rollback: valid image followed by unknown op leaves previous active GIF intact */
        char *rollback = malloc(strlen(b64) + 128);
        sprintf(rollback, "[{\"op\":\"clear\"},{\"op\":\"image\",\"data\":\"%s\"},{\"op\":\"nope\"}]", b64);
        r = do_request(port, "POST", "/draw", "application/json", rollback, strlen(rollback));
        assert(r.status == 400);
        free(r.body);
        free(rollback);
        assert(display_is_gif_active(&disp));
    }

    free(b64);
    free(sprite.buf);
    server_stop(srv);
    display_destroy(&disp);
    printf("[PASS] POST /draw 'image' op (PNG + GIF, alpha/fill, x/y clipping, loop, multi-GIF + scroll, atomic)\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    SetTraceLogLevel(LOG_ERROR);

    test_layout_sizing();
    test_display_and_png_roundtrip();
    test_http_socket_api();
    test_custom_resolution_api();
    test_draw_api();
    test_monochrome_filter();
    test_color_formats();
    test_audit_fixes();
    test_alert_audio_synthesis();
    test_alert_http_api();
    test_gif_decoder_crop_center_and_features();
    test_animate_http_api();
    test_draw_image_op();

    printf("\nAll markee tests passed!\n");
    return 0;
}
