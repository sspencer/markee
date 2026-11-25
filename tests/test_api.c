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

    /* Read headers until \r\n\r\n */
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
    /* 14" MacBook Pro style logical 1512 pts over 302 mm physical width */
    MarkeeLayout L = renderer_compute_layout(0.0f, 1512, 302);
    float inches = L.matrix_w / L.dpi;
    assert(fabsf(inches - MARKEE_MATRIX_WIDTH_INCHES) < 0.01f);
    assert(L.body_w > L.matrix_w);
    assert(L.body_h > L.matrix_h);

    /* Fallback when physical width is unavailable */
    MarkeeLayout L2 = renderer_compute_layout(0.0f, 1920, 0);
    assert(fabsf(L2.dpi - MARKEE_DEFAULT_DPI) < 0.01f);
    assert(fabsf(L2.matrix_w / L2.dpi - MARKEE_MATRIX_WIDTH_INCHES) < 0.01f);

    /* Manual DPI override */
    MarkeeLayout L3 = renderer_compute_layout(144.0f, 1920, 500);
    assert(fabsf(L3.dpi - 144.0f) < 0.01f);
    assert(fabsf(L3.matrix_w - 6.35f * 144.0f) < 0.01f);
    printf("[PASS] layout physical sizing (6.35\" matrix)\n");
}

static void test_display_and_png_roundtrip(void) {
    MarkeeDisplay disp;
    assert(display_init(&disp, false));

    /* Fill with distinct 24-bit colors per pixel */
    uint8_t pattern[MARKEE_FRAME_BYTES];
    for (int y = 0; y < MARKEE_HEIGHT; y++) {
        for (int x = 0; x < MARKEE_WIDTH; x++) {
            size_t idx = (size_t)(y * MARKEE_WIDTH + x) * 3;
            pattern[idx + 0] = (uint8_t)((x * 3 + y * 5) & 0xFF);
            pattern[idx + 1] = (uint8_t)((y * 16 + x) & 0xFF);
            pattern[idx + 2] = (uint8_t)((255 - x * 3) & 0xFF);
        }
    }
    assert(display_set_raw_rgb(&disp, pattern, sizeof(pattern), true) == MARKEE_OK);

    /* Encode to PNG */
    uint8_t *png = NULL;
    size_t png_len = 0;
    assert(display_encode_png(&disp, &png, &png_len) == MARKEE_OK);
    assert(png != NULL && png_len > 32);

    /* Clear, reload PNG, verify bit-exact 24-bit lossless recovery */
    display_clear(&disp, true);
    int aw = 0, ah = 0;
    assert(display_load_png(&disp, png, png_len, &aw, &ah) == MARKEE_OK);
    assert(aw == MARKEE_WIDTH && ah == MARKEE_HEIGHT);

    uint8_t readback[MARKEE_FRAME_BYTES];
    display_get_raw_rgb(&disp, readback);
    assert(memcmp(pattern, readback, MARKEE_FRAME_BYTES) == 0);
    display_free_buffer(png);

    /* Reject a wrong-sized PNG (e.g., 16x16) */
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
    assert(display_init(&disp, true));

    /* Bind to ephemeral port 0 so tests never collide with a running instance */
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

    /* 4. GET /frame -> PNG download, verify pixels */
    r = do_request(port, "GET", "/frame", NULL, NULL, 0);
    assert(r.status == 200);
    assert(strcmp(r.content_type, "image/png") == 0);
    assert(r.body_len > 32);
    uint8_t *saved_png = r.body;
    size_t saved_png_len = r.body_len;

    /* 5. POST /clear -> verify black */
    r = do_request(port, "POST", "/clear", NULL, NULL, 0);
    assert(r.status == 200);
    free(r.body);

    uint8_t pr = 1, pg = 1, pb = 1;
    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0 && pg == 0 && pb == 0);

    /* 6. POST /frame with the previously downloaded PNG -> verify pixel restoration */
    r = do_request(port, "POST", "/frame", "image/png", saved_png, saved_png_len);
    assert(r.status == 200);
    free(r.body);
    free(saved_png);

    assert(display_get_pixel(&disp, 0, 0, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 255 && pg == 64 && pb == 1);
    assert(display_get_pixel(&disp, 35, 7, &pr, &pg, &pb) == MARKEE_OK);
    assert(pr == 0x12 && pg == 0xab && pb == 0xef);

    /* 7. Keep-alive persistent socket: raw RGB upload + raw RGB readback + brightness */
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
    printf("[PASS] HTTP/TCP socket API (PNG I/O, raw RGB24, pixel/fill/clear/brightness, keep-alive)\n");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    SetTraceLogLevel(LOG_ERROR);

    test_layout_sizing();
    test_display_and_png_roundtrip();
    test_http_socket_api();

    printf("\nAll markee tests passed!\n");
    return 0;
}
