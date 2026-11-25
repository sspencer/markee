#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gif.h"
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char err[128];
    MarkeeGif *gif = NULL;

    /* Exercise full-screen decode across all 3 fit modes */
    MarkeeGifFitMode mode = (MarkeeGifFitMode)(size % 3);
    if (gif_decode_fit(data, size, 72, 16, mode, 0, 0, 0, &gif, err, sizeof(err))) {
        uint8_t canvas[72 * 16 * 3];
        memset(canvas, 0, sizeof(canvas));
        if (gif->frame_count > 0) {
            gif_blit_frame(gif, 0, canvas, 72, 16);
            gif_blit_frame(gif, gif->frame_count - 1, canvas, 72, 16);
        }
        gif_free(gif);
        gif = NULL;
    }

    /* Exercise placed decode with alpha preservation and with fill */
    bool has_fill = (size & 1u) != 0;
    int px = (size > 0) ? (int)(int8_t)data[0] / 4 : 0;
    int py = (size > 1) ? (int)(int8_t)data[1] / 8 : 0;
    if (gif_decode_placed(data, size, 72, 16, px, py, has_fill, 10, 20, 30,
                          &gif, err, sizeof(err))) {
        uint8_t canvas[72 * 16 * 3];
        memset(canvas, 0, sizeof(canvas));
        if (gif->frame_count > 0) {
            gif_blit_frame(gif, 0, canvas, 72, 16);
        }
        gif_free(gif);
    }

    return 0;
}

#ifdef FUZZ_STANDALONE
int main(int argc, char **argv) {
    static const char *const SEEDS[] = {
        "GIF89a",
        "GIF87a\x01\x00\x01\x00\x80\x00\x00\xff\x00\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00;",
    };
    return fuzz_standalone_main(argc, argv, SEEDS, sizeof(SEEDS) / sizeof(SEEDS[0]));
}
#endif
