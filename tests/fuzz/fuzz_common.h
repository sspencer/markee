#ifndef MARKEE_FUZZ_COMMON_H
#define MARKEE_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifdef FUZZ_STANDALONE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fuzz_xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void fuzz_run_file(const char *path, uint64_t *rng, int mutations) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    if (sz < 0 || sz > 2 * 1024 * 1024) { fclose(f); return; }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    LLVMFuzzerTestOneInput(buf, got);

    uint8_t *mut = malloc(got + 1);
    if (mut && got > 0) {
        for (int m = 0; m < mutations; m++) {
            memcpy(mut, buf, got);
            size_t len = got;
            unsigned mode = (unsigned)(fuzz_xorshift64(rng) % 4);
            if (mode == 0 && len > 1) {
                /* Truncate */
                len = (size_t)(fuzz_xorshift64(rng) % len);
            } else if (mode == 1) {
                /* Flip 1..4 random bytes */
                unsigned flips = 1 + (unsigned)(fuzz_xorshift64(rng) % 4);
                for (unsigned k = 0; k < flips; k++) {
                    size_t idx = (size_t)(fuzz_xorshift64(rng) % len);
                    mut[idx] ^= (uint8_t)(fuzz_xorshift64(rng) & 0xFF);
                }
            } else if (mode == 2) {
                /* Overwrite a random byte with boundary value */
                static const uint8_t edges[] = {0x00, 0x01, 0x0A, 0x0D, 0x20, 0x22, 0x25, 0x3B, 0x7F, 0x80, 0xFF};
                size_t idx = (size_t)(fuzz_xorshift64(rng) % len);
                mut[idx] = edges[fuzz_xorshift64(rng) % sizeof(edges)];
            } else {
                /* Flip bits in header/prefix */
                size_t cap = len < 32 ? len : 32;
                size_t idx = (size_t)(fuzz_xorshift64(rng) % cap);
                mut[idx] = (uint8_t)(fuzz_xorshift64(rng) & 0xFF);
            }
            LLVMFuzzerTestOneInput(mut, len);
        }
    }
    free(mut);
    free(buf);
}

static int fuzz_standalone_main(int argc, char **argv,
                                const char *const *seeds, size_t seed_count) {
    uint64_t rng = 0x5EED1234CAFEBABEULL;

    /* Empty and 1-byte edge inputs */
    LLVMFuzzerTestOneInput((const uint8_t *)"", 0);
    for (int b = 0; b < 256; b++) {
        uint8_t one = (uint8_t)b;
        LLVMFuzzerTestOneInput(&one, 1);
    }

    /* Built-in seeds + mutations */
    for (size_t s = 0; s < seed_count; s++) {
        size_t len = strlen(seeds[s]);
        LLVMFuzzerTestOneInput((const uint8_t *)seeds[s], len);
        uint8_t *mut = malloc(len + 16);
        if (!mut) continue;
        for (int m = 0; m < 150; m++) {
            memcpy(mut, seeds[s], len);
            size_t mlen = len;
            if (mlen > 0) {
                size_t idx = (size_t)(fuzz_xorshift64(&rng) % mlen);
                mut[idx] ^= (uint8_t)(1u << (fuzz_xorshift64(&rng) % 8));
                if ((fuzz_xorshift64(&rng) & 3u) == 0 && mlen > 1) {
                    mlen = (size_t)(fuzz_xorshift64(&rng) % mlen);
                }
            }
            LLVMFuzzerTestOneInput(mut, mlen);
        }
        free(mut);
    }

    /* Random noise inputs */
    uint8_t noise[256];
    for (int i = 0; i < 400; i++) {
        size_t nlen = (size_t)(fuzz_xorshift64(&rng) % sizeof(noise));
        for (size_t j = 0; j < nlen; j++) {
            noise[j] = (uint8_t)(fuzz_xorshift64(&rng) & 0xFF);
        }
        LLVMFuzzerTestOneInput(noise, nlen);
    }

    /* Corpus files from CLI */
    for (int i = 1; i < argc; i++) {
        fuzz_run_file(argv[i], &rng, 250);
    }

    printf("[PASS] %s standalone fuzz smoke complete\n", argv[0]);
    return 0;
}
#endif /* FUZZ_STANDALONE */

#endif /* MARKEE_FUZZ_COMMON_H */
