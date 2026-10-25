/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * comtest.c - COMTEST.EXE, the Win32 test tool of usbcom's guest legs
 * (docs/design/serial.md 11, "a Win32 test tool built with MSVC 6"). It
 * runs in a Windows 98 SE / ME guest against a COM port served by
 * USBCOM.SYS + USBCOMV.VXD, with this package's simulated CDC-ACM device
 * (acmemu.py --mirror) on the other end: the simulator echoes every byte,
 * drives DSR from DTR and DCD from RTS, and loops a sent break back.
 *
 *   COMTEST COMn full        the whole battery (baud rates and framing,
 *                            echo sizes, timeouts, DTR/RTS, events, break,
 *                            purge, TransmitCommChar, open/close)
 *   COMTEST COMn echo N      one echo of N bytes (N <= 65536)
 *   COMTEST COMn events      the event tests alone (overlapped handle)
 *   COMTEST COMn opens N     open, echo 64 bytes, close, N times
 *   COMTEST COMn quick       open, 115200 8N1, echo 1..1537 bytes, close
 *   COMTEST COMn hold S [W]  echo 64 bytes every 200 ms for S seconds,
 *                            surviving errors (the board is pulled and put
 *                            back meanwhile), close, then wait up to W
 *                            seconds (default 90) for the port to open
 *                            again and run the quick test on it
 *   COMTEST list             which of COM1-COM16 open, and the registry
 *                            values the driver writes
 *   COMTEST loadvxd PATH     has VXDLDR load a dynamic VxD by opening
 *                            \\.\PATH (Win32 dynamic VxD loading), then
 *                            closes it: shows whether the file loads
 *
 * Every line goes to the console, to A:\COMTEST.TXT (appended; -log path
 * changes it) and, unless -noe9, to QEMU's debug console port 0xE9 so it
 * interleaves with the drivers' trace. The last line is
 * "COMTEST RESULT: PASS ..." or "COMTEST RESULT: FAIL ...".
 *
 * Original code, written from the Win32 communications API documentation.
 * It only ever writes to the port it is told to test, and only a
 * test pattern.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <conio.h>

static char  g_LogPath[MAX_PATH] = "A:\\COMTEST.TXT";
static int   g_E9 = 1;
static int   g_Checks, g_Fails;
static char  g_Port[16];

/* ---- logging -------------------------------------------------------- */

static void E9(const char *s)
{
    if (!g_E9)
        return;
    __try {
        /* Windows 98 lets a Win32 program reach an I/O port no VxD
           traps; QEMU's debugcon is one. If it faults, stop using it. */
        _outp(0xE9, 'c'); _outp(0xE9, 't'); _outp(0xE9, ':'); _outp(0xE9, ' ');
        while (*s)
            _outp(0xE9, *s++);
        _outp(0xE9, '\n');
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_E9 = 0;
    }
}

static void Log(const char *fmt, ...)
{
    char line[512];
    va_list a;
    FILE *f;

    va_start(a, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, a);
    va_end(a);
    line[sizeof(line) - 1] = 0;
    printf("%s\n", line);
    fflush(stdout);
    E9(line);
    f = fopen(g_LogPath, "a");
    if (f) {
        fprintf(f, "%s\r\n", line);
        fclose(f);
    }
}

static void Check(int ok, const char *what)
{
    g_Checks++;
    if (!ok)
        g_Fails++;
    Log("%s %s", ok ? "PASS" : "FAIL", what);
}

/* ---- port helpers ---------------------------------------------------- */

static HANDLE Open(DWORD flags)
{
    char path[32];
    HANDLE h;
    wsprintf(path, "\\\\.\\%s", g_Port);
    h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE)                 /* 95-style plain name */
        h = CreateFile(g_Port, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, flags, NULL);
    return h;
}

static BOOL SetLine(HANDLE h, DWORD baud, BYTE bits, BYTE parity, BYTE stop)
{
    DCB d;
    memset(&d, 0, sizeof(d));
    d.DCBlength = sizeof(d);
    if (!GetCommState(h, &d))
        return FALSE;
    d.BaudRate = baud;
    d.ByteSize = bits;
    d.Parity = parity;
    d.StopBits = stop;
    d.fBinary = TRUE;
    d.fParity = parity != NOPARITY;
    d.fOutxCtsFlow = FALSE;
    d.fOutxDsrFlow = FALSE;
    d.fDtrControl = DTR_CONTROL_ENABLE;
    d.fRtsControl = RTS_CONTROL_ENABLE;
    d.fDsrSensitivity = FALSE;
    d.fOutX = FALSE;
    d.fInX = FALSE;
    d.fNull = FALSE;
    d.fAbortOnError = FALSE;
    if (!SetCommState(h, &d))
        return FALSE;
    memset(&d, 0, sizeof(d));
    d.DCBlength = sizeof(d);
    return GetCommState(h, &d) && d.BaudRate == baud && d.ByteSize == bits &&
           d.Parity == parity && d.StopBits == stop;
}

static void Timeouts(HANDLE h, DWORD interval, DWORD mult, DWORD constant)
{
    COMMTIMEOUTS t;
    t.ReadIntervalTimeout = interval;
    t.ReadTotalTimeoutMultiplier = mult;
    t.ReadTotalTimeoutConstant = constant;
    t.WriteTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant = 10000;
    SetCommTimeouts(h, &t);
}

static void Pattern(BYTE *b, DWORD n, DWORD seed, BYTE mask)
{
    DWORD i;
    for (i = 0; i < n; i++)
        b[i] = (BYTE)((seed * 31 + i * 7 + (i >> 8) + 1) & mask);
}

/* Reads exactly n bytes or until `ms` passes; returns the count. */
static DWORD ReadAll(HANDLE h, BYTE *b, DWORD n, DWORD ms, DWORD *err)
{
    DWORD got = 0, start = GetTickCount();
    *err = 0;
    while (got < n && GetTickCount() - start < ms) {
        DWORD r = 0;
        if (!ReadFile(h, b + got, n - got, &r, NULL)) {
            *err = GetLastError();
            ClearCommError(h, NULL, NULL);
            Sleep(10);
            continue;
        }
        got += r;
    }
    return got;
}

static BYTE g_Out[65536], g_In[65536 + 16];

/* After a failed echo: let the rest arrive, then drop it, so the next
   test starts clean. */
static void Drain(HANDLE h)
{
    Sleep(1000);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
}

/* Writes a pattern and reads it back. Returns 1 if every byte matched. */
static int Echo(HANDLE h, DWORD n, DWORD seed, BYTE mask, DWORD *ms, int quiet)
{
    DWORD w = 0, got, err = 0, t0 = GetTickCount(), extra = 0, e2;
    BOOL ok;

    Pattern(g_Out, n, seed, mask);
    memset(g_In, 0xEE, n + 16);
    Timeouts(h, MAXDWORD, MAXDWORD, 500);
    ok = WriteFile(h, g_Out, n, &w, NULL);
    if (!ok || w != n) {
        DWORD e = GetLastError(), errs = 0;
        ClearCommError(h, &errs, NULL);
        if (!quiet)
            Log("  write %lu: ok %d wrote %lu error %lu, comm errors %04lx, %lu ms", n, ok, w, e, errs,
                GetTickCount() - t0);
        Drain(h);
        return 0;
    }
    got = ReadAll(h, g_In, n, 10000 + n / 8, &err);
    if (ms)
        *ms = GetTickCount() - t0;
    if (got == n) {
        /* Nothing more may follow: a duplicate or stray byte is a failure. */
        Timeouts(h, MAXDWORD, 0, 0);
        ReadFile(h, g_In + n, 16, &extra, NULL);
    }
    if (got != n || memcmp(g_In, g_Out, n) != 0 || extra) {
        DWORD i = 0;
        while (i < got && g_In[i] == g_Out[i]) i++;
        e2 = err;
        if (!quiet)
            Log("  echo %lu: got %lu, first difference at %lu, %lu extra, read error %lu",
                n, got, i, extra, e2);
        Drain(h);
        return 0;
    }
    return 1;
}

/* ---- tests ------------------------------------------------------------- */

static const DWORD g_Sizes[] = { 1, 63, 64, 65, 128, 511, 512, 513, 1537, 4096 };

static int EchoSizes(HANDLE h, DWORD seed)
{
    int all = 1;
    unsigned i;
    for (i = 0; i < sizeof(g_Sizes) / sizeof(g_Sizes[0]); i++)
        if (!Echo(h, g_Sizes[i], seed + i, 0xFF, NULL, 0))
            all = 0;
    return all;
}

static void TestBauds(HANDLE h)
{
    static const DWORD bauds[] = { 300, 1200, 9600, 19200, 38400, 57600, 115200,
                                   230400, 460800, 921600, 1000000 };
    char what[96];
    unsigned i;
    for (i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) {
        BOOL set = SetLine(h, bauds[i], 8, NOPARITY, ONESTOPBIT);
        int echo = set && Echo(h, 200, bauds[i], 0xFF, NULL, 0) && Echo(h, 1537, i, 0xFF, NULL, 0);
        wsprintf(what, "baud %lu 8N1: set and read back %d, echo 200 + 1537 bytes %d",
                 bauds[i], set, echo);
        Check(set && echo, what);
    }
}

static void TestFraming(HANDLE h)
{
    static const struct { BYTE bits, parity, stop; const char *name; } f[] = {
        { 7, EVENPARITY, ONESTOPBIT, "7E1" }, { 8, ODDPARITY, ONESTOPBIT, "8O1" },
        { 8, NOPARITY, TWOSTOPBITS, "8N2" }, { 7, MARKPARITY, ONESTOPBIT, "7M1" },
        { 7, SPACEPARITY, ONE5STOPBITS, "7S1.5" }, { 6, NOPARITY, ONESTOPBIT, "6N1" },
        { 5, NOPARITY, ONE5STOPBITS, "5N1.5" }, { 8, NOPARITY, ONESTOPBIT, "8N1" } };
    char what[96];
    unsigned i;
    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        BOOL set = SetLine(h, 9600, f[i].bits, f[i].parity, f[i].stop);
        BYTE mask = (BYTE)((1u << f[i].bits) - 1);
        int echo = set && Echo(h, 64, i, mask, NULL, 0);
        wsprintf(what, "9600 %s: set and read back %d, echo 64 bytes %d", f[i].name, set, echo);
        Check(set && echo, what);
    }
    SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
}

static void TestSizes(HANDLE h)
{
    char what[96];
    DWORD ms = 0;
    int big;
    Check(EchoSizes(h, 100), "echo 1, 63, 64, 65, 128, 511, 512, 513, 1537, 4096 bytes");
    big = Echo(h, 16384, 7, 0xFF, &ms, 0);
    wsprintf(what, "echo 16384 bytes (%lu ms)", ms);
    Check(big, what);
    /* Larger than the receive queue: the echo must wait in the device. */
    big = Echo(h, 65536, 9, 0xFF, &ms, 0);
    wsprintf(what, "echo 65536 bytes, more than the receive queue holds (%lu ms, %lu bytes/s)",
             ms, ms ? 65536000 / ms : 0);
    Check(big, what);
}

static void TestTimeouts(HANDLE h)
{
    BYTE b[128];
    DWORD r, t0, el, w;
    char what[128];

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    Sleep(200);
    Timeouts(h, MAXDWORD, 0, 0);
    t0 = GetTickCount(); r = 99;
    ReadFile(h, b, sizeof(b), &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "timeouts MAXDWORD/0/0 on an empty queue: %lu bytes in %lu ms (now)", r, el);
    Check(r == 0 && el < 300, what);

    Timeouts(h, 0, 0, 500);
    t0 = GetTickCount(); r = 99;
    ReadFile(h, b, sizeof(b), &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "timeouts 0/0/500 on an empty queue: %lu bytes in %lu ms (about 500)", r, el);
    Check(r == 0 && el >= 400 && el < 2000, what);

    Timeouts(h, MAXDWORD, MAXDWORD, 800);
    t0 = GetTickCount(); r = 99;
    ReadFile(h, b, sizeof(b), &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "timeouts MAXDWORD/MAXDWORD/800 on an empty queue: %lu bytes in %lu ms (about 800)", r, el);
    Check(r == 0 && el >= 650 && el < 2500, what);

    Pattern(b, 5, 3, 0xFF);
    WriteFile(h, b, 5, &w, NULL);
    Sleep(300);
    t0 = GetTickCount(); r = 0;
    ReadFile(h, b, sizeof(b), &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "timeouts MAXDWORD/MAXDWORD/800 with 5 bytes waiting: %lu bytes in %lu ms (now)", r, el);
    Check(r == 5 && el < 300, what);

    Timeouts(h, 100, 0, 0);
    Pattern(b, 10, 4, 0xFF);
    WriteFile(h, b, 10, &w, NULL);
    t0 = GetTickCount(); r = 0;
    ReadFile(h, b, sizeof(b), &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "interval timeout 100 ms, 10 of 128 bytes arrive: %lu bytes in %lu ms", r, el);
    Check(r == 10 && el < 2000, what);

    Timeouts(h, 0, 10, 100);
    Pattern(b, 20, 5, 0xFF);
    WriteFile(h, b, 20, &w, NULL);
    t0 = GetTickCount(); r = 0;
    ReadFile(h, b, 20, &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "total timeout 10 ms/byte + 100 ms, 20 bytes asked and sent: %lu bytes in %lu ms", r, el);
    Check(r == 20 && el < 300, what);

    Timeouts(h, 0, 0, 0);
    Pattern(b, 32, 6, 0xFF);
    WriteFile(h, b, 32, &w, NULL);
    t0 = GetTickCount(); r = 0;
    ReadFile(h, b, 32, &r, NULL);
    el = GetTickCount() - t0;
    wsprintf(what, "no timeouts (0/0/0), 32 bytes asked and sent: %lu bytes in %lu ms", r, el);
    Check(r == 32, what);
}

static DWORD WaitModem(HANDLE h, DWORD mask, DWORD want, DWORD ms)
{
    DWORD s = 0, t0 = GetTickCount();
    do {
        GetCommModemStatus(h, &s);
        if ((s & mask) == want)
            return s;
        Sleep(20);
    } while (GetTickCount() - t0 < ms);
    return s;
}

static void TestLines(HANDLE h)
{
    static const struct { DWORD fn; DWORD mask, want; const char *name; } steps[] = {
        { CLRDTR, MS_DSR_ON, 0,          "CLRDTR: DSR drops" },
        { SETDTR, MS_DSR_ON, MS_DSR_ON,  "SETDTR: DSR rises" },
        { CLRRTS, MS_RLSD_ON, 0,         "CLRRTS: DCD drops" },
        { SETRTS, MS_RLSD_ON, MS_RLSD_ON, "SETRTS: DCD rises" },
        { CLRDTR, MS_DSR_ON | MS_RLSD_ON, MS_RLSD_ON, "CLRDTR with RTS up: DSR off, DCD on" },
        { CLRRTS, MS_DSR_ON | MS_RLSD_ON, 0, "CLRRTS with DTR down: both off" },
        { SETRTS, MS_DSR_ON | MS_RLSD_ON, MS_RLSD_ON, "SETRTS with DTR down: DCD only" },
        { SETDTR, MS_DSR_ON | MS_RLSD_ON, MS_DSR_ON | MS_RLSD_ON, "SETDTR: both on" } };
    DWORD s;
    char what[128];
    unsigned i;

    s = WaitModem(h, MS_DSR_ON | MS_RLSD_ON | MS_CTS_ON, MS_DSR_ON | MS_RLSD_ON | MS_CTS_ON, 2000);
    wsprintf(what, "after open (DTR and RTS asserted): modem status %02lx, CTS DSR DCD all on", s);
    Check((s & (MS_DSR_ON | MS_RLSD_ON | MS_CTS_ON)) == (MS_DSR_ON | MS_RLSD_ON | MS_CTS_ON), what);
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        BOOL ok = EscapeCommFunction(h, steps[i].fn);
        s = WaitModem(h, steps[i].mask, steps[i].want, 2000);
        wsprintf(what, "%s (escape %d, modem status %02lx, CTS %s)", steps[i].name, ok, s,
                 (s & MS_CTS_ON) ? "on" : "OFF");
        Check(ok && (s & steps[i].mask) == steps[i].want && (s & MS_CTS_ON), what);
    }
}

/* Events need an overlapped handle: the port is reopened for them. */
static void TestEvents(void)
{
    static const struct { int act; DWORD want; const char *name; } steps[] = {
        { 1, EV_DSR, "EV_DSR on CLRDTR" }, { 2, EV_DSR, "EV_DSR on SETDTR" },
        { 3, EV_RLSD, "EV_RLSD on CLRRTS" }, { 4, EV_RLSD, "EV_RLSD on SETRTS" },
        { 5, EV_BREAK, "EV_BREAK on a looped-back break" },
        { 6, EV_RXCHAR, "EV_RXCHAR on an echoed byte" },
        { 7, EV_TXEMPTY, "EV_TXEMPTY after a write" } };
    OVERLAPPED ov, wo;
    HANDLE h = Open(FILE_FLAG_OVERLAPPED);
    char what[128];
    unsigned i;

    if (h == INVALID_HANDLE_VALUE) {
        Check(0, "overlapped open for the event tests");
        return;
    }
    memset(&ov, 0, sizeof(ov));
    memset(&wo, 0, sizeof(wo));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    wo.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
    Sleep(300);
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        DWORD mask = 0, n = 0, w, errs = 0, wr;
        BOOL pend, got;
        BYTE c = 'E';
        COMSTAT cs;

        SetCommMask(h, EV_DSR | EV_RLSD | EV_BREAK | EV_RXCHAR | EV_TXEMPTY | EV_ERR);
        ResetEvent(ov.hEvent);
        pend = !WaitCommEvent(h, &mask, &ov) && GetLastError() == ERROR_IO_PENDING;
        switch (steps[i].act) {
        case 1: EscapeCommFunction(h, CLRDTR); break;
        case 2: EscapeCommFunction(h, SETDTR); break;
        case 3: EscapeCommFunction(h, CLRRTS); break;
        case 4: EscapeCommFunction(h, SETRTS); break;
        case 5: SetCommBreak(h); Sleep(100); ClearCommBreak(h); break;
        default:
            ResetEvent(wo.hEvent);
            if (!WriteFile(h, &c, 1, &wr, &wo) && GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(h, &wo, &wr, TRUE);
            break;
        }
        if (pend) {
            w = WaitForSingleObject(ov.hEvent, 3000);
            got = w == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &n, FALSE);
        } else {
            got = TRUE;                       /* completed at once */
        }
        ClearCommError(h, &errs, &cs);
        wsprintf(what, "%s (mask %04lx, comm errors %04lx, waited %d)", steps[i].name, mask, errs, pend);
        Check(got && (mask & steps[i].want), what);
        if (steps[i].act == 5) {
            /* Recorded, not checked: whether VCOMM hands CE_BREAK to the
               caller once EV_BREAK has been raised is VCOMM's business. */
            Log("INFO comm errors after the looped-back break: %04lx (CE_BREAK is 0010)", errs);
        }
        if (steps[i].act >= 6)
            PurgeComm(h, PURGE_RXCLEAR);
    }
    SetCommMask(h, 0);
    CloseHandle(ov.hEvent);
    CloseHandle(wo.hEvent);
    CloseHandle(h);
}

static void TestMisc(HANDLE h)
{
    COMMPROP cp;
    COMSTAT cs;
    DWORD errs, r, w, el;
    BYTE b[1024];
    char what[160];
    int ok;

    memset(&cp, 0, sizeof(cp));
    ok = GetCommProperties(h, &cp);
    wsprintf(what, "GetCommProperties: max baud %08lx, subtype %lu, capabilities %04lx, rx queue %lu, tx queue %lu",
             cp.dwMaxBaud, cp.dwProvSubType, cp.dwProvCapabilities, cp.dwCurrentRxQueue, cp.dwCurrentTxQueue);
    Check(ok && cp.dwProvSubType == PST_RS232, what);

    ok = SetupComm(h, 8192, 8192);
    wsprintf(what, "SetupComm 8192/8192 then echo (%d)", ok);
    Check(ok && EchoSizes(h, 300), what);

    /* PurgeComm(RX): echoed data waiting in the queue is dropped. */
    Pattern(b, sizeof(b), 11, 0xFF);
    Timeouts(h, MAXDWORD, 0, 0);
    WriteFile(h, b, sizeof(b), &w, NULL);
    Sleep(1000);
    ClearCommError(h, &errs, &cs);
    r = cs.cbInQue;
    PurgeComm(h, PURGE_RXCLEAR);
    ClearCommError(h, &errs, &cs);
    wsprintf(what, "PurgeComm(RXCLEAR): %lu bytes queued before, %lu after", r, cs.cbInQue);
    Check(r > 0 && cs.cbInQue == 0, what);
    Check(Echo(h, 300, 12, 0xFF, NULL, 0), "echo after the purge has no stale bytes");

    Check(PurgeComm(h, PURGE_TXCLEAR | PURGE_TXABORT | PURGE_RXABORT) &&
          Echo(h, 64, 13, 0xFF, NULL, 0), "PurgeComm(TX) then echo");

    ok = TransmitCommChar(h, 'Z');
    Timeouts(h, MAXDWORD, MAXDWORD, 2000);
    r = 0; b[0] = 0;
    ReadFile(h, b, 1, &r, NULL);
    wsprintf(what, "TransmitCommChar('Z') comes back (%d, %lu bytes, %02x)", ok, r, b[0]);
    Check(ok && r == 1 && b[0] == 'Z', what);

    ok = SetCommBreak(h);
    Sleep(200);
    ok = ClearCommBreak(h) && ok;
    ClearCommError(h, &errs, &cs);
    wsprintf(what, "SetCommBreak / ClearCommBreak (%d), then echo; errors seen %04lx", ok, errs);
    Check(ok && Echo(h, 64, 14, 0xFF, NULL, 0), what);

    /* A big write must not hold up reading (no CE_TXFULL deadlock). */
    ok = Echo(h, 30000, 15, 0xFF, &el, 0);
    wsprintf(what, "echo 30000 bytes in one write (%lu ms)", el);
    Check(ok, what);
}

static void TestOpenClose(void)
{
    HANDLE h, h2;
    char what[96];
    int i, ok = 1;
    DWORD e;

    h = Open(0);
    h2 = Open(0);
    e = GetLastError();
    wsprintf(what, "a second open of an open port fails (error %lu)", e);
    Check(h != INVALID_HANDLE_VALUE && h2 == INVALID_HANDLE_VALUE, what);
    if (h2 != INVALID_HANDLE_VALUE)
        CloseHandle(h2);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    for (i = 0; i < 5; i++) {
        h = Open(0);
        if (h == INVALID_HANDLE_VALUE) { ok = 0; Log("  open %d failed %lu", i, GetLastError()); break; }
        if (!SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT) || !Echo(h, 100, 20 + i, 0xFF, NULL, 0))
            ok = 0;
        CloseHandle(h);
    }
    Check(ok, "open, echo, close five times");
}

static int Quick(void)
{
    HANDLE h = Open(0);
    int ok;
    if (h == INVALID_HANDLE_VALUE) {
        Log("open %s failed: error %lu", g_Port, GetLastError());
        Check(0, "open");
        return 0;
    }
    Check(1, "open");
    ok = SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
    Check(ok, "115200 8N1 set and read back");
    Check(EchoSizes(h, 1), "echo 1, 63, 64, 65, 128, 511, 512, 513, 1537, 4096 bytes");
    Check(CloseHandle(h), "close");
    return 1;
}

static void Full(void)
{
    HANDLE h;
    DCB d;
    char what[160];

    h = Open(0);
    if (h == INVALID_HANDLE_VALUE) {
        Log("open %s failed: error %lu", g_Port, GetLastError());
        Check(0, "open");
        return;
    }
    memset(&d, 0, sizeof(d));
    d.DCBlength = sizeof(d);
    GetCommState(h, &d);
    wsprintf(what, "open; default DCB %lu baud, %u bits, parity %u, stop %u, DTR control %u, RTS control %u",
             d.BaudRate, d.ByteSize, d.Parity, d.StopBits, d.fDtrControl, d.fRtsControl);
    Check(1, what);
    Check(SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT), "115200 8N1 set and read back");
    TestSizes(h);
    TestBauds(h);
    TestFraming(h);
    TestTimeouts(h);
    TestLines(h);
    TestMisc(h);
    Check(CloseHandle(h), "close");
    TestEvents();
    TestOpenClose();
}

static void Hold(DWORD secs, DWORD wait)
{
    HANDLE h = Open(0);
    DWORD t0 = GetTickCount(), ok = 0, bad = 0, last = 2, n = 0, s, e;
    char what[128];

    if (h == INVALID_HANDLE_VALUE) {
        Log("open %s failed: error %lu", g_Port, GetLastError());
        Check(0, "open for the hold test");
        return;
    }
    SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
    Log("holding %s open for %lu s", g_Port, secs);
    while (GetTickCount() - t0 < secs * 1000) {
        int r = Echo(h, 64, n++, 0xFF, NULL, 1);
        s = 0;
        GetCommModemStatus(h, &s);
        if (r) ok++; else bad++;
        if ((DWORD)r != last) {
            e = GetLastError();
            Log("  t=%lu ms: echo now %s (modem status %02lx, last error %lu)", GetTickCount() - t0,
                r ? "works" : "fails", s, e);
            last = (DWORD)r;
        }
        Sleep(200);
    }
    wsprintf(what, "held open %lu s: %lu echoes passed, %lu failed; close", secs, ok, bad);
    Check(CloseHandle(h) && ok > 0, what);
    t0 = GetTickCount();
    do {
        h = Open(0);
        if (h != INVALID_HANDLE_VALUE)
            break;
        Sleep(1000);
    } while (GetTickCount() - t0 < wait * 1000);
    wsprintf(what, "the port opens again (after %lu ms)", GetTickCount() - t0);
    Check(h != INVALID_HANDLE_VALUE, what);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        Quick();
    }
}

static void List(void)
{
    char name[16], path[32], val[64], data[64];
    HKEY k, k2;
    DWORD i, nl, dl, type;
    for (i = 1; i <= 16; i++) {
        HANDLE h;
        wsprintf(name, "COM%lu", i);
        wsprintf(path, "\\\\.\\%s", name);
        Log("trying %s", name);
        h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            Log("%s opens", name);
            CloseHandle(h);
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            Log("%s: open error %lu", name, GetLastError());
        }
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Hardware\\DeviceMap\\SerialComm", 0, KEY_READ, &k) == 0) {
        for (i = 0; ; i++) {
            nl = sizeof(val); dl = sizeof(data);
            if (RegEnumValue(k, i, val, &nl, NULL, &type, (BYTE *)data, &dl) != 0) break;
            Log("SERIALCOMM %s = %s", val, data);
        }
        RegCloseKey(k);
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Enum\\USBCOM\\COMPORT", 0, KEY_READ, &k) == 0) {
        for (i = 0; ; i++) {
            char sub[128], full[200];
            nl = sizeof(sub);
            if (RegEnumKeyEx(k, i, sub, &nl, NULL, NULL, NULL, NULL) != 0) break;
            wsprintf(full, "Enum\\USBCOM\\COMPORT\\%s", sub);
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, full, 0, KEY_READ, &k2) == 0) {
                char pn[32] = "", fn[96] = "", drv[96] = "";
                dl = sizeof(pn); RegQueryValueEx(k2, "PortName", NULL, &type, (BYTE *)pn, &dl);
                dl = sizeof(fn); RegQueryValueEx(k2, "FriendlyName", NULL, &type, (BYTE *)fn, &dl);
                dl = sizeof(drv); RegQueryValueEx(k2, "Driver", NULL, &type, (BYTE *)drv, &dl);
                Log("Enum\\USBCOM\\COMPORT\\%s: PortName %s, FriendlyName %s, Driver %s", sub, pn, fn, drv);
                RegCloseKey(k2);
            }
        }
        RegCloseKey(k);
    }
}

int main(int argc, char **argv)
{
    int i, a = 1;
    const char *mode;

    while (a < argc && argv[a][0] == '-') {
        if (!lstrcmpi(argv[a], "-noe9")) g_E9 = 0;
        else if (!lstrcmpi(argv[a], "-log") && a + 1 < argc) lstrcpyn(g_LogPath, argv[++a], MAX_PATH);
        a++;
    }
    if (argc - a >= 2 && !lstrcmpi(argv[a], "loadvxd")) {
        char path[MAX_PATH];
        HANDLE h;
        wsprintf(path, "\\\\.\\%s", argv[a + 1]);
        Log("COMTEST loadvxd %s", path);
        h = CreateFile(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            Log("COMTEST RESULT: FAIL loadvxd error %lu", GetLastError());
            return 1;
        }
        CloseHandle(h);
        Log("COMTEST RESULT: PASS loadvxd: loaded, opened and closed");
        return 0;
    }
    if (argc - a >= 2 && !lstrcmpi(argv[a], "portkey")) {
        /* portkey VALUE [DATA]: in every Ports software key whose PortDriver
           is USBCOMV.VXD, set VALUE to the string DATA, or delete it. */
        HKEY k, k2;
        char sub[64], full[160], drv[64];
        DWORD i, nl, dl, type, done = 0;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\Class\\Ports", 0,
                         KEY_READ, &k) == 0) {
            for (i = 0; ; i++) {
                nl = sizeof(sub);
                if (RegEnumKeyEx(k, i, sub, &nl, NULL, NULL, NULL, NULL) != 0) break;
                wsprintf(full, "System\\CurrentControlSet\\Services\\Class\\Ports\\%s", sub);
                if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, full, 0, KEY_READ | KEY_WRITE, &k2) != 0) continue;
                dl = sizeof(drv); drv[0] = 0;
                RegQueryValueEx(k2, "PortDriver", NULL, &type, (BYTE *)drv, &dl);
                if (!lstrcmpi(drv, "USBCOMV.VXD") || !lstrcmpi(drv, "USBCOM.VXD")) { /* or the first build's name */
                    LONG r = argc - a >= 3
                        ? RegSetValueEx(k2, argv[a + 1], 0, REG_SZ, (BYTE *)argv[a + 2], lstrlen(argv[a + 2]) + 1)
                        : RegDeleteValue(k2, argv[a + 1]);
                    Log("%s: %s %s: %ld", full, argc - a >= 3 ? "set" : "deleted", argv[a + 1], r);
                    done++;
                }
                RegCloseKey(k2);
            }
            RegCloseKey(k);
        }
        Log("COMTEST RESULT: %s portkey, %lu keys", done ? "PASS" : "FAIL", done);
        return done ? 0 : 1;
    }
    if (a < argc && !lstrcmpi(argv[a], "list")) {
        Log("COMTEST list");
        List();
        Log("COMTEST RESULT: DONE list");
        return 0;
    }
    if (argc - a < 2) {
        printf("usage: COMTEST [-noe9] [-log file] COMn full|quick|hold secs [wait] | COMTEST list\n");
        return 2;
    }
    lstrcpyn(g_Port, argv[a], sizeof(g_Port));
    mode = argv[a + 1];
    Log("COMTEST %s %s (tick %lu)", g_Port, mode, GetTickCount());
    if (!lstrcmpi(mode, "full"))
        Full();
    else if (!lstrcmpi(mode, "quick"))
        Quick();
    else if (!lstrcmpi(mode, "events"))
        TestEvents();
    else if (!lstrcmpi(mode, "opens") && argc - a >= 3) {
        /* opens N: open, echo 64 bytes and close, N times. */
        DWORD n = (DWORD)atol(argv[a + 2]), i, bad = 0;
        char what[80];
        for (i = 0; i < n; i++) {
            HANDLE h = Open(0);
            if (h == INVALID_HANDLE_VALUE) {
                Log("  open %lu failed: error %lu", i + 1, GetLastError());
                bad++;
                continue;
            }
            if (!SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT) || !Echo(h, 64, i, 0xFF, NULL, 0)) {
                Log("  open %lu: echo failed", i + 1);
                bad++;
            }
            CloseHandle(h);
        }
        wsprintf(what, "%lu opens, each with a 64-byte echo: %lu failed", n, bad);
        Check(bad == 0, what);
    }
    else if (!lstrcmpi(mode, "closes") && argc - a >= 4) {
        /* closes N BYTES: open, write BYTES without reading them back, and
           close at once, N times, so every close cancels bulk INs while the
           echo is still coming back. Then open, let the rest arrive and drop
           it, and echo 4096 bytes: the port must still work. */
        DWORD n = (DWORD)atol(argv[a + 2]), bytes = (DWORD)atol(argv[a + 3]), i, bad = 0, w;
        char what[96];
        HANDLE h;
        if (bytes > sizeof(g_Out))
            bytes = sizeof(g_Out);
        for (i = 0; i < n; i++) {
            h = Open(0);
            if (h == INVALID_HANDLE_VALUE) {
                Log("  open %lu failed: error %lu", i + 1, GetLastError());
                bad++;
                continue;
            }
            SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
            Timeouts(h, MAXDWORD, MAXDWORD, 500);
            Pattern(g_Out, bytes, i, 0xFF);
            w = 0;
            if (!WriteFile(h, g_Out, bytes, &w, NULL) || w != bytes) {
                Log("  open %lu: write %lu wrote %lu, error %lu", i + 1, bytes, w, GetLastError());
                bad++;
            }
            CloseHandle(h);
        }
        wsprintf(what, "%lu closes, each %lu bytes after the write: %lu failed", n, bytes, bad);
        Check(bad == 0, what);
        h = Open(0);
        if (h != INVALID_HANDLE_VALUE) {
            SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
            Drain(h);
            Drain(h);
            Check(Echo(h, 4096, 91, 0xFF, NULL, 0), "after the closes: open and echo 4096 bytes");
            CloseHandle(h);
        } else
            Check(0, "after the closes: open");
    }
    else if (!lstrcmpi(mode, "write") && argc - a >= 3) {
        /* write N: one WriteFile of N bytes, no read (any port, e.g. COM1
           on serial.vxd as the reference for VCOMM's write semantics). */
        HANDLE h = Open(0);
        DWORD n = (DWORD)atol(argv[a + 2]), w = 0, t0, e;
        BOOL ok;
        if (h != INVALID_HANDLE_VALUE && n <= sizeof(g_Out)) {
            COMSTAT cs;
            DWORD errs = 0;
            SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
            Timeouts(h, 0, 0, 0);
            Pattern(g_Out, n, 1, 0xFF);
            t0 = GetTickCount();
            ok = WriteFile(h, g_Out, n, &w, NULL);
            e = GetLastError();
            ClearCommError(h, &errs, &cs);
            Log("  write %lu: ok %d, wrote %lu, error %lu, %lu ms, comm errors %04lx, out queue %lu",
                n, ok, w, e, GetTickCount() - t0, errs, cs.cbOutQue);
            Check(ok && w == n, "one WriteFile writes it all");
            CloseHandle(h);
        } else
            Check(0, "open");
    }
    else if (!lstrcmpi(mode, "echo") && argc - a >= 3) {
        HANDLE h = Open(0);
        DWORD ms = 0, n = (DWORD)atol(argv[a + 2]);
        char what[64];
        if (h != INVALID_HANDLE_VALUE && n <= sizeof(g_Out)) {
            SetLine(h, 115200, 8, NOPARITY, ONESTOPBIT);
            wsprintf(what, "echo %lu bytes", n);
            Check(Echo(h, n, 77, 0xFF, &ms, 0), what);
            Log("  %lu ms", ms);
            CloseHandle(h);
        } else
            Check(0, "open");
    }
    else if (!lstrcmpi(mode, "hold") && argc - a >= 3)
        Hold((DWORD)atol(argv[a + 2]), argc - a >= 4 ? (DWORD)atol(argv[a + 3]) : 90);
    else {
        Log("unknown mode %s", mode);
        return 2;
    }
    i = g_Fails ? 1 : 0;
    Log("COMTEST RESULT: %s %d checks, %d failed", i ? "FAIL" : "PASS", g_Checks, g_Fails);
    return i;
}
