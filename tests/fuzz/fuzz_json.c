#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "fuzz_common.h"

static void walk_json(const JsonValue *v, int depth) {
    if (!v || depth > 64) return;
    int iv = 0;
    double dv = 0.0;
    bool bv = false;
    (void)json_as_int(v, &iv);
    (void)json_as_double(v, &dv);
    (void)json_as_bool(v, &bv);
    (void)json_as_string(v);

    if (v->type == JSON_ARRAY) {
        for (size_t i = 0; i < v->u.arr.count; i++) {
            walk_json(v->u.arr.items[i], depth + 1);
        }
    } else if (v->type == JSON_OBJECT) {
        (void)json_obj_get(v, "op");
        for (size_t i = 0; i < v->u.obj.count; i++) {
            walk_json(v->u.obj.entries[i].value, depth + 1);
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char err[128];
    JsonValue *v = json_parse((const char *)data, size, err, sizeof(err));
    if (v) {
        walk_json(v, 0);
        json_free(v);
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(int argc, char **argv) {
    static const char *const SEEDS[] = {
        "{}",
        "[]",
        "{\"a\":1,\"b\":[true,false,null,-1.25e+3,\"hi\\n\\u0041\"]}",
        "[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]",
        "{\"unterminated\":\"abc\\u12",
    };
    return fuzz_standalone_main(argc, argv, SEEDS, sizeof(SEEDS) / sizeof(SEEDS[0]));
}
#endif
