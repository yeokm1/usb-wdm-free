/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Test-only console probe for the 9x kernel-streaming audio stack: lists the
 * KSCATEGORY_AUDIO / RENDER / TOPOLOGY / SYSAUDIO / MIXER interfaces, reads
 * this driver's filters (pins, data ranges, a data intersection, topology),
 * asks SysAudio which devices it built, and counts WinMM devices. With
 * "pin" it also creates and closes this driver's render pin (no data).
 * From public ks.h / ksmedia.h, SetupAPI and the KS IOCTL documentation.
 * Writes the report to A:\KS98.TXT (the exchange floppy) and the console.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ks.h>
#include <ksmedia.h>
#include <string.h>

static GUID gAudio = {0x6994ad04,0x93ef,0x11d0,{0xa3,0xcc,0x00,0xa0,0xc9,0x22,0x31,0x96}};
static GUID gRender = {0x65e8773e,0x8f56,0x11d0,{0xa3,0xb9,0x00,0xa0,0xc9,0x22,0x31,0x96}};
static GUID gTopology = {0xdda54a40,0x1e4c,0x11d1,{0xa0,0x50,0x40,0x57,0x05,0xc1,0x00,0x00}};
static GUID gSysaudio = {0xa7c7a5b1,0x5af3,0x11d1,{0x9c,0xed,0x00,0xa0,0x24,0xbf,0x04,0x07}};
static GUID gMixer = {0xad809c00,0x7b88,0x11d0,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00}};
static GUID gPinSet = {0x8c134960,0x51ad,0x11cf,{0x87,0x8a,0x94,0xf8,0x01,0xc1,0x00,0x00}};
static GUID gTopoSet = {0x720d4ac0,0x7533,0x11d0,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00}};
static GUID gSysSet = {0xcbe3faa0,0xcc75,0x11d0,{0xb4,0x65,0x00,0x00,0x1a,0x18,0x18,0xe6}};
static GUID gTypeAudio = {0x73647561,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static GUID gPcm = {0x00000001,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}};
static GUID gWfx = {0x05589f81,0xc356,0x11ce,{0xbf,0x01,0x00,0xaa,0x00,0x55,0x59,0x5a}};
static GUID gInterfaceStd = {0x1a8766a0,0x62ce,0x11cf,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00}};
static GUID gMediumStd = {0x4747b320,0x62ce,0x11cf,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00}};

static HANDLE logFile = INVALID_HANDLE_VALUE;
static char line[1024];

static void Out(const char *text)
{
    DWORD n;
    HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
    if (con != INVALID_HANDLE_VALUE) WriteFile(con, text, lstrlen(text), &n, NULL);
    if (logFile != INVALID_HANDLE_VALUE) { WriteFile(logFile, text, lstrlen(text), &n, NULL); FlushFileBuffers(logFile); }
}

static void Guid(const GUID *g, char *s)
{
    wsprintf(s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
        g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void Hex(const BYTE *p, DWORD n)
{
    DWORD i;
    char *s = line;
    for (i = 0; i < n && i < 256; i++) {
        if (i % 16 == 0) { if (i) { lstrcpy(s, "\r\n"); Out(line); } s = line; s += wsprintf(s, "      %04lX:", i); }
        s += wsprintf(s, " %02X", p[i]);
    }
    if (n) { lstrcpy(s, "\r\n"); Out(line); }
}

static BOOL Prop(HANDLE h, void *req, DWORD reqLen, void *out, DWORD outLen, DWORD *got, DWORD *err)
{
    BOOL ok;
    *got = 0;
    ok = DeviceIoControl(h, IOCTL_KS_PROPERTY, req, reqLen, out, outLen, got, NULL);
    *err = ok ? 0 : GetLastError();
    return ok;
}

static void Wide(const BYTE *w, DWORD n, char *s, int max)
{
    int i = 0;
    while (i + 1 < max && (DWORD)(2 * i + 1) < n && (w[2 * i] || w[2 * i + 1])) { s[i] = w[2 * i] ? (char)w[2 * i] : '?'; i++; }
    s[i] = 0;
}

/* Every pin property the audio stack reads, plus one intersection with a
 * client range shaped as a mixer asks (PCM, WAVEFORMATEX, wide limits). */
static void Pins(HANDLE f)
{
    static BYTE buf[8192];
    KSP_PIN p;
    KSPROPERTY q;
    DWORD got, err, count = 0, pin, i;
    char g[48];
    ZeroMemory(&q, sizeof(q));
    q.Set = gPinSet; q.Id = KSPROPERTY_PIN_CTYPES; q.Flags = KSPROPERTY_TYPE_GET;
    if (Prop(f, &q, sizeof(q), &count, sizeof(count), &got, &err))
        wsprintf(line, "    pins=%lu\r\n", count);
    else wsprintf(line, "    PIN_CTYPES error=%lu\r\n", err);
    Out(line);
    for (pin = 0; pin < count && pin < 16; pin++) {
        static const DWORD ids[] = { KSPROPERTY_PIN_CINSTANCES, KSPROPERTY_PIN_DATAFLOW,
            KSPROPERTY_PIN_COMMUNICATION, KSPROPERTY_PIN_CATEGORY, KSPROPERTY_PIN_NAME,
            KSPROPERTY_PIN_PHYSICALCONNECTION, KSPROPERTY_PIN_NECESSARYINSTANCES,
            KSPROPERTY_PIN_GLOBALCINSTANCES, KSPROPERTY_PIN_INTERFACES, KSPROPERTY_PIN_MEDIUMS,
            KSPROPERTY_PIN_DATARANGES };
        static const char *names[] = { "CINSTANCES", "DATAFLOW", "COMMUNICATION", "CATEGORY",
            "NAME", "PHYSICALCONNECTION", "NECESSARYINSTANCES", "GLOBALCINSTANCES",
            "INTERFACES", "MEDIUMS", "DATARANGES" };
        wsprintf(line, "    pin %lu\r\n", pin); Out(line);
        for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            ZeroMemory(&p, sizeof(p)); ZeroMemory(buf, sizeof(buf));
            p.Property.Set = gPinSet; p.Property.Id = ids[i]; p.Property.Flags = KSPROPERTY_TYPE_GET;
            p.PinId = pin;
            if (!Prop(f, &p, sizeof(p), buf, sizeof(buf), &got, &err)) {
                wsprintf(line, "      %s error=%lu\r\n", names[i], err); Out(line); continue;
            }
            if (ids[i] == KSPROPERTY_PIN_CATEGORY && got >= sizeof(GUID)) {
                Guid((GUID *)buf, g); wsprintf(line, "      CATEGORY %s\r\n", g); Out(line);
            } else if (ids[i] == KSPROPERTY_PIN_NAME) {
                char s[128]; Wide(buf, got, s, sizeof(s));
                wsprintf(line, "      NAME \"%s\"\r\n", s); Out(line);
            } else if (ids[i] == KSPROPERTY_PIN_PHYSICALCONNECTION && got > 8) {
                char s[300]; Wide(buf + 8, got - 8, s, sizeof(s));
                wsprintf(line, "      PHYSICALCONNECTION pin=%lu link=%s\r\n", ((DWORD *)buf)[1], s); Out(line);
            } else {
                wsprintf(line, "      %s bytes=%lu %08lX %08lX\r\n", names[i], got,
                    got >= 4 ? ((DWORD *)buf)[0] : 0, got >= 8 ? ((DWORD *)buf)[1] : 0); Out(line);
                if (ids[i] == KSPROPERTY_PIN_DATARANGES || ids[i] == KSPROPERTY_PIN_INTERFACES ||
                    ids[i] == KSPROPERTY_PIN_MEDIUMS) Hex(buf, got);
            }
        }
        {
            struct { KSP_PIN p; KSMULTIPLE_ITEM m; KSDATARANGE_AUDIO r; } x;
            ZeroMemory(&x, sizeof(x)); ZeroMemory(buf, sizeof(buf));
            x.p.Property.Set = gPinSet; x.p.Property.Id = KSPROPERTY_PIN_DATAINTERSECTION;
            x.p.Property.Flags = KSPROPERTY_TYPE_GET; x.p.PinId = pin;
            x.m.Size = sizeof(x.m) + sizeof(x.r); x.m.Count = 1;
            x.r.DataRange.FormatSize = sizeof(x.r);
            x.r.DataRange.MajorFormat = gTypeAudio; x.r.DataRange.SubFormat = gPcm;
            x.r.DataRange.Specifier = gWfx;
            x.r.MaximumChannels = 2; x.r.MinimumBitsPerSample = 8; x.r.MaximumBitsPerSample = 16;
            x.r.MinimumSampleFrequency = 1; x.r.MaximumSampleFrequency = 100000;
            if (Prop(f, &x, sizeof(x), buf, sizeof(buf), &got, &err)) {
                wsprintf(line, "      DATAINTERSECTION(pcm wide) bytes=%lu\r\n", got); Out(line); Hex(buf, got);
            } else { wsprintf(line, "      DATAINTERSECTION(pcm wide) error=%lu bytes=%lu\r\n", err, got); Out(line); }
            /* The size-only form callers use first. */
            if (Prop(f, &x, sizeof(x), buf, 0, &got, &err) || err == ERROR_MORE_DATA || err == ERROR_INSUFFICIENT_BUFFER)
                { wsprintf(line, "      DATAINTERSECTION size query err=%lu bytes=%lu\r\n", err, got); Out(line); }
            else { wsprintf(line, "      DATAINTERSECTION size query error=%lu bytes=%lu\r\n", err, got); Out(line); }
        }
    }
}

static void Topology(HANDLE f)
{
    static BYTE buf[4096];
    KSPROPERTY q;
    DWORD got, err, i, n;
    char g[48];
    for (i = 0; i < 3; i++) {
        ZeroMemory(&q, sizeof(q)); ZeroMemory(buf, sizeof(buf));
        q.Set = gTopoSet; q.Id = i; q.Flags = KSPROPERTY_TYPE_GET;
        if (!Prop(f, &q, sizeof(q), buf, sizeof(buf), &got, &err)) {
            wsprintf(line, "    topology %lu error=%lu\r\n", i, err); Out(line); continue;
        }
        wsprintf(line, "    topology %s bytes=%lu count=%lu\r\n", i == 0 ? "CATEGORIES" :
            i == 1 ? "NODES" : "CONNECTIONS", got, ((DWORD *)buf)[1]); Out(line);
        if (i < 2) for (n = 0; n < ((DWORD *)buf)[1] && 8 + 16 * (n + 1) <= got; n++) {
            Guid((GUID *)(buf + 8 + 16 * n), g); wsprintf(line, "      %s\r\n", g); Out(line);
        } else for (n = 0; n < ((DWORD *)buf)[1] && 8 + 16 * (n + 1) <= got; n++) {
            DWORD *c = (DWORD *)(buf + 8 + 16 * n);
            wsprintf(line, "      node %ld pin %lu -> node %ld pin %lu\r\n", (LONG)c[0], c[1], (LONG)c[2], c[3]); Out(line);
        }
    }
}

static void CreatePin(HANDLE f)
{
    typedef DWORD (WINAPI *CREATEPIN)(HANDLE, PKSPIN_CONNECT, ACCESS_MASK, PHANDLE);
    HMODULE lib = LoadLibrary("ksuser.dll");
    CREATEPIN create = lib ? (CREATEPIN)GetProcAddress(lib, "KsCreatePin") : NULL;
    struct { KSPIN_CONNECT c; KSDATAFORMAT_WAVEFORMATEX w; } x;
    HANDLE pin = NULL;
    DWORD r;
    if (!create) { Out("    KsCreatePin unavailable\r\n"); if (lib) FreeLibrary(lib); return; }
    ZeroMemory(&x, sizeof(x));
    x.c.Interface.Set = gInterfaceStd; x.c.Interface.Id = KSINTERFACE_STANDARD_STREAMING;
    x.c.Medium.Set = gMediumStd; x.c.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    x.c.PinId = 0; x.c.Priority.PriorityClass = KSPRIORITY_NORMAL; x.c.Priority.PrioritySubClass = 1;
    x.w.DataFormat.FormatSize = sizeof(x.w);
    x.w.DataFormat.SampleSize = 4;
    x.w.DataFormat.MajorFormat = gTypeAudio; x.w.DataFormat.SubFormat = gPcm; x.w.DataFormat.Specifier = gWfx;
    x.w.WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM; x.w.WaveFormatEx.nChannels = 2;
    x.w.WaveFormatEx.nSamplesPerSec = 48000; x.w.WaveFormatEx.nAvgBytesPerSec = 192000;
    x.w.WaveFormatEx.nBlockAlign = 4; x.w.WaveFormatEx.wBitsPerSample = 16;
    r = create(f, &x.c, GENERIC_WRITE, &pin);
    wsprintf(line, "    KsCreatePin(pin 0, 48000/16/2) result=%lu\r\n", r); Out(line);
    if (!r && pin) { CloseHandle(pin); Out("    pin closed\r\n"); }
    FreeLibrary(lib);
}

static int Ours(const char *path)
{
    char s[512];
    lstrcpyn(s, path, sizeof(s)); CharLower(s);
    /* NT names the USB device; 9x names the devnode (\\.\<devnode>#),
     * where SysAudio's and KMixer's software filters sit on devnode 0. */
    return strstr(s, "vid_041e") != NULL || strstr(s, "uac2") != NULL ||
        (!strncmp(s, "\\\\.\\", 4) && strncmp(s, "\\\\.\\0000000000000000#", 21));
}

static void List(GUID *cat, const char *name, int inspect, int pin)
{
    HDEVINFO set = SetupDiGetClassDevs(cat, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA e;
    static BYTE space[2048];
    PSP_DEVICE_INTERFACE_DETAIL_DATA d = (PSP_DEVICE_INTERFACE_DETAIL_DATA)space;
    DWORD i;
    if (set == INVALID_HANDLE_VALUE) { wsprintf(line, "%s: SetupDiGetClassDevs error=%lu\r\n", name, GetLastError()); Out(line); return; }
    for (i = 0; i < 64; i++) {
        ZeroMemory(&e, sizeof(e)); e.cbSize = sizeof(e);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, cat, i, &e)) break;
        ZeroMemory(space, sizeof(space)); d->cbSize = sizeof(*d);
        if (!SetupDiGetDeviceInterfaceDetail(set, &e, d, sizeof(space), NULL, NULL)) {
            wsprintf(line, "  %s %lu detail error=%lu\r\n", name, i, GetLastError()); Out(line); continue;
        }
        wsprintf(line, "  %s %lu flags=%lX %s\r\n", name, i, e.Flags, d->DevicePath); Out(line);
        if (inspect && Ours(d->DevicePath)) {
            HANDLE f = CreateFile(d->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (f == INVALID_HANDLE_VALUE) { wsprintf(line, "    open error=%lu\r\n", GetLastError()); Out(line); continue; }
            Topology(f); Pins(f);
            if (pin) { char s[512]; lstrcpyn(s, d->DevicePath, sizeof(s)); CharLower(s);
                if (strstr(s, "\\wave")) CreatePin(f); }
            CloseHandle(f);
        }
    }
    wsprintf(line, "%s interfaces=%lu\r\n", name, i); Out(line);
    SetupDiDestroyDeviceInfoList(set);
}

static void Sysaudio(void)
{
    HDEVINFO set = SetupDiGetClassDevs(&gSysaudio, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA e;
    static BYTE space[2048], buf[2048];
    PSP_DEVICE_INTERFACE_DETAIL_DATA d = (PSP_DEVICE_INTERFACE_DETAIL_DATA)space;
    HANDLE f;
    KSPROPERTY q;
    struct { KSPROPERTY p; ULONG index; } r;
    DWORD got, err, count = 0, i;
    if (set == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&e, sizeof(e)); e.cbSize = sizeof(e);
    ZeroMemory(space, sizeof(space)); d->cbSize = sizeof(*d);
    if (!SetupDiEnumDeviceInterfaces(set, NULL, &gSysaudio, 0, &e) ||
        !SetupDiGetDeviceInterfaceDetail(set, &e, d, sizeof(space), NULL, NULL)) {
        Out("SysAudio: no interface\r\n"); SetupDiDestroyDeviceInfoList(set); return;
    }
    f = CreateFile(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    SetupDiDestroyDeviceInfoList(set);
    if (f == INVALID_HANDLE_VALUE) { wsprintf(line, "SysAudio open error=%lu\r\n", GetLastError()); Out(line); return; }
    ZeroMemory(&q, sizeof(q));
    q.Set = gSysSet; q.Id = KSPROPERTY_SYSAUDIO_DEVICE_COUNT; q.Flags = KSPROPERTY_TYPE_GET;
    if (Prop(f, &q, sizeof(q), &count, sizeof(count), &got, &err)) wsprintf(line, "SysAudio devices=%lu\r\n", count);
    else wsprintf(line, "SysAudio DEVICE_COUNT error=%lu\r\n", err);
    Out(line);
    for (i = 0; i < count && i < 16; i++) {
        char s[400];
        ZeroMemory(&r, sizeof(r)); r.p.Set = gSysSet; r.p.Flags = KSPROPERTY_TYPE_GET; r.index = i;
        r.p.Id = KSPROPERTY_SYSAUDIO_DEVICE_FRIENDLY_NAME; ZeroMemory(buf, sizeof(buf));
        if (Prop(f, &r, sizeof(r), buf, sizeof(buf), &got, &err)) { Wide(buf, got, s, sizeof(s)); }
        else wsprintf(s, "error=%lu", err);
        wsprintf(line, "  device %lu name \"%s\"\r\n", i, s); Out(line);
        r.p.Id = KSPROPERTY_SYSAUDIO_DEVICE_INTERFACE_NAME; ZeroMemory(buf, sizeof(buf));
        if (Prop(f, &r, sizeof(r), buf, sizeof(buf), &got, &err)) { Wide(buf, got, s, sizeof(s)); }
        else wsprintf(s, "error=%lu", err);
        wsprintf(line, "  device %lu interface %s\r\n", i, s); Out(line);
    }
    CloseHandle(f);
}

static void Classes(GUID *cat)
{
    char path[200], g[48], sub[300], val[300];
    HKEY k, k2, k3;
    DWORD i, j, n, t;
    Guid(cat, g);
    wsprintf(path, "System\\CurrentControlSet\\Control\\DeviceClasses\\{%s}", g);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k)) { wsprintf(line, "  reg {%s} absent\r\n", g); Out(line); return; }
    for (i = 0; ; i++) {
        n = sizeof(sub);
        if (RegEnumKeyEx(k, i, sub, &n, NULL, NULL, NULL, NULL)) break;
        wsprintf(line, "  reg {%s}\\%s\r\n", g, sub); Out(line);
        if (RegOpenKeyEx(k, sub, 0, KEY_READ, &k2)) continue;
        n = sizeof(val);
        if (!RegQueryValueEx(k2, "DeviceInstance", NULL, &t, (BYTE *)val, &n)) { wsprintf(line, "    DeviceInstance=%s\r\n", val); Out(line); }
        for (j = 0; ; j++) {
            char ref[200];
            n = sizeof(ref);
            if (RegEnumKeyEx(k2, j, ref, &n, NULL, NULL, NULL, NULL)) break;
            wsprintf(line, "    ref %s\r\n", ref); Out(line);
            if (!RegOpenKeyEx(k2, ref, 0, KEY_READ, &k3)) {
                static const char *names[] = { "SymbolicLink", "CLSID", "FriendlyName" };
                int v;
                for (v = 0; v < 3; v++) {
                    n = sizeof(val); val[0] = 0;
                    if (!RegQueryValueEx(k3, names[v], NULL, &t, (BYTE *)val, &n)) { wsprintf(line, "      %s=%s\r\n", names[v], val); Out(line); }
                }
                {   HKEY k4; DWORD c = 0, m;
                    char sk[64];
                    /* Every subkey but Control, with its string values: where
                     * this system keeps the INF's AddInterface AddReg. */
                    for (m = 0; ; m++) {
                        n = sizeof(sk);
                        if (RegEnumKeyEx(k3, m, sk, &n, NULL, NULL, NULL, NULL)) break;
                        if (!lstrcmpi(sk, "Control")) continue;
                        wsprintf(line, "      subkey %s\r\n", sk); Out(line);
                        if (!RegOpenKeyEx(k3, sk, 0, KEY_READ, &k4)) {
                            DWORD w;
                            for (w = 0; ; w++) {
                                char vn[64]; DWORD vl = sizeof(vn);
                                n = sizeof(val); val[0] = 0;
                                if (RegEnumValue(k4, w, vn, &vl, NULL, &t, (BYTE *)val, &n)) break;
                                if (t != REG_SZ) lstrcpy(val, "(binary)");
                                wsprintf(line, "        %s=%s\r\n", vn, val); Out(line);
                            }
                            RegCloseKey(k4);
                        }
                    }
                    if (!RegOpenKeyEx(k3, "Control", 0, KEY_READ, &k4)) {
                        n = sizeof(c); if (!RegQueryValueEx(k4, "Linked", NULL, &t, (BYTE *)&c, &n)) { wsprintf(line, "      Control\\Linked=%lu\r\n", c); Out(line); }
                        else Out("      Control key, no Linked\r\n");
                        RegCloseKey(k4);
                    }
                }
                RegCloseKey(k3);
            }
        }
        RegCloseKey(k2);
    }
    RegCloseKey(k);
}

int main(int argc, char **argv)
{
    int pin = 0, sys = 0, mm = 0, a;
    const char *path = "A:\\KS98.TXT";
    UINT i, n;
    for (a = 1; a < argc; a++) {
        if (!lstrcmpi(argv[a], "pin")) pin = 1;
        if (!lstrcmpi(argv[a], "sys")) sys = 1;
        if (!lstrcmpi(argv[a], "mm")) mm = 1;
        /* A new report name per run: QEMU's virtual FAT floppy has stopped
         * the guest when a file was rewritten in place. */
        if (strchr(argv[a], ':')) path = argv[a];
    }
    logFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    Out("KS98 probe (uac2, test only)\r\n");
    List(&gAudio, "AUDIO", 1, pin);
    List(&gRender, "RENDER", 0, 0);
    List(&gTopology, "TOPOLOGY", 0, 0);
    List(&gMixer, "MIXER", 0, 0);
    List(&gSysaudio, "SYSAUDIO", 0, 0);
    Out("Registry DeviceClasses:\r\n");
    Classes(&gAudio); Classes(&gRender); Classes(&gTopology);
    /* SysAudio and WinMM last and only on request: a SysAudio that has
     * faulted can hang the caller. */
    if (sys) Sysaudio();
    if (!mm) { Out("end\r\n"); if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile); return 0; }
    n = waveOutGetNumDevs(); wsprintf(line, "waveOut=%u", n); Out(line);
    wsprintf(line, " waveIn=%u mixer=%u midiOut=%u midiIn=%u aux=%u\r\n", waveInGetNumDevs(),
        mixerGetNumDevs(), midiOutGetNumDevs(), midiInGetNumDevs(), auxGetNumDevs()); Out(line);
    for (i = 0; i < n && i < 8; i++) {
        WAVEOUTCAPS c; ZeroMemory(&c, sizeof(c));
        waveOutGetDevCaps(i, &c, sizeof(c));
        wsprintf(line, "  waveOut %u \"%s\" formats=%08lX ch=%u\r\n", i, c.szPname, c.dwFormats, c.wChannels); Out(line);
    }
    n = mixerGetNumDevs();
    for (i = 0; i < n && i < 8; i++) {
        MIXERCAPS c; ZeroMemory(&c, sizeof(c));
        mixerGetDevCaps(i, &c, sizeof(c));
        wsprintf(line, "  mixer %u \"%s\" dest=%lu\r\n", i, c.szPname, c.cDestinations); Out(line);
    }
    Out("end\r\n");
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    return 0;
}
