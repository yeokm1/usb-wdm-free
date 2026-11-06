/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * KEYTEST.EXE - guest reading tool for the media-key readings (docs/
 * design/mediakeys.md). Run it in a guest only.
 *
 *   KEYTEST [seconds] [report file]     default 300 s, C:\KEYTEST.TXT
 *   KEYTEST /enum [report file]         default C:\KEYENUM.TXT
 *   KEYTEST /hid [seconds] [report file] default 60 s, C:\KEYHID.TXT
 *
 * The first form opens a window, takes the keyboard focus and writes one
 * line to COM1 and to the report file for each of:
 *
 *   key  t=<ms> msg=<DOWN|UP|SDOWN|SUP> vk=<hex> scan=<hex> ext=<0|1>
 *   app  t=<ms> cmd=<n> dev=<hex> keys=<hex>      (WM_APPCOMMAND)
 *   async t=<ms> vk=<hex> <down|up>   (GetAsyncKeyState, VK A6..B7)
 *   vol  t=<ms> mixer=<n> vol=<n> mute=<n> wave=<hex>
 *   fg   t=<ms> title=<foreground window title>
 *
 * and a "mark" line every 5 s. WM_APPCOMMAND is logged and then passed
 * to DefWindowProc, so that what the system does with it (the shell
 * hook) still happens.
 *
 * The /enum form lists the HID and USB device nodes from the registry
 * (9x: HKLM\Enum; NT: HKLM\SYSTEM\CurrentControlSet\Enum): DeviceDesc,
 * HardwareID, CompatibleIDs, Class, Driver, Service, the driver key's
 * DevLoader and NTMPDriver (9x), and the node's status and problem
 * from the configuration manager.
 *
 * The /hid form lists every HID collection through hid.dll and SetupAPI
 * (the interface path, usage page and usage, the input report length,
 * whether it opens for reading, shared), then reads the Consumer Control
 * collections (page 0x0C, usage 1) for the given time and writes each
 * input report in hex:
 *
 *   hid  t=<ms> n=<collection> report=<hex bytes>
 *
 * Each collection is read by a thread of its own, with blocking reads.
 * It tells whether a user-mode program can read media keys beside the
 * system's own reader of that collection.
 *
 * Win32 only, 98 SE to XP. Original code. Reads no device directly.
 */
#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_APPCOMMAND
#define WM_APPCOMMAND 0x0319
#endif

static HANDLE gCom = INVALID_HANDLE_VALUE;
static FILE *gOut;
static DWORD gStart;
static int gNt;

static CRITICAL_SECTION gEmitLock;

static void Emit(const char *line)
{
    DWORD n;
    EnterCriticalSection(&gEmitLock);
    if (gCom != INVALID_HANDLE_VALUE)
        WriteFile(gCom, line, (DWORD)strlen(line), &n, NULL);
    if (gOut) {
        fputs(line, gOut);
        fflush(gOut);
    }
    LeaveCriticalSection(&gEmitLock);
}

static DWORD Now(void)
{
    return GetTickCount() - gStart;
}

static void OpenCom(void)
{
    gCom = CreateFile("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (gCom != INVALID_HANDLE_VALUE) {
        DCB dcb;
        memset(&dcb, 0, sizeof(dcb));
        dcb.DCBlength = sizeof(dcb);
        if (GetCommState(gCom, &dcb)) {
            dcb.BaudRate = CBR_115200;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            SetCommState(gCom, &dcb);
        }
    }
}

/* ------------------------------------------------------------ the mixer */

static HMIXER gMixer;
static DWORD gVolId = 0xFFFFFFFF, gMuteId = 0xFFFFFFFF;
static long gLastVol = -2, gLastMute = -2;
static DWORD gLastWave = 0xFFFFFFFF;

static void MixerOpen(void)
{
    MIXERLINE line;
    MIXERLINECONTROLS mlc;
    MIXERCONTROL mc;
    char buf[200];
    MMRESULT r;

    sprintf(buf, "mixers=%u waveouts=%u\r\n", mixerGetNumDevs(), waveOutGetNumDevs());
    Emit(buf);
    if (mixerGetNumDevs() == 0 || mixerOpen(&gMixer, 0, 0, 0, MIXER_OBJECTF_MIXER) != MMSYSERR_NOERROR) {
        gMixer = NULL;
        return;
    }
    memset(&line, 0, sizeof(line));
    line.cbStruct = sizeof(line);
    line.dwComponentType = MIXERLINE_COMPONENTTYPE_DST_SPEAKERS;
    r = mixerGetLineInfo((HMIXEROBJ)gMixer, &line, MIXER_GETLINEINFOF_COMPONENTTYPE);
    if (r != MMSYSERR_NOERROR) {
        sprintf(buf, "mixer: no speakers line (%u)\r\n", r);
        Emit(buf);
        return;
    }
    sprintf(buf, "mixer line=%s controls=%lu\r\n", line.szName, line.cControls);
    Emit(buf);
    memset(&mlc, 0, sizeof(mlc));
    memset(&mc, 0, sizeof(mc));
    mlc.cbStruct = sizeof(mlc);
    mlc.dwLineID = line.dwLineID;
    mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    mlc.cControls = 1;
    mlc.cbmxctrl = sizeof(mc);
    mlc.pamxctrl = &mc;
    if (mixerGetLineControls((HMIXEROBJ)gMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR)
        gVolId = mc.dwControlID;
    mlc.dwControlType = MIXERCONTROL_CONTROLTYPE_MUTE;
    memset(&mc, 0, sizeof(mc));
    if (mixerGetLineControls((HMIXEROBJ)gMixer, &mlc, MIXER_GETLINECONTROLSF_ONEBYTYPE) == MMSYSERR_NOERROR)
        gMuteId = mc.dwControlID;
}

static long MixerGet(DWORD id)
{
    MIXERCONTROLDETAILS d;
    MIXERCONTROLDETAILS_UNSIGNED v;
    if (gMixer == NULL || id == 0xFFFFFFFF)
        return -1;
    memset(&d, 0, sizeof(d));
    d.cbStruct = sizeof(d);
    d.dwControlID = id;
    d.cChannels = 1;
    d.cbDetails = sizeof(v);
    d.paDetails = &v;
    v.dwValue = 0;
    if (mixerGetControlDetails((HMIXEROBJ)gMixer, &d, MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR)
        return -1;
    return (long)v.dwValue;
}

static void VolumePoll(int force)
{
    long vol = MixerGet(gVolId), mute = MixerGet(gMuteId);
    DWORD wave = 0xFFFFFFFF;
    char buf[160];
    if (waveOutGetNumDevs() > 0 && waveOutGetVolume((HWAVEOUT)0, &wave) != MMSYSERR_NOERROR)
        wave = 0xFFFFFFFF;
    if (force || vol != gLastVol || mute != gLastMute || wave != gLastWave) {
        sprintf(buf, "vol  t=%lu mixer=%d vol=%ld mute=%ld wave=%08lx\r\n", Now(), gMixer != NULL, vol, mute, wave);
        Emit(buf);
        gLastVol = vol;
        gLastMute = mute;
        gLastWave = wave;
    }
}

/* ------------------------------------------------------------ the window */

static BYTE gAsync[256];
static char gLastFg[128];

static void AsyncPoll(void)
{
    int vk;
    char buf[80];
    for (vk = 0xA6; vk <= 0xB7; vk++) {
        BYTE d = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
        if (d != gAsync[vk]) {
            sprintf(buf, "async t=%lu vk=%02x %s\r\n", Now(), vk, d ? "down" : "up");
            Emit(buf);
            gAsync[vk] = d;
        }
    }
}

static void FgPoll(int force)
{
    char title[128], buf[200];
    HWND w = GetForegroundWindow();
    title[0] = 0;
    if (w)
        GetWindowText(w, title, sizeof(title));
    if (force || strcmp(title, gLastFg)) {
        sprintf(buf, "fg   t=%lu title=%s\r\n", Now(), title);
        Emit(buf);
        strcpy(gLastFg, title);
    }
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    char buf[160];
    const char *name = NULL;
    switch (m) {
    case WM_KEYDOWN: name = "DOWN"; break;
    case WM_KEYUP: name = "UP"; break;
    case WM_SYSKEYDOWN: name = "SDOWN"; break;
    case WM_SYSKEYUP: name = "SUP"; break;
    case WM_APPCOMMAND:
        sprintf(buf, "app  t=%lu cmd=%u dev=%04x keys=%04x\r\n", Now(),
                (unsigned)(HIWORD(lp) & 0x0FFF), (unsigned)(HIWORD(lp) & 0xF000), (unsigned)LOWORD(lp));
        Emit(buf);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    if (name) {
        sprintf(buf, "key  t=%lu msg=%s vk=%02x scan=%02x ext=%d\r\n", Now(), name,
                (unsigned)wp, (unsigned)((lp >> 16) & 0xFF), (int)((lp >> 24) & 1));
        Emit(buf);
    }
    return DefWindowProc(w, m, wp, lp);
}

static int RunWatch(HINSTANCE inst, DWORD seconds)
{
    WNDCLASS wc;
    HWND w;
    MSG msg;
    DWORD lastMark = 0;
    char buf[200];

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "KeyTestWnd";
    RegisterClass(&wc);
    w = CreateWindow("KeyTestWnd", "KEYTEST", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                     200, 120, 320, 200, NULL, NULL, inst, NULL);
    SetForegroundWindow(w);
    SetFocus(w);
    MixerOpen();
    VolumePoll(1);
    FgPoll(1);
    SetTimer(w, 1, 50, NULL);
    for (;;) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (Now() >= seconds * 1000)
            break;
        AsyncPoll();
        VolumePoll(0);
        FgPoll(0);
        if (Now() - lastMark >= 5000) {
            lastMark = Now();
            sprintf(buf, "mark t=%lu focus=%d\r\n", lastMark, GetFocus() == w);
            Emit(buf);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
    DestroyWindow(w);
    return 0;
}

/* ------------------------------------------------------------ /enum */

typedef DWORD (WINAPI *LOCATE)(DWORD *, char *, ULONG);
typedef DWORD (WINAPI *STATUS)(ULONG *, ULONG *, DWORD, ULONG);
static LOCATE pLocate;
static STATUS pStatus;

static void Val(HKEY k, const char *name, char *out, DWORD size)
{
    DWORD type, n = size - 1, i;
    out[0] = 0;
    if (RegQueryValueEx(k, name, NULL, &type, (BYTE *)out, &n) != ERROR_SUCCESS) {
        out[0] = 0;
        return;
    }
    out[n < size ? n : size - 1] = 0;
    if (type == REG_MULTI_SZ)
        for (i = 0; i + 1 < n; i++)
            if (out[i] == 0)
                out[i] = ';';
    if (type == REG_DWORD && n == 4)
        sprintf(out, "0x%lx", *(DWORD *)out);
}

static void EnumNode(const char *base, const char *id)
{
    char path[400], v[600], line[900], drv[200], cls[400];
    HKEY k, d;
    sprintf(path, "%s\\%s", base, id);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    sprintf(line, "node %s\r\n", id);
    Emit(line);
    {
        static const char *names[] = { "DeviceDesc", "HardwareID", "CompatibleIDs", "Class",
                                       "Driver", "Service", "Mfg", "ConfigFlags", NULL };
        int i;
        for (i = 0; names[i]; i++) {
            Val(k, names[i], v, sizeof(v));
            if (v[0]) {
                sprintf(line, "  %s=%s\r\n", names[i], v);
                Emit(line);
            }
        }
    }
    Val(k, "Driver", drv, sizeof(drv));
    RegCloseKey(k);
    if (drv[0]) {
        sprintf(cls, gNt ? "SYSTEM\\CurrentControlSet\\Control\\Class\\%s"
                         : "System\\CurrentControlSet\\Services\\Class\\%s", drv);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, cls, 0, KEY_READ, &d) == ERROR_SUCCESS) {
            static const char *dn[] = { "DevLoader", "NTMPDriver", "DriverDesc", "InfPath", "InfSection", NULL };
            int i;
            for (i = 0; dn[i]; i++) {
                Val(d, dn[i], v, sizeof(v));
                if (v[0]) {
                    sprintf(line, "  drv.%s=%s\r\n", dn[i], v);
                    Emit(line);
                }
            }
            RegCloseKey(d);
        }
    }
    if (pLocate && pStatus) {
        DWORD dn;
        ULONG st = 0, pr = 0;
        DWORD r = pLocate(&dn, (char *)id, 0);
        if (r == 0)
            r = pStatus(&st, &pr, dn, 0);
        sprintf(line, "  cm=%lu status=0x%lx problem=%lu\r\n", r, st, pr);
        Emit(line);
    }
}

static void EnumTree(const char *root, const char *bus)
{
    char base[200], a[200], b[200], id[600];
    HKEY k1, k2;
    DWORD i, j, n;
    sprintf(base, "%s\\%s", root, bus);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &k1) != ERROR_SUCCESS)
        return;
    for (i = 0; n = sizeof(a), RegEnumKeyEx(k1, i, a, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; i++) {
        if (RegOpenKeyEx(k1, a, 0, KEY_READ, &k2) != ERROR_SUCCESS)
            continue;
        for (j = 0; n = sizeof(b), RegEnumKeyEx(k2, j, b, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; j++) {
            sprintf(id, "%s\\%s\\%s", bus, a, b);
            EnumNode(root, id);
        }
        RegCloseKey(k2);
    }
    RegCloseKey(k1);
}

static int RunEnum(void)
{
    HMODULE cm = LoadLibrary("CFGMGR32.DLL");
    const char *root = gNt ? "SYSTEM\\CurrentControlSet\\Enum" : "Enum";
    if (cm) {
        pLocate = (LOCATE)GetProcAddress(cm, "CM_Locate_DevNodeA");
        pStatus = (STATUS)GetProcAddress(cm, "CM_Get_DevNode_Status");
    }
    EnumTree(root, "HID");
    EnumTree(root, "USB");
    Emit("enum end\r\n");
    return 0;
}

/* ------------------------------------------------------------ /hid */

/* HIDP_CAPS as the 98 DDK's hidpi.h declares it (the counts not named). */
typedef struct {
    USHORT Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT Counts[10];
} KT_HIDP_CAPS;

typedef void (__stdcall *GETGUID)(GUID *);
typedef BOOLEAN (__stdcall *GETPP)(HANDLE, void **);
typedef BOOLEAN (__stdcall *FREEPP)(void *);
typedef LONG (__stdcall *GETCAPS)(void *, KT_HIDP_CAPS *);
typedef HDEVINFO (WINAPI *CLASSDEVS)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *ENUMIF)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *IFDETAIL)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD,
                                PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *DESTROY)(HDEVINFO);

#define KT_MAXCC 8

static void HidCaps(HANDLE h, GETPP pGetPp, FREEPP pFreePp, GETCAPS pCaps, KT_HIDP_CAPS *caps)
{
    void *pp;
    memset(caps, 0, sizeof(*caps));
    if (h != INVALID_HANDLE_VALUE && pGetPp(h, &pp)) {
        pCaps(pp, caps);
        pFreePp(pp);
    }
}

/* One blocking reader per Consumer Control collection (98 SE completed no
 * overlapped read on these handles); the process ends with them blocked. */
typedef struct {
    HANDLE h;
    USHORT len;
    int n;
} KT_READER;

static KT_READER gRd[KT_MAXCC];

static DWORD WINAPI ReadThread(void *arg)
{
    KT_READER *r = (KT_READER *)arg;
    BYTE buf[64];
    char line[200], *o;
    DWORD got;
    int j;
    for (;;) {
        if (!ReadFile(r->h, buf, r->len, &got, NULL)) {
            sprintf(line, "hid  t=%lu n=%d read error %lu\r\n", Now(), r->n, GetLastError());
            Emit(line);
            return 1;
        }
        o = line;
        o += sprintf(o, "hid  t=%lu n=%d report=", Now(), r->n);
        for (j = 0; j < (int)got && j < 64; j++)
            o += sprintf(o, "%02x", buf[j]);
        strcpy(o, "\r\n");
        Emit(line);
    }
}

static int RunHid(DWORD seconds)
{
    HMODULE hid = LoadLibrary("HID.DLL"), sa = LoadLibrary("SETUPAPI.DLL");
    GETGUID pGuid;
    GETPP pGetPp;
    FREEPP pFreePp;
    GETCAPS pCaps;
    CLASSDEVS pDevs;
    ENUMIF pEnum;
    IFDETAIL pDetail;
    DESTROY pDestroy;
    GUID guid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i, need;
    char line[700];
    HANDLE cc[KT_MAXCC];
    USHORT len[KT_MAXCC];
    int ncc = 0, k;

    if (!hid || !sa) {
        sprintf(line, "hid: HID.DLL %s, SETUPAPI.DLL %s\r\n", hid ? "loaded" : "missing", sa ? "loaded" : "missing");
        Emit(line);
        return 1;
    }
    pGuid = (GETGUID)GetProcAddress(hid, "HidD_GetHidGuid");
    pGetPp = (GETPP)GetProcAddress(hid, "HidD_GetPreparsedData");
    pFreePp = (FREEPP)GetProcAddress(hid, "HidD_FreePreparsedData");
    pCaps = (GETCAPS)GetProcAddress(hid, "HidP_GetCaps");
    pDevs = (CLASSDEVS)GetProcAddress(sa, "SetupDiGetClassDevsA");
    pEnum = (ENUMIF)GetProcAddress(sa, "SetupDiEnumDeviceInterfaces");
    if (!pEnum)
        pEnum = (ENUMIF)GetProcAddress(sa, "SetupDiEnumInterfaceDevice");
    pDetail = (IFDETAIL)GetProcAddress(sa, "SetupDiGetDeviceInterfaceDetailA");
    if (!pDetail)
        pDetail = (IFDETAIL)GetProcAddress(sa, "SetupDiGetInterfaceDeviceDetailA");
    pDestroy = (DESTROY)GetProcAddress(sa, "SetupDiDestroyDeviceInfoList");
    if (!pGuid || !pGetPp || !pFreePp || !pCaps || !pDevs || !pEnum || !pDetail || !pDestroy) {
        Emit("hid: an export is missing\r\n");
        return 1;
    }
    pGuid(&guid);
    set = pDevs(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        Emit("hid: no device list\r\n");
        return 1;
    }
    for (i = 0; i < 64; i++) {
        BYTE detail[600];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detail;
        HANDLE h;
        KT_HIDP_CAPS caps;
        DWORD err = 0;
        memset(&ifd, 0, sizeof(ifd));
        ifd.cbSize = sizeof(ifd);
        if (!pEnum(set, NULL, &guid, i, &ifd))
            break;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!pDetail(set, &ifd, d, sizeof(detail), &need, NULL))
            continue;
        h = CreateFile(d->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            /* No read access (a collection the system holds): open with no access for the usages only. */
            err = GetLastError();
            h = CreateFile(d->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            HidCaps(h, pGetPp, pFreePp, pCaps, &caps);
            sprintf(line, "hidc n=%lu page=%04x usage=%04x inlen=%u read=no(%lu) path=%.400s\r\n", i,
                    caps.UsagePage, caps.Usage, caps.InputReportByteLength, err, d->DevicePath);
            Emit(line);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
            continue;
        }
        HidCaps(h, pGetPp, pFreePp, pCaps, &caps);
        sprintf(line, "hidc n=%lu page=%04x usage=%04x inlen=%u read=yes path=%.400s\r\n", i, caps.UsagePage,
                caps.Usage, caps.InputReportByteLength, d->DevicePath);
        Emit(line);
        if (caps.UsagePage == 0x0C && caps.Usage == 1 && ncc < KT_MAXCC && caps.InputReportByteLength > 0 &&
            caps.InputReportByteLength <= 64) {
            cc[ncc] = h;
            len[ncc] = caps.InputReportByteLength;
            ncc++;
        } else {
            CloseHandle(h);
        }
    }
    pDestroy(set);
    sprintf(line, "hid: %d consumer collection(s) open; reading %lu s\r\n", ncc, seconds);
    Emit(line);
    for (k = 0; k < ncc; k++) {
        DWORD tid;
        gRd[k].h = cc[k];
        gRd[k].len = len[k];
        gRd[k].n = k;
        CreateThread(NULL, 0, ReadThread, &gRd[k], 0, &tid);
    }
    while (ncc > 0 && Now() < seconds * 1000)
        Sleep(200);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char line[200], path[MAX_PATH];
    DWORD seconds = 300;
    int doEnum = 0, doHid = 0;
    char *arg = cmd;
    OSVERSIONINFO os;
    (void)prev; (void)show;

    InitializeCriticalSection(&gEmitLock);
    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    GetVersionEx(&os);
    gNt = os.dwPlatformId == VER_PLATFORM_WIN32_NT;
    while (*arg == ' ')
        arg++;
    if (!strncmp(arg, "/enum", 5)) {
        doEnum = 1;
        arg += 5;
    } else if (!strncmp(arg, "/hid", 4)) {
        doHid = 1;
        seconds = 60;
        arg += 4;
        while (*arg == ' ')
            arg++;
        if (*arg >= '0' && *arg <= '9')
            seconds = (DWORD)strtoul(arg, &arg, 10);
    } else if (*arg >= '0' && *arg <= '9') {
        seconds = (DWORD)strtoul(arg, &arg, 10);
    }
    while (*arg == ' ')
        arg++;
    strcpy(path, doEnum ? "C:\\KEYENUM.TXT" : doHid ? "C:\\KEYHID.TXT" : "C:\\KEYTEST.TXT");
    if (*arg) {
        strncpy(path, arg, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    if (seconds == 0)
        seconds = 300;
    gOut = fopen(path, "w");
    OpenCom();
    gStart = GetTickCount();
    sprintf(line, "KEYTEST start mode=%s os=%lu.%lu.%lu platform=%lu seconds=%lu\r\n", doEnum ? "enum" : doHid ? "hid" : "watch",
            os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber & 0xFFFF, os.dwPlatformId, seconds);
    Emit(line);
    if (doEnum)
        RunEnum();
    else if (doHid)
        RunHid(seconds);
    else
        RunWatch(inst, seconds);
    Emit("KEYTEST end\r\n");
    /* /hid readers may still be blocked in ReadFile: close the outputs under
     * the lock and leave them invalid, so a late report writes nothing. */
    EnterCriticalSection(&gEmitLock);
    if (gOut)
        fclose(gOut);
    gOut = NULL;
    if (gCom != INVALID_HANDLE_VALUE)
        CloseHandle(gCom);
    gCom = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&gEmitLock);
    ExitProcess(0);
    return 0;
}
