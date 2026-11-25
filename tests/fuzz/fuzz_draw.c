#include <stddef.h>
#include <stdint.h>

#include "display.h"
#include "draw.h"
#include "fuzz_common.h"

static MarkeeDisplay g_disp;
static bool g_disp_init = false;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!g_disp_init) {
        if (!display_init(&g_disp, 72, 16, false)) return 0;
        g_disp_init = true;
    }

    int ops = 0;
    char err[128];
    (void)draw_exec_json(&g_disp, (const char *)data, size, &ops, err, sizeof(err));

    /* Also exercise color parsing directly on short inputs */
    if (size > 0 && size < 64) {
        char tmp[64];
        for (size_t i = 0; i < size; i++) tmp[i] = (char)data[i];
        tmp[size] = '\0';
        MarkeeColor c;
        (void)draw_parse_color_string(tmp, &c);
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(int argc, char **argv) {
    static const char *const SEEDS[] = {
        "[{\"op\":\"clear\"},{\"op\":\"rect\",\"x\":0,\"y\":0,\"w\":72,\"h\":16,\"color\":\"amber\",\"radius\":3,\"alpha\":0.5}]",
        "[{\"op\":\"text\",\"x\":2,\"y\":4,\"text\":\"HELLO\",\"font\":\"prop\",\"align\":\"center\",\"w\":68,\"clip\":[0,0,72,16]}]",
        "[{\"op\":\"polygon\",\"points\":[[0,0],[20,10],[5,15]],\"fill\":\"cyan\"}]",
        "[{\"op\":\"icon\",\"name\":\"heart\",\"x\":4,\"y\":4,\"color\":\"red\",\"scale\":2}]",
        "[{\"op\":\"gradient\",\"x\":0,\"y\":0,\"w\":72,\"h\":16,\"from\":\"#ff0000\",\"to\":\"#0000ff\",\"direction\":\"vertical\"}]",
        "[{\"op\":\"progress\",\"x\":2,\"y\":4,\"w\":68,\"h\":6,\"value\":50,\"max\":100}]",
        "[{\"op\":\"sparkline\",\"x\":2,\"y\":2,\"w\":40,\"h\":12,\"values\":[1,5,2,9,4]}]",
        "[{\"op\":\"image\",\"x\":0,\"y\":0,\"data\":\"R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7\"}]",
        "[{\"op\":\"clock\",\"x\":0,\"y\":4,\"w\":72,\"align\":\"center\"},{\"op\":\"countdown\",\"x\":0,\"y\":4,\"seconds\":10,\"bar\":true}]",
    };
    int rc = fuzz_standalone_main(argc, argv, SEEDS, sizeof(SEEDS) / sizeof(SEEDS[0]));
    if (g_disp_init) {
        display_destroy(&g_disp);
        g_disp_init = false;
    }
    return rc;
}
#endif
