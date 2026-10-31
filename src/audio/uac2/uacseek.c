/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original WinMM / DirectSound seek test for uac2 (test only):
 *   UACSEEK wo|ds <fmt> <rate> <seeks> <ms> [variant [gap_ms]]
 * plays a counted signal on the first "UAC2" output (UACWAV_PAIR picks
 * another, as UACWAV does) and seeks <seeks> times, <ms> milliseconds of
 * playing apart, the way a player does:
 *   wo  WinMM streaming, eight 20 ms buffers refilled as they return.
 *       variant p (default): waveOutPause, waveOutReset, new buffers,
 *       waveOutRestart; r: waveOutReset and new buffers (no pause);
 *       h: waveOutPause and waveOutRestart only (no seek: a pause).
 *   ds  DirectSound, a one-second looping secondary buffer kept full.
 *       variant s (default): Stop, SetCurrentPosition(0), refill, Play;
 *       h: Stop and Play only (no seek: a pause).
 * <fmt>: s16 (WAVE_FORMAT_PCM), f32 (WAVE_FORMAT_IEEE_FLOAT), x16, x24,
 * x32 (WAVE_FORMAT_EXTENSIBLE PCM, container = bits), all two channels.
 * <gap_ms> (default 100) is how long the stream stays paused or stopped.
 * The signal counts frames: frame n of the source carries n's low byte in
 * the left channel's high byte and the next byte in the right's, each
 * sample's low bits 0x40 (never a zero frame), so the simulator's received
 * PCM (uacseek.py) shows every jump, repeat and stale block. A seek moves
 * the source by 30011 frames. Each event is printed with GetTickCount and
 * the source frame, on stdout and COM1 when it opens. The last line is
 * PASS: or FAIL: (the API calls succeeded); never claims audibility.
 * The device's wave volume is set to full first (and the old value
 * printed): a lower one (2000's default) scales the counted samples.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef WIN32
#define WIN32 /* VC6's d3dtypes.h (from dsound.h) tests it */
#endif
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    WAVEFORMATEX Format;
    WORD ValidBits;
    DWORD ChannelMask;
    GUID SubFormat;
} WFX;
static const GUID PcmSub = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#define NBUF 8
#define SEEK_STEP 30011UL

static HANDLE com = INVALID_HANDLE_VALUE;
static DWORD t0;
static unsigned long src;   /* the next source frame */
static unsigned fbits, ffloat, fbytes;

static void Log(const char *fmt, ...)
{
    char line[256]; DWORD n;
    va_list a;
    int k = sprintf(line, "%7lu ", GetTickCount() - t0);
    va_start(a, fmt); k += _vsnprintf(line + k, sizeof(line) - k - 3, fmt, a); va_end(a);
    if (k < 0 || k > (int)sizeof(line) - 3) k = sizeof(line) - 3;
    line[k++] = '\r'; line[k++] = '\n'; line[k] = 0;
    fputs(line, stdout); fflush(stdout);
    if (com != INVALID_HANDLE_VALUE) WriteFile(com, line, k, &n, NULL);
}

static int Uac2(const char *name)
{
    char upper[MAXPNAMELEN];
    lstrcpyn(upper, name, sizeof(upper)); CharUpperBuff(upper, lstrlen(upper));
    return strstr(upper, "UAC2") != NULL;
}

static UINT Wanted(void)
{
    char v[8];
    DWORD n = GetEnvironmentVariable("UACWAV_PAIR", v, sizeof(v));
    if (!n || n >= sizeof(v) || v[0] < '0' || v[0] > '9') return 0;
    return (UINT)atoi(v);
}

static UINT Device(char *name)
{
    WAVEOUTCAPS c; UINT i, n = waveOutGetNumDevs(), skip = Wanted();
    for (i = 0; i < n; i++)
        if (!waveOutGetDevCaps(i, &c, sizeof(c)) && Uac2(c.szPname) && !skip--) {
            lstrcpyn(name, c.szPname, MAXPNAMELEN);
            return i;
        }
    return (UINT)-1;
}

static int Format(WFX *w, const char *fmt, unsigned rate)
{
    ZeroMemory(w, sizeof(*w));
    ffloat = 0;
    if (!lstrcmpi(fmt, "s16") || !lstrcmpi(fmt, "x16")) fbits = 16;
    else if (!lstrcmpi(fmt, "x24")) fbits = 24;
    else if (!lstrcmpi(fmt, "x32")) fbits = 32;
    else if (!lstrcmpi(fmt, "f32")) { fbits = 32; ffloat = 1; }
    else return 0;
    fbytes = 2 * fbits / 8;
    w->Format.nChannels = 2; w->Format.nSamplesPerSec = rate;
    w->Format.wBitsPerSample = (WORD)fbits;
    w->Format.nBlockAlign = (WORD)fbytes;
    w->Format.nAvgBytesPerSec = rate * fbytes;
    if (ffloat) { w->Format.wFormatTag = 3; return 1; } /* WAVE_FORMAT_IEEE_FLOAT */
    if (!lstrcmpi(fmt, "s16")) { w->Format.wFormatTag = WAVE_FORMAT_PCM; return 1; }
    w->Format.wFormatTag = 0xFFFE;
    w->Format.cbSize = 22;
    w->ValidBits = (WORD)fbits;
    w->ChannelMask = 3;
    w->SubFormat = PcmSub;
    return 1;
}

static void Sample(unsigned char *p, unsigned value)
{
    long s = ((long)(value & 0xff) - 128) * 256 + 0x40;
    if (ffloat) { float f = (float)s / 32768.0f; memcpy(p, &f, 4); return; }
    if (fbits == 24) s <<= 8;
    if (fbits == 32) s <<= 16;
    p[0] = (unsigned char)s; p[1] = (unsigned char)(s >> 8);
    if (fbits >= 24) p[2] = (unsigned char)(s >> 16);
    if (fbits == 32) p[3] = (unsigned char)(s >> 24);
}

/* count bytes of whole frames from the source counter. */
static void Fill(unsigned char *p, DWORD count)
{
    DWORD f;
    for (f = 0; f + fbytes <= count; f += fbytes, src++) {
        Sample(p + f, (unsigned)src);
        Sample(p + f + fbytes / 2, (unsigned)(src >> 8));
    }
}

/* Static: a header WinMM has not given back stays valid until the exit. */
static WAVEHDR hdr[NBUF];

static int WaveOut(const WFX *w, unsigned seeks, unsigned ms, char variant, unsigned gap)
{
    HWAVEOUT h; HANDLE ev; MMRESULT r;
    char name[MAXPNAMELEN];
    UINT dev = Device(name), i, k, bad = 0;
    DWORD bytes = (w->Format.nSamplesPerSec / 50) * fbytes, next;
    unsigned char *buf[NBUF];
    if (dev == (UINT)-1) { Log("FAIL: no UAC2 output"); return 1; }
    ev = CreateEvent(NULL, FALSE, FALSE, NULL);
    r = waveOutOpen(&h, dev, (WAVEFORMATEX *)w, (DWORD)ev, 0, CALLBACK_EVENT);
    Log("wo dev=%u \"%s\" tag=%u bits=%u rate=%lu open=%u variant=%c gap=%u",
        dev, name, w->Format.wFormatTag, fbits, w->Format.nSamplesPerSec, r, variant, gap);
    if (r) { Log("FAIL: open"); return 1; }
    for (i = 0; i < NBUF; i++) {
        buf[i] = (unsigned char *)malloc(bytes);
        ZeroMemory(&hdr[i], sizeof(hdr[i]));
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = bytes;
        waveOutPrepareHeader(h, &hdr[i], sizeof(hdr[i]));
    }
    Log("start src=%lu", src);
    for (i = 0; i < NBUF; i++) { Fill(buf[i], bytes); if (waveOutWrite(h, &hdr[i], sizeof(hdr[i]))) bad++; }
    for (k = 0; k <= seeks; k++) {
        next = GetTickCount() + ms;
        while ((LONG)(GetTickCount() - next) < 0) {
            WaitForSingleObject(ev, 10);
            for (i = 0; i < NBUF; i++)
                if (hdr[i].dwFlags & WHDR_DONE) {
                    Fill(buf[i], bytes);
                    if (waveOutWrite(h, &hdr[i], sizeof(hdr[i]))) bad++;
                }
        }
        if (k == seeks) break;
        if (variant == 'h') {
            r = waveOutPause(h); if (r) bad++;
            Log("pause %u src=%lu (%u)", k, src, r);
            Sleep(gap);
            r = waveOutRestart(h); if (r) bad++;
            Log("restart %u (%u)", k, r);
            continue;
        }
        if (variant == 'p') { r = waveOutPause(h); if (r) bad++; Log("pause %u (%u)", k, r); }
        r = waveOutReset(h);
        Log("reset %u src=%lu (%u)", k, src, r);
        /* Every buffer must be back before it is filled again. */
        if (r) { bad++; break; }
        for (i = 0; i < NBUF; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) { Log("buffer %u not returned", i); bad++; }
        if (bad) break;
        if (variant == 'p') Sleep(gap);
        src += SEEK_STEP;
        Log("seek %u src=%lu", k, src);
        for (i = 0; i < NBUF; i++) { Fill(buf[i], bytes); if (waveOutWrite(h, &hdr[i], sizeof(hdr[i]))) bad++; }
        if (variant == 'p') { r = waveOutRestart(h); if (r) bad++; Log("restart %u (%u)", k, r); }
    }
    r = waveOutReset(h); if (r) bad++;
    Log("end src=%lu reset=%u", src, r);
    /* A buffer is freed only when the device has given it back. */
    for (i = 0; i < NBUF; i++) {
        if (!(hdr[i].dwFlags & WHDR_DONE) || waveOutUnprepareHeader(h, &hdr[i], sizeof(hdr[i]))) {
            Log("buffer %u not returned: kept", i); bad++; continue;
        }
        free(buf[i]);
    }
    r = waveOutClose(h); if (r) bad++;
    Log("close=%u failed=%u", r, bad);
    Log(bad ? "FAIL: calls failed" : "PASS: calls succeeded");
    return bad ? 1 : 0;
}

/* DirectSound: a one-second looping buffer, kept full behind the play
 * cursor from a 10 ms poll. The first UAC2 output's DirectSound device
 * is the one whose description names it. */
static GUID dsGuid; static int dsFound; static char dsName[128]; static UINT dsSkip;
static BOOL CALLBACK Enum(LPGUID guid, LPCSTR desc, LPCSTR module, LPVOID ctx)
{
    UNREFERENCED_PARAMETER(module); UNREFERENCED_PARAMETER(ctx);
    if (guid && !dsFound && Uac2(desc) && !dsSkip--) { dsGuid = *guid; dsFound = 1; lstrcpyn(dsName, desc, sizeof(dsName)); }
    return TRUE;
}

static int Write(LPDIRECTSOUNDBUFFER b, DWORD at, DWORD count)
{
    LPVOID p1, p2; DWORD n1, n2;
    HRESULT hr = IDirectSoundBuffer_Lock(b, at, count, &p1, &n1, &p2, &n2, 0);
    if (FAILED(hr)) return 0;
    Fill((unsigned char *)p1, n1);
    if (p2) Fill((unsigned char *)p2, n2);
    IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    return 1;
}

static int DSound(WFX *w, unsigned seeks, unsigned ms, char variant, unsigned gap)
{
    LPDIRECTSOUND ds; LPDIRECTSOUNDBUFFER b; DSBUFFERDESC d; HRESULT hr;
    DWORD size = w->Format.nAvgBytesPerSec, wr = 0, play, wcur, next, room;
    unsigned k, bad = 0;
    dsSkip = Wanted();
    DirectSoundEnumerate(Enum, NULL);
    if (!dsFound) { Log("FAIL: no UAC2 DirectSound device"); return 1; }
    hr = DirectSoundCreate(&dsGuid, &ds, NULL);
    if (FAILED(hr)) { Log("FAIL: DirectSoundCreate %08lX", hr); return 1; }
    IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    ZeroMemory(&d, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    d.dwBufferBytes = size; d.lpwfxFormat = (WAVEFORMATEX *)w;
    hr = IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL);
    Log("ds \"%s\" tag=%u bits=%u rate=%lu buffer=%lu create=%08lX variant=%c gap=%u",
        dsName, w->Format.wFormatTag, fbits, w->Format.nSamplesPerSec, size, hr, variant, gap);
    if (FAILED(hr)) { IDirectSound_Release(ds); Log("FAIL: buffer"); return 1; }
    Log("start src=%lu", src);
    if (!Write(b, 0, size)) bad++;
    if (FAILED(IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING))) bad++;
    for (k = 0; k <= seeks; k++) {
        next = GetTickCount() + ms;
        while ((LONG)(GetTickCount() - next) < 0) {
            Sleep(10);
            if (FAILED(IDirectSoundBuffer_GetCurrentPosition(b, &play, &wcur))) { bad++; continue; }
            room = (play + size - wr) % size;
            room -= room % fbytes;
            if (room) { if (!Write(b, wr, room)) bad++; wr = (wr + room) % size; }
        }
        if (k == seeks) break;
        IDirectSoundBuffer_GetCurrentPosition(b, &play, &wcur);
        hr = IDirectSoundBuffer_Stop(b); if (FAILED(hr)) bad++;
        Log("stop %u src=%lu play=%lu (%08lX)", k, src, play, hr);
        Sleep(gap);
        if (variant != 'h') {
            hr = IDirectSoundBuffer_SetCurrentPosition(b, 0); if (FAILED(hr)) bad++;
            Log("setpos %u (%08lX)", k, hr);
            src += SEEK_STEP;
            Log("seek %u src=%lu", k, src);
            wr = 0;
            if (!Write(b, 0, size)) bad++;
        }
        hr = IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING); if (FAILED(hr)) bad++;
        Log("play %u (%08lX)", k, hr);
    }
    Log("end src=%lu stop=%08lX", src, IDirectSoundBuffer_Stop(b));
    IDirectSoundBuffer_Release(b);
    IDirectSound_Release(ds);
    Log(bad ? "FAIL: calls failed (%u)" : "PASS: calls succeeded", bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    WFX w;
    unsigned rate, seeks, ms, gap = 100;
    char variant;
    t0 = GetTickCount();
    if (argc < 6 || argc > 8) {
        printf("usage: UACSEEK wo|ds s16|f32|x16|x24|x32 <rate> <seeks> <ms> [variant [gap_ms]]\n");
        return 2;
    }
    rate = atoi(argv[3]); seeks = atoi(argv[4]); ms = atoi(argv[5]);
    if (argc >= 8) gap = atoi(argv[7]);
    if (!Format(&w, argv[2], rate) || rate < 8000 || rate > 384000 || seeks > 100 || ms < 100 || ms > 60000 ||
        gap > 10000) {
        printf("FAIL: arguments\n"); return 2;
    }
    com = CreateFile("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    src = 1000;
    {
        char name[MAXPNAMELEN];
        UINT dev = Device(name);
        DWORD old = 0;
        if (dev != (UINT)-1) {
            MMRESULT g = waveOutGetVolume((HWAVEOUT)(UINT_PTR)dev, &old);
            MMRESULT r = waveOutSetVolume((HWAVEOUT)(UINT_PTR)dev, 0xFFFFFFFFUL);
            Log("wave volume of dev %u was %08lX (%u), set to FFFFFFFF (%u)", dev, old, g, r);
        }
    }
    if (!lstrcmpi(argv[1], "wo")) {
        variant = argc >= 7 ? argv[6][0] : 'p';
        if (variant != 'p' && variant != 'r' && variant != 'h') { printf("FAIL: variant\n"); return 2; }
        return WaveOut(&w, seeks, ms, variant, gap);
    }
    if (!lstrcmpi(argv[1], "ds")) {
        variant = argc >= 7 ? argv[6][0] : 's';
        if (variant != 's' && variant != 'h') { printf("FAIL: variant\n"); return 2; }
        return DSound(&w, seeks, ms, variant, gap);
    }
    printf("FAIL: api\n");
    return 2;
}
