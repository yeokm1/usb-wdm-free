/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * uacci.c - test-only PROBE class co-installer for the MEDIA class
 * (docs/design/uac2.md section 16, step 1). It changes nothing: it appends
 * one line per call, before and after the default handler, to
 * %windir%\UACCI.LOG - the DIF code, the device's instance id and class
 * GUID at that moment, its compatible ids, every driver node of the
 * compatible list (INF, section, rank, flags) and the selected one. Never
 * shipped. Original code from the Windows driver kit documentation of
 * class co-installers and SetupAPI; no sample code.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

/* Every record is at most 1024 characters: wsprintfA's own limit, and each
 * string field is cut by its precision so the sum stays below it.
 * One record per call, the line and its CRLF in one write, under a named
 * mutex so two installer processes never write at the same offset. */
static void LogLine(const char *text)
{
    char path[MAX_PATH], rec[1100];
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
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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

static const char *DifName(DI_FUNCTION f)
{
    switch (f) {
    case DIF_SELECTDEVICE: return "SELECTDEVICE";
    case DIF_INSTALLDEVICE: return "INSTALLDEVICE";
    case DIF_ALLOW_INSTALL: return "ALLOW_INSTALL";
    case DIF_SELECTBESTCOMPATDRV: return "SELECTBESTCOMPATDRV";
    case DIF_INSTALLDEVICEFILES: return "INSTALLDEVICEFILES";
    case DIF_REGISTER_COINSTALLERS: return "REGISTER_COINSTALLERS";
    case DIF_INSTALLINTERFACES: return "INSTALLINTERFACES";
    case 0x1E: return "NEWDEVICEWIZARD_FINISHINSTALL";
    case DIF_PROPERTYCHANGE: return "PROPERTYCHANGE";
    case DIF_REMOVE: return "REMOVE";
    case DIF_DESTROYPRIVATEDATA: return "DESTROYPRIVATEDATA";
    case DIF_REGISTERDEVICE: return "REGISTERDEVICE";
    case 0x2A: return "FINISHINSTALL_ACTION";
    default: return "?";
    }
}

static void Guid(char *out, const GUID *g)
{
    wsprintfA(out, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
              g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static void LogDevice(const char *tag, DI_FUNCTION f, HDEVINFO set, PSP_DEVINFO_DATA dev)
{
    char line[1024], id[512], guid[64], ids[512];
    DWORD type, need, i;
    SP_DRVINFO_DATA_A drv;
    SP_DRVINFO_DATA_A sel;
    SP_DRVINSTALL_PARAMS ip;
    SP_DEVINSTALL_PARAMS_A dp;
    BOOL haveSel;
    DWORD derr;

    id[0] = 0;
    ids[0] = 0;
    if (dev) {
        SetupDiGetDeviceInstanceIdA(set, dev, id, sizeof(id), NULL);
        Guid(guid, &dev->ClassGuid);
        if (SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_COMPATIBLEIDS, &type,
                                              (PBYTE)ids, sizeof(ids) - 2, &need)) {
            for (i = 0; i + 1 < sizeof(ids) && !(ids[i] == 0 && ids[i + 1] == 0); i++)
                if (ids[i] == 0) ids[i] = '|';
        } else ids[0] = 0;
    } else lstrcpyA(guid, "-");
    dp.cbSize = sizeof(dp);
    if (!SetupDiGetDeviceInstallParamsA(set, dev, &dp)) { dp.Flags = 0; dp.FlagsEx = 0; }
    wsprintfA(line, "%.4s %.32s(0x%02lX) pid=%lu dev=%.200s class=%.40s flags=%08lX ex=%08lX compat=%.300s",
              tag, DifName(f), (unsigned long)f, GetCurrentProcessId(), id, guid,
              dp.Flags, dp.FlagsEx, ids);
    LogLine(line);
    if (!dev) return;
    sel.cbSize = sizeof(sel);
    haveSel = SetupDiGetSelectedDriverA(set, dev, &sel);
    for (i = 0; i < 16; i++) {
        char detail[2048];
        PSP_DRVINFO_DETAIL_DATA_A d = (PSP_DRVINFO_DETAIL_DATA_A)detail;
        drv.cbSize = sizeof(drv);
        if (!SetupDiEnumDriverInfoA(set, dev, SPDIT_COMPATDRIVER, i, &drv)) break;
        d->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_A);
        derr = 0;
        if (!SetupDiGetDriverInfoDetailA(set, dev, &drv, d, sizeof(detail), &need)) {
            /* Too small a buffer still fills the fixed fields; only the
             * id list is short then. Anything else: nothing to trust. */
            derr = GetLastError();
            d->HardwareID[0] = 0;
            if (derr != ERROR_INSUFFICIENT_BUFFER) {
                d->InfFileName[0] = 0;
                d->SectionName[0] = 0;
            }
        }
        ip.cbSize = sizeof(ip);
        if (!SetupDiGetDriverInstallParamsA(set, dev, &drv, &ip)) {
            ip.Rank = 0xffffffffUL;
            ip.Flags = 0;
        }
        wsprintfA(line, "   drv%lu%.1s inf=%.200s sec=%.120s hw=%.200s rank=%08lX dnf=%08lX err=%lu desc=%.120s", i,
                  (haveSel && drv.Reserved == sel.Reserved) ? "*" : " ",
                  d->InfFileName, d->SectionName, d->HardwareID, ip.Rank, ip.Flags,
                  derr, drv.Description);
        LogLine(line);
    }
    if (haveSel) {
        wsprintfA(line, "   selected: %.200s (%.200s)", sel.Description, sel.ProviderName);
        LogLine(line);
    }
}

DWORD CALLBACK UacCoInstaller(DI_FUNCTION f, HDEVINFO set, PSP_DEVINFO_DATA dev,
                              PCOINSTALLER_CONTEXT_DATA ctx)
{
    if (ctx && ctx->PostProcessing) {
        char line[64];
        wsprintfA(line, "   (post status %08lX)", ctx->InstallResult);
        LogLine(line);
        LogDevice("POST", f, set, dev);
        return ctx->InstallResult;
    }
    LogDevice("PRE ", f, set, dev);
    /* DIF_ALLOW_INSTALL is not to be post-processed by a co-installer. */
    return f == DIF_ALLOW_INSTALL ? NO_ERROR : ERROR_DI_POSTPROCESSING_REQUIRED;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) LogLine("== UACCI loaded");
    return TRUE;
}
