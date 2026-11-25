#ifndef MARKEE_JSON_H
#define MARKEE_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *value;
} JsonEntry;

struct JsonValue {
    JsonType type;
    union {
        bool bval;
        double nval;
        char *sval;
        struct {
            JsonValue **items;
            size_t count;
        } arr;
        struct {
            JsonEntry *entries;
            size_t count;
        } obj;
    } u;
};

/* Parse a JSON string (need not be NUL-terminated if len is given).
 * Returns NULL on syntax error; optionally writes error message to err_buf. */
JsonValue *json_parse(const char *text, size_t len, char *err_buf, size_t err_size);
void json_free(JsonValue *val);

/* Object lookup helpers */
const JsonValue *json_obj_get(const JsonValue *obj, const char *key);
bool json_as_int(const JsonValue *val, int *out);
bool json_as_double(const JsonValue *val, double *out);
bool json_as_bool(const JsonValue *val, bool *out);
const char *json_as_string(const JsonValue *val);

#endif /* MARKEE_JSON_H */
