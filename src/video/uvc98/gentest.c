/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Yeo Kheng Meng */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uvcgen.h"

static unsigned char guard[UVCSIM_BYTES + 32];
static unsigned char first[UVCSIM_BYTES];

int main(int argc, char **argv)
{
    unsigned int frame, i;
    assert(!UvcSimGenerate(NULL, UVCSIM_BYTES, 0));
    memset(guard, 0xcc, sizeof(guard));
    assert(!UvcSimGenerate(guard + 16, UVCSIM_BYTES - 1, 0));
    for (i = 0; i < sizeof(guard); i++) assert(guard[i] == 0xcc);
    for (frame = 0; frame < 21; frame++) {
        assert(UvcSimGenerate(guard + 16, UVCSIM_BYTES, frame) == UVCSIM_BYTES);
        for (i = 0; i < 16; i++) assert(guard[i] == 0xcc && guard[16 + UVCSIM_BYTES + i] == 0xcc);
        for (i = 0; i < UVCSIM_BYTES; i++) {
            if (i & 1) assert(guard[16 + i] == 128);
            else assert(guard[16 + i] >= 16 && guard[16 + i] <= 235);
        }
        assert(guard[16] == 16);
        assert(guard[16 + (8 * UVCSIM_WIDTH + 16) * 2] == 235); /* SIMULATED label */
        assert(guard[16 + (205 * UVCSIM_WIDTH + ((frame % 20) * 16 + 8)) * 2] == 235);
        if (!frame) memcpy(first, guard + 16, sizeof(first));
        if (frame == 1) assert(memcmp(first, guard + 16, sizeof(first)) != 0);
        if (frame == 20) assert(memcmp(first, guard + 16, sizeof(first)) == 0);
    }
    assert(UvcSimGenerate(guard + 16, UVCSIM_BYTES, ~0u) == UVCSIM_BYTES);
    if (argc == 2) {
        FILE *file;
        assert(UvcSimGenerate(guard + 16, UVCSIM_BYTES, 0) == UVCSIM_BYTES);
        file = fopen(argv[1], "wb");
        assert(file);
        assert(fwrite(guard + 16, 1, UVCSIM_BYTES, file) == UVCSIM_BYTES);
        assert(!fclose(file));
    } else assert(argc == 1);
    puts("PASS synthetic YUY2 generator: guards, no-write failure, fixed layout/range, label, bounded moving marker, wrap");
    return 0;
}
