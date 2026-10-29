/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * uacco.c - UACCI1.DLL, the MEDIA class co-installer shipped with uac2
 * (docs/design/uac2.md section 16.4). For a device whose compatible ids
 * hold USB\Class_01&SubClass_0h&Prot_20 (a USB Audio 2.0 function), it
 * marks every driver node of Windows' wdma_usb.inf that installs the
 * usbaudio service DNF_BAD_DRIVER at DIF_SELECTBESTCOMPATDRV, and refuses
 * such a node at DIF_ALLOW_INSTALL; every other device and request is left
 * alone. UacInstall and UacUninstall (rundll32 entries) add and remove its
 * CoDeviceInstallers string. Original code from the Windows driver kit
 * documentation of class co-installers, SetupAPI and the INF format; no
 * sample code.
 */
#include <windows.h>
#include <setupapi.h>

#ifndef DNF_BAD_DRIVER
#define DNF_BAD_DRIVER 0x00000800
#endif

#define UACCO_NAME  "UACCI1.DLL"
#define UACCO_ENTRY "UACCI1.DLL,UacCoInstaller"
#define UACCO_KEY   "SYSTEM\\CurrentControlSet\\Control\\CoDeviceInstallers"
#define UACCO_MEDIA "{4D36E96C-E325-11CE-BFC1-08002BE10318}"

/* ---- the log: one bounded record per call, serialised ---------------- */

static void LogLine(const char *text)
{
    char path[MAX_PATH], rec[600];
    HANDLE f, m;
    DWORD n;
    int len;
    UINT w = GetWindowsDirectoryA(path, MAX_PATH - 12);
    if (w == 0 || w >= MAX_PATH - 12) return;
    lstrcatA(path, "\\UACCI.LOG");
    lstrcpynA(rec, text, sizeof(rec) - 2);
    len = lstrlenA(rec);
    rec[len++] = '\r';
    rec[len++] = '\n';
    m = CreateMutexA(NULL, FALSE, "UACCI_LOG");
    if (m) WaitForSingleObject(m, 5000);
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, NULL, FILE_END);
        WriteFile(f, rec, (DWORD)len, &n, NULL);
        CloseHandle(f);
    }
    if (m) {
        ReleaseMutex(m);
        CloseHandle(m);
    }
}

static void Log2(const char *what, const char *dev, const char *more, DWORD err)
{
    char line[600];
    wsprintfA(line, "UACCI1 %.40s dev=%.200s %.200s err=%lu", what, dev, more, err);
    LogLine(line);
}

#ifdef UACCO_TEST
/* Test build only: HKLM\SOFTWARE\usb-wdm-free\uacci, DWORD Inject: 1 the
 * selected node's details fail, 2 a wdma_usb.inf node's section fails. */
static DWORD Inject(void)
{
    HKEY k;
    DWORD v = 0, t, n = sizeof(v);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\usb-wdm-free\\uacci", 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "Inject", NULL, &t, (LPBYTE)&v, &n) != ERROR_SUCCESS ||
            t != REG_DWORD) v = 0;
        RegCloseKey(k);
    }
    return v;
}
#else
#define Inject() 0UL
#endif

/* ---- the rule ---------------------------------------------------------- */

static int HexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* One id of exactly USB\Class_01&SubClass_0h&Prot_20, any case. */
static int IsUac2Id(const char *s)
{
    static const char head[] = "usb\\class_01&subclass_0";
    static const char tail[] = "&prot_20";
    int i;
    for (i = 0; head[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != head[i]) return 0;
    }
    if (!HexDigit(s[i])) return 0;
    s += i + 1;
    for (i = 0; tail[i]; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != tail[i]) return 0;
    }
    return s[i] == 0;
}

/* 1 the rule is met, 0 not, -1 the ids could not be read. */
static int RuleMet(HDEVINFO set, PSP_DEVINFO_DATA dev)
{
    char ids[2048];
    DWORD type, need, i;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_COMPATIBLEIDS, &type,
                                           (PBYTE)ids, sizeof(ids) - 2, &need)) {
        DWORD e = GetLastError();
        return (e == ERROR_INVALID_DATA) ? 0 : -1;   /* no compatible ids */
    }
    if (type != REG_MULTI_SZ) return 0;
    ids[sizeof(ids) - 2] = 0;
    ids[sizeof(ids) - 1] = 0;
    for (i = 0; i < sizeof(ids) - 1 && ids[i]; ) {
        if (IsUac2Id(ids + i)) return 1;
        i += (DWORD)lstrlenA(ids + i) + 1;
    }
    return 0;
}

/* ---- Windows' node ----------------------------------------------------- */

static const char *BaseName(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    return b;
}

/* 1 usbaudio's (wdma_usb.inf, a .Services section adding usbaudio), 0 not,
 * -1 a wdma_usb.inf node whose section or services could not be read, -2
 * the node's details could not be read at all. */
static int IsUsbAudioNode(HDEVINFO set, PSP_DEVINFO_DATA dev, PSP_DRVINFO_DATA_A drv,
                          int selected)
{
    char detail[4096], actual[260], svc[300], name[64];
    PSP_DRVINFO_DETAIL_DATA_A d = (PSP_DRVINFO_DETAIL_DATA_A)detail;
    DWORD need;
    HINF inf;
    INFCONTEXT ctx;
    int found = 0;

    d->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_A);
    if ((selected && Inject() == 1) ||
        (!SetupDiGetDriverInfoDetailA(set, dev, drv, d, sizeof(detail), &need) &&
         GetLastError() != ERROR_INSUFFICIENT_BUFFER))
        return -2;
    if (lstrcmpiA(BaseName(d->InfFileName), "wdma_usb.inf") != 0) return 0;
    if (Inject() == 2) return -1;
    inf = SetupOpenInfFileA(d->InfFileName, NULL, INF_STYLE_WIN4, NULL);
    if (inf == INVALID_HANDLE_VALUE) return -1;
    if (!SetupDiGetActualSectionToInstallA(inf, d->SectionName, actual,
                                           sizeof(actual) - 10, NULL, NULL)) {
        SetupCloseInfFile(inf);
        return -1;
    }
    lstrcatA(actual, ".Services");
    /* Fail closed: a wdma_usb.inf node is usbaudio's unless its services
     * section reads and names no usbaudio. */
    if (SetupGetLineCountA(inf, actual) < 0) {
        SetupCloseInfFile(inf);
        return -1;
    }
    if (SetupFindFirstLineA(inf, actual, "AddService", &ctx)) {
        do {
            name[0] = 0;
            if (!SetupGetStringFieldA(&ctx, 1, name, sizeof(name), NULL)) found = -1;
            else if (lstrcmpiA(name, "usbaudio") == 0) found = 1;
        } while (found == 0 && SetupFindNextMatchLineA(&ctx, "AddService", &ctx));
    }
    SetupCloseInfFile(inf);
    (void)svc;
    return found;
}

static void DevId(HDEVINFO set, PSP_DEVINFO_DATA dev, char *id, DWORD size)
{
    id[0] = 0;
    if (!SetupDiGetDeviceInstanceIdA(set, dev, id, size, NULL)) id[0] = 0;
}

/* DIF_SELECTBESTCOMPATDRV, before the default handler. */
static DWORD MarkNodes(HDEVINFO set, PSP_DEVINFO_DATA dev, const char *id)
{
    SP_DRVINFO_DATA_A drv;
    SP_DRVINSTALL_PARAMS ip;
    DWORD i, marked = 0;
    char more[64];

    for (i = 0; ; i++) {
        int r;
        drv.cbSize = sizeof(drv);
        if (!SetupDiEnumDriverInfoA(set, dev, SPDIT_COMPATDRIVER, i, &drv)) {
            DWORD e = GetLastError();
            if (e == ERROR_NO_MORE_ITEMS) break;
            Log2("select: list unreadable, refused", id, "", e);
            return e;
        }
        r = IsUsbAudioNode(set, dev, &drv, 0);
        if (r == 0) continue;
        if (r < 0) {
            Log2("select: a node unreadable, refused", id, "", GetLastError());
            return ERROR_DI_DONT_INSTALL;
        }
        ip.cbSize = sizeof(ip);
        if (!SetupDiGetDriverInstallParamsA(set, dev, &drv, &ip)) {
            DWORD e = GetLastError();
            Log2("select: params unreadable, refused", id, "", e);
            return e;
        }
        ip.Flags |= DNF_BAD_DRIVER;
        if (!SetupDiSetDriverInstallParamsA(set, dev, &drv, &ip)) {
            DWORD e = GetLastError();
            Log2("select: mark failed, refused", id, "", e);
            return e;
        }
        marked++;
    }
    wsprintfA(more, "usbaudio nodes marked=%lu", marked);
    Log2("select", id, more, 0);
    return NO_ERROR;
}

/* DIF_ALLOW_INSTALL. */
static DWORD CheckSelected(HDEVINFO set, PSP_DEVINFO_DATA dev, const char *id)
{
    SP_DRVINFO_DATA_A sel;
    int r;
    sel.cbSize = sizeof(sel);
    if (!SetupDiGetSelectedDriverA(set, dev, &sel)) {
        DWORD e = GetLastError();
        if (e == ERROR_NO_DRIVER_SELECTED) return NO_ERROR;   /* a null driver */
        Log2("allow: selected unreadable, refused", id, "", e);
        return ERROR_DI_DONT_INSTALL;
    }
    r = IsUsbAudioNode(set, dev, &sel, 1);
    if (r == 0) return NO_ERROR;
    Log2(r > 0 ? "allow: usbaudio refused" : "allow: selected node unreadable, refused",
         id, "", 0);
    return ERROR_DI_DONT_INSTALL;
}

DWORD CALLBACK UacCoInstaller(DI_FUNCTION f, HDEVINFO set, PSP_DEVINFO_DATA dev,
                              PCOINSTALLER_CONTEXT_DATA ctx)
{
    char id[260];
    int rule;
    if (ctx && ctx->PostProcessing) return ctx->InstallResult;
    if (!dev || (f != DIF_SELECTBESTCOMPATDRV && f != DIF_ALLOW_INSTALL)) return NO_ERROR;
    rule = RuleMet(set, dev);
    if (rule <= 0) {
        if (rule < 0) {
            DevId(set, dev, id, sizeof(id));
            Log2("ids unreadable, left alone", id, "", GetLastError());
        }
        return NO_ERROR;
    }
    DevId(set, dev, id, sizeof(id));
    return f == DIF_SELECTBESTCOMPATDRV ? MarkNodes(set, dev, id) : CheckSelected(set, dev, id);
}

/* ---- registration (rundll32 UACCI1.DLL,UacInstall / UacUninstall) ------ */

static int IsOurs(const char *s)
{
    /* UACCIn.DLL,... for any n: this and every other version. */
    char c[6];
    int i;
    for (i = 0; i < 5; i++) {
        c[i] = s[i];
        if (c[i] >= 'a' && c[i] <= 'z') c[i] = (char)(c[i] - 'a' + 'A');
    }
    c[5] = 0;
    if (lstrcmpA(c, "UACCI") != 0) return 0;
    for (i = 5; s[i] >= '0' && s[i] <= '9'; i++) ;
    if (i == 5) return 0;
    return (lstrcmpiA(s + i, ".DLL,UacCoInstaller") == 0);
}

/* Rewrites the MEDIA class value without our strings, plus ours when add. */
static DWORD Rewrite(int add)
{
    HKEY k;
    char *in = NULL, *out = NULL;
    DWORD type = REG_MULTI_SZ, n = 0, o = 0, i, e, cap;
    e = RegCreateKeyExA(HKEY_LOCAL_MACHINE, UACCO_KEY, 0, NULL, 0,
                        KEY_READ | KEY_WRITE, NULL, &k, NULL);
    if (e != ERROR_SUCCESS) return e;
    e = RegQueryValueExA(k, UACCO_MEDIA, NULL, &type, NULL, &n);
    if (e == ERROR_FILE_NOT_FOUND) { n = 0; e = ERROR_SUCCESS; }
    if (e != ERROR_SUCCESS || (n && type != REG_MULTI_SZ)) {
        RegCloseKey(k);
        return e != ERROR_SUCCESS ? e : ERROR_INVALID_DATA;
    }
    cap = n + sizeof(UACCO_ENTRY) + 4;
    in = (char *)LocalAlloc(LPTR, cap);
    out = (char *)LocalAlloc(LPTR, cap);
    if (!in || !out) { e = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (n) {
        DWORD got = n;
        e = RegQueryValueExA(k, UACCO_MEDIA, NULL, &type, (LPBYTE)in, &got);
        if (e != ERROR_SUCCESS || type != REG_MULTI_SZ || got > n) {
            if (e == ERROR_SUCCESS) e = ERROR_INVALID_DATA;
            goto done;
        }
        n = got;
    }
    /* in has two NULs past n (LPTR zeroed, cap > n + 2), so every string
     * ends inside the buffer. */
    for (i = 0; i < n && in[i]; ) {
        DWORD len = (DWORD)lstrlenA(in + i);
        if (!IsOurs(in + i)) {
            CopyMemory(out + o, in + i, len + 1);
            o += len + 1;
        }
        i += len + 1;
    }
    if (add) {
        CopyMemory(out + o, UACCO_ENTRY, sizeof(UACCO_ENTRY));
        o += sizeof(UACCO_ENTRY);
    }
    if (o == 0) e = RegDeleteValueA(k, UACCO_MEDIA);
    else {
        out[o++] = 0;
        e = RegSetValueExA(k, UACCO_MEDIA, 0, REG_MULTI_SZ, (const BYTE *)out, o);
    }
    if (e == ERROR_FILE_NOT_FOUND) e = ERROR_SUCCESS;
done:
    if (in) LocalFree(in);
    if (out) LocalFree(out);
    RegCloseKey(k);
    return e;
}

void CALLBACK UacInstall(HWND w, HINSTANCE h, LPSTR cmd, int show)
{
    DWORD e = Rewrite(1);
    (void)w; (void)h; (void)cmd; (void)show;
    Log2("install", "-", UACCO_ENTRY, e);
}

void CALLBACK UacUninstall(HWND w, HINSTANCE h, LPSTR cmd, int show)
{
    /* The file stays: deleting it later could remove one a reinstall
     * registered again before the restart. */
    DWORD e = Rewrite(0);
    (void)w; (void)h; (void)cmd; (void)show;
    Log2("uninstall", "-", UACCO_ENTRY, e);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
