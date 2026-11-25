#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 32

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    int depth;
    char *err_buf;
    size_t err_size;
} JsonParser;

static void set_err(JsonParser *p, const char *msg) {
    if (p->err_buf && p->err_size > 0 && p->err_buf[0] == '\0') {
        snprintf(p->err_buf, p->err_size, "%s at byte %zu", msg, p->pos);
    }
}

static void skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else {
            break;
        }
    }
}

static JsonValue *alloc_val(JsonType t) {
    JsonValue *v = calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static JsonValue *parse_value(JsonParser *p);

static char *parse_string_raw(JsonParser *p) {
    if (p->pos >= p->len || p->src[p->pos] != '"') {
        set_err(p, "expected '\"'");
        return NULL;
    }
    p->pos++; /* skip opening quote */

    size_t cap = 32;
    size_t out_len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (p->pos < p->len) {
        char c = p->src[p->pos++];
        if (c == '"') {
            buf[out_len] = '\0';
            return buf;
        }
        if ((unsigned char)c < 0x20) {
            set_err(p, "unescaped control character in string");
            free(buf);
            return NULL;
        }
        if (c == '\\') {
            if (p->pos >= p->len) {
                set_err(p, "unterminated escape sequence");
                free(buf);
                return NULL;
            }
            char e = p->src[p->pos++];
            switch (e) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    if (p->pos + 4 > p->len) {
                        set_err(p, "short unicode escape");
                        free(buf);
                        return NULL;
                    }
                    unsigned int cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = p->src[p->pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
                        else {
                            set_err(p, "invalid hex in \\u escape");
                            free(buf);
                            return NULL;
                        }
                    }
                    c = (cp >= 0x20 && cp <= 0x7E) ? (char)cp : '?';
                    break;
                }
                default:
                    set_err(p, "invalid escape sequence");
                    free(buf);
                    return NULL;
            }
        }
        if (out_len + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        buf[out_len++] = c;
    }

    set_err(p, "unterminated string");
    free(buf);
    return NULL;
}

static JsonValue *parse_number(JsonParser *p) {
    size_t start = p->pos;
    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
        set_err(p, "invalid number");
        return NULL;
    }
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            set_err(p, "invalid fraction in number");
            return NULL;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->src[p->pos])) {
            set_err(p, "invalid exponent in number");
            return NULL;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    size_t nlen = p->pos - start;
    char tmp[64];
    if (nlen >= sizeof(tmp)) {
        set_err(p, "number too long");
        return NULL;
    }
    memcpy(tmp, p->src + start, nlen);
    tmp[nlen] = '\0';

    char *end = NULL;
    double val = strtod(tmp, &end);
    if (end == tmp || !isfinite(val)) {
        set_err(p, "number out of range");
        return NULL;
    }

    JsonValue *v = alloc_val(JSON_NUMBER);
    if (v) v->u.nval = val;
    return v;
}

static JsonValue *parse_array(JsonParser *p) {
    p->pos++; /* skip '[' */
    JsonValue *arr = alloc_val(JSON_ARRAY);
    if (!arr) return NULL;

    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->pos++;
        return arr;
    }

    size_t cap = 8;
    arr->u.arr.items = malloc(cap * sizeof(JsonValue *));
    if (!arr->u.arr.items) {
        free(arr);
        return NULL;
    }

    for (;;) {
        skip_ws(p);
        JsonValue *item = parse_value(p);
        if (!item) {
            json_free(arr);
            return NULL;
        }
        if (arr->u.arr.count == cap) {
            cap *= 2;
            JsonValue **ni = realloc(arr->u.arr.items, cap * sizeof(JsonValue *));
            if (!ni) {
                json_free(item);
                json_free(arr);
                return NULL;
            }
            arr->u.arr.items = ni;
        }
        arr->u.arr.items[arr->u.arr.count++] = item;

        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == ']') {
            p->pos++;
            return arr;
        }
        set_err(p, "expected ',' or ']' in array");
        json_free(arr);
        return NULL;
    }
}

static JsonValue *parse_object(JsonParser *p) {
    p->pos++; /* skip '{' */
    JsonValue *obj = alloc_val(JSON_OBJECT);
    if (!obj) return NULL;

    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->pos++;
        return obj;
    }

    size_t cap = 8;
    obj->u.obj.entries = malloc(cap * sizeof(JsonEntry));
    if (!obj->u.obj.entries) {
        free(obj);
        return NULL;
    }

    for (;;) {
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (!key) {
            json_free(obj);
            return NULL;
        }
        skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            set_err(p, "expected ':' after object key");
            free(key);
            json_free(obj);
            return NULL;
        }
        p->pos++;

        skip_ws(p);
        JsonValue *val = parse_value(p);
        if (!val) {
            free(key);
            json_free(obj);
            return NULL;
        }

        if (obj->u.obj.count == cap) {
            cap *= 2;
            JsonEntry *ne = realloc(obj->u.obj.entries, cap * sizeof(JsonEntry));
            if (!ne) {
                free(key);
                json_free(val);
                json_free(obj);
                return NULL;
            }
            obj->u.obj.entries = ne;
        }
        obj->u.obj.entries[obj->u.obj.count].key = key;
        obj->u.obj.entries[obj->u.obj.count].value = val;
        obj->u.obj.count++;

        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == ',') {
            p->pos++;
            continue;
        }
        if (p->pos < p->len && p->src[p->pos] == '}') {
            p->pos++;
            return obj;
        }
        set_err(p, "expected ',' or '}' in object");
        json_free(obj);
        return NULL;
    }
}

static JsonValue *parse_value(JsonParser *p) {
    if (p->depth >= JSON_MAX_DEPTH) {
        set_err(p, "JSON nesting too deep");
        return NULL;
    }
    skip_ws(p);
    if (p->pos >= p->len) {
        set_err(p, "unexpected end of JSON");
        return NULL;
    }

    char c = p->src[p->pos];
    if (c == '{') {
        p->depth++;
        JsonValue *v = parse_object(p);
        p->depth--;
        return v;
    }
    if (c == '[') {
        p->depth++;
        JsonValue *v = parse_array(p);
        p->depth--;
        return v;
    }
    if (c == '"') {
        char *s = parse_string_raw(p);
        if (!s) return NULL;
        JsonValue *v = alloc_val(JSON_STRING);
        if (!v) {
            free(s);
            return NULL;
        }
        v->u.sval = s;
        return v;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }
    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JsonValue *v = alloc_val(JSON_BOOL);
        if (v) v->u.bval = true;
        return v;
    }
    if (p->pos + 5 <= p->len && memcmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JsonValue *v = alloc_val(JSON_BOOL);
        if (v) v->u.bval = false;
        return v;
    }
    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return alloc_val(JSON_NULL);
    }

    set_err(p, "unexpected token");
    return NULL;
}

JsonValue *json_parse(const char *text, size_t len, char *err_buf, size_t err_size) {
    if (err_buf && err_size > 0) err_buf[0] = '\0';
    if (!text || len == 0) {
        if (err_buf && err_size > 0) snprintf(err_buf, err_size, "empty JSON body");
        return NULL;
    }

    JsonParser p = {
        .src = text,
        .len = len,
        .pos = 0,
        .depth = 0,
        .err_buf = err_buf,
        .err_size = err_size
    };

    JsonValue *root = parse_value(&p);
    if (!root) return NULL;

    skip_ws(&p);
    if (p.pos != p.len) {
        set_err(&p, "trailing garbage after JSON value");
        json_free(root);
        return NULL;
    }
    return root;
}

void json_free(JsonValue *val) {
    if (!val) return;
    switch (val->type) {
        case JSON_STRING:
            free(val->u.sval);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < val->u.arr.count; i++) {
                json_free(val->u.arr.items[i]);
            }
            free(val->u.arr.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < val->u.obj.count; i++) {
                free(val->u.obj.entries[i].key);
                json_free(val->u.obj.entries[i].value);
            }
            free(val->u.obj.entries);
            break;
        default:
            break;
    }
    free(val);
}

const JsonValue *json_obj_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.entries[i].key, key) == 0) {
            return obj->u.obj.entries[i].value;
        }
    }
    return NULL;
}

bool json_as_int(const JsonValue *val, int *out) {
    if (!val || val->type != JSON_NUMBER || !out) return false;
    *out = (int)lround(val->u.nval);
    return true;
}

bool json_as_double(const JsonValue *val, double *out) {
    if (!val || val->type != JSON_NUMBER || !out) return false;
    *out = val->u.nval;
    return true;
}

bool json_as_bool(const JsonValue *val, bool *out) {
    if (!val || !out) return false;
    if (val->type == JSON_BOOL) {
        *out = val->u.bval;
        return true;
    }
    if (val->type == JSON_NUMBER) {
        *out = (val->u.nval != 0.0);
        return true;
    }
    return false;
}

const char *json_as_string(const JsonValue *val) {
    if (!val || val->type != JSON_STRING) return NULL;
    return val->u.sval;
}
