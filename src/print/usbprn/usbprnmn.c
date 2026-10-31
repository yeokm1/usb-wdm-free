/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbprnmn.c - USBPRNMN.DLL, the Windows 98 SE print spooler port monitor
 * of usbprn (docs/design/printing.md section 4).
 *
 * Ports USB001, USB002, ...: one per printer USBPRN.SYS has numbered. The
 * port list comes from the PortName values USBPRN.SYS writes into its
 * devnodes (HKLM\Enum\USB\<id>\<instance>), so a printer that is unplugged
 * keeps its port. Port USB00n opens \\.\USBPRN<n> for the length of a job
 * (StartDocPort to EndDocPort) and for brief status reads.
 *
 * Written from the Windows 98 DDK printer documentation ("Port Monitors",
 * "Required Functions", the AddPort ... WritePort reference pages) and its
 * header INC_WIN98_WINSPLP.H (the MONITOR table). No C run-time: kernel32,
 * user32, advapi32 and winspool.drv only.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <windows.h>
#include <winspool.h>
#include "winsplp.h"        /* the 98 DDK's INC_WIN98_WINSPLP.H, copied by the build */
#include "prncore.h"
#include "usbprn.h"

#define UM_MONITOR_NAME     "USB Printer Port"
#define UM_PORT_DESC        "USB printer port"
#define UM_WRITE_DEFAULT_MS 45000       /* until the spooler sets the port timeouts */
#define UM_READ_DEFAULT_MS  1000
#define UM_TIMEOUT_MIN_MS   1000
#define UM_TIMEOUT_MAX_MS   3600000     /* 1 hour */
#define UM_SETTINGS_KEY     "Software\\usb-wdm-free\\usbprn"
#define UM_MAGIC            0x504E5355  /* 'USNP' */

static HINSTANCE g_Inst;

typedef struct UM_PORT {
    DWORD   Magic;
    DWORD   Number;             /* n of USB00n */
    HANDLE  Dev;                /* INVALID_HANDLE_VALUE when closed */
    DWORD   JobId;
    BOOL    InJob;
    DWORD   WriteMs, ReadMs;      /* what the driver is given */
    DWORD   CtoWriteMs;           /* SetPortTimeOuts' write timeout; 0 = none given */
    DWORD   RetryMs;              /* the printer's "Transmission retry"; 0 = unknown */
    char    Printer[260];
} UM_PORT;

/* ---- trace (test flavour only: UM_TRACE appends to C:\USBPRNMN.LOG) ------------- */

#if defined(UM_TRACE)
static void UmTrace(const char *fmt, DWORD a, DWORD b)
{
    char line[256];
    DWORD n, w;
    HANDLE h = CreateFileA("C:\\USBPRNMN.LOG", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    n = (DWORD)wsprintfA(line, fmt, a, b);
    line[n++] = '\r';
    line[n++] = '\n';
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, n, &w, NULL);
    CloseHandle(h);
}
#define UMT(f, a, b) UmTrace(f, (DWORD)(a), (DWORD)(b))
#else
#define UMT(f, a, b) ((void)0)
#endif

/* ---- small helpers ----------------------------------------------------------- */

static void UmZero(void *p, DWORD n)
{
    BYTE *b = (BYTE *)p;
    while (n--)
        *b++ = 0;
}

static UM_PORT *UmPort(HANDLE h)
{
    UM_PORT *p = (UM_PORT *)h;
    return (p && p->Magic == UM_MAGIC) ? p : NULL;
}

static HANDLE UmOpenDevice(DWORD n, BOOL write)
{
    char path[16];

    if (!PcDevicePath(n, path))
        return INVALID_HANDLE_VALUE;
    return CreateFileA(path, write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL UmIoctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *got)
{
    DWORD dummy = 0;
    return DeviceIoControl(h, code, in, inLen, out, outLen, got ? got : &dummy, NULL);
}

/*
 * The printer's "Transmission retry" in ms, 0 when it is not usable. On
 * 98 SE the field (and the write timeout SetPortTimeOuts passes) came as
 * 45000000 for the dialog's "45 seconds": a thousand times the value in
 * ms (printing.md 7.1). So 1 to 3600 is read as seconds (the documented
 * unit), 3601 to 999999 as ms, and from 1000000 as that larger unit; a
 * result outside 1 s to 1 hour is not used.
 */
static DWORD UmRetryMs(DWORD v)
{
    DWORD ms;

    if (v == 0)
        return 0;
    if (v <= UM_TIMEOUT_MAX_MS / 1000)     /* 1 to 3600: seconds, as documented */
        ms = v * 1000;
    else if (v < 1000000)                   /* 3601 to 999999: milliseconds */
        ms = v;
    else                                    /* 1000000 and up: as 98 SE gave it */
        ms = v / 1000;
    if (ms < UM_TIMEOUT_MIN_MS || ms > UM_TIMEOUT_MAX_MS)
        return 0;
    return ms;
}

/*
 * The write timeout: the printer's "Transmission retry" (Properties,
 * Details) when it reads as 1 s to 1 hour; else the one SetPortTimeOuts
 * gave, when that is 1 s to 1 hour in ms; else 45 s. The printer's value
 * comes first because on 98 SE SetPortTimeOuts passed it multiplied by a
 * thousand (45000000 for 45 s), which cannot be told from a long ms
 * value. A write that moves nothing for that long fails, and the spooler
 * offers Retry.
 */
static void UmPickWriteTimeout(UM_PORT *p)
{
    if (p->RetryMs)
        p->WriteMs = p->RetryMs;
    else if (p->CtoWriteMs >= UM_TIMEOUT_MIN_MS && p->CtoWriteMs <= UM_TIMEOUT_MAX_MS)
        p->WriteMs = p->CtoWriteMs;
    else
        p->WriteMs = UM_WRITE_DEFAULT_MS;
}

static void UmSendTimeouts(UM_PORT *p)
{
    UP_TIMEOUTS t;

    t.WriteMs = p->WriteMs;
    t.ReadMs = p->ReadMs;
    if (p->Dev != INVALID_HANDLE_VALUE)
        UmIoctl(p->Dev, IOCTL_USBPRN_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, NULL);
}

static void UmClose(UM_PORT *p)
{
    if (p->Dev != INVALID_HANDLE_VALUE) {
        CloseHandle(p->Dev);
        p->Dev = INVALID_HANDLE_VALUE;
    }
}

static BOOL UmEnsureOpen(UM_PORT *p)
{
    if (p->Dev != INVALID_HANDLE_VALUE)
        return TRUE;
    p->Dev = UmOpenDevice(p->Number, TRUE);
    UMT("open USB%03lu: %lu", p->Number, p->Dev != INVALID_HANDLE_VALUE);
    if (p->Dev == INVALID_HANDLE_VALUE)
        return FALSE;
    UmSendTimeouts(p);
    return TRUE;
}

/*
 * The error a stalled write reports. On 98 SE the spooler offers Retry
 * (and retries by itself every 5 s) for ERROR_OUT_OF_PAPER ("The printer
 * is out of paper") and ERROR_NOT_READY ("The printer is not ready");
 * ERROR_GEN_FAILURE and ERROR_TIMEOUT got an OK-only box and the job was
 * deleted (printing.md 7.1). So every other cause is ERROR_NOT_READY.
 */
static DWORD UmWin32Error(int e)
{
    return e == PC_ERR_PAPER ? ERROR_OUT_OF_PAPER : ERROR_NOT_READY;
}

static DWORD UmReadSetting(const char *name, DWORD def)
{
    HKEY k;
    DWORD v = def, type = 0, size = sizeof(v);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, UM_SETTINGS_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return def;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS ||
        type != REG_DWORD || size != sizeof(v))
        v = def;
    RegCloseKey(k);
    return v;
}

static void UmWriteSetting(const char *name, DWORD v)
{
    HKEY k;
    DWORD disp;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, UM_SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL,
                        &k, &disp) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

/* ---- the port list (design 4.2) ---------------------------------------------- */

/* Whether the devnode's driver (Services\Class\<Driver>) is USBPRN.SYS. */
static BOOL UmIsOurs(HKEY inst)
{
    char drv[128], path[200], mp[64];
    DWORD type, size = sizeof(drv);
    HKEY k;
    BOOL ours = FALSE;

    if (RegQueryValueExA(inst, "Driver", NULL, &type, (BYTE *)drv, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size < 2 || size > sizeof(drv) - 1)
        return FALSE;
    drv[size] = 0;
    wsprintfA(path, "System\\CurrentControlSet\\Services\\Class\\%s", drv);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return FALSE;
    size = sizeof(mp);
    if (RegQueryValueExA(k, "NTMPDriver", NULL, &type, (BYTE *)mp, &size) == ERROR_SUCCESS &&
        type == REG_SZ && size >= 2 && size < sizeof(mp)) {
        mp[size] = 0;
        ours = lstrcmpiA(mp, "USBPRN.SYS") == 0;
    }
    RegCloseKey(k);
    return ours;
}

/* Marks in used[] every port number a USBPRN.SYS devnode names. */
static void UmScanPorts(BYTE used[PC_PORT_MAX / 8 + 1])
{
    HKEY root, id, inst;
    char n0[260], n1[260], pn[16];
    DWORD i0, i1, size, type, budget = 2048;

    UmZero(used, PC_PORT_MAX / 8 + 1);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum\\USB", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return;
    for (i0 = 0; budget && RegEnumKeyA(root, i0, n0, sizeof(n0)) == ERROR_SUCCESS; i0++) {
        if (RegOpenKeyExA(root, n0, 0, KEY_READ, &id) != ERROR_SUCCESS)
            continue;
        for (i1 = 0; budget && RegEnumKeyA(id, i1, n1, sizeof(n1)) == ERROR_SUCCESS; i1++, budget--) {
            DWORD n;
            if (RegOpenKeyExA(id, n1, 0, KEY_READ, &inst) != ERROR_SUCCESS)
                continue;
            size = sizeof(pn) - 1;
            if (RegQueryValueExA(inst, "PortName", NULL, &type, (BYTE *)pn, &size) == ERROR_SUCCESS &&
                type == REG_SZ && size >= 1) {
                pn[size] = 0;
                n = PcParsePortName(pn, (DWORD)lstrlenA(pn));
                if (n && UmIsOurs(inst))
                    used[n >> 3] |= (BYTE)(1u << (n & 7));
            }
            RegCloseKey(inst);
        }
        RegCloseKey(id);
    }
    RegCloseKey(root);
}

static BOOL WINAPI UmEnumPorts(LPSTR pName, DWORD Level, LPBYTE pPorts, DWORD cbBuf,
                               LPDWORD pcbNeeded, LPDWORD pcReturned)
{
    BYTE used[PC_PORT_MAX / 8 + 1];
    DWORD n, count = 0, need, fixed, str, i;
    LPSTR end;
    char name[8];

    UNREFERENCED_PARAMETER(pName);
    if (pcbNeeded)
        *pcbNeeded = 0;
    if (pcReturned)
        *pcReturned = 0;
    if (Level != 1 && Level != 2) {
        SetLastError(ERROR_INVALID_LEVEL);
        return FALSE;
    }
    UmScanPorts(used);
    for (n = 1; n <= PC_PORT_MAX; n++)
        if (used[n >> 3] & (1u << (n & 7)))
            count++;
    fixed = Level == 1 ? sizeof(PORT_INFO_1A) : sizeof(PORT_INFO_2A);
    str = 7 + (Level == 2 ? sizeof(UM_MONITOR_NAME) + sizeof(UM_PORT_DESC) : 0);
    need = count * (fixed + str);
    if (pcbNeeded)
        *pcbNeeded = need;
    UMT("EnumPorts level %lu: %lu ports", Level, count);
    if (cbBuf < need || (need && pPorts == NULL)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    end = (LPSTR)pPorts + cbBuf;
    for (n = 1, i = 0; n <= PC_PORT_MAX; n++) {
        if (!(used[n >> 3] & (1u << (n & 7))))
            continue;
        PcPortName(n, name);
        end -= 7;
        lstrcpyA(end, name);
        if (Level == 1) {
            ((PORT_INFO_1A *)pPorts)[i].pName = end;
        } else {
            PORT_INFO_2A *p2 = &((PORT_INFO_2A *)pPorts)[i];
            p2->pPortName = end;
            end -= sizeof(UM_MONITOR_NAME);
            lstrcpyA(end, UM_MONITOR_NAME);
            p2->pMonitorName = end;
            end -= sizeof(UM_PORT_DESC);
            lstrcpyA(end, UM_PORT_DESC);
            p2->pDescription = end;
            p2->fPortType = PORT_TYPE_WRITE | PORT_TYPE_READ;
            p2->Reserved = 0;
        }
        i++;
    }
    if (pcReturned)
        *pcReturned = count;
    return TRUE;
}

/* ---- the job (design 4.3) ---------------------------------------------------- */

static BOOL WINAPI UmOpenPort(LPSTR pName, PHANDLE pHandle)
{
    DWORD n = pName ? PcParsePortName(pName, (DWORD)lstrlenA(pName)) : 0;
    UM_PORT *p;

    UMT("OpenPort USB%03lu", n, 0);
    if (n == 0 || pHandle == NULL) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    p = (UM_PORT *)LocalAlloc(LPTR, sizeof(UM_PORT));
    if (p == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    p->Magic = UM_MAGIC;
    p->Number = n;
    p->Dev = INVALID_HANDLE_VALUE;
    p->WriteMs = UM_WRITE_DEFAULT_MS;
    p->ReadMs = UM_READ_DEFAULT_MS;
    *pHandle = (HANDLE)p;
    return TRUE;
}

static BOOL WINAPI UmClosePort(HANDLE hPort)
{
    UM_PORT *p = UmPort(hPort);

    if (p == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    UMT("ClosePort USB%03lu", p->Number, 0);
    UmClose(p);
    p->Magic = 0;
    LocalFree(p);
    return TRUE;
}

/*
 * Opens the device for the job. A printer that is not connected does not
 * fail the job here: WritePort fails, and the spooler's retry dialog lets
 * the user plug it in.
 */
static BOOL WINAPI UmStartDocPort(HANDLE hPort, LPSTR pPrinterName, DWORD JobId, DWORD Level,
                                  LPBYTE pDocInfo)
{
    UM_PORT *p = UmPort(hPort);

    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(pDocInfo);
    if (p == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    p->JobId = JobId;
    p->InJob = TRUE;
    p->Printer[0] = 0;
    p->RetryMs = 0;
    if (pPrinterName) {
        HANDLE pr = NULL;
        lstrcpynA(p->Printer, pPrinterName, sizeof(p->Printer));
        if (OpenPrinterA(p->Printer, &pr, NULL)) {
            BYTE buf[1024];
            DWORD need = 0;
            if (GetPrinterA(pr, 5, buf, sizeof(buf), &need))
                p->RetryMs = UmRetryMs(((PRINTER_INFO_5A *)buf)->TransmissionRetryTimeout);
            ClosePrinter(pr);
        }
    }
    UmPickWriteTimeout(p);
    UMT("StartDocPort USB%03lu job %lu", p->Number, JobId);
    UMT("write timeout %lu ms (transmission retry %lu ms)", p->WriteMs, p->RetryMs);
    UmEnsureOpen(p);
    return TRUE;
}

/*
 * One WriteFile. A write that moved some bytes returns TRUE with them (the
 * spooler sends the rest); one that moved none fails with the reason from
 * the status byte (design 7.1), and the spooler shows its retry dialog.
 */
static BOOL WINAPI UmWritePort(HANDLE hPort, LPBYTE pBuffer, DWORD cbBuf, LPDWORD pcbWritten)
{
    UM_PORT *p = UmPort(hPort);
    DWORD done = 0;
    UP_INFO info;
    int e;

    if (pcbWritten)
        *pcbWritten = 0;
    if (p == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!UmEnsureOpen(p)) {
        UMT("WritePort USB%03lu: no device", p->Number, 0);
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    if (!WriteFile(p->Dev, pBuffer, cbBuf, &done, NULL)) {
        /* Removed (or asleep): the handle is dead; Retry reopens. */
        UMT("WritePort USB%03lu: WriteFile failed %lu", p->Number, GetLastError());
        UmClose(p);
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    if (pcbWritten)
        *pcbWritten = done;
    if (done == cbBuf || done > 0)
        return TRUE;
    UmZero(&info, sizeof(info));
    if (UmIoctl(p->Dev, IOCTL_USBPRN_GET_INFO, NULL, 0, &info, sizeof(info), NULL))
        e = PcStatusError((info.Flags & UP_F_STATUS_VALID) != 0, info.LastStatus);
    else
        e = PC_ERR_TIMEOUT;
    UMT("WritePort USB%03lu: nothing moved, status %02lx", p->Number, info.LastStatus);
    SetLastError(UmWin32Error(e));
    return FALSE;
}

static BOOL WINAPI UmReadPort(HANDLE hPort, LPBYTE pBuffer, DWORD cbBuffer, LPDWORD pcbRead)
{
    UM_PORT *p = UmPort(hPort);
    UP_TIMEOUTS t;
    HANDLE h;
    DWORD got = 0;
    BOOL ok;

    if (pcbRead)
        *pcbRead = 0;
    if (p == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    /* A handle of its own: a language monitor reads from another thread
       while WritePort and EndDocPort may close the job's handle. */
    h = UmOpenDevice(p->Number, FALSE);
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_NOT_READY);
        return FALSE;
    }
    t.WriteMs = p->WriteMs;
    t.ReadMs = p->ReadMs;
    UmIoctl(h, IOCTL_USBPRN_SET_TIMEOUTS, &t, sizeof(t), NULL, 0, NULL);
    ok = ReadFile(h, pBuffer, cbBuffer, &got, NULL);
    CloseHandle(h);
    UMT("ReadPort USB%03lu: %lu bytes", p->Number, ok ? got : 0);
    if (!ok || got == 0) {
        SetLastError(ok ? ERROR_TIMEOUT : ERROR_NOT_READY);
        return FALSE;
    }
    if (pcbRead)
        *pcbRead = got;
    return TRUE;
}

/*
 * Ends the job: a job deleted while printing gets SOFT_RESET when the
 * "reset on cancel" setting is on (design 8, off by default); the handle is
 * closed and the job deleted from the queue (printer_0rat: the port monitor
 * calls SetJob when the job is done).
 */
static BOOL WINAPI UmEndDocPort(HANDLE hPort)
{
    UM_PORT *p = UmPort(hPort);
    HANDLE pr = NULL;
    BOOL deleting = FALSE;

    if (p == NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (p->Printer[0] && OpenPrinterA(p->Printer, &pr, NULL)) {
        BYTE buf[1024];
        DWORD need = 0;
        if (GetJobA(pr, p->JobId, 1, buf, sizeof(buf), &need)) {
            deleting = (((JOB_INFO_1A *)buf)->Status & JOB_STATUS_DELETING) != 0;
        } else if (need > sizeof(buf)) {
            /* long document or user names: the structure with its strings */
            BYTE *big = (BYTE *)LocalAlloc(LMEM_FIXED, need);
            if (big) {
                if (GetJobA(pr, p->JobId, 1, big, need, &need))
                    deleting = (((JOB_INFO_1A *)big)->Status & JOB_STATUS_DELETING) != 0;
                LocalFree(big);
            }
        }
    }
    if (deleting && p->Dev != INVALID_HANDLE_VALUE && UmReadSetting("ResetOnCancel", 0)) {
        UMT("EndDocPort USB%03lu: job deleted, soft reset", p->Number, 0);
        UmIoctl(p->Dev, IOCTL_USBPRINT_SOFT_RESET, NULL, 0, NULL, 0, NULL);
    }
    UmClose(p);
    if (pr) {
        SetJobA(pr, p->JobId, 0, NULL, JOB_CONTROL_CANCEL);
        ClosePrinter(pr);
    }
    UMT("EndDocPort USB%03lu job %lu", p->Number, p->JobId);
    p->InJob = FALSE;
    return TRUE;
}

static BOOL WINAPI UmSetPortTimeOuts(HANDLE hPort, LPCOMMTIMEOUTS lpCTO, DWORD reserved)
{
    UM_PORT *p = UmPort(hPort);

    UNREFERENCED_PARAMETER(reserved);
    if (p == NULL || lpCTO == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    p->CtoWriteMs = lpCTO->WriteTotalTimeoutConstant;
    UmPickWriteTimeout(p);
    p->ReadMs = lpCTO->ReadTotalTimeoutConstant ? lpCTO->ReadTotalTimeoutConstant : UM_READ_DEFAULT_MS;
    UMT("SetPortTimeOuts write %lu read %lu", p->WriteMs, p->ReadMs);
    UmSendTimeouts(p);
    return TRUE;
}

/* No IEEE 1284 control codes are documented (design 7.3): the language
   monitor falls back to ReadPort. */
static BOOL WINAPI UmGetPrinterDataFromPort(HANDLE hPort, DWORD ControlID, LPSTR pValueName,
                                            LPSTR lpInBuffer, DWORD cbInBuffer, LPSTR lpOutBuffer,
                                            DWORD cbOutBuffer, LPDWORD lpcbReturned)
{
    UNREFERENCED_PARAMETER(hPort);
    UNREFERENCED_PARAMETER(pValueName);
    UNREFERENCED_PARAMETER(lpInBuffer);
    UNREFERENCED_PARAMETER(cbInBuffer);
    UNREFERENCED_PARAMETER(lpOutBuffer);
    UNREFERENCED_PARAMETER(cbOutBuffer);
    UMT("GetPrinterDataFromPort ControlID %lu", ControlID, 0);
    if (lpcbReturned)
        *lpcbReturned = 0;
    SetLastError(ERROR_INVALID_FUNCTION);
    return FALSE;
}

/* ---- port management (design 4.2) -------------------------------------------- */

static BOOL WINAPI UmAddPort(LPSTR pName, HWND hWnd, LPSTR pMonitorName)
{
    UNREFERENCED_PARAMETER(pName);
    UNREFERENCED_PARAMETER(pMonitorName);
    MessageBoxA(hWnd, "USB printer ports are added by themselves when a USB printer is "
                "connected: USB001 for the first printer, USB002 for the second, and so on.",
                UM_MONITOR_NAME, MB_OK | MB_ICONINFORMATION);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL WINAPI UmDeletePort(LPSTR pName, HWND hWnd, LPSTR pPortName)
{
    UNREFERENCED_PARAMETER(pName);
    UNREFERENCED_PARAMETER(pPortName);
    MessageBoxA(hWnd, "A USB printer port stays while Windows knows its printer. To remove it, "
                "remove the printer's USB device in Device Manager.",
                UM_MONITOR_NAME, MB_OK | MB_ICONINFORMATION);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL WINAPI UmConfigurePort(LPSTR pName, HWND hWnd, LPSTR pPortName)
{
    DWORD n = pPortName ? PcParsePortName(pPortName, (DWORD)lstrlenA(pPortName)) : 0;
    char text[512], name[8];
    const char *state = "not connected";
    UP_INFO info;
    HANDLE h;
    DWORD reset = UmReadSetting("ResetOnCancel", 0);
    BYTE st = 0;

    UNREFERENCED_PARAMETER(pName);
    if (n == 0) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    PcPortName(n, name);
    UmZero(&info, sizeof(info));
    h = UmOpenDevice(n, FALSE);
    if (h != INVALID_HANDLE_VALUE) {
        UmIoctl(h, IOCTL_USBPRN_GET_INFO, NULL, 0, &info, sizeof(info), NULL);
        if (UmIoctl(h, IOCTL_USBPRINT_GET_LPT_STATUS, NULL, 0, &st, 1, NULL)) {
            switch (PcStatusError(1, st)) {
            case PC_ERR_PAPER:   state = "out of paper"; break;
            case PC_ERR_FAULT:   state = "error"; break;
            case PC_ERR_OFFLINE: state = "offline"; break;
            default:             state = "ready"; break;
            }
        } else {
            state = "connected, no status";
        }
        CloseHandle(h);
    }
    info.Description[sizeof(info.Description) - 1] = 0;
    wsprintfA(text, "Port %s\r\nPrinter: %s\r\nStatus: %s\r\n\r\n"
              "Reset the printer when a job is cancelled: %s\r\n"
              "Change this setting (it applies to every USB printer port)?",
              name, info.Description[0] ? info.Description : "(not connected)", state,
              reset ? "On" : "Off");
    if (MessageBoxA(hWnd, text, UM_MONITOR_NAME, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        UmWriteSetting("ResetOnCancel", reset ? 0 : 1);
    return TRUE;
}

/* ---- entry points ------------------------------------------------------------- */

static MONITOR g_Monitor = {
    UmEnumPorts,
    UmOpenPort,
    NULL,                       /* OpenPortEx: language monitors only */
    UmStartDocPort,
    UmWritePort,
    UmReadPort,
    UmEndDocPort,
    UmClosePort,
    UmAddPort,
    UmConfigurePort,
    UmDeletePort,
    UmGetPrinterDataFromPort,
    UmSetPortTimeOuts,
};

BOOL WINAPI InitializeMonitorEx(LPSTR pRegisterRoot, LPMONITOR pMonitor)
{
    UNREFERENCED_PARAMETER(pRegisterRoot);
    UMT("InitializeMonitorEx", 0, 0);
    if (pMonitor == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *pMonitor = g_Monitor;
    return TRUE;
}

/*
 * RUNDLL32 USBPRNMN.DLL,InstallMonitor - registers the monitor with the
 * running spooler (AddMonitor), so its ports appear without a restart
 * (design 4.1). The INF runs it once after the install.
 */
void CALLBACK InstallMonitor(HWND hwnd, HINSTANCE hinst, LPSTR cmd, int show)
{
    MONITOR_INFO_2A mi;
    BOOL ok;

    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(hinst);
    UNREFERENCED_PARAMETER(cmd);
    UNREFERENCED_PARAMETER(show);
    mi.pName = UM_MONITOR_NAME;
    mi.pEnvironment = "Windows 4.0";  /* 98: NULL is refused (ERROR_INVALID_ENVIRONMENT) */
    mi.pDLLName = "USBPRNMN.DLL";
    ok = AddMonitorA(NULL, 2, (LPBYTE)&mi);
    UMT("AddMonitor: %lu (%lu)", ok, ok ? 0 : GetLastError());
}

BOOL WINAPI DllEntryPoint(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        g_Inst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}
