/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Test-only Win32 GUI: device status, M0 registry diagnostics, WinMM
 * enumeration and format/open results. No playback data is submitted.
 * Saves only this test report to A:\UACLOG.TXT (the virtual exchange disk).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <winioctl.h>
#include <ks.h>

static GUID mediaClass = { 0x4d36e96c, 0xe325, 0x11ce,
    {0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18} };
static char report[16384];
static HWND edit;
static GUID audioCategory = {0x6994ad04,0x93ef,0x11d0,{0xa3,0xcc,0x00,0xa0,0xc9,0x22,0x31,0x96}};
static GUID renderCategory = {0x65e8773e,0x8f56,0x11d0,{0xa3,0xb9,0x00,0xa0,0xc9,0x22,0x31,0x96}};
static GUID topologyCategory = {0xdda54a40,0x1e4c,0x11d1,{0xa0,0x50,0x40,0x57,0x05,0xc1,0x00,0x00}};
static GUID audioDeviceCategory = {0xfbf6f530,0x07b9,0x11d2,{0xa7,0x1e,0x00,0x00,0xf8,0x00,0x47,0x88}};
static GUID pinProperties = {0x8c134960,0x51ad,0x11cf,{0x87,0x8a,0x94,0xf8,0x01,0xc1,0x00,0x00}};
static GUID topologyProperties = {0x720d4ac0,0x7533,0x11d0,{0xa5,0xd6,0x28,0xdb,0x04,0xc1,0x00,0x00}};
typedef DWORD (WINAPI *CMSTATUS)(PULONG, PULONG, DWORD, ULONG);
typedef BOOL (WINAPI *UPDATE)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);

static void Save(void)
{
    DWORD bytes;
    HANDLE output = CreateFile("A:\\UACLOG.TXT", GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output != INVALID_HANDLE_VALUE) {
        WriteFile(output, report, lstrlen(report), &bytes, NULL);
        CloseHandle(output);
    }
}

static void Append(const char *text)
{
    if (lstrlen(report) + lstrlen(text) < (int)sizeof(report)) lstrcat(report, text);
}

static void Graph(HANDLE filter)
{
    KSP_PIN request;
    DWORD output[1024], returned, error, i, pin, property;
    BOOL ok;
    char line[160];
    ZeroMemory(&request, sizeof(request));
    request.Property.Set = topologyProperties;
    request.Property.Flags = KSPROPERTY_TYPE_GET;
    for (i = 0; i < 3; i++) {
        request.Property.Id = i;
        wsprintf(line, "  KS topology query %lu begin\r\n", i); Append(line); Save();
        ZeroMemory(output, sizeof(output)); returned = 0;
        ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request.Property,
            sizeof(KSPROPERTY), output, sizeof(output), &returned, NULL);
        error = ok ? 0 : GetLastError();
        wsprintf(line, "  KS topology %lu ok=%u error=%lu bytes=%lu size=%lu count=%lu\r\n",
            i, ok, error, returned, output[0], output[1]); Append(line); Save();
    }
    request.Property.Set = pinProperties;
    for (pin = 0; pin < 2; pin++) {
        request.PinId = pin;
        for (i = 0; i < 5; i++) {
            static const DWORD ids[] = { KSPROPERTY_PIN_CTYPES, KSPROPERTY_PIN_DATAFLOW,
                KSPROPERTY_PIN_DATARANGES, KSPROPERTY_PIN_COMMUNICATION, KSPROPERTY_PIN_PHYSICALCONNECTION };
            property = ids[i]; request.Property.Id = property;
            wsprintf(line, "  KS pin %lu property %lu begin\r\n", pin, property); Append(line); Save();
            ZeroMemory(output, sizeof(output)); returned = 0;
            ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY, &request, sizeof(request),
                output, sizeof(output), &returned, NULL);
            error = ok ? 0 : GetLastError();
            wsprintf(line, "  KS pin %lu property %lu ok=%u error=%lu bytes=%lu first=%lu second=%lu\r\n",
                pin, property, ok, error, returned, output[0], output[1]); Append(line); Save();
        }
    }
}

static void Interfaces(GUID *category, const char *name, int inspect)
{
    HDEVINFO set = SetupDiGetClassDevs(category, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA entry;
    DWORD index;
    char line[128];
    if (set == INVALID_HANDLE_VALUE) return;
    for (index = 0; index < 128; index++) {
        ZeroMemory(&entry, sizeof(entry)); entry.cbSize = sizeof(entry);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, category, index, &entry)) break;
        wsprintf(line, "  %s interface %lu flags=%08lX\r\n", name, index, entry.Flags); Append(line);
        if (inspect) {
            BYTE bytes[4096];
            PSP_DEVICE_INTERFACE_DETAIL_DATA detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)bytes;
            SP_DEVINFO_DATA device;
            char service[128];
            DWORD type;
            HANDLE filter;
            ZeroMemory(bytes, sizeof(bytes)); detail->cbSize = sizeof(*detail);
            ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
            if (SetupDiGetDeviceInterfaceDetail(set, &entry, detail, sizeof(bytes), NULL, &device) &&
                SetupDiGetDeviceRegistryProperty(set, &device, SPDRP_SERVICE, &type,
                    (PBYTE)service, sizeof(service), NULL) && lstrcmpi(service, "uac2wdm") == 0) {
                Append("  Opening our uac2wdm filter for read-only KS queries\r\n"); Save();
                filter = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                if (filter == INVALID_HANDLE_VALUE) {
                    wsprintf(line, "  KS filter open error=%lu\r\n", GetLastError()); Append(line);
                } else { Graph(filter); CloseHandle(filter); Append("  KS filter closed\r\n"); }
            }
        }
    }
    wsprintf(line, "%s interfaces=%lu\r\n", name, index); Append(line);
    SetupDiDestroyDeviceInfoList(set);
}

static void Collect(int audio)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    HMODULE cfg;
    CMSTATUS statusFunction;
    DWORD index, type, size, stage, status, flags, problem;
    HKEY key;
    char description[256], service[256], line[768];
    UINT i, count;
    WAVEOUTCAPS caps;
    WAVEFORMATEX format;
    HWAVEOUT wave;
    MMRESULT result;

    report[0] = 0;
    Append("UAC2 M0 report; no audio data submitted\r\n\r\n");
    {
        SC_HANDLE manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (manager) {
            static const char *names[] = { "AudioSrv", "sysaudio", "wdmaud", "kmixer" };
            UINT s;
            for (s = 0; s < 4; s++) {
                SC_HANDLE object = OpenService(manager, names[s], SERVICE_QUERY_STATUS);
                SERVICE_STATUS state;
                if (object) {
                    if (QueryServiceStatus(object, &state)) {
                        wsprintf(line, "Service %s state=%lu exit=%lu\r\n", names[s], state.dwCurrentState, state.dwWin32ExitCode); Append(line);
                    }
                    CloseServiceHandle(object);
                }
            }
            CloseServiceHandle(manager);
        }
    }
    {
        char source[MAX_PATH];
        UINT length = GetSystemDirectory(source, sizeof(source));
        if (length && length < MAX_PATH - 24) {
            BOOL ok;
            lstrcat(source, "\\drivers\\uac2wdm.sys");
            /* Our installed experimental SYS only, even while detached.
             * Never copy system or vendor binaries. Host hashes identity. */
            ok = CopyFile(source, "A:\\RUNDRV.SYS", FALSE);
            wsprintf(line, "Export our installed SYS ok=%u error=%lu\r\n", ok, ok ? 0 : GetLastError()); Append(line);
        }
    }
    cfg = LoadLibrary("cfgmgr32.dll");
    statusFunction = cfg ? (CMSTATUS)GetProcAddress(cfg, "CM_Get_DevNode_Status") : NULL;
    set = SetupDiGetClassDevs(&mediaClass, NULL, NULL, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        for (index = 0; index < 128; index++) {
            ZeroMemory(&dev, sizeof(dev)); dev.cbSize = sizeof(dev);
            if (!SetupDiEnumDeviceInfo(set, index, &dev)) break;
            description[0] = service[0] = 0;
            SetupDiGetDeviceRegistryProperty(set, &dev, SPDRP_DEVICEDESC, &type,
                (PBYTE)description, sizeof(description), NULL);
            SetupDiGetDeviceRegistryProperty(set, &dev, SPDRP_SERVICE, &type,
                (PBYTE)service, sizeof(service), NULL);
            description[255] = service[255] = 0;
            wsprintf(line, "%s [service=%s]\r\n", description, service); Append(line);
            flags = problem = 0;
            if (statusFunction && statusFunction(&flags, &problem, dev.DevInst, 0) == 0) {
                wsprintf(line, "  devnode flags=%08lX problem=%lu\r\n", flags, problem); Append(line);
            }
            key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
            if (key != INVALID_HANDLE_VALUE) {
                size = sizeof(stage);
                if (RegQueryValueEx(key, "Uac2Build", NULL, &type, (PBYTE)&stage, &size) == ERROR_SUCCESS &&
                    type == REG_DWORD && size == sizeof(stage)) {
                    wsprintf(line, "  Driver diagnostic build=%lu\r\n", stage); Append(line);
                }
                size = sizeof(stage);
                if (RegQueryValueEx(key, "Uac2M0Stage", NULL, &type, (PBYTE)&stage, &size) == ERROR_SUCCESS) {
                    size = sizeof(status);
                    if (RegQueryValueEx(key, "Uac2M0Status", NULL, &type, (PBYTE)&status, &size) == ERROR_SUCCESS) {
                        wsprintf(line, "  M0 stage=%lu status=%08lX\r\n", stage, status); Append(line);
                    }
                }
                {
                    DWORD counts[4];
                    size = sizeof(counts);
                    if (RegQueryValueEx(key, "Uac2Streams", NULL, &type, (PBYTE)counts, &size) == ERROR_SUCCESS &&
                        type == REG_BINARY && size == sizeof(counts)) {
                        wsprintf(line, "  Streams attempts=%lu created=%lu closed=%lu active_snapshot=%lu\r\n",
                            counts[0], counts[1], counts[2], counts[3]); Append(line);
                    }
                }
                {
                    static const char *labels[] = { "SubmittedBytes", "SubmittedNonzeroBytes", "CompletedBytes",
                        "DataUrbs", "FeedbackUrbs", "PacketErrors", "GoodFeedback", "BadFeedback", "Cancelled",
                        "LastUsbStatus", "LastFeedbackValue", "LastFeedbackLength", "FeedbackKeepalives", "LastStatus", "Running",
                        "HighSpeed", "ClockHz", "MuteKnown", "Mute", "VolumeKnown", "Volume256Db", "FeatureStatus" };
                    DWORD values[64], field;
                    size = sizeof(values);
                    if (RegQueryValueEx(key, "Uac2UsbStats", NULL, &type, (PBYTE)values, &size) == ERROR_SUCCESS &&
                        type == REG_BINARY && size == 22 * sizeof(DWORD)) {
                        Append("  USB completion snapshot (recorded at stop/close):\r\n");
                        for (field = 0; field < 22; field++) {
                            wsprintf(line, "    %s=%08lX (%ld)\r\n", labels[field], values[field], (LONG)values[field]); Append(line);
                        }
                    }
                }
                {
                    DWORD parse[4];
                    PBYTE config = (PBYTE)LocalAlloc(LMEM_FIXED, 65535);
                    size = sizeof(parse);
                    if (RegQueryValueEx(key, "Uac2M0Parse", NULL, &type, (PBYTE)parse, &size) == ERROR_SUCCESS) {
                        wsprintf(line, "  Parse first=%lu count=%lu control=%lu result=%lu\r\n",
                            parse[0], parse[1], parse[2], parse[3]); Append(line);
                    }
                    size = 65535;
                    if (config && RegQueryValueEx(key, "Uac2M0Config", NULL, &type, config, &size) == ERROR_SUCCESS && size >= 9) {
                        DWORD offset;
                        HANDLE file;
                        DWORD written;
                        wsprintf(line, "  Config length=%lu header=%u/%u total=%u ifaces=%u\r\n",
                            size, config[0], config[1], config[2] | (config[3] << 8), config[4]); Append(line);
                        for (offset = 0; offset + 2 <= size; offset += config[offset]) {
                            PBYTE p = config + offset;
                            if (p[0] < 2 || p[0] > size - offset) break;
                            if (p[1] == 11 && p[0] >= 8) {
                                wsprintf(line, "    IAD first=%u count=%u class=%02X/%02X/%02X\r\n",
                                    p[2],p[3],p[4],p[5],p[6]); Append(line);
                            }
                            if (p[1] == 4 && p[0] >= 9 && p[3] == 0) {
                                wsprintf(line, "    IF=%u alt=%u class=%02X/%02X/%02X\r\n",
                                    p[2],p[3],p[5],p[6],p[7]); Append(line);
                            }
                        }
                        file = CreateFile("A:\\XPCONFIG.BIN", GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (file != INVALID_HANDLE_VALUE) {
                            WriteFile(file, config, size, &written, NULL); CloseHandle(file);
                        }
                    }
                    if (config) LocalFree(config);
                }
                RegCloseKey(key);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (cfg) FreeLibrary(cfg);
    Interfaces(&audioCategory, "AUDIO", audio == 2);
    Interfaces(&renderCategory, "RENDER", 0);
    Interfaces(&topologyCategory, "TOPOLOGY", 0);
    Interfaces(&audioDeviceCategory, "AUDIO_DEVICE", 0);
    if (audio != 1) { Append("\r\nWinMM probe not requested.\r\n"); Save(); return; }
    count = waveOutGetNumDevs();
    wsprintf(line, "\r\nwaveOutGetNumDevs=%u\r\n", count); Append(line);
    for (i = 0; i < count && i < 32; i++) {
        ZeroMemory(&caps, sizeof(caps));
        result = waveOutGetDevCaps(i, &caps, sizeof(caps));
        wsprintf(line, "  %u result=%u name=%s channels=%u formats=%08lX\r\n",
            i, result, caps.szPname, caps.wChannels, caps.dwFormats); Append(line);
    }
    ZeroMemory(&format, sizeof(format));
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2; format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16; format.nBlockAlign = 4;
    format.nAvgBytesPerSec = 192000;
    result = waveOutOpen(NULL, WAVE_MAPPER, &format, 0, 0, WAVE_FORMAT_QUERY);
    wsprintf(line, "format query stereo/16/48000=%u\r\n", result); Append(line);
    wave = NULL;
    result = waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    wsprintf(line, "waveOutOpen stereo/16/48000=%u\r\n", result); Append(line);
    if (!result) waveOutClose(wave);
    Save();
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SIZE && edit) MoveWindow(edit, 0, 68, LOWORD(l), HIWORD(l) - 68, TRUE);
    if (message == WM_COMMAND) {
        if (LOWORD(w) == 1) {
            HMODULE library = LoadLibrary("newdev.dll");
            UPDATE update = library ? (UPDATE)GetProcAddress(library,
                "UpdateDriverForPlugAndPlayDevicesA") : NULL;
            BOOL reboot = FALSE;
            BOOL ok = FALSE;
            DWORD error = ERROR_CALL_NOT_IMPLEMENTED;
            char line[128];
            if (update) {
                ok = update(window, "USB\\VID_041E&PID_3278&MI_03",
                    "A:\\uac2wdm.inf", 1 /* INSTALLFLAG_FORCE */, &reboot);
                error = ok ? 0 : GetLastError();
            }
            if (library) FreeLibrary(library);
            Collect(0);
            wsprintf(line, "\r\nX4 MI_03 update ok=%u error=%08lX reboot=%u\r\n", ok, error, reboot);
            Append(line); Save(); SetWindowText(edit, report);
            return 0;
        }
        if (LOWORD(w) == 2) { Collect(0); SetWindowText(edit, report); return 0; }
        if (LOWORD(w) == 3) { Collect(1); SetWindowText(edit, report); return 0; }
        if (LOWORD(w) == 4) { Collect(2); SetWindowText(edit, report); return 0; }
        if (LOWORD(w) == 5) {
            HDEVINFO set = SetupDiGetClassDevs(&mediaClass, NULL, NULL, DIGCF_PRESENT);
            SP_DEVINFO_DATA device;
            DWORD index, type, pathLength;
            BOOL present = FALSE;
            char service[256], destination[MAX_PATH], line[160];
            if (set == INVALID_HANDLE_VALUE) return 0;
            for (index = 0; index < 128; index++) {
                ZeroMemory(&device, sizeof(device)); device.cbSize = sizeof(device);
                if (!SetupDiEnumDeviceInfo(set, index, &device)) {
                    if (GetLastError() != ERROR_NO_MORE_ITEMS) present = TRUE;
                    break;
                }
                ZeroMemory(service, sizeof(service));
                if (SetupDiGetDeviceRegistryProperty(set, &device, SPDRP_SERVICE, &type,
                    (PBYTE)service, sizeof(service) - 1, NULL) && lstrcmpi(service, "uac2wdm") == 0) present = TRUE;
            }
            SetupDiDestroyDeviceInfoList(set);
            if (present) Append("\r\nDriver copy refused: uac2wdm device still present.\r\n");
            else if ((pathLength = GetSystemDirectory(destination, sizeof(destination))) != 0 &&
                    pathLength < MAX_PATH - 24) {
                BOOL ok;
                lstrcat(destination, "\\drivers\\uac2wdm.sys");
                ok = CopyFile("A:\\uac2wdm.sys", destination, FALSE);
                wsprintf(line, "\r\nDetached driver copy ok=%u error=%lu\r\n", ok, ok ? 0 : GetLastError()); Append(line);
            }
            Save(); SetWindowText(edit, report); return 0;
        }
        if (LOWORD(w) == 6) {
            static const char *names[] = { "wdmaudio.inf", "ks.inf", "wdma_usb.inf" };
            static const char *outputs[] = { "A:\\XPWDM.INF", "A:\\XPKS.INF", "A:\\XPUSB.INF" };
            UINT i;
            char source[MAX_PATH], line[160];
            for (i = 0; i < 3; i++) {
                UINT length = GetWindowsDirectory(source, sizeof(source));
                if (length && length < MAX_PATH - 32) {
                    BOOL ok;
                    lstrcat(source, "\\inf\\"); lstrcat(source, names[i]);
                    ok = CopyFile(source, outputs[i], FALSE);
                    wsprintf(line, "\r\nExport inbox INF %s ok=%u error=%lu\r\n", names[i], ok, ok ? 0 : GetLastError()); Append(line);
                }
            }
            Save(); SetWindowText(edit, report); return 0;
        }
        if (LOWORD(w) == 7) {
            SC_HANDLE manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
            if (manager) {
                static const char *names[] = { "sysaudio", "wdmaud", "kmixer" };
                UINT i;
                char line[160];
                for (i = 0; i < 3; i++) {
                    SC_HANDLE object = OpenService(manager, names[i], SERVICE_START);
                    if (object) {
                        BOOL ok;
                        wsprintf(line, "\r\nStart %s begin\r\n", names[i]); Append(line); Save();
                        ok = StartService(object, 0, NULL);
                        wsprintf(line, "Start %s ok=%u error=%lu\r\n", names[i], ok, ok ? 0 : GetLastError()); Append(line); Save();
                        CloseServiceHandle(object);
                    }
                }
                CloseServiceHandle(manager);
            }
            SetWindowText(edit, report); return 0;
        }
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(window, message, w, l);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WNDCLASS wc;
    HWND window;
    HWND install;
    MSG message;
    (void)previous; (void)command;
    Collect(0);
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
    wc.lpszClassName = "UacProbe"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClass(&wc)) return 1;
    window = CreateWindow("UacProbe", "UAC2 M0 report - A:\\UACLOG.TXT",
        WS_OVERLAPPEDWINDOW, 20, 20, 780, 530, NULL, NULL, instance, NULL);
    install = CreateWindow("BUTTON", "&Install X4 MI_03 from A:", WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 240, 30, window, (HMENU)1, instance, NULL);
    CreateWindow("BUTTON", "&Refresh report", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        245, 0, 180, 30, window, (HMENU)2, instance, NULL);
    CreateWindow("BUTTON", "&Probe WinMM (no data)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        430, 0, 220, 30, window, (HMENU)3, instance, NULL);
    CreateWindow("BUTTON", "Probe KS", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        655, 0, 110, 30, window, (HMENU)4, instance, NULL);
    CreateWindow("BUTTON", "Copy A: driver while detached", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 34, 300, 30, window, (HMENU)5, instance, NULL);
    CreateWindow("BUTTON", "Export inbox INF text", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        305, 34, 300, 30, window, (HMENU)6, instance, NULL);
    CreateWindow("BUTTON", "Start XP audio filters", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        610, 34, 150, 30, window, (HMENU)7, instance, NULL);
    edit = CreateWindow("EDIT", report, WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        WS_HSCROLL | ES_MULTILINE | ES_READONLY, 0, 68, 760, 422, window, NULL, instance, NULL);
    SendMessage(edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
    ShowWindow(window, show); UpdateWindow(window);
    SetFocus(install);
    while (GetMessage(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessage(window, &message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    return 0;
}
