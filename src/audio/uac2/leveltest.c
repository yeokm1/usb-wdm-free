/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of uaclevel.c, the playback level engine moved out of
 * uacwdm.cpp (docs/design/uac2.md 28.15): vectors on the engine against a
 * scripted fake device, and the replay of the guest baselines (testdata/
 * levels/*.lvl): every Feature Unit request the shipped driver sent in a
 * guest, in order, with its data, interleaved with what made it happen
 * (start, a KS SET, a RUN, a power change). The fake device answers each
 * request with the recorded answer and fails the test at the first
 * request that differs from the recorded one, or when one is left over.
 * Built for USB Audio 2.0, and with UAC_V1 for 1.0 (vectors only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uaclevel.h"

static unsigned int checks;
static const char *Requests(void);
static void Check(int ok, const char *expression, const char *where, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL %s line %u: %s\n  requests: %s\n", where, line, expression, Requests()); exit(1); }
}
static const char *where = "vectors";
#define CHECK(x) Check((x), #x, where, __LINE__)

/* ---- the fake host -------------------------------------------------- */

#define FAIL_STATUS ((long)0xC000009CL)   /* STATUS_DEVICE_DATA_ERROR */
#define MAXREQ 4096
typedef struct REQ {
    unsigned int Type, Request, Value, Index, Length, Status, Actual;
    unsigned char Data[64];
} REQ;

static struct {
    /* replay */
    REQ Req[MAXREQ];
    unsigned int Count, Next, Line;
    int Replay;
    /* scripted device (vectors) */
    int Cur[256][2][UAC_VOL_SLOTS];       /* [unit][selector-1][channel] */
    int Range[3];                         /* MIN MAX RES of every volume */
    int StallSet, StallGet, StallRange, ShortRange;
    unsigned int Sets, Gets;
    char Log[8192];
    /* host */
    int Ready, Running, Removed;
    unsigned long AboveZero;
    UAC_LEVEL_SAVE Saved;
    int HaveSaved;
    UAC_LEVEL_DIAG Diag;
    int HaveDiag;
    unsigned long Gain[UAC_VOL_CHANNELS];
    unsigned int Delays, Locks, TakenUnit;
} f;

static const char *Requests(void) { return f.Log; }
static void Put16(unsigned char *b, int v) { b[0] = (unsigned char)v; b[1] = (unsigned char)((unsigned int)v >> 8); }

static long Feature(void *context, UAC_BYTE unit, unsigned long selector, unsigned long channel,
                    int read, unsigned long attribute, void *bytes, unsigned long size, unsigned long *got)
{
    unsigned int type, request, value;
    (void)context;
    type = read ? 0xA1 : 0x21;
#ifdef UAC_V1
    request = !read ? 0x01 : attribute == UAC_CTL_CUR ? 0x81 : (unsigned int)attribute;
#else
    request = read ? (unsigned int)attribute : UAC_CTL_CUR;
#endif
    value = (unsigned int)(selector << 8 | channel);
    if (f.Replay) {
        REQ *r;
        char want[96];
        sprintf(want, "%02X %02X %04X unit %u len %lu", type, request, value, unit, size);
        if (f.Next >= f.Count) { printf("FAIL %s: extra request %s\n", where, want); exit(1); }
        r = &f.Req[f.Next++];
        if (r->Type != type || r->Request != request || r->Value != value || (r->Index >> 8) != unit ||
            r->Length != size) {
            printf("FAIL %s: request %u is %s, the baseline has %02X %02X %04X unit %u len %u\n",
                   where, f.Next, want, r->Type, r->Request, r->Value, r->Index >> 8, r->Length);
            exit(1);
        }
        if (!read && memcmp(r->Data, bytes, size)) {
            printf("FAIL %s: request %u (%s) writes another value than the baseline\n", where, f.Next, want);
            exit(1);
        }
        checks++;
        if (r->Status) { if (got) *got = 0; return FAIL_STATUS; }
        if (read) {
            memcpy(bytes, r->Data, r->Actual < size ? r->Actual : size);
            if (got) *got = r->Actual;
            else if (r->Actual != size) return FAIL_STATUS;
        }
        return 0;
    }
    /* The scripted device: CUR per unit, selector, channel; one RANGE.
     * The log names a RANGE read once (USB Audio 1.0's GET_MIN only). */
    if (read && (attribute == 0x83 || attribute == 0x84)) goto answer;
    sprintf(f.Log + strlen(f.Log), "%s%u.%lu.%lu", read ? (attribute == UAC_CTL_CUR ? "g" : "r") : "s",
            unit, selector, channel);
    if (!read) {
        f.Sets++;
        if (f.StallSet) { f.StallSet--; strcat(f.Log, "! "); return FAIL_STATUS; }
        f.Cur[unit][selector - 1][channel] = selector == 2 ?
            (int)(short)(((unsigned char *)bytes)[0] | ((unsigned char *)bytes)[1] << 8) :
            ((unsigned char *)bytes)[0];
        sprintf(f.Log + strlen(f.Log), "=%d ", f.Cur[unit][selector - 1][channel]);
        return 0;
    }
    strcat(f.Log, " ");
answer:
    f.Gets++;
    if (attribute == UAC_CTL_CUR) {
        if (f.StallGet) { f.StallGet--; return FAIL_STATUS; }
        if (selector == 2) Put16((unsigned char *)bytes, f.Cur[unit][1][channel]);
        else ((unsigned char *)bytes)[0] = (unsigned char)f.Cur[unit][0][channel];
        if (got) *got = size;
        return 0;
    }
#ifdef UAC_V1
    if (f.StallRange) return FAIL_STATUS;
    Put16((unsigned char *)bytes, f.Range[attribute - 0x82]);
    if (got) *got = 2;
    return 0;
#else
    if (f.StallRange) { f.StallRange--; return FAIL_STATUS; }
    if (f.ShortRange && size > 8) { f.ShortRange--; return FAIL_STATUS; }  /* refuses the long request */
    memset(bytes, 0, size);
    ((unsigned char *)bytes)[0] = 1;
    Put16((unsigned char *)bytes + 2, f.Range[0]); Put16((unsigned char *)bytes + 4, f.Range[1]);
    Put16((unsigned char *)bytes + 6, f.Range[2]);
    if (got) *got = 8;
    return 0;
#endif
}
static int Ready(void *c) { (void)c; return f.Ready; }
static int Running(void *c) { (void)c; return f.Running; }
static int Removed(void *c) { (void)c; return f.Removed; }
static void Delay40(void *c) { (void)c; f.Delays++; }
static void Lock(void *c) { (void)c; f.Locks++; }
static void Unlock(void *c) { (void)c; }
static void Publish(void *c, long *target, const unsigned long *gain)
{
    unsigned int k;
    (void)c;
    for (k = 0; k < UAC_VOL_CHANNELS; k++) target[k] = (long)gain[k];
    memcpy(f.Gain, gain, sizeof(f.Gain));
}
static unsigned long GetDword(void *c, const char *name)
{
    (void)c;
    return !strcmp(name, "Uac2VolAboveZero") ? f.AboveZero : 0xFFFFFFFFUL;
}
static int GetValue(void *c, int pair, void *data, unsigned long size)
{
    (void)c; (void)pair;
    if (!f.HaveSaved || size != sizeof(f.Saved)) return 0;
    memcpy(data, &f.Saved, size);
    return 1;
}
static void PutValue(void *c, int pair, void *data, unsigned long size)
{
    (void)c; (void)pair;
    if (size == sizeof(f.Saved)) { memcpy(&f.Saved, data, size); f.HaveSaved = 1; }
}
static void PutDiag(void *c, void *data, unsigned long size)
{
    (void)c;
    if (size == sizeof(f.Diag)) { memcpy(&f.Diag, data, size); f.HaveDiag = 1; }
}
static int Taken(void *c, UAC_BYTE unit) { (void)c; return unit == f.TakenUnit; }

static const UAC_LEVEL_OPS ops = {
    NULL, Feature, Ready, Running, Removed, Delay40, Lock, Unlock, Publish, GetDword, GetValue, PutValue,
    PutDiag, Taken
};

/* ---- built descriptors (as voltest builds them) ----------------------- */

static UAC_DESC desc;
static UAC_BYTE bytes[4096];
static unsigned int used;
static void Reset(void)
{
    memset(&desc, 0, sizeof(desc));
    memset(bytes, 0, sizeof(bytes));
    used = 9;
    desc.Bytes = bytes;
}
static void Add(const UAC_BYTE *d)
{
    desc.Entity[d[3]] = (UAC_WORD)used;
    memcpy(bytes + used, d, d[0]);
    used += d[0];
    desc.Length = used;
}
#ifndef UAC_V1
/* UAC 2.0: IT 1 (USB streaming) -> FU 2 (controls per slot, 2 channels)
 * -> OT 3 (speaker). */
static void Path(unsigned long master, unsigned long left, unsigned long right)
{
    static const UAC_BYTE it[17] = { 17, 0x24, 2, 1, 0x01, 0x01, 0, 9, 2, 3, 0, 0, 0, 0, 0, 0, 0 };
    static const UAC_BYTE ot[12] = { 12, 0x24, 3, 3, 0x01, 0x03, 0, 2, 9, 0, 0, 0 };
    UAC_BYTE fu[18];
    unsigned long c[3];
    unsigned int i;
    c[0] = master; c[1] = left; c[2] = right;
    memset(fu, 0, sizeof(fu));
    fu[0] = 18; fu[1] = 0x24; fu[2] = 6; fu[3] = 2; fu[4] = 1;
    for (i = 0; i < 3; i++) {
        fu[5 + 4 * i] = (UAC_BYTE)c[i]; fu[6 + 4 * i] = (UAC_BYTE)(c[i] >> 8);
    }
    Reset(); Add(it); Add(fu); Add(ot);

}
#else
/* UAC 1.0: IT 1 -> FU 2 (bControlSize 1: D0 mute, D1 volume) -> OT 3. */
static void Path(unsigned long master, unsigned long left, unsigned long right)
{
    static const UAC_BYTE it[12] = { 12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 3, 0, 0, 0 };
    static const UAC_BYTE ot[9] = { 9, 0x24, 3, 3, 0x01, 0x03, 0, 2, 0 };
    UAC_BYTE fu[10];
    fu[0] = 10; fu[1] = 0x24; fu[2] = 6; fu[3] = 2; fu[4] = 1; fu[5] = 1;
    fu[6] = (UAC_BYTE)master; fu[7] = (UAC_BYTE)left; fu[8] = (UAC_BYTE)right; fu[9] = 0;
    Reset(); Add(it); Add(fu); Add(ot);
}
#endif

static UAC_LEVEL lv;
static void Fresh(void)
{
    memset(&f, 0, sizeof(f));
    f.Ready = 1;
    f.Range[0] = -24576; f.Range[1] = 0; f.Range[2] = 256;
}
static void Start(void)
{
    f.Log[0] = 0;
    UacLevelSetup(&lv, &ops, 0, &desc, 1, 2, 1);
    UacLevelApply(&lv);
}
static void Set(int volume, long channel, long value)
{
    f.Log[0] = 0;
    UacLevelSet(&lv, volume, channel, value);
    UacLevelApply(&lv);
}

#ifndef UAC_V1
#define RW_BOTH 0x0f
#define RW_VOL 0x0c
#define RW_MUTE 0x03
#else
#define RW_BOTH 0x03
#define RW_VOL 0x02
#define RW_MUTE 0x01
#endif

static void Vectors(void)
{
    UAC_LEVEL_STATS st;
    where = "vectors";
    /* Master mute and volume, channel volumes as extras: start reads the
     * ranges and CURs, writes nothing; the device's level stands. */
    Fresh(); Path(RW_BOTH, RW_VOL, RW_VOL);
    f.Cur[2][1][0] = -2560; f.Cur[2][1][1] = -10240; f.Cur[2][1][2] = -10240;
    Start();
    CHECK(lv.Present && lv.Map.VolumeMode == UAC_VOL_MASTER && lv.Map.VolumeExtra == 3);
    CHECK(f.Sets == 0 && !lv.VolumeOwned && !lv.MuteOwned);
    CHECK(lv.Level[0] == -2560L * 256 && lv.Level[1] == -2560L * 256);
    CHECK(f.Gain[0] == UAC_VOL_UNITY && f.Gain[1] == UAC_VOL_UNITY);   /* not owned: no software volume */
    CHECK(f.HaveDiag && !f.HaveSaved);   /* Change 0 == Saved 0: nothing saved */
    /* A SET owns the volume: the extras go to 0 dB, the master to the
     * level; the raisings after the lowering; saved once. */
    Set(1, -1, -6L * 65536);
    CHECK(lv.VolumeOwned && lv.Actual[0] == -1536 && lv.Actual[1] == 0 && lv.Actual[2] == 0);
    CHECK(!strcmp(f.Log, "s2.2.0=-1536 s2.2.1=0 s2.2.2=0 "));
    CHECK(f.HaveSaved && f.Saved.Version == 2 && f.Saved.Owned == UAC_LEVEL_VOLUME &&
          f.Saved.Level[0] == -6L * 65536 && lv.Saved == 1);
    CHECK(lv.VolumeNormal);
    /* Mute on, then off: the master only (no channel mute). */
    Set(0, -1, 1);
    CHECK(!strcmp(f.Log, "s2.1.0=1 ") && f.Gain[0] == UAC_VOL_UNITY);
    Set(0, -1, 0);
    CHECK(!strcmp(f.Log, "s2.1.0=0 "));
    /* A SET snaps to the range: -6.3 dB to the nearest 1 dB step. */
    Set(1, 0, -6L * 65536 - 20000);
    CHECK(lv.Level[0] == -6L * 65536 && f.Log[0] == 0);
    /* The loudest channel answers channel -1; balance writes the master
     * for the loudest and the software takes the rest. */
    Set(1, 1, -20L * 65536);
    CHECK(UacLevelGet(&lv, 1, -1) == -6L * 65536 && UacLevelGet(&lv, 1, 1) == -20L * 65536);
    CHECK(f.Gain[0] == UAC_VOL_UNITY && f.Gain[1] < UAC_VOL_UNITY / 4);
    /* RUN: nothing stale; D3 makes every slot unknown, RUN reads again
     * and writes what differs. */
    CHECK(!UacLevelStale(&lv));
    UacLevelPowerDown(&lv);
    CHECK(UacLevelStale(&lv));
    f.Cur[2][1][0] = 0; f.Cur[2][1][1] = -10240;
    f.Log[0] = 0; f.Running = 1;
    UacLevelApply(&lv);
    CHECK(!strcmp(f.Log, "g2.2.0 g2.1.0 g2.2.1 g2.2.2 s2.2.0=-1536 s2.2.1=0 "));
    CHECK(f.Delays == 1);   /* a channel louder while running: 40 ms first */
    f.Running = 0;
    UacLevelStats(&lv, &st);
    CHECK(st.VolLayout == 25 && st.VolMode == UAC_VOL_MASTER && st.VolRangeRead == 7 && st.VolStartKnown == 7);
    CHECK(UacLevelNotHostCpu(&lv, 0) && !UacLevelNotHostCpu(&lv, 1));

    /* Restore: the saved value owns both and writes at start. */
    Fresh(); Path(RW_BOTH, RW_VOL, RW_VOL);
    f.HaveSaved = 1; f.Saved.Version = 2; f.Saved.Channels = 2; f.Saved.Owned = 3; f.Saved.Mute = 1;
    f.Saved.Level[0] = f.Saved.Level[1] = -10L * 65536;
    Start();
    CHECK(lv.VolumeOwned && lv.MuteOwned && lv.Mute == 1);
    CHECK(!strcmp(f.Log, "r2.2.0 r2.2.1 r2.2.2 g2.2.0 g2.1.0 g2.2.1 g2.2.2 s2.2.0=-2560 s2.1.0=1 "));
    /* A saved value of another channel count is not read. */
    Fresh(); Path(RW_BOTH, RW_VOL, RW_VOL);
    f.HaveSaved = 1; f.Saved.Version = 2; f.Saved.Channels = 1; f.Saved.Owned = 3;
    Start();
    CHECK(!lv.VolumeOwned && !lv.MuteOwned && f.Sets == 0);

    /* Section 20.3: a CUR above the top offered is lowered at start, that
     * control alone; with Uac2VolAboveZero = 1 it is not. */
    Fresh(); Path(0, RW_VOL, RW_VOL);
    f.Range[0] = -32512; f.Range[1] = 3072; f.Range[2] = 256;
    f.Cur[2][1][1] = 3072; f.Cur[2][1][2] = -2560;
    Start();
    CHECK(lv.Map.VolumeMode == UAC_VOL_CHANNEL && lv.Lowered && lv.Actual[1] == 0 && lv.Actual[2] == -2560);
    CHECK(f.Sets == 1 && lv.Range[1].KsMax == 0 && lv.Range[1].KsTop == 3072L * 256);
    Fresh(); Path(0, RW_VOL, RW_VOL);
    f.Range[0] = -32512; f.Range[1] = 3072; f.Range[2] = 256; f.AboveZero = 1;
    f.Cur[2][1][1] = 3072;
    Start();
    CHECK(!lv.Lowered && f.Sets == 0 && lv.Range[1].KsMax == 3072L * 256);
    /* A failed lowering leaves the control unknown: Assumed (KsTop). */
    Fresh(); Path(0, RW_VOL, RW_VOL);
    f.Range[0] = -32512; f.Range[1] = 3072; f.Range[2] = 256;
    f.Cur[2][1][1] = 3072; f.StallSet = 1;
    UacLevelSetup(&lv, &ops, 0, &desc, 1, 2, 1);
    CHECK(!lv.Lowered && !lv.Known[1] && lv.Failures == 1 && lv.LastStatus == FAIL_STATUS);
    f.Log[0] = 0;
    UacLevelApply(&lv);   /* Prepare's apply reads it again; not owned: no write */
    CHECK(!strcmp(f.Log, "g2.2.1 ") && lv.Known[1] && lv.Actual[1] == 3072);

    /* A failed write holds the later passes and stays dirty; the next
     * apply reads the unknown control and writes again. */
    Fresh(); Path(RW_BOTH, 0, 0);
    f.Cur[2][1][0] = -24576;
    Start();
    f.StallSet = 1;
    Set(1, -1, 0);
    CHECK(!lv.Known[0] && lv.VolumeDirty && UacLevelStale(&lv));
    f.Log[0] = 0;
    UacLevelApply(&lv);
    CHECK(!strcmp(f.Log, "g2.2.0 s2.2.0=0 ") && lv.Known[0] && !lv.VolumeDirty);

    /* An extra whose RANGE fails is read, never written; while its CUR is
     * unknown it silences its channel (Gains). */
    Fresh(); Path(RW_VOL, RW_VOL, RW_VOL);
#ifndef UAC_V1
    f.StallRange = 0;
#endif
    Start();
    CHECK(lv.NoRange == 0);
    Fresh(); Path(RW_VOL, RW_VOL, RW_VOL);
    f.Cur[2][1][2] = -1280;
    UacLevelSetup(&lv, &ops, 0, &desc, 1, 2, 1);   /* ranges read: master, ch1, ch2 */
    CHECK(lv.NoRange == 0);
#ifndef UAC_V1
    /* The one-subrange retry: a device refusing the long RANGE request. */
    Fresh(); Path(RW_VOL, 0, 0);
    f.ShortRange = 1;
    Start();
    CHECK(lv.Map.VolumeMode == UAC_VOL_MASTER && (lv.RangeRead & 1));
    CHECK(!strncmp(f.Log, "r2.2.0 r2.2.0 g2.2.0", 20));
    /* A RANGE that fails both times: software volume, no write ever. */
    Fresh(); Path(RW_VOL, 0, 0);
    f.StallRange = 2;
    Start();
    CHECK(lv.Map.VolumeMode == UAC_VOL_NONE && lv.Range[0].KsMin == UAC_VOL_SW_MIN);
    Set(1, -1, -12L * 65536);
    CHECK(f.Sets == 0 && f.Gain[0] < UAC_VOL_UNITY && f.Gain[0] > 0);
#endif
    /* A later pair never drives a unit an earlier one drives. */
    Fresh(); Path(RW_BOTH, 0, 0); f.TakenUnit = 2;
    UacLevelSetup(&lv, &ops, 1, &desc, 1, 2, 1);
    CHECK(lv.Map.VolumeMode == UAC_VOL_NONE && lv.Map.MuteMode == UAC_VOL_NONE && f.Gets == 0);
    /* No render format: no line. */
    Fresh(); Path(RW_BOTH, 0, 0);
    UacLevelSetup(&lv, &ops, 0, &desc, 0, 2, 1);
    CHECK(!lv.Present && f.Gets == 0 && lv.Gain[0] == (long)UAC_VOL_UNITY);
    /* Removed: no registry write. */
    Fresh(); Path(RW_BOTH, 0, 0);
    Start();
    f.Removed = 1; f.HaveDiag = 0;
    Set(1, -1, -3L * 65536);
    CHECK(!f.HaveSaved && !f.HaveDiag);
}

/* ---- replay ---------------------------------------------------------- */

#ifndef UAC_V1
static UAC_BYTE file[65536];
static UAC_DESC model;

static int Hex(const char *s, unsigned char *out, unsigned int max)
{
    unsigned int n = 0, v;
    if (!strcmp(s, "-")) return 0;
    while (s[0] && s[1] && n < max) { sscanf(s, "%2x", &v); out[n++] = (unsigned char)v; s += 2; }
    return (int)n;
}

/* Events run in order; the requests between two events are what the
 * engine must send for the first of them. */
static void Pending(const char *at)
{
    if (f.Next != f.Count) {
        printf("FAIL %s: %u baseline request(s) not sent before %s (next: %02X %02X %04X unit %u)\n", where,
               f.Count - f.Next, at, f.Req[f.Next].Type, f.Req[f.Next].Request, f.Req[f.Next].Value,
               f.Req[f.Next].Index >> 8);
        exit(1);
    }
    f.Next = f.Count = 0;
}

#define MAXLINES 8192
static char lines[MAXLINES][1024];

static int Action(const char *l)
{
    return !strncmp(l, "!start", 6) || !strncmp(l, "!vol", 4) || !strncmp(l, "!mute", 5) || !strncmp(l, "!run", 4);
}

/* One '!' line. An action (start, a SET, run) has its requests loaded. */
static void Event(const char *dir, const char *line, unsigned int *starts, unsigned int *sets)
{
    char path[512], a[64], b[1024], c[64], d[256];
    FILE *df;
    unsigned int n;
    a[0] = b[0] = c[0] = d[0] = 0;
    sscanf(line + 1, "%63s %1023s %63s %255s", a, b, c, d);
    if (!strcmp(a, "profile")) {
        /* !profile <config descriptor> <AC interface> <first> <count> */
        unsigned int ac, first, count;
        char fn[256];
        CHECK(sscanf(line + 1, "%*s %255s %u %u %u", fn, &ac, &first, &count) == 4);
        sprintf(path, "%s\\%s", dir, fn);
        df = fopen(path, "rb");
        CHECK(df != NULL);
        n = (unsigned int)fread(file, 1, sizeof(file), df);
        fclose(df);
        CHECK(UacParse(file, n, ac, first, count, &model) == UAC_OK);
    } else if (!strcmp(a, "dword")) {
        f.AboveZero = (unsigned long)strtoul(c, NULL, 0);
    } else if (!strcmp(a, "saved")) {
        CHECK(Hex(b, (unsigned char *)&f.Saved, sizeof(f.Saved)) == sizeof(f.Saved));
        f.HaveSaved = 1;
    } else if (!strcmp(a, "nosaved")) {
        f.HaveSaved = 0;
    } else if (!strcmp(a, "start")) {
        /* !start <formats> <most channels> <terminal>: Prepare's LevelSetup and ApplyLocked */
        f.Running = 0; f.Ready = 1; f.Removed = 0;
        UacLevelSetup(&lv, &ops, 0, &model, (unsigned long)atoi(b), (unsigned long)atoi(c), (unsigned int)atoi(d));
        UacLevelApply(&lv);
        (*starts)++;
    } else if (!strcmp(a, "vol") || !strcmp(a, "mute")) {
        /* !vol <channel> <KS level> | !mute <channel> <0|1>: a KS SET at PASSIVE */
        UacLevelSet(&lv, !strcmp(a, "vol"), atol(b), atol(c));
        UacLevelApply(&lv);
        (*sets)++;
    } else if (!strcmp(a, "run")) {
        /* Run: the transport exists, the level applied when stale, then the stream starts. */
        f.Running = 1;
        if (UacLevelStale(&lv)) UacLevelApply(&lv);
    } else if (!strcmp(a, "stop")) {
        f.Running = 0;
    } else if (!strcmp(a, "d3")) {
        UacLevelPowerDown(&lv); f.Ready = 0; f.Running = 0;
    } else if (!strcmp(a, "d0")) {
        f.Ready = 1;
    } else if (!strcmp(a, "level")) {
        /* !level <ch> <KS>: the KS level the guest's Uac2VolMap held */
        CHECK(lv.Level[atoi(b)] == atol(c));
    } else if (!strcmp(a, "change")) {
        CHECK(lv.Change == (unsigned long)atol(b));
    } else if (!strcmp(a, "diag")) {
        /* !diag <hex of Uac2VolMap as the guest read it> */
        UAC_LEVEL_DIAG g;
        CHECK(f.HaveDiag);
        CHECK(Hex(b, (unsigned char *)&g, sizeof(g)) == sizeof(g));
        if (memcmp(&g, &f.Diag, sizeof(g))) {
            unsigned int k;
            for (k = 0; k < sizeof(g) / 4; k++)
                if (((unsigned long *)&g)[k] != ((unsigned long *)&f.Diag)[k])
                    printf("  Uac2VolMap word %u: guest %08lX, engine %08lX\n", k,
                           ((unsigned long *)&g)[k], ((unsigned long *)&f.Diag)[k]);
            printf("FAIL %s: Uac2VolMap differs at %s", where, line);
            exit(1);
        }
        checks++;
    } else if (!strcmp(a, "savedis")) {
        UAC_LEVEL_SAVE sv;
        CHECK(f.HaveSaved);
        CHECK(Hex(b, (unsigned char *)&sv, sizeof(sv)) == sizeof(sv));
        CHECK(!memcmp(&sv, &f.Saved, sizeof(sv)));
    } else if (!strcmp(a, "end")) {
    } else {
        printf("FAIL %s: unknown event %s\n", where, a);
        exit(1);
    }
}

static void Request(const char *line)
{
    REQ *r;
    unsigned int t, q, v, x, l, s, act;
    char d[256];
    CHECK(f.Count < MAXREQ);
    r = &f.Req[f.Count];
    memset(r, 0, sizeof(*r));
    d[0] = 0;
    CHECK(sscanf(line, "%x %x %x %x %u %u %u %255s", &t, &q, &v, &x, &l, &s, &act, d) >= 7);
    r->Type = t; r->Request = q; r->Value = v; r->Index = x; r->Length = l; r->Status = s; r->Actual = act;
    Hex(d, r->Data, sizeof(r->Data));
    f.Count++;
}

/* Each action runs with every request line up to the next action (the
 * '!' lines between them touch no device: settings and checks), then the
 * '!' lines after it run in order. */
static void Replay(const char *dir, const char *name)
{
    char path[512];
    FILE *in;
    unsigned int n = 0, i, j, k, starts = 0, sets = 0;
    sprintf(path, "%s\\%s", dir, name);
    in = fopen(path, "r");
    where = name;
    CHECK(in != NULL);
    while (n < MAXLINES && fgets(lines[n], sizeof(lines[n]), in))
        if (lines[n][0] != '#' && lines[n][0] != '\n' && lines[n][0] != '\r') n++;
    fclose(in);
    memset(&f, 0, sizeof(f));
    f.Replay = 1; f.Ready = 1;
    for (i = 0; i < n && !Action(lines[i]); i++) {
        CHECK(lines[i][0] == '!');   /* no request before the first action */
        Event(dir, lines[i], &starts, &sets);
    }
    while (i < n) {
        /* Actions with no request line between them (the two channel
         * SETs of one mixer change) are a group: the trace cannot show
         * which of them sent which request, only that together they sent
         * the requests that follow, in that order. */
        unsigned int g = i, e;
        for (;;) {
            for (e = g + 1; e < n && lines[e][0] == '!' && !Action(lines[e]); e++) ;
            if (e < n && Action(lines[e])) g = e; else break;
        }
        f.Count = 0;
        for (j = g + 1; j < n && !Action(lines[j]); j++)
            if (lines[j][0] != '!') Request(lines[j]);
        f.Next = 0;
        for (k = i; k <= g; k++) if (lines[k][0] == '!') Event(dir, lines[k], &starts, &sets);
        Pending(lines[g]);
        for (k = g + 1; k < j; k++) if (lines[k][0] == '!') Event(dir, lines[k], &starts, &sets);
        i = j;
    }
    CHECK(starts > 0);
    printf("replay %s: %u start(s), %u SET(s), identical\n", name, starts, sets);
}
#endif

int main(int argc, char **argv)
{
    int i;
    Vectors();
#ifndef UAC_V1
    for (i = 2; i < argc; i++) Replay(argv[1], argv[i]);
#else
    (void)argc; (void)argv; (void)i;
#endif
    printf("leveltest: %u checks passed\n", checks);
    return 0;
}
