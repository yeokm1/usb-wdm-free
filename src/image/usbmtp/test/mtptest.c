/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mtptest.c - MTPTEST.EXE, a small test client of USBMTP.SYS's interface
 * 1.0 and 1.1 (docs/design/mtp.md 5) for the M1 load and lifetime legs and
 * the standby legs on 98 SE and ME. Not shipped; it speaks only the few
 * PTP containers it needs.
 *
 *   MTPTEST <test> [n] [file]     (n: \\.\USBMTP<n>, default 0; file
 *                                  default COM1; one line per fact)
 *
 *   info     GET_INFO (with interface 1.1's power fields when the driver
 *            has them), then string descriptors 1 to 3 (language 0).
 *   power    GET_INFO's power fields watched from an info handle, a line
 *            per change, across a standby and the device's removal and
 *            return (600 s); beside another MTPTEST that moves data.
 *   session  CLAIM on A; a second CLAIM on C (refused); BIND on B, a
 *            second BIND (refused); SET_TIMEOUTS on A and on B (refused on
 *            B); GetDeviceInfo (1001h) on A: the command, the data phase,
 *            the response; Get Device Status on B; WAIT_EVENT with 0 and
 *            with 500 ms on B; a read pending on A stopped by ABORT on B;
 *            then the handles closed.
 *   zombie   CLAIM on A, B opened; GET_INFO on B once a second until it
 *            fails (the unplug is the host's), then a read on A; the
 *            handles closed 5 s later (B, then A); then a new open.
 *   storage  read-only operations only, for a real device: CLAIM;
 *            OpenSession (1002h); GetDeviceInfo (1001h) with its strings
 *            and operation codes; GetStorageIDs (1004h); GetStorageInfo
 *            (1005h) of up to four storages; GetObjectHandles (1007h) of
 *            each storage's root (the count only); CloseSession (1003h).
 *            It stops at the first response other than OK (2001h).
 *   standby  for a standby leg: CLAIM, bulk timeouts of 200 ms, then a
 *            ReadFile of one packet again and again (100 ms apart after a
 *            success), one line each time the result's Win32 code changes,
 *            with the time since the start, until a code of the device
 *            gone (5, 1167, 6) or 600 s; the standby is the host's.
 *   xfer     data moving across a standby (the host's): CLAIM, OpenSession,
 *            GetStorageIDs, the largest object at the first storage's root
 *            (GetObjectHandles, GetObjectInfo), then GetObject of it again
 *            and again, checked against the test pattern (byte i:
 *            (i * 7 + 5Ah) ^ (i >> 9), low 8 bits): 16 bytes of every
 *            4 KiB, so the passes spend their time in transfers; a line
 *            for passes 1 to 3 and every 200th. A failed transfer is logged
 *            with the last good pass, its phase, its Win32 code and the
 *            bytes moved in that phase; on a code of the device gone the
 *            test ends; otherwise it sends Cancel (64h) for that
 *            transaction until it is accepted, reads Get Device Status
 *            until OK (at most 10 times), drains bulk IN, takes the session
 *            as lost (CloseSession, its answer logged; OpenSession; the
 *            object found again) and runs three more passes, every byte
 *            checked, each logged. CloseSession at the end; 600 s at most
 *            without a failure.
 *   xferout  as xfer, for a writable device: each pass sends an object of
 *            2 MiB + 100 bytes (SendObjectInfo, SendObject; byte i of pass
 *            p: (i * 7 + p) ^ (i >> 9)), reads it back with GetObject,
 *            checks it and deletes it (DeleteObject).
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include "../sys/usbmtp.h"

static HANDLE g_Out = INVALID_HANDLE_VALUE;
static char g_Name[32];

static void Say(const char *s)
{
    DWORD n;
    if (g_Out != INVALID_HANDLE_VALUE) WriteFile(g_Out, s, lstrlen(s), &n, NULL);
}

static void SayF(const char *fmt, DWORD a, DWORD b, DWORD c, DWORD d)
{
    char line[256];
    wsprintf(line, fmt, a, b, c, d);
    Say(line);
}

static HANDLE Open(void)
{
    return CreateFile(g_Name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL Ioctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *got)
{
    DWORD n = 0;
    BOOL ok = DeviceIoControl(h, code, in, inLen, out, outLen, &n, NULL);
    if (got) *got = n;
    return ok;
}

static DWORD Err(BOOL ok)
{
    return ok ? 0 : GetLastError();
}

/* GET_INFO with room past this header's structure: a 1.0 driver returns
   44 bytes, a 1.1 driver 52, whatever header this program was built with.
   The power fields of interface 1.1 (mtp.md 5.3) are read by their
   offsets, so the program builds against either header. */
typedef union INFOBUF { USBMTP_INFO i; unsigned char b[64]; } INFOBUF;
#define INFO_POWER_AT 44    /* PowerGate, Held, HeldAtVeto, reserved, Vetoes (UINT16), Forced (UINT16) */
#define INFO_SIZE_1_1 52

static BOOL GetInfo(HANDLE h, INFOBUF *u, DWORD *got)
{
    ZeroMemory(u, sizeof(*u));
    return Ioctl(h, IOCTL_USBMTP_GET_INFO, NULL, 0, u, sizeof(*u), got);
}

static DWORD U16At(const INFOBUF *u, int at) { return (DWORD)u->b[at] | ((DWORD)u->b[at + 1] << 8); }

static void Info(HANDLE h)
{
    INFOBUF u;
    USBMTP_INFO i;
    DWORD got, k;
    BOOL ok;
    ok = GetInfo(h, &u, &got);
    i = u.i;
    SayF("info: ok %d, error %lu, bytes %lu, cbSize %lu\r\n", ok, Err(ok), got, i.cbSize);
    if (!ok) return;
    SayF("info: interface %u.%u, driver %08lX, generation %lu\r\n", i.Major, i.Minor, i.DriverVersion, i.Generation);
    SayF("info: n %lu, VID %04X PID %04X bcdDevice %04X\r\n", i.N, i.Vid, i.Pid, i.BcdDevice);
    SayF("info: interface %u, class %02X/%02X/%02X\r\n", i.Interface, i.Class, i.SubClass, i.Protocol);
    SayF("info: max packet in %u, out %u, int %u; speed %u\r\n", i.MaxPacketIn, i.MaxPacketOut, i.MaxPacketInt, i.Speed);
    SayF("info: flags %02X; endpoints in %02X, out %02X, int %02X\r\n", i.Flags, i.EndpointIn, i.EndpointOut, i.EndpointInt);
    if (got >= INFO_SIZE_1_1)
        SayF("info: power gate %02X, held %02X, held at the last refusal %02X, refusals %lu", u.b[INFO_POWER_AT],
             u.b[INFO_POWER_AT + 1], u.b[INFO_POWER_AT + 2], U16At(&u, INFO_POWER_AT + 4));
    if (got >= INFO_SIZE_1_1) SayF(", forced sets %lu\r\n", U16At(&u, INFO_POWER_AT + 6), 0, 0, 0);
    for (k = 1; k <= 3; k++) {
        USBMTP_STRING_IN si;
        unsigned char buf[256];
        char text[130];
        DWORD j, m = 0;
        si.Index = (UCHAR)k;
        si.Reserved = 0;
        si.LangId = 0;
        ZeroMemory(buf, sizeof(buf));
        ok = Ioctl(h, IOCTL_USBMTP_GET_STRING, &si, sizeof(si), buf, sizeof(buf), &got);
        for (j = 2; j + 1 < got && m < sizeof(text) - 1; j += 2) text[m++] = buf[j + 1] ? '?' : (char)buf[j];
        text[m] = 0;
        SayF("string %lu: ok %d, error %lu, bytes %lu, ", k, ok, Err(ok), got);
        Say("\"");
        Say(text);
        Say("\"\r\n");
    }
}

/* A PTP command container: length 12 + 4 per parameter (up to 3), type 1. */
static BOOL CommandP(HANDLE h, WORD code, DWORD tid, int np, DWORD p1, DWORD p2, DWORD p3, DWORD *err)
{
    unsigned char c[24];
    DWORD n = 0, len = 12 + 4 * (DWORD)np, p[3];
    BOOL ok;
    int k;
    p[0] = p1; p[1] = p2; p[2] = p3;
    c[0] = (unsigned char)len; c[1] = c[2] = c[3] = 0;
    c[4] = 1; c[5] = 0;
    c[6] = (unsigned char)code; c[7] = (unsigned char)(code >> 8);
    c[8] = (unsigned char)tid; c[9] = (unsigned char)(tid >> 8);
    c[10] = (unsigned char)(tid >> 16); c[11] = (unsigned char)(tid >> 24);
    for (k = 0; k < np; k++) {
        c[12 + 4 * k] = (unsigned char)p[k]; c[13 + 4 * k] = (unsigned char)(p[k] >> 8);
        c[14 + 4 * k] = (unsigned char)(p[k] >> 16); c[15 + 4 * k] = (unsigned char)(p[k] >> 24);
    }
    ok = WriteFile(h, c, len, &n, NULL);
    *err = Err(ok);
    SayF("command %04X: ok %d, error %lu, written %lu\r\n", code, ok, *err, n);
    return ok && n == len;
}

static BOOL Command(HANDLE h, WORD code, DWORD tid, DWORD *err)
{
    return CommandP(h, code, tid, 0, 0, 0, 0, err);
}

static DWORD Le32(const unsigned char *p)
{
    return p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

/* One container: first exactly one packet, then whole packets until the
   container's length or a short packet. */
static BOOL Container(HANDLE h, DWORD mps, unsigned char *buf, DWORD cap, DWORD *total, const char *what)
{
    DWORD n = 0, want, len, reads = 1;
    BOOL ok = ReadFile(h, buf, mps, &n, NULL);
    *total = n;
    if (!ok || n < 12) {
        SayF("%s: first read ok %d, error %lu, bytes %lu\r\n", (DWORD)what, ok, Err(ok), n);
        return FALSE;
    }
    len = Le32(buf);
    while (*total < len && n == mps) {
        want = len - *total;
        want = (want + mps - 1) / mps * mps;
        if (*total + want > cap) want = (cap - *total) / mps * mps;
        if (!want) break;
        ok = ReadFile(h, buf + *total, want, &n, NULL);
        reads++;
        if (!ok) {
            SayF("%s: read %lu failed, error %lu\r\n", (DWORD)what, reads, GetLastError(), 0);
            return FALSE;
        }
        *total += n;
        if (n < want) break;
    }
    if (*total == len && len % mps == 0) {
        ok = ReadFile(h, buf + *total, mps, &n, NULL);  /* the zero-length packet */
        reads++;
        SayF("%s: zero-length read ok %d, bytes %lu\r\n", (DWORD)what, ok, n, 0);
    }
    SayF("%s: container length %lu, type %u, code %04X, ", (DWORD)what, len, buf[4] | (buf[5] << 8), buf[6] | (buf[7] << 8));
    SayF("%lu bytes in %lu reads\r\n", *total, reads, 0, 0);
    return *total == len;
}

typedef struct READER { HANDLE H; DWORD Len, Ok, Err, Bytes, Ms; } READER;

static DWORD WINAPI ReaderThread(LPVOID p)
{
    READER *r = (READER *)p;
    static unsigned char buf[4096];
    DWORD t0 = GetTickCount();
    r->Ok = ReadFile(r->H, buf, r->Len, &r->Bytes, NULL);
    r->Err = r->Ok ? 0 : GetLastError();
    r->Ms = GetTickCount() - t0;
    return 0;
}

static void Session(void)
{
    HANDLE a = Open(), b = Open(), c = Open();
    USBMTP_INFO i;
    USBMTP_CLAIM_INFO ci, ci2;
    USBMTP_TIMEOUTS t;
    USBMTP_CLASS_IN cl;
    USBMTP_WAIT_IN wi;
    USBMTP_EVENT ev;
    static unsigned char buf[65536];
    unsigned char st[64];
    DWORD got, err, left, ms, total;
    BOOL ok;
    READER r;
    HANDLE th;
    SayF("session: open A %d, B %d, C %d, error %lu\r\n", a != INVALID_HANDLE_VALUE, b != INVALID_HANDLE_VALUE,
         c != INVALID_HANDLE_VALUE, GetLastError());
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE || c == INVALID_HANDLE_VALUE) return;
    ok = Ioctl(a, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
    if (!ok) { SayF("session: GET_INFO failed, error %lu\r\n", GetLastError(), 0, 0, 0); return; }
    ok = Ioctl(a, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
    SayF("claim A: ok %d, error %lu, bytes %lu, generation %lu", ok, Err(ok), got, ci.Generation);
    SayF(", epoch %lu\r\n", ci.Epoch, 0, 0, 0);
    if (!ok) return;
    ok = Ioctl(c, IOCTL_USBMTP_CLAIM, NULL, 0, &ci2, sizeof(ci2), &got);
    SayF("claim C (second): ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    ok = Ioctl(b, IOCTL_USBMTP_BIND, &ci, sizeof(ci), NULL, 0, &got);
    SayF("bind B: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    ok = Ioctl(b, IOCTL_USBMTP_BIND, &ci, sizeof(ci), NULL, 0, &got);
    SayF("bind B again: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    ci2 = ci;
    ci2.Epoch++;
    ok = Ioctl(c, IOCTL_USBMTP_BIND, &ci2, sizeof(ci2), NULL, 0, &got);
    SayF("bind C with a wrong epoch: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    t.BulkInMs = 5000;
    t.BulkOutMs = 5000;
    ok = Ioctl(a, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    SayF("set timeouts A: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    ok = Ioctl(b, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    SayF("set timeouts B: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    ok = ReadFile(c, buf, i.MaxPacketIn, &got, NULL);
    SayF("read on C (info handle): ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    /* GetDeviceInfo, outside a session (transaction id 0). */
    if (Command(a, 0x1001, 0, &err) && Container(a, i.MaxPacketIn, buf, sizeof(buf), &total, "data"))
        Container(a, i.MaxPacketIn, buf, sizeof(buf), &total, "response");
    cl.Request = USBMTP_REQ_STATUS;
    cl.TransactionId = 0;
    cl.DataLength = sizeof(st);
    ZeroMemory(st, sizeof(st));
    ok = Ioctl(b, IOCTL_USBMTP_CLASS_REQUEST, &cl, sizeof(cl), st, sizeof(st), &got);
    SayF("get device status B: ok %d, error %lu, bytes %lu, code %04X\r\n", ok, Err(ok), got, got >= 4 ? st[2] | (st[3] << 8) : 0);
    ms = 0;
    ok = Ioctl(b, IOCTL_USBMTP_WAIT_EVENT, &ms, sizeof(ms), &ev, sizeof(ev), &got);
    SayF("wait event 0 ms B: ok %d, error %lu, bytes %lu\r\n", ok, Err(ok), got, 0);
    ms = 500;
    left = GetTickCount();
    ok = Ioctl(b, IOCTL_USBMTP_WAIT_EVENT, &ms, sizeof(ms), &ev, sizeof(ev), &got);
    SayF("wait event 500 ms B: ok %d, error %lu, bytes %lu, %lu ms\r\n", ok, Err(ok), got, GetTickCount() - left);
    /* A read left pending (the device has nothing to send), stopped by ABORT on B. */
    r.H = a;
    r.Len = i.MaxPacketIn;
    r.Ok = r.Err = r.Bytes = r.Ms = 0xFFFFFFFF;
    th = CreateThread(NULL, 0, ReaderThread, &r, 0, &got);
    Sleep(1500);
    SayF("read pending on A after 1500 ms: %d\r\n", r.Ms == 0xFFFFFFFF, 0, 0, 0);
    wi.What = USBMTP_MASK_BULK_IN;
    wi.WaitMs = 2000;
    left = 0xFFFFFFFF;
    ok = Ioctl(b, IOCTL_USBMTP_ABORT, &wi, sizeof(wi), &left, sizeof(left), &got);
    SayF("abort B: ok %d, error %lu, still not quiet %lu\r\n", ok, Err(ok), left, 0);
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    SayF("read A after the abort: ok %lu, error %lu, bytes %lu, %lu ms\r\n", r.Ok, r.Err, r.Bytes, r.Ms);
    SayF("close B %d, C %d, A %d\r\n", CloseHandle(b), CloseHandle(c), CloseHandle(a), 0);
    a = Open();
    if (a != INVALID_HANDLE_VALUE) {
        ok = Ioctl(a, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
        SayF("claim after the close: ok %d, error %lu, epoch %lu\r\n", ok, Err(ok), ci.Epoch, 0);
        CloseHandle(a);
    }
}

/* ------------------------------------------------------------ storage (read-only) */

static unsigned char g_Data[1048576];

static DWORD Le16(const unsigned char *p) { return p[0] | ((DWORD)p[1] << 8); }

/* A PTP string at *at of a dataset of Total bytes: a count of UCS-2
   characters (the terminator included), then the characters. Into Text as
   ASCII ('?' for the rest). FALSE if it runs past the dataset. */
static BOOL PtpString(const unsigned char *d, DWORD total, DWORD *at, char *text, DWORD cap)
{
    DWORD n, k, m = 0;
    if (*at + 1 > total) return FALSE;
    n = d[*at];
    if (*at + 1 + 2 * n > total) return FALSE;
    for (k = 0; k < n && m + 1 < cap; k++) {
        DWORD c = Le16(d + *at + 1 + 2 * k);
        if (!c) break;
        text[m++] = c < 0x20 || c > 0x7E ? '?' : (char)c;
    }
    text[m] = 0;
    *at += 1 + 2 * n;
    return TRUE;
}

/* A PTP array of 16-bit codes: printed as Label, eight to a line. */
static BOOL PtpCodes(const unsigned char *d, DWORD total, DWORD *at, const char *label)
{
    DWORD n, k;
    char line[128];
    if (*at + 4 > total) return FALSE;
    n = Le32(d + *at);
    if (n > (total - *at - 4) / 2) return FALSE;
    SayF("%s: %lu\r\n", (DWORD)label, n, 0, 0);
    for (k = 0; k < n; k++) {
        if (k % 8 == 0) lstrcpy(line, "  ");
        wsprintf(line + lstrlen(line), " %04X", Le16(d + *at + 4 + 2 * k));
        if (k % 8 == 7 || k + 1 == n) { lstrcat(line, "\r\n"); Say(line); }
    }
    *at += 4 + 2 * n;
    return TRUE;
}

/* One transaction: the command, an optional data phase into g_Data (its
   payload length in *Data), the response. Returns the response code, 0 if
   the exchange itself failed. */
static DWORD Transact(HANDLE h, DWORD mps, WORD code, DWORD tid, int np, DWORD p1, DWORD p2, DWORD p3, DWORD *Data)
{
    static unsigned char resp[1024];
    DWORD err, total;
    *Data = 0;
    if (!CommandP(h, code, tid, np, p1, p2, p3, &err)) return 0;
    if (!Container(h, mps, g_Data, sizeof(g_Data), &total, "phase")) return 0;
    if (Le16(g_Data + 4) == 2) {
        *Data = total - 12;
        if (!Container(h, mps, resp, sizeof(resp), &total, "response")) return 0;
        if (Le16(resp + 4) != 3) return 0;
        return Le16(resp + 6);
    }
    return Le16(g_Data + 4) == 3 ? Le16(g_Data + 6) : 0;
}

static void Storage(void)
{
    HANDLE a = Open();
    USBMTP_INFO i;
    USBMTP_CLAIM_INFO ci;
    DWORD got, rc, len, at, tid = 0, ids[4], nids = 0, k, v;
    char text[256];
    const unsigned char *d = g_Data + 12;
    BOOL ok;
    SayF("storage: open %d, error %lu\r\n", a != INVALID_HANDLE_VALUE, GetLastError(), 0, 0);
    if (a == INVALID_HANDLE_VALUE) return;
    ok = Ioctl(a, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
    if (ok) ok = Ioctl(a, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
    SayF("storage: info and claim ok %d, error %lu, max packet in %u\r\n", ok, Err(ok), i.MaxPacketIn, 0);
    if (!ok) { CloseHandle(a); return; }
    rc = Transact(a, i.MaxPacketIn, 0x1002, tid, 1, 1, 0, 0, &len);        /* OpenSession 1, transaction 0 */
    SayF("OpenSession: response %04X\r\n", rc, 0, 0, 0);
    if (rc != 0x2001) goto out;
    rc = Transact(a, i.MaxPacketIn, 0x1001, ++tid, 0, 0, 0, 0, &len);
    SayF("GetDeviceInfo: response %04X, %lu bytes\r\n", rc, len, 0, 0);
    if (rc != 0x2001) goto close;
    at = 0;
    if (len >= 8) {
        SayF("deviceinfo: standard version %u, vendor extension id %08lX, version %u\r\n", Le16(d), Le32(d + 2), Le16(d + 6), 0);
        at = 8;
        if (PtpString(d, len, &at, text, sizeof(text))) { Say("deviceinfo: vendor extension \""); Say(text); Say("\"\r\n"); }
        if (at + 2 <= len) { SayF("deviceinfo: functional mode %04X\r\n", Le16(d + at), 0, 0, 0); at += 2; }
        if (PtpCodes(d, len, &at, "deviceinfo: operations") && PtpCodes(d, len, &at, "deviceinfo: events") &&
            PtpCodes(d, len, &at, "deviceinfo: device properties") && PtpCodes(d, len, &at, "deviceinfo: capture formats") &&
            PtpCodes(d, len, &at, "deviceinfo: playback formats")) {
            if (PtpString(d, len, &at, text, sizeof(text))) { Say("deviceinfo: manufacturer \""); Say(text); Say("\"\r\n"); }
            if (PtpString(d, len, &at, text, sizeof(text))) { Say("deviceinfo: model \""); Say(text); Say("\"\r\n"); }
            if (PtpString(d, len, &at, text, sizeof(text))) { Say("deviceinfo: device version \""); Say(text); Say("\"\r\n"); }
            if (PtpString(d, len, &at, text, sizeof(text)))     /* not printed: the owner's own device */
                SayF("deviceinfo: serial number of %lu characters\r\n", lstrlen(text), 0, 0, 0);
        }
        SayF("deviceinfo: parsed %lu of %lu bytes\r\n", at, len, 0, 0);
    }
    rc = Transact(a, i.MaxPacketIn, 0x1004, ++tid, 0, 0, 0, 0, &len);
    SayF("GetStorageIDs: response %04X, %lu bytes\r\n", rc, len, 0, 0);
    if (rc != 0x2001) goto close;
    if (len >= 4) {
        v = Le32(d);
        SayF("storage ids: %lu\r\n", v, 0, 0, 0);
        for (k = 0; k < v && 4 + 4 * k + 4 <= len; k++) {
            SayF("  storage %08lX\r\n", Le32(d + 4 + 4 * k), 0, 0, 0);
            if (nids < 4) ids[nids++] = Le32(d + 4 + 4 * k);
        }
    }
    for (k = 0; k < nids; k++) {
        rc = Transact(a, i.MaxPacketIn, 0x1005, ++tid, 1, ids[k], 0, 0, &len);
        SayF("GetStorageInfo %08lX: response %04X, %lu bytes\r\n", ids[k], rc, len, 0);
        if (rc != 0x2001) goto close;
        if (len >= 26) {
            SayF("  type %04X, filesystem %04X, access %04X\r\n", Le16(d), Le16(d + 2), Le16(d + 4), 0);
            SayF("  capacity %lu MiB, free %lu MiB, free objects %08lX\r\n",
                 (Le32(d + 10) << 12) | (Le32(d + 6) >> 20), (Le32(d + 18) << 12) | (Le32(d + 14) >> 20), Le32(d + 22), 0);
            at = 26;
            if (PtpString(d, len, &at, text, sizeof(text))) { Say("  description \""); Say(text); Say("\"\r\n"); }
            if (PtpString(d, len, &at, text, sizeof(text))) { Say("  volume \""); Say(text); Say("\"\r\n"); }
        }
        rc = Transact(a, i.MaxPacketIn, 0x1007, ++tid, 3, ids[k], 0, 0xFFFFFFFFUL, &len);     /* any format, the root's children */
        SayF("GetObjectHandles %08lX root: response %04X, %lu bytes, %lu handles\r\n", ids[k], rc, len,
             len >= 4 ? Le32(d) : 0);
        if (rc != 0x2001) goto close;
    }
close:
    rc = Transact(a, i.MaxPacketIn, 0x1003, ++tid, 0, 0, 0, 0, &len);
    SayF("CloseSession: response %04X\r\n", rc, 0, 0, 0);
out:
    SayF("storage: close %d\r\n", CloseHandle(a), 0, 0, 0);
}

static void Standby(void)
{
    HANDLE a = Open();
    USBMTP_INFO i;
    USBMTP_CLAIM_INFO ci;
    USBMTP_TIMEOUTS t;
    static unsigned char buf[1024];
    DWORD got, t0, e, last = 0xFFFFFFFF, n = 0;
    BOOL ok;
    SayF("standby: open %d, error %lu\r\n", a != INVALID_HANDLE_VALUE, GetLastError(), 0, 0);
    if (a == INVALID_HANDLE_VALUE) return;
    ok = Ioctl(a, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
    if (ok) ok = Ioctl(a, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
    t.BulkInMs = 200;
    t.BulkOutMs = 200;
    if (ok) ok = Ioctl(a, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    SayF("standby: info, claim and timeouts ok %d, error %lu, generation %lu\r\n", ok, Err(ok), ci.Generation, 0);
    if (!ok || !i.MaxPacketIn || i.MaxPacketIn > sizeof(buf)) { CloseHandle(a); return; }
    Say("standby: ready for the standby\r\n");
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 600000) {
        ok = ReadFile(a, buf, i.MaxPacketIn, &got, NULL);
        e = ok ? 0 : GetLastError();
        n++;
        if (e != last) {
            SayF("standby: %lu ms, read %lu: error %lu, bytes %lu\r\n", GetTickCount() - t0, n, e, ok ? got : 0);
            last = e;
        }
        if (e == 5 || e == 1167 || e == 6) break;
        if (ok) Sleep(100);
    }
    SayF("standby: %lu reads; close %d\r\n", n, CloseHandle(a), 0, 0);
}

/* ------------------------------------------------------------ xfer, xferout */

#define XF_GONE(e) ((e) == 5 || (e) == 1167 || (e) == 6)
#define XF_PROTO   0xE0000000UL         /* an error of the exchange, not of a call: | detail */

static unsigned char Pat(DWORD i, DWORD seed) { return (unsigned char)((i * 7 + seed) ^ (i >> 9)); }

typedef struct XF {
    HANDLE H;
    DWORD Mps, Tid, Err, Moved, Bad, Checked, Full;
    const char *Phase;
} XF;

/* Payload bytes at offset Off checked against the pattern: every byte when
   Full, else 16 bytes of every 4 KiB (so the passes before a standby spend
   their time in transfers, not in the check). */
static void XfCheck(XF *x, const unsigned char *p, DWORD n, DWORD off, DWORD seed)
{
    DWORD k;
    for (k = 0; k < n; k++) {
        if (!x->Full && ((off + k) & 4095) >= 16) {
            k += 4095 - ((off + k) & 4095);     /* to the last byte of this 4 KiB */
            continue;
        }
        x->Checked++;
        if (p[k] != Pat(off + k, seed)) x->Bad++;
    }
}

static BOOL XfFail(XF *x, const char *phase, DWORD err, DWORD moved)
{
    x->Phase = phase;
    x->Err = err;
    x->Moved = moved;
    return FALSE;
}

/* The command container of transaction ++Tid. */
static BOOL XfCommand(XF *x, WORD code, int np, DWORD p1, DWORD p2, DWORD p3)
{
    unsigned char c[24];
    DWORD n = 0, len = 12 + 4 * (DWORD)np, p[3];
    BOOL ok;
    int k;
    p[0] = p1; p[1] = p2; p[2] = p3;
    x->Tid++;
    c[0] = (unsigned char)len; c[1] = c[2] = c[3] = 0;
    c[4] = 1; c[5] = 0;
    c[6] = (unsigned char)code; c[7] = (unsigned char)(code >> 8);
    c[8] = (unsigned char)x->Tid; c[9] = (unsigned char)(x->Tid >> 8);
    c[10] = (unsigned char)(x->Tid >> 16); c[11] = (unsigned char)(x->Tid >> 24);
    for (k = 0; k < np; k++) {
        c[12 + 4 * k] = (unsigned char)p[k]; c[13 + 4 * k] = (unsigned char)(p[k] >> 8);
        c[14 + 4 * k] = (unsigned char)(p[k] >> 16); c[15 + 4 * k] = (unsigned char)(p[k] >> 24);
    }
    ok = WriteFile(x->H, c, len, &n, NULL);
    if (!ok) return XfFail(x, "command", GetLastError(), n);
    if (n != len) return XfFail(x, "command", XF_PROTO | 1, n);
    return TRUE;
}

/* The response container: its code into *Code, its third parameter into *P3. */
static BOOL XfResponse(XF *x, DWORD *Code, DWORD *P3)
{
    static unsigned char r[1024];
    DWORD n = 0;
    BOOL ok = ReadFile(x->H, r, x->Mps, &n, NULL);
    if (!ok) return XfFail(x, "response", GetLastError(), n);
    if (n < 12 || Le16(r + 4) != 3) return XfFail(x, "response", XF_PROTO | 2, n);
    *Code = Le16(r + 6);
    if (P3) *P3 = n >= 24 ? Le32(r + 20) : 0;
    return TRUE;
}

/* A data phase from the device into g_Data in pieces of up to 1 MiB (whole
   packets). Seed below 256: every payload byte is checked against the
   pattern, mismatches counted in Bad; else the payload is kept in g_Data
   (it must fit). *Size: the payload length. */
static BOOL XfDataIn(XF *x, DWORD seed, DWORD *Size)
{
    DWORD n = 0, len, total, want, keep = seed > 255;
    BOOL ok, more;
    x->Bad = x->Checked = 0;
    ok = ReadFile(x->H, g_Data, x->Mps, &n, NULL);
    if (!ok) return XfFail(x, "data", GetLastError(), 0);
    if (n < 12) return XfFail(x, "data", XF_PROTO | 3, n);
    len = Le32(g_Data);
    if (Le16(g_Data + 4) == 3) return XfFail(x, "data", XF_PROTO | 0x10000 | Le16(g_Data + 6), 0);
    if (Le16(g_Data + 4) != 2 || len < 12 || (keep && len > sizeof(g_Data))) return XfFail(x, "data", XF_PROTO | 4, 0);
    total = n;
    if (!keep) XfCheck(x, g_Data + 12, n - 12, 0, seed);
    more = n == x->Mps;
    while (total < len && more) {
        want = (len - total + x->Mps - 1) / x->Mps * x->Mps;
        if (keep) { if (want > sizeof(g_Data) - total) want = (sizeof(g_Data) - total) / x->Mps * x->Mps; }
        else if (want > sizeof(g_Data)) want = sizeof(g_Data);
        if (!want) break;
        ok = ReadFile(x->H, keep ? g_Data + total : g_Data, want, &n, NULL);
        if (!ok) return XfFail(x, "data", GetLastError(), total - 12);
        if (!keep) XfCheck(x, g_Data, n, total - 12, seed);
        total += n;
        more = n == want;
    }
    if (total != len) return XfFail(x, "data", XF_PROTO | 5, total - 12);
    if (len % x->Mps == 0) {
        static unsigned char zlp[1024];
        ok = ReadFile(x->H, zlp, x->Mps, &n, NULL);     /* the zero-length packet */
        if (!ok) return XfFail(x, "zero-length packet", GetLastError(), total - 12);
    }
    *Size = len - 12;
    return TRUE;
}

/* A data phase to the device: the 12-byte header and Size bytes of the
   pattern of Seed (or of Src when given), in writes of 1 MiB at most. */
static BOOL XfDataOut(XF *x, WORD code, DWORD size, DWORD seed, const unsigned char *src)
{
    DWORD len = 12 + size, sent = 0, piece, k, at, n = 0;
    unsigned char hdr[12];
    BOOL ok;
    hdr[0] = (unsigned char)len; hdr[1] = (unsigned char)(len >> 8);
    hdr[2] = (unsigned char)(len >> 16); hdr[3] = (unsigned char)(len >> 24);
    hdr[4] = 2; hdr[5] = 0;
    hdr[6] = (unsigned char)(code & 0xFF); hdr[7] = (unsigned char)(code >> 8);
    hdr[8] = (unsigned char)x->Tid; hdr[9] = (unsigned char)(x->Tid >> 8);
    hdr[10] = (unsigned char)(x->Tid >> 16); hdr[11] = (unsigned char)(x->Tid >> 24);
    while (sent < len) {
        piece = len - sent;
        if (piece > sizeof(g_Data)) piece = sizeof(g_Data);
        for (k = 0; k < piece; k++) {
            at = sent + k;
            g_Data[k] = at < 12 ? hdr[at] : src ? src[at - 12] : Pat(at - 12, seed);
        }
        ok = WriteFile(x->H, g_Data, piece, &n, NULL);
        if (!ok) return XfFail(x, "data out", GetLastError(), sent < 12 ? 0 : sent - 12);
        if (n != piece) return XfFail(x, "data out", XF_PROTO | 6, sent + n < 12 ? 0 : sent + n - 12);
        sent += piece;
    }
    if (len % x->Mps == 0) {
        ok = Ioctl(x->H, IOCTL_USBMTP_WRITE_ZLP, NULL, 0, NULL, 0, &n);
        if (!ok) return XfFail(x, "zero-length packet", GetLastError(), size);
    }
    return TRUE;
}

static BOOL XfOk(XF *x, const char *op, DWORD code)
{
    if (code == 0x2001) return TRUE;
    return XfFail(x, op, XF_PROTO | 0x10000 | code, 0);
}

/* One pass. In: GetObject of Handle (Size bytes, seed 5Ah). Out: an object
   of Size bytes with seed Pass sent, read back, deleted. */
static BOOL XfPass(XF *x, int out, DWORD storage, DWORD handle, DWORD size, DWORD pass)
{
    static unsigned char info[128];     /* the ObjectInfo dataset: 74 bytes */
    DWORD code, p3, got, seed = out ? (pass & 255) : 0x5A, at;
    const char *name = "XFER.BIN";
    if (out) {
        /* ObjectInfo: storage, format 3000h, protection, size, thumb and
           image fields 0, parent root, association 0, sequence 0, the
           file name, three empty strings */
        ZeroMemory(info, sizeof(info));
        info[0] = (unsigned char)storage; info[1] = (unsigned char)(storage >> 8);
        info[2] = (unsigned char)(storage >> 16); info[3] = (unsigned char)(storage >> 24);
        info[4] = 0x00; info[5] = 0x30;
        info[8] = (unsigned char)size; info[9] = (unsigned char)(size >> 8);
        info[10] = (unsigned char)(size >> 16); info[11] = (unsigned char)(size >> 24);
        at = 52;
        info[at++] = (unsigned char)(lstrlen(name) + 1);
        for (; *name; name++) { info[at++] = (unsigned char)*name; info[at++] = 0; }
        info[at++] = 0; info[at++] = 0;
        info[at++] = 0; info[at++] = 0; info[at++] = 0;
        if (!XfCommand(x, 0x100C, 2, storage, 0xFFFFFFFFUL, 0)) return FALSE;
        if (!XfDataOut(x, 0x100C, at, 0, info)) return FALSE;
        if (!XfResponse(x, &code, &handle) || !XfOk(x, "SendObjectInfo", code)) return FALSE;
        if (!XfCommand(x, 0x100D, 0, 0, 0, 0)) return FALSE;
        if (!XfDataOut(x, 0x100D, size, seed, NULL)) return FALSE;
        if (!XfResponse(x, &code, NULL) || !XfOk(x, "SendObject", code)) return FALSE;
    }
    if (!XfCommand(x, 0x1009, 1, handle, 0, 0)) return FALSE;
    if (!XfDataIn(x, seed, &got)) return FALSE;
    if (!XfResponse(x, &code, NULL) || !XfOk(x, "GetObject", code)) return FALSE;
    if (got != size) return XfFail(x, "GetObject", XF_PROTO | 7, got);
    x->Moved = got;
    if (out) {
        DWORD bad = x->Bad, checked = x->Checked;
        if (!XfCommand(x, 0x100B, 2, handle, 0, 0)) return FALSE;
        if (!XfResponse(x, &code, &p3) || !XfOk(x, "DeleteObject", code)) return FALSE;
        x->Bad = bad;
        x->Checked = checked;
        x->Moved = got;
    }
    return TRUE;
}

/* After a failed transfer that was not the device's removal: Cancel for
   the transaction until accepted, Get Device Status until OK (at most 10),
   bulk IN drained. FALSE if the device went away meanwhile. */
static BOOL XfRecover(XF *x, DWORD t0)
{
    USBMTP_CLASS_IN cl;
    USBMTP_TIMEOUTS t;
    unsigned char st[64];
    DWORD got, e = 0, last = 0xFFFFFFFF, k, drained = 0, reads = 0;
    BOOL ok;
    for (;;) {
        cl.Request = USBMTP_REQ_CANCEL;
        cl.TransactionId = x->Tid;
        cl.DataLength = 0;
        ok = Ioctl(x->H, IOCTL_USBMTP_CLASS_REQUEST, &cl, sizeof(cl), NULL, 0, &got);
        e = Err(ok);
        if (e != last) { SayF("xfer: %lu ms: Cancel for transaction %lu: error %lu\r\n", GetTickCount() - t0, x->Tid, e, 0); last = e; }
        if (ok) break;
        if (XF_GONE(e) || GetTickCount() - t0 > 1200000) return FALSE;
        Sleep(250);
    }
    for (k = 0; k < 10; k++) {
        cl.Request = USBMTP_REQ_STATUS;
        cl.TransactionId = 0;
        cl.DataLength = sizeof(st);
        ZeroMemory(st, sizeof(st));
        ok = Ioctl(x->H, IOCTL_USBMTP_CLASS_REQUEST, &cl, sizeof(cl), st, sizeof(st), &got);
        SayF("xfer: %lu ms: Get Device Status: error %lu, bytes %lu, code %04X\r\n", GetTickCount() - t0, Err(ok), got,
             ok && got >= 4 ? Le16(st + 2) : 0);
        if (!ok && XF_GONE(GetLastError())) return FALSE;
        if (ok && (got < 4 || Le16(st + 2) == 0x2001)) break;
        Sleep(100);
    }
    t.BulkInMs = 200;
    t.BulkOutMs = 200;
    Ioctl(x->H, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    for (k = 0; k < 64; k++) {
        ok = ReadFile(x->H, g_Data, sizeof(g_Data), &got, NULL);
        if (!ok) { e = GetLastError(); break; }
        reads++;
        drained += got;
    }
    SayF("xfer: %lu ms: bulk IN drained: %lu bytes in %lu reads, then error %lu\r\n", GetTickCount() - t0, drained, reads, e);
    t.BulkInMs = 10000;
    t.BulkOutMs = 10000;
    Ioctl(x->H, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    return !XF_GONE(e);
}

/* OpenSession (a session left open: closed and opened again), the first
   storage, and for In the largest object at its root. FALSE: logged. */
static BOOL XfOpen(XF *x, int out, DWORD *Storage, DWORD *Handle, DWORD *Size)
{
    DWORD code, got, n, k, nh = 0, handles[32];
    x->Tid = 0xFFFFFFFFUL;                  /* OpenSession is transaction 0 */
    if (!XfCommand(x, 0x1002, 1, 1, 0, 0) || !XfResponse(x, &code, NULL)) goto failed;
    if (code == 0x201E) {
        if (!XfCommand(x, 0x1003, 0, 0, 0, 0) || !XfResponse(x, &code, NULL)) goto failed;
        SayF("xfer: a session was open; CloseSession: response %04X\r\n", code, 0, 0, 0);
        x->Tid = 0xFFFFFFFFUL;
        if (!XfCommand(x, 0x1002, 1, 1, 0, 0) || !XfResponse(x, &code, NULL)) goto failed;
    }
    SayF("xfer: OpenSession: response %04X\r\n", code, 0, 0, 0);
    if (code != 0x2001) return FALSE;
    if (!XfCommand(x, 0x1004, 0, 0, 0, 0) || !XfDataIn(x, 0x100, &got) || !XfResponse(x, &code, NULL)) goto failed;
    SayF("xfer: GetStorageIDs: response %04X, %lu bytes\r\n", code, got, 0, 0);
    if (code != 0x2001 || got < 8) return FALSE;
    *Storage = Le32(g_Data + 16);
    *Handle = 0;
    if (out) {
        *Size = 2097152UL + 100;
        return TRUE;
    }
    *Size = 0;
    if (!XfCommand(x, 0x1007, 3, *Storage, 0, 0xFFFFFFFFUL) || !XfDataIn(x, 0x100, &got) ||
        !XfResponse(x, &code, NULL)) goto failed;
    if (code != 0x2001 || got < 4) { SayF("xfer: GetObjectHandles: response %04X\r\n", code, 0, 0, 0); return FALSE; }
    n = Le32(g_Data + 12);
    for (k = 0; k < n && k < 32 && 4 + 4 * k + 4 <= got; k++) handles[nh++] = Le32(g_Data + 16 + 4 * k);
    for (k = 0; k < nh; k++) {
        if (!XfCommand(x, 0x1008, 1, handles[k], 0, 0) || !XfDataIn(x, 0x100, &got) ||
            !XfResponse(x, &code, NULL)) goto failed;
        if (code == 0x2001 && got >= 12 && Le16(g_Data + 16) != 0x3001 && Le32(g_Data + 20) > *Size) {
            *Handle = handles[k];
            *Size = Le32(g_Data + 20);
        }
    }
    SayF("xfer: GetObjectHandles, GetObjectInfo of %lu objects: the largest %08lX, %lu bytes\r\n", nh, *Handle, *Size, 0);
    return *Handle != 0;
failed:
    if (x->Err & XF_PROTO) SayF("xfer: %s failed, exchange error %08lX\r\n", (DWORD)x->Phase, x->Err, 0, 0);
    else SayF("xfer: %s failed, error %lu\r\n", (DWORD)x->Phase, x->Err, 0, 0);
    return FALSE;
}

static void Xfer(int out)
{
    USBMTP_INFO i;
    USBMTP_CLAIM_INFO ci;
    USBMTP_TIMEOUTS t;
    XF x;
    DWORD got, code, storage = 0, handle = 0, size = 0, t0, tl, pass = 0, after = 0, recoveries = 0;
    DWORD lastPass = 0, lastMs = 0, lastChecked = 0;
    BOOL ok;
    ZeroMemory(&x, sizeof(x));
    x.H = Open();
    SayF("xfer: open %d, error %lu, %s\r\n", x.H != INVALID_HANDLE_VALUE, GetLastError(), (DWORD)(out ? "out and in" : "in"), 0);
    if (x.H == INVALID_HANDLE_VALUE) return;
    ok = Ioctl(x.H, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
    if (ok) ok = Ioctl(x.H, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
    t.BulkInMs = 10000;
    t.BulkOutMs = 10000;
    if (ok) ok = Ioctl(x.H, IOCTL_USBMTP_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, &got);
    SayF("xfer: info, claim and timeouts ok %d, error %lu, generation %lu, max packet %u\r\n", ok, Err(ok), ci.Generation,
         i.MaxPacketIn);
    if (!ok || !i.MaxPacketIn || sizeof(g_Data) % i.MaxPacketIn) { CloseHandle(x.H); return; }
    x.Mps = i.MaxPacketIn;
    if (!XfOpen(&x, out, &storage, &handle, &size)) goto close;
    SayF("xfer: storage %08lX, %lu bytes a pass; ready for the standby\r\n", storage, size, 0, 0);
    t0 = tl = GetTickCount();
    for (;;) {
        pass++;
        if (XfPass(&x, out, storage, handle, size, pass)) {
            if (pass <= 3 || recoveries || pass % 200 == 0 || x.Bad) {
                SayF("xfer: pass %lu, %lu ms: %lu bytes, %lu checked, ", pass, GetTickCount() - t0, x.Moved, x.Checked);
                SayF("%lu wrong\r\n", x.Bad, 0, 0, 0);
            }
            lastPass = pass;
            lastMs = GetTickCount() - t0;
            lastChecked = x.Checked;
            if (x.Bad) break;
            if (recoveries && ++after >= 3) break;
            if (GetTickCount() - tl > 600000) { Say("xfer: 600 s without a failure\r\n"); break; }
            continue;
        }
        SayF("xfer: the last good pass %lu, %lu ms, %lu checked\r\n", lastPass, lastMs, lastChecked, 0);
        if (x.Err & XF_PROTO)
            SayF("xfer: pass %lu, %lu ms: %s failed, exchange error %08lX, ", pass, GetTickCount() - t0, (DWORD)x.Phase, x.Err);
        else
            SayF("xfer: pass %lu, %lu ms: %s failed, error %lu, ", pass, GetTickCount() - t0, (DWORD)x.Phase, x.Err);
        SayF("%lu of %lu bytes in that phase, transaction %lu\r\n", x.Moved, size, x.Tid, 0);
        if (XF_GONE(x.Err)) { Say("xfer: the device is gone\r\n"); goto end; }
        if (++recoveries > 3) break;
        if (!XfRecover(&x, t0)) { Say("xfer: the device is gone\r\n"); goto end; }
        /* the session taken as lost: CloseSession (its answer logged), a new one, the object found again */
        if (XfCommand(&x, 0x1003, 0, 0, 0, 0) && XfResponse(&x, &code, NULL))
            SayF("xfer: CloseSession after the recovery: response %04X\r\n", code, 0, 0, 0);
        x.Full = 1;
        if (!XfOpen(&x, out, &storage, &handle, &size)) goto close;
        after = 0;
        tl = GetTickCount();
    }
close:
    if (XfCommand(&x, 0x1003, 0, 0, 0, 0) && XfResponse(&x, &code, NULL))
        SayF("xfer: CloseSession: response %04X\r\n", code, 0, 0, 0);
    else
        SayF("xfer: CloseSession: %s failed, error %08lX\r\n", (DWORD)x.Phase, x.Err, 0, 0);
end:
    SayF("xfer: %lu passes; close %d\r\n", pass, CloseHandle(x.H), 0, 0);
}

static void Zombie(void)
{
    HANDLE a = Open(), b = Open(), c;
    USBMTP_INFO i;
    USBMTP_CLAIM_INFO ci;
    static unsigned char buf[4096];
    DWORD got, t0, k;
    BOOL ok = FALSE;
    SayF("zombie: open A %d, B %d, error %lu\r\n", a != INVALID_HANDLE_VALUE, b != INVALID_HANDLE_VALUE, GetLastError(), 0);
    if (a == INVALID_HANDLE_VALUE || b == INVALID_HANDLE_VALUE) return;
    ok = Ioctl(a, IOCTL_USBMTP_CLAIM, NULL, 0, &ci, sizeof(ci), &got);
    SayF("zombie: claim A ok %d, error %lu, generation %lu\r\n", ok, Err(ok), ci.Generation, 0);
    Say("zombie: ready for the unplug\r\n");
    t0 = GetTickCount();
    for (k = 0; k < 600; k++) {
        ok = Ioctl(b, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
        if (!ok) break;
        Sleep(1000);
    }
    SayF("zombie: GET_INFO on B failed after %lu s, error %lu\r\n", (GetTickCount() - t0) / 1000, Err(ok), 0, 0);
    ok = ReadFile(a, buf, 512, &got, NULL);
    SayF("zombie: read on A: ok %d, error %lu\r\n", ok, Err(ok), 0, 0);
    c = Open();
    SayF("zombie: a new open while the zombie lives: %d, error %lu\r\n", c != INVALID_HANDLE_VALUE, GetLastError(), 0, 0);
    if (c != INVALID_HANDLE_VALUE) {
        ok = Ioctl(c, IOCTL_USBMTP_GET_INFO, NULL, 0, &i, sizeof(i), &got);
        SayF("zombie: GET_INFO on it: ok %d, error %lu, generation %lu\r\n", ok, Err(ok), ok ? i.Generation : 0, 0);
        CloseHandle(c);
    }
    Sleep(5000);
    SayF("zombie: close B %d\r\n", CloseHandle(b), 0, 0, 0);
    Sleep(2000);
    SayF("zombie: close A %d\r\n", CloseHandle(a), 0, 0, 0);
    Sleep(2000);
    c = Open();
    SayF("zombie: open after the last close: %d, error %lu\r\n", c != INVALID_HANDLE_VALUE, GetLastError(), 0, 0);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
}

/* power: an info handle reads GET_INFO every 250 ms for 600 s and logs a
   line, with the time since the start, whenever the power fields of
   interface 1.1 (or the flags, or the generation) change. When the device
   is gone (a code of 5, 1167, 6, or an open that fails) it opens
   \\.\USBMTP<n> again once a second, and goes on with the new device. It
   takes no claim and moves no data: it runs beside another MTPTEST that
   does. */
static void PowerWatch(void)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    INFOBUF u;
    DWORD t0 = GetTickCount(), got, e, now;
    DWORD last[6] = { 0xFFFFFFFFUL, 0, 0, 0, 0, 0 }, cur[6];
    BOOL ok;
    int k, same;
    while ((now = GetTickCount() - t0) < 600000UL) {
        if (h == INVALID_HANDLE_VALUE) {
            h = Open();
            if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
            ok = GetInfo(h, &u, &got);
            SayF("power: %lu ms: opened, generation %lu, cbSize %lu, error %lu\r\n", now, ok ? u.i.Generation : 0,
                 ok ? u.i.cbSize : 0, Err(ok));
            if (ok && got < INFO_SIZE_1_1) {
                Say("power: an interface 1.0 driver: no power fields\r\n");
                break;
            }
            last[0] = 0xFFFFFFFFUL;
        }
        ok = GetInfo(h, &u, &got);
        if (!ok) {
            e = GetLastError();
            SayF("power: %lu ms: GET_INFO failed, error %lu\r\n", now, e, 0, 0);
            if (e == 5 || e == 1167 || e == 6) {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
                continue;
            }
            Sleep(1000);
            continue;
        }
        cur[0] = u.b[INFO_POWER_AT];
        cur[1] = u.b[INFO_POWER_AT + 1];
        cur[2] = u.b[INFO_POWER_AT + 2];
        cur[3] = U16At(&u, INFO_POWER_AT + 4);
        cur[4] = U16At(&u, INFO_POWER_AT + 6);
        cur[5] = u.i.Flags | (u.i.Generation << 8);
        for (same = 1, k = 0; k < 6; k++) if (cur[k] != last[k]) same = 0;
        if (!same) {
            SayF("power: %lu ms: gate %02X, held %02X, held at the last refusal %02X", now, cur[0], cur[1], cur[2]);
            SayF(", refusals %lu, forced sets %lu, flags %02X\r\n", cur[3], cur[4], u.i.Flags, 0);
            for (k = 0; k < 6; k++) last[k] = cur[k];
        }
        Sleep(250);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    char test[32], file[MAX_PATH];
    char *p = cmd;
    int k, n = 0;
    (void)hi; (void)hp; (void)show;
    while (*p == ' ') p++;
    for (k = 0; *p && *p != ' ' && k < (int)sizeof(test) - 1; k++) test[k] = *p++;
    test[k] = 0;
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        n = *p++ - '0';
        if (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        while (*p == ' ') p++;
    }
    for (k = 0; *p && *p != ' ' && k < (int)sizeof(file) - 1; k++) file[k] = *p++;
    file[k] = 0;
    if (!file[0]) lstrcpy(file, "COM1");
    wsprintf(g_Name, "\\\\.\\USBMTP%d", n);
    g_Out = CreateFile(file, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Out != INVALID_HANDLE_VALUE) SetFilePointer(g_Out, 0, NULL, FILE_END);
    Say("MTPTEST ");
    Say(test);
    Say(" ");
    Say(g_Name);
    Say("\r\n");
    if (!lstrcmpi(test, "info")) {
        HANDLE h = Open();
        SayF("info: open %d, error %lu\r\n", h != INVALID_HANDLE_VALUE, GetLastError(), 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            Info(h);
            CloseHandle(h);
        }
    } else if (!lstrcmpi(test, "session")) {
        Session();
    } else if (!lstrcmpi(test, "zombie")) {
        Zombie();
    } else if (!lstrcmpi(test, "storage")) {
        Storage();
    } else if (!lstrcmpi(test, "standby")) {
        Standby();
    } else if (!lstrcmpi(test, "xfer")) {
        Xfer(0);
    } else if (!lstrcmpi(test, "xferout")) {
        Xfer(1);
    } else if (!lstrcmpi(test, "power")) {
        PowerWatch();
    } else {
        Say("usage: MTPTEST info|session|zombie|storage|standby|xfer|xferout|power [n] [file]\r\n");
    }
    Say("MTPTEST done\r\n");
    if (g_Out != INVALID_HANDLE_VALUE) CloseHandle(g_Out);
    return 0;
}
