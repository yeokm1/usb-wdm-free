/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * JOYTEST.EXE - guest reading tool for xpad98's legs (gamepad.md section
 * 11, "a joyGetPosEx dump tool"). Polls every WinMM joystick (ids 0-15)
 * with joyGetPosEx at about 100 Hz for N seconds and records, per id:
 * joyGetDevCaps (name, axes, buttons, POV), every button seen pressed,
 * each axis's minimum and maximum, the POV values seen, and the first
 * state changes. Written for the simulator's pattern (xpademu.py), which
 * walks every button, the d-pad and both ends of every axis: an id
 * PASSES when every button it reports was seen pressed, every axis it
 * reports reached both ends of its range (within 10 %), and, with a POV,
 * the four directions and centre were seen.
 *
 *   JOYTEST [seconds] [report file]      default 45 s, C:\JOYTEST.TXT
 *
 * Win32 only (WinMM), 98 SE to Vista. Original code. Reads no device
 * directly and writes only the report file.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXID 16
#define MAXPOV 16
#define MAXLOG 200

typedef struct {
    int present, capsOk;
    JOYCAPS caps;
    DWORD samples, changes;
    DWORD pressed;              /* buttons seen down */
    DWORD minv[6], maxv[6];
    DWORD pov[MAXPOV];
    int npov;
    JOYINFOEX last;
    int nlog;
    char log[MAXLOG][96];
} JSTATE;

static JSTATE J[MAXID];

static DWORD Axis(const JOYINFOEX *j, int a)
{
    switch (a) {
    case 0: return j->dwXpos;
    case 1: return j->dwYpos;
    case 2: return j->dwZpos;
    case 3: return j->dwRpos;
    case 4: return j->dwUpos;
    default: return j->dwVpos;
    }
}

static void Range(const JOYCAPS *c, int a, DWORD *lo, DWORD *hi)
{
    switch (a) {
    case 0: *lo = c->wXmin; *hi = c->wXmax; break;
    case 1: *lo = c->wYmin; *hi = c->wYmax; break;
    case 2: *lo = c->wZmin; *hi = c->wZmax; break;
    case 3: *lo = c->wRmin; *hi = c->wRmax; break;
    case 4: *lo = c->wUmin; *hi = c->wUmax; break;
    default: *lo = c->wVmin; *hi = c->wVmax; break;
    }
}

/* Which of X Y Z R U V the caps say exist (X and Y always). */
static int HasAxis(const JOYCAPS *c, int a)
{
    if (a < 2) return 1;
    if (a == 2) return (c->wCaps & JOYCAPS_HASZ) != 0;
    if (a == 3) return (c->wCaps & JOYCAPS_HASR) != 0;
    if (a == 4) return (c->wCaps & JOYCAPS_HASU) != 0;
    return (c->wCaps & JOYCAPS_HASV) != 0;
}

static void Sample(int id, const JOYINFOEX *j, DWORD t)
{
    JSTATE *s = &J[id];
    int a, i;
    if (!s->present) {
        s->present = 1;
        for (a = 0; a < 6; a++) { s->minv[a] = 0xFFFFFFFF; s->maxv[a] = 0; }
        s->capsOk = joyGetDevCaps(id, &s->caps, sizeof(s->caps)) == JOYERR_NOERROR;
        memset(&s->last, 0xFF, sizeof(s->last));
    }
    s->samples++;
    s->pressed |= j->dwButtons;
    for (a = 0; a < 6; a++) {
        DWORD v = Axis(j, a);
        if (v < s->minv[a]) s->minv[a] = v;
        if (v > s->maxv[a]) s->maxv[a] = v;
    }
    for (i = 0; i < s->npov; i++)
        if (s->pov[i] == j->dwPOV) break;
    if (i == s->npov && s->npov < MAXPOV)
        s->pov[s->npov++] = j->dwPOV;
    if (j->dwButtons != s->last.dwButtons || j->dwXpos != s->last.dwXpos || j->dwYpos != s->last.dwYpos ||
        j->dwZpos != s->last.dwZpos || j->dwRpos != s->last.dwRpos || j->dwUpos != s->last.dwUpos ||
        j->dwVpos != s->last.dwVpos || j->dwPOV != s->last.dwPOV) {
        s->changes++;
        if (s->nlog < MAXLOG)
            sprintf(s->log[s->nlog++], "%6lu ms b=%04lx x=%5lu y=%5lu z=%5lu r=%5lu u=%5lu v=%5lu pov=%lu",
                    t, j->dwButtons, j->dwXpos, j->dwYpos, j->dwZpos, j->dwRpos, j->dwUpos, j->dwVpos, j->dwPOV);
        s->last = *j;
    }
}

static int Verdict(int id, FILE *f)
{
    JSTATE *s = &J[id];
    const JOYCAPS *c = &s->caps;
    int ok = 1, a, i;
    DWORD want, lo, hi, span;
    if (!s->capsOk) {
        fprintf(f, "  joyGetDevCaps failed\n");
        return 0;
    }
    want = c->wNumButtons >= 32 ? 0xFFFFFFFF : ((1UL << c->wNumButtons) - 1);
    fprintf(f, "  buttons: %u reported, seen pressed %08lx (want %08lx) %s\n", c->wNumButtons,
            s->pressed, want, (s->pressed & want) == want ? "ok" : "MISSING");
    if ((s->pressed & want) != want) ok = 0;
    for (a = 0; a < 6; a++) {
        if (!HasAxis(c, a)) continue;
        Range(c, a, &lo, &hi);
        span = (hi - lo) / 10;
        fprintf(f, "  axis %c: range %lu..%lu, seen %lu..%lu %s\n", "XYZRUV"[a], lo, hi, s->minv[a], s->maxv[a],
                (s->minv[a] <= lo + span && s->maxv[a] >= hi - span) ? "ok" : "NOT FULL");
        if (!(s->minv[a] <= lo + span && s->maxv[a] >= hi - span)) ok = 0;
    }
    if (c->wCaps & JOYCAPS_HASPOV) {
        static const DWORD need[5] = { 0, 9000, 18000, 27000, 0xFFFF };
        int k;
        fprintf(f, "  POV seen:");
        for (i = 0; i < s->npov; i++) fprintf(f, " %lu", s->pov[i]);
        for (k = 0; k < 5; k++) {
            for (i = 0; i < s->npov; i++) if (s->pov[i] == need[k] || (need[k] == 0xFFFF && s->pov[i] == 0xFFFFFFFF)) break;
            if (i == s->npov) { ok = 0; fprintf(f, " [missing %lu]", need[k]); }
        }
        fprintf(f, "\n");
    } else {
        fprintf(f, "  no POV reported\n");
    }
    return ok;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int seconds = 45, id, found = 0, passed = 0, i;
    char path[MAX_PATH] = "C:\\JOYTEST.TXT", msg[512];
    DWORD start, now;
    FILE *f;
    UNREFERENCED_PARAMETER(inst); UNREFERENCED_PARAMETER(prev); UNREFERENCED_PARAMETER(show);
    if (cmd && *cmd) {
        char *p = cmd;
        seconds = atoi(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p) { strncpy(path, p, sizeof(path) - 1); path[sizeof(path) - 1] = 0; }
        if (seconds <= 0) seconds = 45;
    }
    timeBeginPeriod(1);
    start = timeGetTime();
    do {
        for (id = 0; id < MAXID; id++) {
            JOYINFOEX j;
            memset(&j, 0, sizeof(j));
            j.dwSize = sizeof(j);
            j.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(id, &j) == JOYERR_NOERROR)
                Sample(id, &j, timeGetTime() - start);
        }
        Sleep(10);
        now = timeGetTime();
    } while (now - start < (DWORD)seconds * 1000);
    timeEndPeriod(1);

    f = fopen(path, "wt");
    if (!f) { MessageBox(NULL, "Cannot write the report file", "JOYTEST", MB_OK); return 2; }
    fprintf(f, "JOYTEST %d s, joyGetNumDevs %u\n", seconds, joyGetNumDevs());
    for (id = 0; id < MAXID; id++) {
        JSTATE *s = &J[id];
        int ok;
        if (!s->present) continue;
        found++;
        fprintf(f, "id %d: \"%s\" MID %u PID %u, %u axes (max %u), %u buttons, caps %04x, %lu samples, %lu changes\n",
                id, s->capsOk ? s->caps.szPname : "?", s->caps.wMid, s->caps.wPid, s->caps.wNumAxes,
                s->caps.wMaxAxes, s->caps.wNumButtons, s->caps.wCaps, s->samples, s->changes);
        ok = Verdict(id, f);
        fprintf(f, "  %s\n", ok ? "PASS" : "FAIL");
        if (ok) passed++;
        for (i = 0; i < s->nlog; i++) fprintf(f, "  %s\n", s->log[i]);
    }
    fprintf(f, "%s: %d joystick id(s), %d passed\n", found && passed == found ? "PASS" : "FAIL", found, passed);
    fclose(f);
    sprintf(msg, "%s: %d joystick id(s) answered, %d passed. Report: %s",
            found && passed == found ? "PASS" : "FAIL", found, passed, path);
    MessageBox(NULL, msg, "JOYTEST", MB_OK);
    return found && passed == found ? 0 : 1;
}
