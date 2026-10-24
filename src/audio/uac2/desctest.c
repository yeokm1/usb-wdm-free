/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host executable: recorded hardware input and hostile descriptor tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "uacdesc.h"
#include "uacplan.h"

static unsigned int checks;
static void Check(int ok, const char *expression, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL line %u: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

static UAC_DESC parsed;
static UAC_BYTE original[65536], mutated[65536];
static UAC_BYTE captured[65536];

static int Parse(const UAC_BYTE *b, unsigned int n)
{
    return UacParse(b, n, 3, 3, 4, &parsed);
}

/* Keep USB framing and both totals consistent while shortening the FU.
 * These cases exercise entity structure, not an earlier framing failure. */
static void CheckControlStructure(const UAC_BYTE *bytes, unsigned int size)
{
    unsigned int off, header = 0, fu, total, i, removed, shortened;
    unsigned int badTotal[6], badFeatureLength[3] = { 9, 11, 13 };
    int inControl = 0;
    CHECK(Parse(bytes, size) == UAC_OK);
    fu = parsed.Entity[1];
    CHECK(fu && bytes[fu + 2] == 6 && bytes[fu] == 42);
    for (off = 9; off < size; off += bytes[off]) {
        if (bytes[off + 1] == 4) inControl = bytes[off + 2] == 3;
        if (inControl && bytes[off + 1] == 0x24 && bytes[off + 2] == 1) {
            header = off; break;
        }
    }
    CHECK(header != 0);
    total = bytes[header + 6] | ((unsigned int)bytes[header + 7] << 8);
    badTotal[0] = 0; badTotal[1] = 8;
    badTotal[2] = total - 1; badTotal[3] = total + 1;
    badTotal[4] = size; badTotal[5] = 65535;
    for (i = 0; i < 6; i++) {
        memcpy(mutated, bytes, size);
        mutated[header + 6] = (UAC_BYTE)badTotal[i];
        mutated[header + 7] = (UAC_BYTE)(badTotal[i] >> 8);
        CHECK(Parse(mutated, size) == UAC_BAD_DESCRIPTOR);
    }
    for (i = 0; i < 3; i++) {
        memcpy(mutated, bytes, size);
        shortened = badFeatureLength[i]; removed = bytes[fu] - shortened;
        memmove(mutated + fu + shortened, mutated + fu + bytes[fu],
                size - fu - bytes[fu]);
        mutated[fu] = (UAC_BYTE)shortened;
        mutated[2] = (UAC_BYTE)(size - removed);
        mutated[3] = (UAC_BYTE)((size - removed) >> 8);
        mutated[header + 6] = (UAC_BYTE)(total - removed);
        mutated[header + 7] = (UAC_BYTE)((total - removed) >> 8);
        CHECK(Parse(mutated, size - removed) == UAC_BAD_DESCRIPTOR);
    }
    CHECK(Parse(bytes, size) == UAC_OK);
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned int n, i, off, len, playback = 0, capture = 0, spdif = 0;
    unsigned int control, count, filtered;
    UAC_BYTE source, selection[256], numbers[8], pipes[8];
    static const UAC_BYTE wholePipes[7] = { 1, 1, 2, 1, 0, 0, 0 };
    UAC_BYTE *guard;
    SYSTEM_INFO system;
    DWORD previous;
    if (argc != 3) { printf("usage: desctest config0.bin xpconf.bin\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    n = (unsigned int)fread(original, 1, sizeof(original), f);
    CHECK(!ferror(f));
    fclose(f);
    CHECK(n == 1143);
    CHECK(Parse(original, n) == UAC_OK);
    CHECK(UacFunction(original, n, 3, &control, &count) == UAC_OK);
    CHECK(control == 3 && count == 4);
    CHECK(UacFunction(original, n, 1, &control, &count) != UAC_OK);
    CHECK(UacFunction(original, n, 4, &control, &count) != UAC_OK);
    CHECK(parsed.AltCount == 10 && parsed.Skipped == 1);
    for (i = 0; i < parsed.AltCount; i++) {
        UAC_ALT *a = &parsed.Alt[i];
        CHECK(a->Subslot == 2 || a->Subslot == 3);
        CHECK(a->Bits == a->Subslot * 8);
        if (a->Interface == 4) {
            playback++;
            CHECK(a->DataEndpoint == 1 && a->FeedbackEndpoint == 0x83);
            CHECK(a->Interval == 1 && a->FeedbackInterval == 4);
        } else if (a->Interface == 5) {
            capture++;
            CHECK(a->DataEndpoint == 0x81 && !a->FeedbackEndpoint);
            CHECK((a->Attributes & 0x30) == 0x20);
            CHECK(a->Interval == 3);
        } else if (a->Interface == 6) {
            spdif++;
            CHECK(a->Alternate != 2);
            CHECK(a->DataEndpoint == 2 && a->FeedbackEndpoint == 0x85);
        } else CHECK(0);
    }
    CHECK(playback == 6 && capture == 2 && spdif == 2);
    CHECK(UacClock(&parsed, 3, NULL, &source) == UAC_OK && source == 33);
    CHECK(UacClock(&parsed, 6, NULL, &source) == UAC_OK && source == 37);
    CHECK(UacClock(&parsed, 17, NULL, &source) == UAC_OK && source == 35);
    CHECK(UacClock(&parsed, 255, NULL, &source) == UAC_CLOCK_INVALID);
    /* XP USBCCGP returns this function's interfaces without the IAD.
     * Build that layout from the original captured configuration. */
    for (off = 9; off < n; off += original[off])
        if (original[off + 1] == 4 && original[off + 2] == 3) break;
    CHECK(off < n);
    filtered = 9 + n - off;
    memcpy(mutated, original, 9);
    memcpy(mutated + 9, original + off, n - off);
    mutated[2] = (UAC_BYTE)filtered; mutated[3] = (UAC_BYTE)(filtered >> 8);
    mutated[4] = 4;
    CHECK(filtered == 1044);
    CHECK(UacFunction(mutated, filtered, 3, &control, &count) == UAC_OK);
    CHECK(control == 3 && count == 4);
    CHECK(Parse(mutated, filtered) == UAC_OK && parsed.AltCount == 10);
    CHECK(UacFunction(mutated, filtered, 4, &control, &count) != UAC_OK);
    f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 2; }
    CHECK(fread(captured, 1, sizeof(captured), f) == filtered);
    CHECK(!ferror(f)); fclose(f);
    CHECK(memcmp(captured, mutated, filtered) == 0);
    CHECK(UacFunction(captured, filtered, 3, &control, &count) == UAC_OK);
    CHECK(UacParse(captured, filtered, control, 3, count, &parsed) == UAC_OK);
    /* Whole device (98 SE under NUSB: no split, no MI number). */
    CHECK(UacWholeFirst(original, n, &control) == UAC_OK && control == 3);
    CHECK(UacWholeFirst(captured, filtered, &control) == UAC_OK && control == 3);
    CHECK(UacAltZero(original, n, numbers, pipes, 8, &count) == UAC_OK && count == 7);
    for (i = 0; i < 7; i++) CHECK(numbers[i] == i && pipes[i] == wholePipes[i]);
    CHECK(UacAltZero(original, n, numbers, pipes, 6, &count) != UAC_OK);
    CHECK(UacAltZero(captured, filtered, numbers, pipes, 8, &count) == UAC_OK && count == 4);
    for (i = 0; i < 4; i++) CHECK(numbers[i] == 3 + i && pipes[i] == wholePipes[3 + i]);
    memcpy(mutated, original, n);
    mutated[4] = 6; /* bNumInterfaces disagrees with the interfaces */
    CHECK(UacAltZero(mutated, n, numbers, pipes, 8, &count) != UAC_OK);
    for (off = 9; off < n; off += original[off])
        if (original[off + 1] == 4 && original[off + 2] == 5 && original[off + 3] == 1) break;
    CHECK(off < n);
    memcpy(mutated, original, n);
    mutated[off + 3] = 0; /* a second alternate 0 of interface 5 */
    CHECK(UacAltZero(mutated, n, numbers, pipes, 8, &count) != UAC_OK);
    for (off = 9; off < n; off += original[off])
        if (original[off + 1] == 11 && original[off + 4] == 1) break;
    CHECK(off < n);
    memcpy(mutated, original, n);
    mutated[off + 4] = 0xfe; /* no audio IAD: the AC interface anchors */
    CHECK(UacWholeFirst(mutated, n, &control) == UAC_OK && control == 3);
    for (off = 9; off < n; off += original[off])
        if (original[off + 1] == 4 && original[off + 2] == 3 && !original[off + 3]) break;
    CHECK(off < n);
    mutated[off + 6] = 2; /* nor an AudioControl interface */
    CHECK(UacWholeFirst(mutated, n, &control) == UAC_NO_AUDIO);
    CheckControlStructure(original, n);
    CheckControlStructure(captured, filtered);
    /* Planning over the X4 (uacplan.c). */
    {
        UAC_FORMAT formats[UAC_PLAN_FORMATS];
        UAC_CLOCK_PATH path;
        unsigned int index, k, t3 = 0, t17 = 0;
        CHECK(Parse(original, n) == UAC_OK);
        CHECK(UacPlanTerminal(&parsed, 0, 1) == 3 && UacPlanTerminal(&parsed, 1, 1) == 6);
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 4 && parsed.Alt[index].Alternate == 1);
        CHECK(UacPlanPick(&parsed, 0, 3, 6, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 4 && parsed.Alt[index].Alternate == 2);
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 4, 24, UAC_PLAN_ANY_CONFIG, 96000, 1, &index) == UAC_NO_AUDIO); /* subslot must match */
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 3, 24, UAC_PLAN_ANY_CONFIG, 96000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 4 && parsed.Alt[index].Alternate == 4);
        /* Any terminal: the smallest endpoint, the first of equals (IF4,
         * not the S/PDIF IF6 alt 1; IF6 alt 2 is the skipped non-PCM one). */
        CHECK(UacPlanPick(&parsed, 0, 0, 2, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 4 && parsed.Alt[index].Alternate == 1);
        CHECK(UacPlanPick(&parsed, 0, 17, 2, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 6 && parsed.Alt[index].Alternate == 1);
        /* 112 bytes cannot carry 192 kHz 8-channel... nor stereo 24-bit at
         * 384 kHz (49 frames * 6 = 294 > 168). */
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 3, 24, UAC_PLAN_ANY_CONFIG, 384000, 1, &index) == UAC_NO_AUDIO);
        CHECK(UacPlanPick(&parsed, 1, 6, 2, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_OK);
        CHECK(parsed.Alt[index].Interface == 5 && parsed.Alt[index].Alternate == 1);
        CHECK(UacPlanPick(&parsed, 1, 6, 8, 2, 16, UAC_PLAN_ANY_CONFIG, 48000, 1, &index) == UAC_NO_AUDIO);
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 2, 16, UAC_PLAN_ANY_CONFIG, 8000000, 1, &index) == UAC_NO_AUDIO);
        /* Full Speed: capture's bInterval 3 is not a Full Speed interval. */
        CHECK(UacPlanTerminal(&parsed, 1, 0) == 0);
        CHECK(UacPlanFormats(&parsed, 0, 1, formats, UAC_PLAN_FORMATS) == 8);
        for (k = 0; k < 8; k++) { if (formats[k].Terminal == 3) t3++; if (formats[k].Terminal == 17) t17++; }
        CHECK(t3 == 6 && t17 == 2);
        CHECK(formats[0].Channels == 2 && formats[0].Subslot == 2 && formats[0].Bits == 16);
        CHECK(UacPlanFormats(&parsed, 1, 1, formats, UAC_PLAN_FORMATS) == 2);
        CHECK(UacPlanFormats(&parsed, 0, 1, formats, 2) == 2);
        CHECK(UacPlanPacketBytes(48000, 1, 1, 4, 1) == 28);
        CHECK(UacPlanPacketBytes(44100, 1, 1, 4, 1) == 28);
        CHECK(UacPlanPacketBytes(48000, 0, 1, 4, 1) == 196);
        CHECK(UacPlanPacketBytes(48000, 0, 1, 4, 0) == 192); /* synchronous: exact */
        CHECK(UacPlanPacketBytes(192000, 1, 1, 32, 1) == 800);
        CHECK(UacPlanPacketBytes(48000, 1, 4, 4, 1) == 196);
        CHECK(UacPlanPacketBytes(48000, 0, 2, 4, 1) == 0);
        CHECK(UacPlanPacketBytes(7999, 1, 1, 4, 1) == 0);
        CHECK(UacPlanCapacity(0x1400) == 3072 && UacPlanCapacity(112) == 112);
        CHECK(UacPlanChannelMask(0x3, 2) == 0x3 && UacPlanChannelMask(0x63f, 8) == 0x63f);
        CHECK(UacPlanChannelMask(0x3f, 8) == 0x3f && UacPlanChannelMask(0x40000 | 3, 3) == 3);
        CHECK(UacPlanChannelMask(0x4, 1) == 0x4 && UacPlanChannelMask(0x7, 2) == 0);
        CHECK(UacPlanChannelMask(0x80000003U, 2) == 0 && UacPlanChannelMask(0, 2) == 0);
        /* An alternate of another layout is not picked. */
        CHECK(UacPlanPick(&parsed, 0, 3, 2, 2, 16, parsed.Alt[0].ChannelConfig ^ 0x30, 48000, 1, &index) == UAC_NO_AUDIO);
        CHECK(UacClockPath(&parsed, 3, NULL, &path) == UAC_OK && path.Source == 33);
        CHECK(path.Count == 1 && path.Kind[0] == 11 && path.Pin[0] == 1);
    }
    /* Every truncation must fail without crossing the supplied boundary. */
    GetSystemInfo(&system);
    CHECK(n < system.dwPageSize);
    guard = (UAC_BYTE *)VirtualAlloc(NULL, 2 * system.dwPageSize,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(guard != NULL);
    CHECK(VirtualProtect(guard + system.dwPageSize, system.dwPageSize,
                         PAGE_NOACCESS, &previous) != 0);
    for (i = 0; i < n; i++) {
        UAC_BYTE *input = guard + system.dwPageSize - i;
        memcpy(input, original, i);
        CHECK(Parse(input, i) != UAC_OK);
        CHECK(UacFunction(input, i, 3, &control, &count) != UAC_OK);
        CHECK(UacWholeFirst(input, i, &control) != UAC_OK);
        CHECK(UacAltZero(input, i, numbers, pipes, 8, &count) != UAC_OK);
    }
    CHECK(VirtualFree(guard, 0, MEM_RELEASE) != 0);
    for (off = 9; off < n; off += len) {
        len = original[off];
        memcpy(mutated, original, n);
        mutated[off] = 0;
        CHECK(Parse(mutated, n) != UAC_OK);
        mutated[off] = 1;
        CHECK(Parse(mutated, n) != UAC_OK);
    }
    CHECK(UacParse(NULL, n, 3, 3, 4, &parsed) != UAC_OK);
    CHECK(UacParse(original, n, 3, 0, 7, &parsed) != UAC_OK);
    CHECK(UacParse(original, n, 3, 3, 0, &parsed) != UAC_OK);
    CHECK(UacParse(original, n, 3, 3, 256, &parsed) != UAC_OK);
    CHECK(Parse(original, n) == UAC_OK);
    off = parsed.Entity[34];
    memcpy(mutated, original, n);
    mutated[off + 5] = 34; /* selector points to itself */
    CHECK(Parse(mutated, n) == UAC_OK);
    CHECK(UacClock(&parsed, 3, NULL, &source) == UAC_CLOCK_INVALID);
    memcpy(mutated, original, n);
    mutated[off + 5] = 255;
    CHECK(Parse(mutated, n) == UAC_OK);
    CHECK(UacClock(&parsed, 3, NULL, &source) == UAC_CLOCK_INVALID);
    /* Turn the 1-input selector into a 2-input descriptor using its iClock
     * byte and a following byte, keeping descriptor framing intact. */
    memcpy(mutated, original, n);
    mutated[off + 4] = 2; /* descriptor too short: fail before indexing */
    CHECK(Parse(mutated, n) != UAC_OK);
    memset(selection, 0, sizeof(selection));
    CHECK(Parse(original, n) == UAC_OK);
    CHECK(UacClock(&parsed, 3, selection, &source) == UAC_OK && source == 33);
    /* Single-byte mutations exercise the same pure C parser as the driver.
     * Safety smoke, not an assertion that every mutation is invalid. */
    for (i = 0; i < n; i++) {
        memcpy(mutated, original, n);
        mutated[i] ^= 0xff;
        if (Parse(mutated, n) == UAC_OK) {
            unsigned int j;
            CHECK(parsed.AltCount <= UAC_MAX_ALTS);
            for (j = 0; j < parsed.AltCount; j++)
                (void)UacClock(&parsed, parsed.Alt[j].Terminal, NULL, &source);
        }
    }
    printf("PASS %u assertions, %u byte mutations; X4 PCM/feedback/clocks\n", checks, n);
    return 0;
}
