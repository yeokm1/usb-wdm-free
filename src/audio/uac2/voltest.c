/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of uacvol.c: the Feature Unit walk (the recorded X4 layout and
 * built graphs), the RANGE answer, the dB conversion both ways, the
 * software gain and sample scaling against independent floating-point
 * oracles, the plan, and the control request setup bytes (uacctl.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "uacvol.h"

static unsigned int checks;
static void Check(int ok, const char *expression, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL line %u: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

static UAC_DESC desc;
static UAC_BYTE file[65536], bytes[4096];
static unsigned int used;

/* A built AudioControl: entities appended to bytes[], indexed by id. */
static void Reset(void)
{
    memset(&desc, 0, sizeof(desc));
    memset(bytes, 0, sizeof(bytes));
    used = 9;  /* a configuration header's room; offsets are never 0 */
    desc.Bytes = bytes;
}
static void Add(const UAC_BYTE *d)
{
    desc.Entity[d[3]] = (UAC_WORD)used;
    memcpy(bytes + used, d, d[0]);
    used += d[0];
    desc.Length = used;
}
static void It(UAC_BYTE id, unsigned int type)
{
    UAC_BYTE d[17] = { 17, 0x24, 0x02, 0, 0, 0, 0, 0x10, 2 };
    d[3] = id; d[4] = (UAC_BYTE)type; d[5] = (UAC_BYTE)(type >> 8);
    Add(d);
}
static void Ot(UAC_BYTE id, unsigned int type, UAC_BYTE source)
{
    UAC_BYTE d[12] = { 12, 0x24, 0x03, 0, 0, 0, 0, 0, 0x10 };
    d[3] = id; d[4] = (UAC_BYTE)type; d[5] = (UAC_BYTE)(type >> 8); d[7] = source;
    Add(d);
}
/* controls[0] master, controls[1..channels] the logical channels. */
static void Fu(UAC_BYTE id, UAC_BYTE source, unsigned int channels, const unsigned long *controls)
{
    UAC_BYTE d[64];
    unsigned int i;
    memset(d, 0, sizeof(d));
    d[0] = (UAC_BYTE)(6 + 4 * (channels + 1)); d[1] = 0x24; d[2] = 0x06; d[3] = id; d[4] = source;
    for (i = 0; i <= channels; i++) {
        d[5 + 4 * i] = (UAC_BYTE)controls[i]; d[6 + 4 * i] = (UAC_BYTE)(controls[i] >> 8);
    }
    Add(d);
}
static void Mixer(UAC_BYTE id, UAC_BYTE a, UAC_BYTE b)
{
    UAC_BYTE d[13] = { 13, 0x24, 0x04, 0, 2, 0, 0, 2 };
    d[3] = id; d[5] = a; d[6] = b;
    Add(d);
}

static void Map(unsigned int terminal, unsigned int channels, int found, UAC_BYTE out,
                UAC_BYTE vu, unsigned int vm, unsigned int vc,
                UAC_BYTE mu, unsigned int mm, unsigned int mc, unsigned int line)
{
    UAC_VOL_MAP m;
    int r = UacVolFind(&desc, terminal, channels, &m);
    Check(r == found && m.Output == out && m.VolumeUnit == vu && m.VolumeMode == vm &&
          m.VolumeChannels == vc && m.MuteUnit == mu && m.MuteMode == mm && m.MuteChannels == mc,
          "volume map", line);
}

static void Walks(const char *x4)
{
    static const unsigned long both[3] = { 0x0f, 0x0f, 0x0f };      /* mute+volume rw */
    static const unsigned long chans[3] = { 0, 0x0f, 0x0f };
    static const unsigned long readOnly[3] = { 0x05, 0x05, 0x05 };  /* mute+volume read */
    static const unsigned long muteOnly[3] = { 0x03, 0, 0 };
    static const unsigned long volOnly[3] = { 0x0c, 0, 0 };
    static const unsigned long oneOfTwo[3] = { 0, 0x0f, 0x05 };
    static const unsigned long reserved[3] = { 0x0a, 0, 0 };        /* lanes 10b */
    UAC_VOL_MAP m;
    FILE *f;
    unsigned int n;

    /* The recorded X4: IT 3 -> mixer 7 -> FU 1 -> OT 5 (speaker); FU 1
     * has master mute and volume read/write. */
    f = fopen(x4, "rb");
    CHECK(f != NULL);
    n = (unsigned int)fread(file, 1, sizeof(file), f);
    fclose(f);
    CHECK(UacParse(file, n, 3, 3, 4, &desc) == UAC_OK);
    Map(3, 2, 1, 5, 1, UAC_VOL_MASTER, 0, 1, UAC_VOL_MASTER, 0, __LINE__);
    Map(3, 8, 1, 5, 1, UAC_VOL_MASTER, 0, 1, UAC_VOL_MASTER, 0, __LINE__);
    /* FU 1's eight channels have volume read/write too: the driver sets
     * them to 0 dB once it owns the master. No channel mute. */
    CHECK(UacVolFind(&desc, 3, 2, &m) && m.VolumeExtra == 0xff && m.MuteExtra == 0);
    /* IT 17 (the X4's second USB streaming input) -> FU 14 -> OT 20. */
    Map(17, 2, 1, 20, 14, UAC_VOL_MASTER, 0, 14, UAC_VOL_MASTER, 0, __LINE__);
    /* IT 4 (an analog input) reaches OT 5 through FU 13 and mixer 7. */
    Map(4, 2, 1, 5, 1, UAC_VOL_MASTER, 0, 1, UAC_VOL_MASTER, 0, __LINE__);
    Map(99, 2, 0, 0, 0, 0, 0, 0, 0, 0, __LINE__);         /* absent */

    Reset(); It(1, 0x0101); Fu(2, 1, 2, both); Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 2, UAC_VOL_MASTER, 0, 2, UAC_VOL_MASTER, 0, __LINE__);
    CHECK(UacVolFind(&desc, 1, 2, &m) && m.VolumeExtra == 3 && m.MuteExtra == 3);
    Map(2, 2, 0, 0, 0, 0, 0, 0, 0, 0, __LINE__);          /* a unit is not a terminal */
    Reset(); It(1, 0x0101); Fu(2, 1, 2, chans); Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 2, UAC_VOL_CHANNEL, 2, 2, UAC_VOL_CHANNEL, 2, __LINE__);
    CHECK(UacVolFind(&desc, 1, 2, &m) && !m.VolumeExtra && !m.MuteExtra);
    Map(1, 1, 1, 3, 2, UAC_VOL_CHANNEL, 2, 2, UAC_VOL_CHANNEL, 2, __LINE__);
    Map(1, 3, 1, 3, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__); /* unit too narrow */
    Reset(); It(1, 0x0101); Fu(2, 1, 2, oneOfTwo); Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__);
    Reset(); It(1, 0x0101); Fu(2, 1, 2, readOnly); Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__);
    Reset(); It(1, 0x0101); Fu(2, 1, 0, reserved); Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__);
    /* No unit at all: a path, software volume and mute. */
    Reset(); It(1, 0x0101); Ot(3, 0x0301, 1);
    Map(1, 2, 1, 3, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__);
    /* Only a USB streaming output (a capture path): no path. */
    Reset(); It(1, 0x0201); Fu(2, 1, 2, both); Ot(3, 0x0101, 2);
    Map(1, 2, 0, 0, 0, 0, 0, 0, 0, 0, __LINE__);
    /* Two units: mute nearest the output, volume behind it. */
    Reset(); It(1, 0x0101); Fu(2, 1, 2, volOnly); Fu(4, 2, 2, muteOnly); Ot(3, 0x0301, 4);
    Map(1, 2, 1, 3, 2, UAC_VOL_MASTER, 0, 4, UAC_VOL_MASTER, 0, __LINE__);
    /* Master on the far unit wins over channels on the near one. */
    Reset(); It(1, 0x0101); Fu(2, 1, 2, both); Fu(4, 2, 2, chans); Ot(3, 0x0301, 4);
    Map(1, 2, 1, 3, 2, UAC_VOL_MASTER, 0, 2, UAC_VOL_MASTER, 0, __LINE__);
    /* A mixer: the terminal behind its second input; a unit on the other
     * input is not on the path. */
    Reset(); It(1, 0x0101); It(5, 0x0201); Fu(6, 5, 2, both); Mixer(7, 6, 1); Fu(2, 7, 2, chans);
    Ot(3, 0x0301, 2);
    Map(1, 2, 1, 3, 2, UAC_VOL_CHANNEL, 2, 2, UAC_VOL_CHANNEL, 2, __LINE__);
    Map(5, 2, 1, 3, 6, UAC_VOL_MASTER, 0, 6, UAC_VOL_MASTER, 0, __LINE__);
    /* A loop between units ends; the second output's path is taken. */
    Reset(); It(1, 0x0101); Fu(5, 6, 2, both); Fu(6, 5, 2, both); Ot(3, 0x0301, 5);
    Ot(4, 0x0302, 1);
    Map(1, 2, 1, 4, 0, UAC_VOL_NONE, 0, 0, UAC_VOL_NONE, 0, __LINE__);
    /* A unit whose descriptor runs past the configuration is not read. */
    Reset(); It(1, 0x0101); Fu(2, 1, 2, both); Ot(3, 0x0301, 2);
    desc.Length = desc.Entity[3] + 11;
    Map(1, 2, 0, 0, 0, 0, 0, 0, 0, 0, __LINE__);
    Reset(); It(1, 0x0101); Fu(2, 1, 2, both); Ot(3, 0x0301, 2);
    desc.Length = desc.Entity[3] - 1;  /* FU 2 ends one byte short */
    Map(1, 2, 0, 0, 0, 0, 0, 0, 0, 0, __LINE__);
    CHECK(!UacVolFind(NULL, 1, 2, &m) && m.Channels == 2 && !m.VolumeUnit);
    CHECK(!UacVolFind(&desc, 1, 2, NULL));
    CHECK(!UacVolFind(&desc, 0, 0, &m) && m.Channels == 1);
    CHECK(!UacVolFind(&desc, 0, 99, &m) && m.Channels == UAC_VOL_CHANNELS);
}

static void Put16(unsigned char *p, int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static unsigned int Answer(unsigned char *b, unsigned int count, const int *triples)
{
    unsigned int i;
    Put16(b, (int)count);
    for (i = 0; i < 3 * count; i++) Put16(b + 2 + 2 * i, triples[i]);
    return 2 + 6 * count;
}

/* Nearest allowed value by brute force over every 16-bit value. */
static int Nearest(long level, const UAC_VOL_RANGE *r)
{
    long v;
    int best = 0, have = 0;
    double bestD = 0;
    if (level < r->KsMin) level = r->KsMin;
    if (level > r->KsMax) level = r->KsMax;
    for (v = -32767; v <= 32767; v++) {
        unsigned int i;
        int ok = 0;
        for (i = 0; i < r->Count; i++)
            if (v >= r->Min[i] && v <= r->Max[i] &&
                (r->Res[i] ? (v - r->Min[i]) % r->Res[i] == 0 : v == r->Min[i])) ok = 1;
        if (ok) {
            double d = fabs(v * 256.0 - level);
            if (!have || d < bestD) { best = (int)v; bestD = d; have = 1; }
        }
    }
    return best;
}

static void Ranges(void)
{
    unsigned char b[64];
    UAC_VOL_RANGE r, keep;
    static const int x[3] = { -0x6000, 0, 0x100 };
    static const int silence[3] = { -32768, 0, 0x80 };
    static const int two[6] = { -0x4000, -0x2000, 0x100, -0x1000, 0x600, 0x80 };
    static const int fixed[3] = { -0x0300, -0x0300, 0 };
    static const int zeroRes[3] = { -0x100, 0, 0 };
    static const int big[3] = { -32767, 32767, 1 };
    long level;
    int v;
    unsigned int n, i;

    n = Answer(b, 1, x);
    CHECK(UacVolRange(b, n, &r) && r.Count == 1 && r.KsMin == -96L * 65536L && r.KsMax == 0 &&
          r.KsStep == 65536L);
    CHECK(UacVolFromKs(-10L * 65536L, &r) == -0x0a00);
    CHECK(UacVolFromKs(-10L * 65536L - 32767L, &r) == -0x0a00);   /* -10.49998 dB: -10 */
    CHECK(UacVolFromKs(-10L * 65536L - 32768L, &r) == -0x0b00);   /* -10.5 dB, a tie: the lower */
    CHECK(UacVolFromKs(-10L * 65536L - 32769L, &r) == -0x0b00);
    CHECK(UacVolFromKs(100L * 65536L, &r) == 0 && UacVolFromKs(-200L * 65536L, &r) == -0x6000);
    n = Answer(b, 1, silence);
    CHECK(UacVolRange(b, n, &r) && r.Min[0] == -32767 && r.KsMin == -32767L * 256L &&
          r.KsStep == 0x80L * 256L);
    n = Answer(b, 2, two);
    CHECK(UacVolRange(b, n, &r) && r.Count == 2 && r.KsMin == -0x4000L * 256L &&
          r.KsMax == 0x600L * 256L && r.KsStep == 0x80L * 256L);
    CHECK(UacVolFromKs(-0x1900L * 256L, &r) == -0x2000);  /* in the gap, nearer the first */
    CHECK(UacVolFromKs(-0x1700L * 256L, &r) == -0x1000);  /* nearer the second */
    CHECK(UacVolFromKs(-0x1800L * 256L, &r) == -0x2000);  /* gap middle: the lower */
    /* (MAX - MIN) not a whole number of steps: 10 is not reachable. */
    {
        static const int odd[3] = { 0, 10, 6 };
        n = Answer(b, 1, odd);
        CHECK(UacVolRange(b, n, &r) && r.KsMax == 6 * 256L);
        CHECK(UacVolFromKs(10 * 256L, &r) == 6 && UacVolFromKs(100 * 256L, &r) == 6);
        CHECK(UacVolFromKs(2 * 256L, &r) == 0 && UacVolFromKs(4 * 256L, &r) == 6);
        for (level = -256L; level <= 12 * 256L; level += 17L)
            CHECK(UacVolFromKs(level, &r) == Nearest(level, &r));
    }
    n = Answer(b, 1, fixed);
    CHECK(UacVolRange(b, n, &r) && r.KsMin == r.KsMax && r.KsStep == 256L);
    CHECK(UacVolFromKs(0, &r) == -0x300);
    /* RES 0 on a wider range: 1, as Linux reads it. */
    n = Answer(b, 1, zeroRes);
    CHECK(UacVolRange(b, n, &r) && r.Res[0] == 1 && r.KsStep == 256L);
    CHECK(UacVolFromKs(-65536L - 100L, &r) == -256);
    keep = r;
    n = Answer(b, 1, x);
    b[3] = 0x7f;                                           /* MIN above MAX */
    CHECK(!UacVolRange(b, n, &r) && !memcmp(&keep, &r, sizeof(r)));
    n = Answer(b, 1, x);
    CHECK(!UacVolRange(b, n - 1, &r) && !UacVolRange(NULL, n, &r) && !UacVolRange(b, n, NULL));
    CHECK(!UacVolRange(b, 1, &r) && !UacVolRange(b, 2 + 6 * UAC_VOL_RANGES + 6, &r));
    Put16(b, 9);
    for (i = 0; i < 9; i++) { Put16(b + 2 + 6 * i, 0x100 * (int)i); Put16(b + 4 + 6 * i, 0x100 * (int)i + 0x40); Put16(b + 6 + 6 * i, 1); }
    CHECK(!UacVolRange(b, 2 + 54, &r));                    /* more than UAC_VOL_RANGES */
    /* Every value, both ways, on a full range at 1/256 dB. */
    n = Answer(b, 1, big);
    CHECK(UacVolRange(b, n, &r));
    for (v = -32767; v <= 32767; v++) {
        CHECK(UacVolToKs(v) == (long)v * 256L);
        CHECK(UacVolFromKs(UacVolToKs(v), &r) == v);
    }
    CHECK(UacVolToKs(-32768) == -32767L * 256L);
    /* Against brute force on stepped and split ranges. */
    n = Answer(b, 2, two);
    CHECK(UacVolRange(b, n, &r));
    for (level = -0x4100L * 256L; level <= 0x700L * 256L; level += 997L)
        CHECK(UacVolFromKs(level, &r) == Nearest(level, &r));
    n = Answer(b, 1, x);
    CHECK(UacVolRange(b, n, &r));
    for (level = -0x6100L * 256L; level <= 0x100L * 256L; level += 4093L)
        CHECK(UacVolFromKs(level, &r) == Nearest(level, &r));
    UacVolSoftRange(&r);
    CHECK(r.KsMin == UAC_VOL_SW_MIN && r.KsMax == 0 && r.KsStep == UAC_VOL_SW_STEP);
    CHECK(UacVolFromKs(-3L * 65536L - 1, &r) == -3 * 256);
    CHECK(UacVolFromKs(-3L * 65536L - 16384L, &r) == -3 * 256 - 128);
    for (level = -97L * 65536L; level <= 65536L; level += 2111L)
        CHECK(UacVolFromKs(level, &r) == Nearest(level, &r));
}

static void Gains(void)
{
    long level;
    unsigned long previous = UAC_VOL_UNITY + 1;
    CHECK(UacVolGain(0) == UAC_VOL_UNITY && UacVolGain(65536L) == UAC_VOL_UNITY);
    CHECK(UacVolGain(-96L * 65536L) == 0 && UacVolGain(-200L * 65536L) == 0);
    CHECK(UacVolGain(-6L * 65536L) == 32846UL && UacVolGain(-20L * 65536L) == 6554UL);
    for (level = 0; level > -96L * 65536L; level -= 331L) {
        unsigned long g = UacVolGain(level);
        double exact = 65536.0 * pow(10.0, level / 65536.0 / 20.0);
        CHECK(g <= previous);
        previous = g;
        /* Linear between whole dB: at most 0.1 dB high, plus rounding. */
        CHECK(g + 1.0 >= exact * 0.999 && g <= exact * 1.0116 + 1.0);
    }
}

static void Plans(void)
{
    UAC_VOL_MAP m;
    UAC_VOL_RANGE r[UAC_VOL_SLOTS];
    unsigned char b[8];
    static const int x[3] = { -0x6000, 0, 0x100 };
    static const int y[3] = { -0x3000, 0x600, 0x80 };
    long levels[UAC_VOL_CHANNELS];
    int hw[UAC_VOL_SLOTS], actual[UAC_VOL_SLOTS];
    unsigned long g[UAC_VOL_CHANNELS];
    unsigned int i;
    for (i = 0; i < UAC_VOL_SLOTS; i++) CHECK(UacVolRange(b, Answer(b, 1, x), &r[i]));
    CHECK(UacVolRange(b, Answer(b, 1, y), &r[2]));     /* channel 2 has its own range */
    memset(&m, 0, sizeof(m));
    m.Channels = 2;
    for (i = 0; i < UAC_VOL_SLOTS; i++) actual[i] = 0;
    levels[0] = -10L * 65536L; levels[1] = -20L * 65536L;
    /* Master: the loudest channel in hardware. */
    m.VolumeMode = UAC_VOL_MASTER; m.MuteMode = UAC_VOL_MASTER;
    UacVolTargets(levels, 2, &m, r, hw);
    CHECK(hw[0] == -0x0a00 && !hw[1] && !hw[2]);
    /* Gains against what the device does now. Still at 0 dB (before the
     * write): both channels at their full levels in software, never
     * louder than asked. Once written: the other channel's difference. */
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UacVolGain(-10L * 65536L) && g[1] == UacVolGain(-20L * 65536L));
    actual[0] = hw[0];
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UAC_VOL_UNITY && g[1] == UacVolGain(-10L * 65536L));
    /* The device quieter than asked (a write still to come, or one that
     * failed): software does not raise it. */
    actual[0] = -0x1400;
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UAC_VOL_UNITY && g[1] == UAC_VOL_UNITY);
    actual[0] = UAC_VOL_SILENCE;                       /* silent: nothing to add */
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UAC_VOL_UNITY && g[1] == UAC_VOL_UNITY);
    /* Mute: zero until the device's mute is confirmed on. */
    actual[0] = -0x0a00;
    UacVolGains(levels, 2, 1, &m, actual, 0, g);
    CHECK(g[0] == 0 && g[1] == 0);
    UacVolGains(levels, 2, 1, &m, actual, 1, g);
    CHECK(g[0] == UAC_VOL_UNITY && g[1] == UacVolGain(-10L * 65536L));
    /* Below the device's minimum: the rest in software. */
    levels[0] = levels[1] = -100L * 65536L;
    UacVolTargets(levels, 2, &m, r, hw);
    CHECK(hw[0] == -0x6000);
    actual[0] = hw[0];
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UacVolGain(-4L * 65536L) && g[1] == g[0]);
    /* Between steps: the device rounds to 1 dB; above the level, software
     * takes the rest; below it, nothing is added. */
    levels[0] = -10L * 65536L - 40000L; levels[1] = -10L * 65536L - 20000L;
    UacVolTargets(levels, 2, &m, r, hw);
    actual[0] = hw[0];
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(hw[0] == -0x0a00 && g[1] == UacVolGain(-20000L) && g[0] == UacVolGain(-40000L));
    levels[0] = levels[1] = -10L * 65536L - 50000L;
    UacVolTargets(levels, 2, &m, r, hw);
    actual[0] = hw[0];
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(hw[0] == -0x0b00 && g[0] == UAC_VOL_UNITY && g[1] == UAC_VOL_UNITY);
    /* Channel mode over four logical channels: node channel (n-1)%2, each
     * from its own range (channel 2: 0.5 dB steps from -48 dB). */
    m.VolumeMode = UAC_VOL_CHANNEL; m.VolumeChannels = 4; m.MuteMode = UAC_VOL_CHANNEL;
    levels[0] = -3L * 65536L; levels[1] = -60L * 65536L - 20000L;
    UacVolTargets(levels, 2, &m, r, hw);
    CHECK(!hw[0] && hw[1] == -0x300 && hw[2] == -0x3000 && hw[3] == -0x300 && hw[4] == -0x3c00 && !hw[5]);
    for (i = 0; i < UAC_VOL_SLOTS; i++) actual[i] = hw[i];
    UacVolGains(levels, 2, 0, &m, actual, 0, g);
    CHECK(g[0] == UAC_VOL_UNITY && g[1] == UacVolGain(-12L * 65536L - 20000L));
    /* Software only. */
    m.VolumeMode = UAC_VOL_NONE; m.MuteMode = UAC_VOL_NONE;
    UacVolSoftRange(&r[0]);
    levels[1] = -7L * 65536L;
    UacVolTargets(levels, 2, &m, r, hw);
    for (i = 0; i < UAC_VOL_SLOTS; i++) CHECK(!hw[i]);
    UacVolGains(levels, 2, 0, &m, hw, 0, g);
    CHECK(g[0] == UacVolGain(-3L * 65536L) && g[1] == UacVolGain(-7L * 65536L));
    UacVolGains(levels, 2, 1, &m, hw, 0, g);
    CHECK(!g[0] && !g[1]);
    UacVolGains(levels, 2, 1, &m, hw, 1, g);            /* no device mute: hwMuted means nothing */
    CHECK(!g[0] && !g[1]);
}

static unsigned long seed = 12345;
static unsigned long Next(void) { seed = seed * 1664525UL + 1013904223UL; return seed >> 8; }

static void Scales(void)
{
    unsigned char b[4 * 3 * 64], ref[sizeof(b)];
    unsigned long g[3];
    unsigned int subslot, i, trial;
    for (subslot = 2; subslot <= 4; subslot++)
        for (trial = 0; trial < 2000; trial++) {
            unsigned int channels = 1 + trial % 3, frames = 1 + trial % 17, count;
            for (i = 0; i < 3; i++) {
                unsigned long r = Next() % 4;
                g[i] = r == 0 ? UAC_VOL_UNITY : r == 1 ? 0 : Next() % UAC_VOL_UNITY;
            }
            count = frames * channels * subslot + (trial % 5 == 0 ? subslot + 1 : 0);
            for (i = 0; i < count; i++) b[i] = (unsigned char)Next();
            if (trial < 3) for (i = 0; i < count; i++) b[i] = (unsigned char)(i % subslot == subslot - 1 ? 0x80 : 0);
            memcpy(ref, b, count);
            UacVolScale(b, count, subslot, channels, g, 3);
            for (i = 0; i + subslot <= count; i += subslot) {
                unsigned int c = (i / subslot) % channels, k;
                double s = 0, scaled;
                long want;
                for (k = 0; k < subslot; k++) s += ref[i + k] * pow(256.0, k);
                if (ref[i + subslot - 1] & 0x80) s -= pow(256.0, subslot);
                scaled = g[c] >= UAC_VOL_UNITY ? s : floor(s * g[c] / 65536.0);
                want = (long)scaled;
                for (k = 0; k < subslot; k++)
                    CHECK(b[i + k] == (unsigned char)((unsigned long)want >> (8 * k)));
            }
            for (; i < count; i++) CHECK(b[i] == ref[i]);
        }
    /* Bad arguments change nothing. */
    memset(b, 0x55, 16); memset(g, 0, sizeof(g));
    UacVolScale(b, 16, 1, 2, g, 3); UacVolScale(b, 16, 5, 2, g, 3);
    UacVolScale(b, 16, 2, 0, g, 3); UacVolScale(b, 16, 2, 2, g, 0); UacVolScale(b, 16, 2, 2, NULL, 3);
    for (i = 0; i < 16; i++) CHECK(b[i] == 0x55);
    /* More stream channels than gains: channel c takes gain c % gains. */
    for (i = 0; i < 12; i++) { b[2 * i] = 0; b[2 * i + 1] = 0x40; }
    g[0] = 0; g[1] = UAC_VOL_UNITY;
    UacVolScale(b, 24, 2, 6, g, 2);
    for (i = 0; i < 12; i++) CHECK(b[2 * i + 1] == (i % 2 ? 0x40 : 0));
}

/* The requests the driver sends, through uacctl.c (UAC2 5.2.2, 5.2.5.7). */
static void Setups(void)
{
    UAC_CTL_TARGET t;
    unsigned char s[8];
    t.Interface = 3; t.Entity = 1; t.Endpoint = 0; t.Channel = 0; t.MaxChannel = 8;
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_CUR, 0, 2, 0, s));
    CHECK(!memcmp(s, "\x21\x01\x00\x02\x03\x01\x02\x00", 8));       /* SET_CUR volume, master */
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_CUR, 1, 2, 0, s));
    CHECK(!memcmp(s, "\xa1\x01\x00\x02\x03\x01\x02\x00", 8));       /* GET_CUR */
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_RANGE_ATTRIBUTE, 1, 2, 0, s));
    CHECK(!memcmp(s, "\xa1\x02\x00\x02\x03\x01\x02\x00", 8));       /* GET_RANGE count */
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_RANGE_ATTRIBUTE, 1, 50, 0, s));
    CHECK(s[6] == 50 && s[7] == 0);
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 1, &t, 3, UAC_CTL_CUR, 0, 1, 0, s));
    CHECK(!memcmp(s, "\x21\x01\x00\x01\x03\x01\x01\x00", 8));       /* SET_CUR mute */
    t.Channel = 2;
    CHECK(UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_CUR, 0, 2, 0, s));
    CHECK(!memcmp(s, "\x21\x01\x02\x02\x03\x01\x02\x00", 8));       /* channel 2 */
    CHECK(!UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 1, UAC_CTL_CUR, 0, 2, 0, s)); /* read only: no SET */
    CHECK(!UacCtlSetup(UAC_CTL_FEATURE, 1, &t, 3, UAC_CTL_RANGE_ATTRIBUTE, 1, 2, 0, s)); /* mute has no RANGE */
    CHECK(!UacCtlSetup(UAC_CTL_FEATURE, 2, &t, 3, UAC_CTL_CUR, 0, 1, 0, s)); /* volume CUR is 2 bytes */
    {
        unsigned char v[2];
        Put16(v, -32768);
        CHECK(UacCtlValue(UAC_CTL_FEATURE, 2, v, 2, 0));             /* silence CUR is valid */
        v[0] = 2; CHECK(!UacCtlValue(UAC_CTL_FEATURE, 1, v, 1, 0));  /* mute is 0 or 1 */
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: voltest <X4 config0.bin>\n"); return 2; }
    Walks(argv[1]);
    Ranges();
    Gains();
    Plans();
    Scales();
    Setups();
    printf("PASS voltest: %u volume/mute checks (walk, RANGE, dB both ways, gain, plan, scale, setup)\n", checks);
    return 0;
}
