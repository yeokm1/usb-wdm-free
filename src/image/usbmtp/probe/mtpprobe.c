/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mtpprobe.c - MTPPROBE.EXE, the M0 reading tool of docs/design/mtp.md
 * (sections 3, 5.6 and 12). Not shipped.
 *
 *   MTPPROBE /drv <test> [file]: the MTPPRB.SYS tests (below, at Drv).
 *
 *   MTPPROBE /pnp [VID:PID] [file]
 *     Writes, for every present device node whose hardware ids contain
 *     USB\ (or the VID:PID given), its instance id, hardware ids,
 *     compatible ids, description, class, service, driver key, maker,
 *     location, its parent's instance id and its Configuration Manager
 *     status and problem, to file (default A:\MTPPROBE.TXT). Read-only:
 *     it installs, removes and changes nothing.
 *
 * Published SetupAPI and Configuration Manager calls only; every import
 * is in 98 SE's and ME's export tables (setupapi, cfgmgr32 loaded at run
 * time, kernel32, user32).
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

typedef DWORD (WINAPI *CM_STATUS_FN)(PULONG, PULONG, DWORD, ULONG);
typedef DWORD (WINAPI *CM_PARENT_FN)(PDWORD, DWORD, ULONG);
typedef DWORD (WINAPI *CM_ID_FN)(DWORD, PCHAR, ULONG, ULONG);

static char g_Report[65536];

static void Put(const char *s)
{
    if (lstrlen(g_Report) + lstrlen(s) < (int)sizeof(g_Report) - 1) {
        lstrcat(g_Report, s);
    }
}

static void PutMulti(const char *label, const char *multi, DWORD bytes)
{
    const char *p = multi;
    const char *end = multi + bytes;
    Put("  ");
    Put(label);
    Put(":");
    if (!bytes || !*p) {
        Put(" (none)");
    }
    while (p < end && *p) {
        Put(" ");
        Put(p);
        p += lstrlen(p) + 1;
    }
    Put("\r\n");
}

static void Upper(char *s)
{
    for (; *s; s++) {
        if (*s >= 'a' && *s <= 'z') {
            *s = (char)(*s - 'a' + 'A');
        }
    }
}

/* "1234:abcd" -> "VID_1234&PID_ABCD"; 0 if malformed. */
static int Target(const char *arg, char *match)
{
    int i;
    char c;
    if (lstrlen(arg) != 9 || arg[4] != ':') {
        return 0;
    }
    lstrcpy(match, "VID_0000&PID_0000");
    for (i = 0; i < 9; i++) {
        if (i == 4) {
            continue;
        }
        c = arg[i];
        if (c >= 'a' && c <= 'f') {
            c = (char)(c - 'a' + 'A');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
        match[i < 4 ? i + 4 : i + 8] = c;
    }
    return 1;
}

static char *NextArg(char **p)
{
    char *s = *p, *start;
    while (*s == ' ') {
        s++;
    }
    if (!*s) {
        *p = s;
        return NULL;
    }
    start = s;
    while (*s && *s != ' ') {
        s++;
    }
    if (*s) {
        *s++ = 0;
    }
    *p = s;
    return start;
}

static int Pnp(const char *match, const char *file)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD i, type, bytes, status, problem, cr, parent;
    char hw[4096], up[4096], buf[2048], line[2600];
    HMODULE cfg;
    CM_STATUS_FN cmStatus;
    CM_PARENT_FN cmParent;
    CM_ID_FN cmId;
    HANDLE h;
    int found = 0;
    static const struct { DWORD Prop; const char *Name; } props[] = {
        { SPDRP_DEVICEDESC, "description" }, { SPDRP_CLASS, "class" },
        { SPDRP_SERVICE, "service" }, { SPDRP_DRIVER, "driver key" },
        { SPDRP_MFG, "maker" }, { SPDRP_LOCATION_INFORMATION, "location" },
    };
    int k;

    cfg = LoadLibrary("cfgmgr32.dll");
    cmStatus = cfg ? (CM_STATUS_FN)GetProcAddress(cfg, "CM_Get_DevNode_Status") : NULL;
    cmParent = cfg ? (CM_PARENT_FN)GetProcAddress(cfg, "CM_Get_Parent") : NULL;
    cmId = cfg ? (CM_ID_FN)GetProcAddress(cfg, "CM_Get_Device_IDA") : NULL;
    wsprintf(line, "MTPPROBE /pnp %s\r\n", match[0] ? match : "(every USB device node)");
    Put(line);
    set = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        Put("SetupDiGetClassDevs failed\r\n");
    } else {
        for (i = 0;; i++) {
            ZeroMemory(&dev, sizeof(dev));
            dev.cbSize = sizeof(dev);
            if (!SetupDiEnumDeviceInfo(set, i, &dev)) {
                break;
            }
            ZeroMemory(hw, sizeof(hw));
            if (!SetupDiGetDeviceRegistryProperty(set, &dev, SPDRP_HARDWAREID, &type, (PBYTE)hw,
                                                  sizeof(hw) - 2, &bytes)) {
                bytes = 0;
            }
            CopyMemory(up, hw, sizeof(up));
            {
                /* search every string of the multi-sz */
                char *p = up;
                int hit = 0;
                while (*p) {
                    Upper(p);
                    if ((match[0] && strstr(p, match)) || (!match[0] && !strncmp(p, "USB\\", 4))) {
                        hit = 1;
                    }
                    p += lstrlen(p) + 1;
                }
                if (!hit) {
                    continue;
                }
            }
            found++;
            buf[0] = 0;
            SetupDiGetDeviceInstanceId(set, &dev, buf, sizeof(buf), NULL);
            wsprintf(line, "device %s\r\n", buf);
            Put(line);
            PutMulti("hardware ids", hw, bytes);
            ZeroMemory(hw, sizeof(hw));
            if (!SetupDiGetDeviceRegistryProperty(set, &dev, SPDRP_COMPATIBLEIDS, &type, (PBYTE)hw,
                                                  sizeof(hw) - 2, &bytes)) {
                bytes = 0;
            }
            PutMulti("compatible ids", hw, bytes);
            for (k = 0; k < (int)(sizeof(props) / sizeof(props[0])); k++) {
                buf[0] = 0;
                if (SetupDiGetDeviceRegistryProperty(set, &dev, props[k].Prop, &type, (PBYTE)buf,
                                                     sizeof(buf) - 1, NULL)) {
                    wsprintf(line, "  %s: %s\r\n", props[k].Name, buf);
                    Put(line);
                }
            }
            if (cmParent && cmId && cmParent(&parent, dev.DevInst, 0) == 0) {
                buf[0] = 0;
                cmId(parent, buf, sizeof(buf), 0);
                wsprintf(line, "  parent: %s\r\n", buf);
                Put(line);
            }
            if (cmStatus) {
                status = problem = 0;
                cr = cmStatus(&status, &problem, dev.DevInst, 0);
                wsprintf(line, "  CM: result %lu, status %08lX, problem %lu\r\n", cr, status, problem);
                Put(line);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    wsprintf(line, "%d device node(s)\r\n", found);
    Put(line);
    h = CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 3;
    }
    WriteFile(h, g_Report, (DWORD)lstrlen(g_Report), &bytes, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    return 0;
}

/* ---------------------------------------------------------------- /drv: MTPPRB.SYS tests
 *
 *   MTPPROBE /drv <test> [file]     (file default COM1; one line per fact)
 *
 * Three handles: A carries the call under test, B the stop, and O (an
 * observer) the driver's state and counters, so no reading depends on a
 * handle that has a call outstanding. Before every stop the request is
 * confirmed pending in the stack through O (its lower IRP out and its
 * IoCallDriver returned); without that confirmation the stop is not made
 * and the line says so. A call's result is reported only once its thread
 * has returned; a stop reports the request it stopped (0: none).
 *
 *   status      each status of the driver's list: DeviceIoControl's result,
 *               GetLastError and the byte count Win32 reports
 *   state       the driver's started flag and slot states
 *   counters    the driver's counters (creates, cleanups, closes, late IRPs,
 *               dropped actions, REMOVE received, deletes, leaked removals)
 *   cancel      a 512-byte read pending on A; IoCancelIrp of its lower IRP
 *               from B; the read's result
 *   abort       the same, ending it with URB_FUNCTION_ABORT_PIPE
 *   close       the same, ending it with CloseHandle(A) from the main thread
 *   intcancel   an interrupt read pending on A, IoCancelIrp from B
 *   intabort    an interrupt read pending on A, the abort from B
 *   ctlcancel   a Get Device Status pending on A (an unanswering device),
 *               IoCancelIrp of the control URB from B
 *   resetcancel a reset of the bulk IN pipe on A, IoCancelIrp of it from B
 *               if it is confirmed pending (else its result as it came)
 *   exit        a read pending on A and an interrupt read pending on B, both
 *               shown pending by one state observation through O, taken
 *               again just before the action (reported), then ExitProcess:
 *               the driver's port E9h log shows the order
 *   terminate   the same, ending with TerminateProcess of this process
 *   outcancel   a 64-byte bulk OUT write pending on A (the simulator's
 *               out_hang fault holds it), IoCancelIrp of it from B
 *   outabort    the same, ending it with URB_FUNCTION_ABORT_PIPE
 *   samehandle  on ONE handle A: an interrupt read pending; then the state
 *               control on A (does it return?); then a Get Device Status on
 *               A (held by the simulator's ep0_hang: does a second call on
 *               the same handle reach the driver and stay pending beside the
 *               first?); both stopped through O
 *   waitclose   the software wait (IOCTL_PRB_WAIT) pending on A, then
 *               CloseHandle(A) from the main thread; a helper thread stops
 *               the wait through O after 20 s; when CloseHandle returned
 *   waitexit    the software wait pending on A and a read pending on B (one
 *               observation), then ExitProcess
 *   hold        a read pending on A and the software wait pending on B (one
 *               observation), then nothing for 600 s: end the program from
 *               the Close Program dialog (Ctrl+Alt+Del, End Task)
 *   unplugall   a read on A, a bulk OUT write on B (out_hang), a Get Device
 *               Status on D (ep0_hang) and the software wait on O2, all
 *               pending in one observation; then the device is unplugged;
 *               when REMOVE came (up to 600 s) and each call's result; if
 *               it did not come, the calls are stopped through O
 *   unplugctl   the same with only the Get Device Status pending
 *   unplugout   the same with only the bulk OUT write pending
 *   unplugin    the same with the read and the software wait pending
 *   serial      whether NTKERN serialises calls across handles: a read
 *               pending on A; then a read on B (does it reach the driver,
 *               which refuses it busy, or is it held?); a pending interrupt
 *               read on B (does an IOCTL get through?); the state through
 *               O; then A's read cancelled through O and what B's calls do
 *   zombie      handles A, B and O kept open across an unplug and a plug of
 *               a device (the same or another): REMOVE confirmed through O,
 *               a new handle N opened on the new device, then a read and
 *               the state control on A, the counters on B and O; the
 *               counters through N must show no IRP of an old handle
 *               reached the new device (foreign 0); then A, B and O closed
 *               and the old device's deletion read through N
 *   afterremove a read pending on A; the device is then unplugged from
 *               outside (the emulator's device_del or a usbredir disconnect;
 *               disabling it in Device Manager sends STOP, not REMOVE) within
 *               120 s; removal is confirmed by a rise of the driver's counters
 *               O (REMOVE received, and the device deleted or its removal
 *               leaked); only then, through the handles kept open, a read and
 *               the state control on A, the counters on B, the three
 *               CloseHandle calls and a new open
 */
#include <winioctl.h>
#include "mtpprb.h"

static HANDLE g_Out;

static void Say(const char *s)
{
    DWORD n;
    if (g_Out != INVALID_HANDLE_VALUE) {
        WriteFile(g_Out, s, (DWORD)lstrlen(s), &n, NULL);
    }
}

static HANDLE OpenPrb(void)
{
    return CreateFile("\\\\.\\MTPPRB0", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
}

#define CODE_WRITE 1    /* BLOCKED.Code: WriteFile (control codes are larger) */

typedef struct BLOCKED {
    HANDLE H;
    DWORD Code;         /* 0: ReadFile; else the control */
    DWORD In;           /* the control's input ULONG, when InLen */
    DWORD InLen;
    DWORD Len;
    BOOL Ok;
    DWORD Err, Bytes, Ms;
    char Buf[512];
} BLOCKED;

static DWORD WINAPI Blocker(LPVOID p)
{
    BLOCKED *b = (BLOCKED *)p;
    DWORD t0 = GetTickCount();
    b->Bytes = 0;
    if (b->Code == CODE_WRITE) {
        b->Ok = WriteFile(b->H, b->Buf, b->Len, &b->Bytes, NULL);
    } else if (b->Code) {
        b->Ok = DeviceIoControl(b->H, b->Code, b->InLen ? &b->In : NULL, b->InLen, b->Len ? b->Buf : NULL, b->Len,
                                &b->Bytes, NULL);
    } else {
        b->Ok = ReadFile(b->H, b->Buf, b->Len, &b->Bytes, NULL);
    }
    b->Err = b->Ok ? 0 : GetLastError();
    b->Ms = GetTickCount() - t0;
    return 0;
}

/* Start a call on its own thread; NULL (reported) if the thread could not
   start. */
static HANDLE Begin(const char *test, BLOCKED *b, HANDLE h, DWORD code, DWORD len, DWORD in, DWORD inLen)
{
    DWORD tid;
    HANDLE th;
    char line[160];
    ZeroMemory(b, sizeof(*b));
    b->H = h;
    b->Code = code;
    b->Len = len;
    b->In = in;
    b->InLen = inLen;
    th = CreateThread(NULL, 0, Blocker, b, 0, &tid);
    if (!th) {
        wsprintf(line, "drv %s: CreateThread failed, error %lu\r\n", test, GetLastError());
        Say(line);
    }
    return th;
}

/* The call's result, only once its thread has returned. */
static void Report(const char *test, const char *what, BLOCKED *b, HANDLE th, DWORD ms)
{
    char line[200];
    DWORD wait = th ? WaitForSingleObject(th, ms) : WAIT_FAILED;
    if (wait == WAIT_OBJECT_0) {
        wsprintf(line, "drv %s: %s returned, ok %d, error %lu, bytes %lu, %lu ms\r\n", test, what, b->Ok, b->Err,
                 b->Bytes, b->Ms);
    } else {
        wsprintf(line, "drv %s: %s %s after %lu ms (wait %lu); no result\r\n", test, what,
                 th ? "still blocked" : "never started", ms, wait);
    }
    Say(line);
}

/* The driver's state (IOCTL_PRB_STATE) through h; FALSE on failure. */
static BOOL State(HANDLE h, DWORD *v)
{
    DWORD n = 0;
    return DeviceIoControl(h, IOCTL_PRB_STATE, NULL, 0, v, PRB_STATE_ULONGS * sizeof(DWORD), &n, NULL) &&
           n == PRB_STATE_ULONGS * sizeof(DWORD);
}

/* Poll the state through the observer o until slot `which` holds a request
   pending in the stack (active, lower IRP out, no pin) or the call's thread
   th has ended, up to 5 s. Returns the request number, or 0 (reported). */
static DWORD WaitPending(const char *test, HANDLE o, DWORD which, HANDLE th)
{
    DWORD v[PRB_STATE_ULONGS], t0 = GetTickCount(), st = 0, req = 0, word = 0;
    BOOL seen = FALSE;
    char line[200];
    for (;;) {
        if (State(o, v)) {
            seen = TRUE;
            st = v[1 + which * 3];
            req = v[2 + which * 3];
            word = v[3 + which * 3];
            if (st == 1 && (which == PRB_WAIT ? (word & 0xFF00) != 0 : (word & 1) != 0) && !(word & 0xFF0000)) {
                wsprintf(line, "drv %s: slot %lu request %lu pending in the stack after %lu ms\r\n", test, which, req,
                         GetTickCount() - t0);
                Say(line);
                return req;
            }
        }
        if (!th || WaitForSingleObject(th, 0) == WAIT_OBJECT_0 || GetTickCount() - t0 > 5000) {
            break;
        }
        Sleep(10);
    }
    wsprintf(line, "drv %s: slot %lu NOT confirmed pending (state read %d, state %lu, request %lu, word %08lX, call %s)\r\n",
             test, which, seen, st, req, word,
             !th ? "never started" : WaitForSingleObject(th, 0) == WAIT_OBJECT_0 ? "ended" : "running");
    Say(line);
    return 0;
}

/* In one state observation v: slot `which` holds a request pending in the
   stack (active, lower IRP out, no pin). */
static BOOL Pend(const DWORD *v, DWORD which)
{
    DWORD word = v[3 + which * 3];
    return v[1 + which * 3] == 1 && (which == PRB_WAIT ? (word & 0xFF00) != 0 : (word & 1) != 0) &&
           !(word & 0xFF0000);
}

/* What one observation showed, for the bulk IN and interrupt slots. */
static void SayBoth(const char *test, const char *what, BOOL ok, const DWORD *v)
{
    char line[300];
    wsprintf(line, "drv %s: %s: state read %d; bin state %lu request %lu word %08lX; int state %lu request %lu "
             "word %08lX\r\n", test, what, ok, v[1], v[2], v[3], v[4], v[5], v[6]);
    Say(line);
}

/* What one observation showed for all four slots. */
static void SayAll(const char *test, const char *what, BOOL ok, const DWORD *v)
{
    char line[400];
    wsprintf(line, "drv %s: %s: state read %d; bin %lu/%lu/%08lX; int-or-out %lu/%lu/%08lX; ctl %lu/%lu/%08lX; "
             "wait %lu/%lu/%08lX\r\n", test, what, ok, v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
             v[10], v[11], v[12]);
    Say(line);
}

/* Poll through o until every slot in Mask (bit = PRB_* value) is pending in
   one observation, a thread in Th[] ends, or 5 s pass; v holds the last
   observation. TRUE if all were seen pending together. */
static BOOL WaitAll(HANDLE o, DWORD Mask, HANDLE *Th, int N, DWORD *v)
{
    DWORD t0 = GetTickCount(), w;
    BOOL ok;
    int i, ended;
    for (;;) {
        ZeroMemory(v, PRB_STATE_ULONGS * sizeof(DWORD));
        ok = State(o, v);
        if (ok) {
            for (w = 0; w < 4; w++) {
                if ((Mask & (1UL << w)) && !Pend(v, w)) {
                    break;
                }
            }
            if (w == 4) {
                return TRUE;
            }
        }
        for (ended = 0, i = 0; i < N; i++) {
            if (!Th[i] || WaitForSingleObject(Th[i], 0) == WAIT_OBJECT_0) {
                ended = 1;
            }
        }
        if (ended || GetTickCount() - t0 > 5000) {
            return FALSE;
        }
        Sleep(10);
    }
}

static HANDLE g_StopO;

/* waitclose's helper: after 20 s, stop the software wait through O. */
static DWORD WINAPI StopWaitLater(LPVOID p)
{
    DWORD which = PRB_WAIT, req = 0, n = 0;
    char line[160];
    (void)p;
    Sleep(20000);
    DeviceIoControl(g_StopO, IOCTL_PRB_CANCEL, &which, sizeof(which), &req, sizeof(req), &n, NULL);
    wsprintf(line, "drv waitclose: after 20 s, the wait stopped through O (request %lu)\r\n", n ? req : 0);
    Say(line);
    return 0;
}

/* IOCTL_PRB_CANCEL or IOCTL_PRB_ABORT of slot `which` through h. */
static void StopOn(const char *test, HANDLE h, DWORD code, DWORD which)
{
    DWORD req = 0, n = 0;
    BOOL ok;
    char line[200];
    ok = DeviceIoControl(h, code, &which, sizeof(which), &req, sizeof(req), &n, NULL);
    wsprintf(line, "drv %s: %s of slot %lu on B: ok %d, error %lu, request stopped %lu\r\n", test,
             code == IOCTL_PRB_ABORT ? "abort" : "cancel", which, ok, ok ? 0 : GetLastError(), n ? req : 0);
    Say(line);
}

/* The counters through h into v (eight); FALSE on failure. */
static BOOL GetCounters(HANDLE h, DWORD *v)
{
    DWORD n = 0;
    ZeroMemory(v, PRB_COUNTERS_BYTES);
    return DeviceIoControl(h, IOCTL_PRB_COUNTERS, NULL, 0, v, PRB_COUNTERS_BYTES, &n, NULL) &&
           n == PRB_COUNTERS_BYTES;
}

static void SayCounters(const char *test, const char *what, BOOL ok, const DWORD *v)
{
    char line[300];
    wsprintf(line, "drv %s: %s ok %d, error %lu, creates %lu, cleanups %lu, closes %lu, late %lu, dropped %lu, "
             "removes %lu, deletes %lu, leaks %lu, zombies %lu, foreign %lu\r\n", test, what, ok,
             ok ? 0 : GetLastError(), v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    Say(line);
}

static void SayClose(const char *test, const char *what, HANDLE h)
{
    char line[160];
    BOOL ok = CloseHandle(h);
    wsprintf(line, "drv %s: CloseHandle(%s) %d, error %lu\r\n", test, what, ok, ok ? 0 : GetLastError());
    Say(line);
}

static int Drv(const char *test, const char *file)
{
    HANDLE a, b2, o, th, th2;
    BLOCKED blk, blk2, io[3];
    DWORD n, which, i, out[4], v[PRB_STATE_ULONGS], c[PRB_COUNTERS_BYTES / 4], base[PRB_COUNTERS_BYTES / 4], t0;
    DWORD r1, r2, k, which2;
    HANDLE d, th3, th4;
    BLOCKED blk3, blk4;
    HANDLE nh;
    BLOCKED sb[2];
    BOOL ok;
    char line[300];

    g_Out = CreateFile(file, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Out != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_Out, 0, NULL, FILE_END);
    }
    a = OpenPrb();
    b2 = OpenPrb();
    o = OpenPrb();
    wsprintf(line, "drv %s: open A %s, B %s, O %s (error %lu)\r\n", test, a == INVALID_HANDLE_VALUE ? "failed" : "ok",
             b2 == INVALID_HANDLE_VALUE ? "failed" : "ok", o == INVALID_HANDLE_VALUE ? "failed" : "ok",
             GetLastError());
    Say(line);
    if (a == INVALID_HANDLE_VALUE || b2 == INVALID_HANDLE_VALUE || o == INVALID_HANDLE_VALUE) {
        return 3;
    }
    if (!lstrcmpi(test, "status")) {
        for (i = 0; i < 10; i++) {
            n = 99;
            ok = DeviceIoControl(a, IOCTL_PRB_STATUS, &i, sizeof(i), out, sizeof(out), &n, NULL);
            wsprintf(line, "drv status %lu: ok %d, error %lu, bytes %lu\r\n", i, ok, ok ? 0 : GetLastError(), n);
            Say(line);
        }
    } else if (!lstrcmpi(test, "counters")) {
        ok = GetCounters(o, c);
        SayCounters(test, "counters", ok, c);
    } else if (!lstrcmpi(test, "state")) {
        ZeroMemory(v, sizeof(v));
        ok = State(o, v);
        wsprintf(line, "drv state: ok %d, started %lu, bin %lu/%lu/%08lX, int %lu/%lu/%08lX, ctl %lu/%lu/%08lX\r\n",
                 ok, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
        Say(line);
    } else if (!lstrcmpi(test, "exit") || !lstrcmpi(test, "terminate")) {
        /* Both requests must show pending in ONE observation through O,
           taken again just before the action. */
        th = Begin(test, &blk, a, 0, 512, 0, 0);
        th2 = Begin(test, &blk2, b2, IOCTL_PRB_INT, 64, 0, 0);
        t0 = GetTickCount();
        ZeroMemory(v, sizeof(v));
        for (;;) {
            ok = State(o, v);
            if (ok && Pend(v, PRB_BIN) && Pend(v, PRB_INT)) {
                break;
            }
            if (!th || !th2 || WaitForSingleObject(th, 0) == WAIT_OBJECT_0 ||
                WaitForSingleObject(th2, 0) == WAIT_OBJECT_0 || GetTickCount() - t0 > 5000) {
                SayBoth(test, "NOT made: no observation showed both pending; the last", ok, v);
                return 5;
            }
            Sleep(10);
        }
        ZeroMemory(v, sizeof(v));
        ok = State(o, v);
        if (!ok || !Pend(v, PRB_BIN) || !Pend(v, PRB_INT)) {
            SayBoth(test, "NOT made: the observation just before the action did not show both pending", ok, v);
            return 5;
        }
        SayBoth(test, "the observation just before the action shows both pending", ok, v);
        r1 = v[2];
        r2 = v[5];
        if (!lstrcmpi(test, "exit")) {
            wsprintf(line, "drv exit: ExitProcess; requests %lu and %lu were last observed pending (the observation above)\r\n", r1, r2);
            Say(line);
            ExitProcess(7);
        }
        wsprintf(line, "drv terminate: TerminateProcess; requests %lu and %lu were last observed pending (the observation above)\r\n", r1, r2);
        Say(line);
        ok = TerminateProcess(GetCurrentProcess(), 7);
        wsprintf(line, "drv terminate: TerminateProcess returned %d, error %lu\r\n", ok, GetLastError());
        Say(line);
        return 6;
    } else if (!lstrcmpi(test, "outcancel") || !lstrcmpi(test, "outabort")) {
        th = Begin(test, &blk, a, CODE_WRITE, 64, 0, 0);
        if (!WaitPending(test, o, PRB_INT, th)) {
            wsprintf(line, "drv %s: stop NOT made (the write not confirmed pending; arm out_hang first)\r\n", test);
            Say(line);
        } else {
            StopOn(test, b2, !lstrcmpi(test, "outabort") ? IOCTL_PRB_ABORT : IOCTL_PRB_CANCEL, PRB_INT);
        }
        Report(test, "write", &blk, th, 10000);
    } else if (!lstrcmpi(test, "samehandle")) {
        HANDLE ts[2];
        th = Begin(test, &blk, a, IOCTL_PRB_INT, 64, 0, 0);
        if (!WaitPending(test, o, PRB_INT, th)) {
            Say("drv samehandle: NOT made: the interrupt read on A not confirmed pending\r\n");
            return 5;
        }
        th2 = Begin(test, &io[0], a, IOCTL_PRB_STATE, PRB_STATE_ULONGS * sizeof(DWORD), 0, 0);
        Report(test, "the state control on A, with A's interrupt read pending,", &io[0], th2, 3000);
        th3 = Begin(test, &blk3, a, IOCTL_PRB_DEVSTATUS, 64, 0, 0);
        ts[0] = th;
        ts[1] = th3;
        ok = WaitAll(o, (1UL << PRB_INT) | (1UL << PRB_CTL), ts, 2, v);
        SayAll(test, ok ? "both calls on A pending together" : "NOT both pending on A together; the last observation",
               TRUE, v);
        StopOn(test, o, IOCTL_PRB_CANCEL, PRB_INT);
        StopOn(test, o, IOCTL_PRB_CANCEL, PRB_CTL);
        Report(test, "A's interrupt read", &blk, th, 10000);
        Report(test, "A's Get Device Status", &blk3, th3, 10000);
    } else if (!lstrcmpi(test, "waitclose")) {
        th = Begin(test, &blk, a, IOCTL_PRB_WAIT, 0, 0, 0);
        if (!WaitPending(test, o, PRB_WAIT, th)) {
            Say("drv waitclose: NOT made: the wait not confirmed pending\r\n");
            return 5;
        }
        g_StopO = o;
        th2 = CreateThread(NULL, 0, StopWaitLater, NULL, 0, &k);
        t0 = GetTickCount();
        Say("drv waitclose: CloseHandle(A) with its wait pending\r\n");
        ok = CloseHandle(a);
        wsprintf(line, "drv waitclose: CloseHandle(A) returned %d after %lu ms\r\n", ok, GetTickCount() - t0);
        Say(line);
        Report(test, "the wait", &blk, th, 10000);
        if (th2) {
            WaitForSingleObject(th2, 30000);
        }
    } else if (!lstrcmpi(test, "waitexit") || !lstrcmpi(test, "hold")) {
        HANDLE ts[2];
        th = Begin(test, &blk, a, 0, 512, 0, 0);
        th2 = Begin(test, &blk2, b2, IOCTL_PRB_WAIT, 0, 0, 0);
        ts[0] = th;
        ts[1] = th2;
        ok = WaitAll(o, (1UL << PRB_BIN) | (1UL << PRB_WAIT), ts, 2, v);
        SayAll(test, ok ? "a read on A and the wait on B pending in one observation" : "NOT made; the last observation",
               TRUE, v);
        if (!ok) {
            return 5;
        }
        if (!lstrcmpi(test, "waitexit")) {
            wsprintf(line, "drv waitexit: ExitProcess; requests %lu (read) and %lu (wait) were last observed pending\r\n",
                     v[2], v[11]);
            Say(line);
            ExitProcess(7);
        }
        wsprintf(line, "drv hold: requests %lu (read) and %lu (wait) last observed pending; end this program from "
                 "the Close Program dialog now (it waits 600 s)\r\n", v[2], v[11]);
        Say(line);
        Sleep(600000);
        Say("drv hold: not ended within 600 s\r\n");
    } else if (!lstrcmpi(test, "unplugall") || !lstrcmpi(test, "unplugctl") || !lstrcmpi(test, "unplugout") ||
               !lstrcmpi(test, "unplugin")) {
        /* The calls pending at the unplug: all four, only the control
           request, only the bulk OUT, or the bulk IN with the software wait. */
        HANDLE ts[4];
        HANDLE o2 = OpenPrb();
        int nts = 0;
        which2 = !lstrcmpi(test, "unplugctl") ? (1UL << PRB_CTL) :
                 !lstrcmpi(test, "unplugout") ? (1UL << PRB_INT) :
                 !lstrcmpi(test, "unplugin") ? ((1UL << PRB_BIN) | (1UL << PRB_WAIT)) :
                 ((1UL << PRB_BIN) | (1UL << PRB_INT) | (1UL << PRB_CTL) | (1UL << PRB_WAIT));
        d = OpenPrb();
        if (d == INVALID_HANDLE_VALUE || o2 == INVALID_HANDLE_VALUE || !GetCounters(o, base)) {
            wsprintf(line, "drv %s: NOT made: the extra handles or the counters failed\r\n", test);
            Say(line);
            return 5;
        }
        th = th2 = th3 = th4 = NULL;
        if (which2 & (1UL << PRB_BIN)) ts[nts++] = th = Begin(test, &blk, a, 0, 512, 0, 0);
        if (which2 & (1UL << PRB_INT)) ts[nts++] = th2 = Begin(test, &blk2, b2, CODE_WRITE, 64, 0, 0);
        if (which2 & (1UL << PRB_CTL)) ts[nts++] = th3 = Begin(test, &blk3, d, IOCTL_PRB_DEVSTATUS, 64, 0, 0);
        if (which2 & (1UL << PRB_WAIT)) ts[nts++] = th4 = Begin(test, &blk4, o2, IOCTL_PRB_WAIT, 0, 0, 0);
        ok = WaitAll(o, which2, ts, nts, v);
        SayAll(test, ok ? "every call of this case pending in one observation" : "NOT all pending; the last observation",
               TRUE, v);
        if (!ok) {
            return 5;
        }
        wsprintf(line, "drv %s: unplug the device now (waiting up to 600 s for REMOVE)\r\n", test);
        Say(line);
        t0 = GetTickCount();
        ok = FALSE;
        while (GetTickCount() - t0 < 600000) {
            if (GetCounters(o, c) && c[5] > base[5]) {
                ok = TRUE;
                break;
            }
            Sleep(250);
        }
        wsprintf(line, "drv %s: REMOVE %s %lu ms after the prompt\r\n", test, ok ? "seen" : "NOT seen", GetTickCount() - t0);
        Say(line);
        SayCounters(test, "counters:", GetCounters(o, c), c);
        if (th) Report(test, "the read (bulk IN)", &blk, th, 5000);
        if (th2) Report(test, "the write (bulk OUT)", &blk2, th2, 5000);
        if (th3) Report(test, "the Get Device Status (control)", &blk3, th3, 5000);
        if (th4) Report(test, "the software wait", &blk4, th4, 5000);
        if (!ok) {
            /* Free the calls so no CloseHandle blocks: stop what is left. */
            for (k = 0; k < 4; k++) {
                if (which2 & (1UL << k)) StopOn(test, o, IOCTL_PRB_CANCEL, k);
            }
            if (th) Report(test, "the read after the stop", &blk, th, 5000);
            if (th2) Report(test, "the write after the stop", &blk2, th2, 5000);
            if (th3) Report(test, "the control request after the stop", &blk3, th3, 5000);
            if (th4) Report(test, "the wait after the stop", &blk4, th4, 5000);
        }
        CloseHandle(d);
        CloseHandle(o2);
    } else if (!lstrcmpi(test, "serial")) {
        /* A's read pending; then B's read and B's interrupt read, each on
           its own thread; the state through O at each step. */
        th = Begin(test, &blk, a, 0, 512, 0, 0);
        if (!WaitPending(test, o, PRB_BIN, th)) {
            Say("drv serial: NOT made: A's read was not confirmed pending\r\n");
            return 5;
        }
        th2 = Begin(test, &sb[0], b2, 0, 512, 0, 0);
        Sleep(3000);
        ZeroMemory(v, sizeof(v));
        ok = State(o, v);
        SayBoth(test, "3 s after B's ReadFile, the state through O", ok, v);
        Report(test, "B's ReadFile (with A's read pending)", &sb[0], th2, 0);
        nh = Begin(test, &sb[1], b2, IOCTL_PRB_INT, 64, 0, 0);
        k = WaitPending(test, o, PRB_INT, nh);
        wsprintf(line, "drv serial: B's interrupt-read IOCTL with A's read pending: %s (request %lu)\r\n",
                 k ? "reached the driver and is pending" : "not seen pending", k);
        Say(line);
        StopOn(test, o, IOCTL_PRB_CANCEL, PRB_BIN);
        Report(test, "A's read", &blk, th, 10000);
        Sleep(3000);
        ZeroMemory(v, sizeof(v));
        ok = State(o, v);
        SayBoth(test, "3 s after A's read ended, the state through O", ok, v);
        Report(test, "B's ReadFile (after A's read ended)", &sb[0], th2, 0);
        StopOn(test, o, IOCTL_PRB_CANCEL, PRB_BIN);
        StopOn(test, o, IOCTL_PRB_CANCEL, PRB_INT);
        Report(test, "B's ReadFile", &sb[0], th2, 10000);
        Report(test, "B's interrupt read", &sb[1], nh, 10000);
    } else if (!lstrcmpi(test, "zombie")) {
        if (!GetCounters(o, base)) {
            Say("drv zombie: NOT made: the counters could not be read\r\n");
            return 5;
        }
        SayCounters(test, "counters before the unplug:", TRUE, base);
        Say("drv zombie: unplug the device, then plug one (the same or another) (waiting up to 180 s)\r\n");
        t0 = GetTickCount();
        ok = FALSE;
        for (;;) {
            if (GetCounters(o, c) && c[5] > base[5]) {
                ok = TRUE;
                break;
            }
            if (GetTickCount() - t0 > 180000) {
                break;
            }
            Sleep(250);
        }
        SayCounters(test, ok ? "REMOVE confirmed through O:" : "REMOVE NOT confirmed;", GetCounters(o, c), c);
        if (!ok) {
            return 5;
        }
        nh = INVALID_HANDLE_VALUE;
        while (GetTickCount() - t0 < 180000) {
            nh = OpenPrb();
            if (nh != INVALID_HANDLE_VALUE) {
                break;
            }
            Sleep(500);
        }
        wsprintf(line, "drv zombie: new device %s %lu ms after the prompt\r\n",
                 nh == INVALID_HANDLE_VALUE ? "NOT opened" : "opened (handle N)", GetTickCount() - t0);
        Say(line);
        if (nh == INVALID_HANDLE_VALUE) {
            return 5;
        }
        ok = GetCounters(nh, c);
        SayCounters(test, "through N, before the old handles are used:", ok, c);
        th2 = Begin(test, &io[0], a, 0, 512, 0, 0);
        Report(test, "read on old A", &io[0], th2, 10000);
        th2 = Begin(test, &io[1], a, IOCTL_PRB_STATE, PRB_STATE_ULONGS * sizeof(DWORD), 0, 0);
        Report(test, "state on old A", &io[1], th2, 10000);
        ok = GetCounters(b2, c);
        SayCounters(test, "through old B:", ok, c);
        ok = GetCounters(nh, c);
        SayCounters(test, "through N, after the old handles were used:", ok, c);
        wsprintf(line, "drv zombie: IRPs of old handles that reached the new device: %lu\r\n", c[9] - base[9]);
        Say(line);
        SayClose(test, "A", a);
        SayClose(test, "B", b2);
        SayClose(test, "O", o);
        ok = GetCounters(nh, c);
        SayCounters(test, "through N, after A, B and O were closed:", ok, c);
        wsprintf(line, "drv zombie: zombies %lu (before %lu), deletes %lu (before %lu), foreign %lu (before %lu)\r\n",
                 c[8], base[8], c[6], base[6], c[9], base[9]);
        Say(line);
        SayClose(test, "N", nh);
        if (g_Out != INVALID_HANDLE_VALUE) {
            CloseHandle(g_Out);
        }
        return 0;
    } else if (!lstrcmpi(test, "afterremove")) {
        /* The counters are driver-wide and count earlier devices too: the
           milestone is a rise over the values read before the unplug. */
        if (!GetCounters(o, base)) {
            Say("drv afterremove: NOT made: the counters could not be read\r\n");
            return 5;
        }
        SayCounters(test, "counters before the unplug:", TRUE, base);
        th = Begin(test, &blk, a, 0, 512, 0, 0);
        if (!WaitPending(test, o, PRB_BIN, th)) {
            Say("drv afterremove: NOT made: the read was not confirmed pending\r\n");
            return 5;
        }
        Say("drv afterremove: unplug the device now (waiting up to 120 s for the driver's removal counters)\r\n");
        t0 = GetTickCount();
        ok = FALSE;
        for (;;) {
            if (GetCounters(o, c) && c[5] > base[5] && (c[6] > base[6] || c[7] > base[7] || c[8] > base[8])) {
                ok = TRUE;
                break;
            }
            if (GetTickCount() - t0 > 120000) {
                break;
            }
            Sleep(250);
        }
        SayCounters(test, ok ? "removal confirmed after" : "removal NOT confirmed;", GetCounters(o, c), c);
        Report(test, "pending read", &blk, th, 10000);
        if (!ok) {
            Say("drv afterremove: no reading after removal\r\n");
            return 5;
        }
        wsprintf(line, "drv afterremove: removal confirmed %lu ms after the prompt (%s)\r\n", GetTickCount() - t0,
                 c[8] > base[8] ? "device kept as a zombie for the open handles" :
                 c[6] > base[6] ? "device deleted" : "removal leaked, device kept");
        Say(line);
        th2 = Begin(test, &io[0], a, 0, 512, 0, 0);
        Report(test, "read on A after removal", &io[0], th2, 10000);
        th2 = Begin(test, &io[1], a, IOCTL_PRB_STATE, PRB_STATE_ULONGS * sizeof(DWORD), 0, 0);
        Report(test, "state on A after removal", &io[1], th2, 10000);
        th2 = Begin(test, &io[2], b2, IOCTL_PRB_COUNTERS, PRB_COUNTERS_BYTES, 0, 0);
        Report(test, "counters on B after removal", &io[2], th2, 10000);
        SayClose(test, "A", a);
        SayClose(test, "B", b2);
        ok = GetCounters(o, c);
        SayCounters(test, "counters on O after closing A and B:", ok, c);
        SayClose(test, "O", o);
        a = OpenPrb();
        wsprintf(line, "drv afterremove: new open %s (error %lu)\r\n", a == INVALID_HANDLE_VALUE ? "failed" : "ok",
                 a == INVALID_HANDLE_VALUE ? GetLastError() : 0);
        Say(line);
        if (a != INVALID_HANDLE_VALUE) {
            CloseHandle(a);
        }
        if (g_Out != INVALID_HANDLE_VALUE) {
            CloseHandle(g_Out);
        }
        return 0;
    } else {
        which = PRB_BIN;
        if (!lstrcmpi(test, "intcancel") || !lstrcmpi(test, "intabort")) {
            th = Begin(test, &blk, a, IOCTL_PRB_INT, 64, 0, 0);
            which = PRB_INT;
        } else if (!lstrcmpi(test, "ctlcancel")) {
            th = Begin(test, &blk, a, IOCTL_PRB_DEVSTATUS, 64, 0, 0);
            which = PRB_CTL;
        } else if (!lstrcmpi(test, "resetcancel")) {
            th = Begin(test, &blk, a, IOCTL_PRB_RESET, 0, PRB_BIN, sizeof(DWORD));
            which = PRB_CTL;
        } else if (!lstrcmpi(test, "cancel") || !lstrcmpi(test, "abort") || !lstrcmpi(test, "close")) {
            th = Begin(test, &blk, a, 0, 512, 0, 0);
        } else {
            Say("drv: unknown test\r\n");
            return 4;
        }
        if (!WaitPending(test, o, which, th)) {
            wsprintf(line, "drv %s: stop NOT made (not confirmed pending)\r\n", test);
            Say(line);
        } else if (!lstrcmpi(test, "close")) {
            Say("drv close: CloseHandle(A) with its read pending\r\n");
            SayClose(test, "A", a);
        } else {
            StopOn(test, b2, (!lstrcmpi(test, "abort") || !lstrcmpi(test, "intabort")) ? IOCTL_PRB_ABORT
                                                                                       : IOCTL_PRB_CANCEL, which);
        }
        Report(test, "call", &blk, th, 10000);
    }
    CloseHandle(o);
    CloseHandle(b2);
    if (g_Out != INVALID_HANDLE_VALUE) {
        CloseHandle(g_Out);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char line[512], match[24];
    char *p = line, *mode, *a, *b;
    const char *file = "A:\\MTPPROBE.TXT";
    (void)inst;
    (void)prev;
    (void)show;
    lstrcpyn(line, cmd, sizeof(line));
    mode = NextArg(&p);
    if (!mode || (lstrcmpi(mode, "/pnp") && lstrcmpi(mode, "/drv"))) {
        return 4;
    }
    if (!lstrcmpi(mode, "/drv")) {
        a = NextArg(&p);
        b = NextArg(&p);
        return Drv(a ? a : "status", b ? b : "COM1");
    }
    match[0] = 0;
    a = NextArg(&p);
    b = NextArg(&p);
    if (a && Target(a, match)) {
        if (b) {
            file = b;
        }
    } else if (a) {
        file = a;
    }
    return Pnp(match, file);
}
