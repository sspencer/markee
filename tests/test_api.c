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

#include "config.h"
#include "display.h"
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
    printf("[PASS] layout physical sizing, zoom, pixel shape, and custom resolution\n");
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

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    SetTraceLogLevel(LOG_ERROR);

    test_layout_sizing();
    test_display_and_png_roundtrip();
    test_http_socket_api();
    test_custom_resolution_api();
    test_draw_api();
    test_monochrome_filter();

    printf("\nAll markee tests passed!\n");
    return 0;
}
