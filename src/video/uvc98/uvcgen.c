/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original grayscale bars/marker/letter pixels; no camera or image assets.
 */
#include "uvcgen.h"

static unsigned char Luma(unsigned int x, unsigned int y, unsigned int frame)
{
    static const unsigned char letters[9][7] = {
        {31,16,16,31,1,1,31}, {31,4,4,4,4,4,31}, {17,27,21,17,17,17,17},
        {17,17,17,17,17,17,31}, {16,16,16,16,16,16,31}, {14,17,17,31,17,17,17},
        {31,4,4,4,4,4,4}, {31,16,16,30,16,16,31}, {30,17,17,17,17,17,30}
    };
    unsigned int cell, col, row;
    if (y < 30) {
        if (x >= 16 && y >= 8 && y < 22) {
            cell = (x - 16) / 12;
            col = ((x - 16) % 12) / 2;
            row = (y - 8) / 2;
            if (cell < 9 && col < 5 && (letters[cell][row] & (16u >> col))) return 235;
        }
        return 16;
    }
    if (y >= 200 && y < 220 && x / 16 == frame % 20) return 235;
    return (unsigned char)(16 + (x / 40) * 28);
}

unsigned int UvcSimGenerate(unsigned char *bytes, unsigned int capacity,
                           unsigned int frame)
{
    unsigned int x, y, offset = 0;
    if (!bytes || capacity < UVCSIM_BYTES) return 0;
    for (y = 0; y < UVCSIM_HEIGHT; y++) for (x = 0; x < UVCSIM_WIDTH; x += 2) {
        bytes[offset++] = Luma(x, y, frame);
        bytes[offset++] = 128;
        bytes[offset++] = Luma(x + 1, y, frame);
        bytes[offset++] = 128;
    }
    return offset;
}
