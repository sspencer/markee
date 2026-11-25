#define _POSIX_C_SOURCE 200809L

#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"

bool cli_is_subcommand(const char *arg) {
    if (!arg || !*arg) return false;
    static const char *const CMDS[] = {
        "say", "notify", "gif", "timer", "clock", "alert", "clear", "info"
    };
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (strcmp(arg, CMDS[i]) == 0) return true;
    }
    return false;
}

bool cli_parse_duration_sec(const char *s, double *out_sec) {
    if (!s || !*s || !out_sec) return false;
    const char *p = s;
    double total = 0.0;
    bool any = false;
    while (*p) {
        if (!isdigit((unsigned char)*p) && *p != '.') return false;
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p || v < 0.0) return false;
        p = end;
        double mult = 1.0;
        if (*p == 'h' || *p == 'H') { mult = 3600.0; p++; }
        else if (*p == 'm' || *p == 'M') { mult = 60.0; p++; }
        else if (*p == 's' || *p == 'S') { mult = 1.0; p++; }
        else if (*p != '\0') return false;
        total += v * mult;
        any = true;
    }
    if (!any || !(total > 0.0) || total > 359999.0) return false;
    *out_sec = total;
    return true;
}

static void json_esc(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        size_t rlen = 1;
        switch (*p) {
            case '"':  rep = "\\\""; rlen = 2; break;
            case '\\': rep = "\\\\"; rlen = 2; break;
            case '\n': rep = "\\n";  rlen = 2; break;
            case '\r': rep = "\\r";  rlen = 2; break;
            case '\t': rep = "\\t";  rlen = 2; break;
            default:
                tmp[0] = (char)*p;
                tmp[1] = '\0';
                rep = tmp;
                break;
        }
        if (o + rlen >= out_size) break;
        memcpy(out + o, rep, rlen);
        o += rlen;
    }
    out[o] = '\0';
}

static void url_enc(const char *in, char *out, size_t out_size) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (o + 1 >= out_size) break;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= out_size) break;
            out[o++] = '%';
            out[o++] = HEX[(*p >> 4) & 0xF];
            out[o++] = HEX[*p & 0xF];
        }
    }
    out[o] = '\0';
}

static int http_call(const char *host, const char *port, const char *token,
                     const char *method, const char *path,
                     const char *content_type, const void *body, size_t body_len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "markee: cannot resolve %s:%s (%s)\n", host, port, gai_strerror(rc));
        return 1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "markee: could not connect to http://%s:%s\n", host, port);
        return 1;
    }

    char auth_hdr[320] = {0};
    if (token && *token) {
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s\r\n", token);
    }
    char hdr[1024];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s:%s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "%s"
                        "Connection: close\r\n"
                        "\r\n",
                        method, path, host, port,
                        content_type ? content_type : "application/octet-stream",
                        body_len, auth_hdr);
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        close(fd);
        return 1;
    }

    const uint8_t *p = (const uint8_t *)hdr;
    size_t rem = (size_t)hlen;
    while (rem > 0) {
        ssize_t n = send(fd, p, rem, 0);
        if (n <= 0) { close(fd); return 1; }
        p += (size_t)n;
        rem -= (size_t)n;
    }
    if (body && body_len > 0) {
        p = (const uint8_t *)body;
        rem = body_len;
        while (rem > 0) {
            ssize_t n = send(fd, p, rem, 0);
            if (n <= 0) { close(fd); return 1; }
            p += (size_t)n;
            rem -= (size_t)n;
        }
    }

    size_t cap = 4096, got = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return 1; }
    for (;;) {
        if (got + 1 >= cap) {
            cap *= 2;
            char *nr = realloc(resp, cap);
            if (!nr) { free(resp); close(fd); return 1; }
            resp = nr;
        }
        ssize_t n = recv(fd, resp + got, cap - got - 1, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    resp[got] = '\0';
    close(fd);

    int status = 0;
    sscanf(resp, "HTTP/1.%*d %d", &status);
    char *sep = strstr(resp, "\r\n\r\n");
    const char *rbody = sep ? sep + 4 : resp;
    if (status >= 200 && status < 300) {
        if (*rbody) printf("%s\n", rbody);
        free(resp);
        return 0;
    }
    fprintf(stderr, "HTTP %d: %s\n", status, rbody);
    free(resp);
    return 1;
}

int cli_run(int argc, char **argv) {
    const char *cmd = argv[1];
    const char *host = getenv("MARKEE_HOST");
    if (!host || !*host) host = MARKEE_DEFAULT_HOST;
    const char *port = getenv("MARKEE_PORT");
    if (!port || !*port) port = "8080";
    const char *token = getenv("MARKEE_TOKEN");

    const char *positional = NULL;
    const char *color = "warmwhite";
    const char *bg = "black";
    const char *border = NULL;
    const char *font = "6x8";
    const char *fill = "black";
    const char *fit = "crop";
    const char *sound = NULL;
    const char *format = "24h";
    const char *transition = NULL;
    double ttl = 5.0;
    double speed = 1.0;
    int loop = -1;
    int volume = 100;
    bool scroll = false;
    bool has_bar = true;

    for (int i = 2; i < argc; i++) {
        if ((strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            token = argv[++i];
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--color") == 0) && i + 1 < argc) {
            color = argv[++i];
        } else if (strcmp(argv[i], "--bg") == 0 && i + 1 < argc) {
            bg = argv[++i];
        } else if (strcmp(argv[i], "--border") == 0 && i + 1 < argc) {
            border = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font = argv[++i];
        } else if (strcmp(argv[i], "--fill") == 0 && i + 1 < argc) {
            fill = argv[++i];
        } else if (strcmp(argv[i], "--fit") == 0 && i + 1 < argc) {
            fit = argv[++i];
        } else if (strcmp(argv[i], "--ttl") == 0 && i + 1 < argc) {
            ttl = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            speed = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--loop") == 0 && i + 1 < argc) {
            loop = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sound") == 0 && i + 1 < argc) {
            sound = argv[++i];
        } else if (strcmp(argv[i], "--chime") == 0) {
            sound = "chime";
        } else if (strcmp(argv[i], "--ding") == 0) {
            sound = "ding";
        } else if (strcmp(argv[i], "--bell") == 0) {
            sound = "bell";
        } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            volume = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            format = argv[++i];
        } else if (strcmp(argv[i], "--transition") == 0 && i + 1 < argc) {
            transition = argv[++i];
        } else if (strcmp(argv[i], "--scroll") == 0) {
            scroll = true;
        } else if (strcmp(argv[i], "--no-bar") == 0) {
            has_bar = false;
        } else if (argv[i][0] != '-' && !positional) {
            positional = argv[i];
        } else {
            fprintf(stderr, "Unknown option for 'markee %s': %s\n", cmd, argv[i]);
            return 1;
        }
    }

    if (strcmp(cmd, "info") == 0) {
        return http_call(host, port, token, "GET", "/info", NULL, NULL, 0);
    }
    if (strcmp(cmd, "clear") == 0) {
        return http_call(host, port, token, "POST", "/clear", NULL, NULL, 0);
    }
    if (strcmp(cmd, "alert") == 0) {
        const char *snd = positional ? positional : (sound ? sound : "ding");
        char enc[64], path[128];
        url_enc(snd, enc, sizeof(enc));
        snprintf(path, sizeof(path), "/alert/%s?volume=%d", enc, volume);
        return http_call(host, port, token, "POST", path, NULL, NULL, 0);
    }
    if (strcmp(cmd, "clock") == 0) {
        char ec[64], eb[64], ef[32], efmt[32], json[512];
        json_esc(color, ec, sizeof(ec));
        json_esc(bg, eb, sizeof(eb));
        json_esc(font, ef, sizeof(ef));
        json_esc(format, efmt, sizeof(efmt));
        snprintf(json, sizeof(json),
                 "[{\"op\":\"clear\",\"color\":\"%s\"},"
                 "{\"op\":\"clock\",\"color\":\"%s\",\"font\":\"%s\",\"format\":\"%s\"}]",
                 eb, ec, ef, efmt);
        return http_call(host, port, token, "POST", "/draw", "application/json", json, strlen(json));
    }
    if (strcmp(cmd, "timer") == 0) {
        double secs = 0.0;
        if (!positional || !cli_parse_duration_sec(positional, &secs)) {
            fprintf(stderr, "Usage: markee timer <duration> (e.g. 25m, 90s, 1h30m)\n");
            return 1;
        }
        char ec[64], eb[64], ef[32], es[64], json[640];
        json_esc(color, ec, sizeof(ec));
        json_esc(bg, eb, sizeof(eb));
        json_esc(font, ef, sizeof(ef));
        if (sound) {
            json_esc(sound, es, sizeof(es));
            snprintf(json, sizeof(json),
                     "[{\"op\":\"clear\",\"color\":\"%s\"},"
                     "{\"op\":\"countdown\",\"seconds\":%.2f,\"color\":\"%s\",\"font\":\"%s\","
                     "\"bar\":%s,\"sound\":\"%s\"}]",
                     eb, secs, ec, ef, has_bar ? "true" : "false", es);
        } else {
            snprintf(json, sizeof(json),
                     "[{\"op\":\"clear\",\"color\":\"%s\"},"
                     "{\"op\":\"countdown\",\"seconds\":%.2f,\"color\":\"%s\",\"font\":\"%s\","
                     "\"bar\":%s}]",
                     eb, secs, ec, ef, has_bar ? "true" : "false");
        }
        return http_call(host, port, token, "POST", "/draw", "application/json", json, strlen(json));
    }
    if (strcmp(cmd, "say") == 0 || strcmp(cmd, "notify") == 0) {
        if (!positional) {
            fprintf(stderr, "Usage: markee %s <text> [--color C] [--bg C] [--ttl S] [--sound S]\n", cmd);
            return 1;
        }
        char et[512], ec[64], eb[64], ef[32], json[1200];
        json_esc(positional, et, sizeof(et));
        json_esc(color, ec, sizeof(ec));
        json_esc(bg, eb, sizeof(eb));
        json_esc(font, ef, sizeof(ef));
        if (strcmp(cmd, "notify") == 0) {
            char ebdr[64] = {0}, esnd[64] = {0};
            if (border) json_esc(border, ebdr, sizeof(ebdr));
            if (sound) json_esc(sound, esnd, sizeof(esnd));
            snprintf(json, sizeof(json),
                     "{\"text\":\"%s\",\"color\":\"%s\",\"bg\":\"%s\",\"font\":\"%s\","
                     "\"ttl\":%.2f%s%s%s%s%s%s}",
                     et, ec, eb, ef, ttl,
                     border ? ",\"border\":\"" : "", border ? ebdr : "", border ? "\"" : "",
                     sound ? ",\"sound\":\"" : "", sound ? esnd : "", sound ? "\"" : "");
            return http_call(host, port, token, "POST", "/notify",
                             "application/json", json, strlen(json));
        }
        snprintf(json, sizeof(json),
                 "[{\"op\":\"clear\",\"color\":\"%s\"},"
                 "{\"op\":\"text\",\"y\":4,\"text\":\"%s\",\"color\":\"%s\",\"font\":\"%s\","
                 "\"align\":\"center\",\"scroll\":%s}]",
                 eb, et, ec, ef, scroll ? "true" : "false");
        char path[128] = "/draw";
        if (transition) {
            char etr[32];
            url_enc(transition, etr, sizeof(etr));
            snprintf(path, sizeof(path), "/draw?transition=%s", etr);
        }
        int r = http_call(host, port, token, "POST", path, "application/json", json, strlen(json));
        if (r == 0 && sound) {
            char enc[64], apath[128];
            url_enc(sound, enc, sizeof(enc));
            snprintf(apath, sizeof(apath), "/alert/%s?volume=%d", enc, volume);
            (void)http_call(host, port, token, "POST", apath, NULL, NULL, 0);
        }
        return r;
    }
    if (strcmp(cmd, "gif") == 0) {
        if (!positional) {
            fprintf(stderr, "Usage: markee gif <file.gif> [--fill C] [--fit crop|contain|stretch] [--speed S] [--loop N]\n");
            return 1;
        }
        FILE *fp = fopen(positional, "rb");
        if (!fp) {
            perror(positional);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0 || sz > MARKEE_MAX_BODY_BYTES) {
            fprintf(stderr, "markee: GIF file size invalid or exceeds 4 MiB\n");
            fclose(fp);
            return 1;
        }
        uint8_t *buf = malloc((size_t)sz);
        if (!buf || fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
            free(buf);
            fclose(fp);
            return 1;
        }
        fclose(fp);

        char efill[64], efit[32], path[256];
        url_enc(fill, efill, sizeof(efill));
        url_enc(fit, efit, sizeof(efit));
        if (loop >= 0) {
            snprintf(path, sizeof(path), "/animate?fill=%s&fit=%s&speed=%.2f&loop=%d",
                     efill, efit, speed, loop);
        } else {
            snprintf(path, sizeof(path), "/animate?fill=%s&fit=%s&speed=%.2f",
                     efill, efit, speed);
        }
        int r = http_call(host, port, token, "POST", path, "image/gif", buf, (size_t)sz);
        free(buf);
        return r;
    }

    return 1;
}
