/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * HIDTEST.EXE - guest diagnosis tool for xpad98's legs: reads a HID game
 * controller through the user-mode HID API (hid.dll, loaded at run time,
 * and setupapi.dll), as the HID parser in the guest sees it. For every
 * present HID device interface whose VID matches (default: any), it writes
 * HidP_GetCaps, every value and button capability, then reads N input
 * reports and for each value both HidP_GetUsageValue and
 * HidP_GetScaledUsageValue with their status codes. Report: C:\HIDTEST.TXT.
 *
 *   HIDTEST [reports] [vid hex]          default 20 reports, any VID
 *
 * The structure layouts and function signatures are those of the
 * published hidsdi.h / hidpi.h interface (Windows 98 DDK and WDK 7.1),
 * declared here so that the tool builds with MSVC 6.0 alone. Original code.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef USHORT USAGE;
typedef LONG NTSTATUS_HID;
typedef PVOID PHIDP_PREPARSED_DATA;

typedef struct {
    ULONG Size; USHORT VendorID; USHORT ProductID; USHORT VersionNumber;
} HIDD_ATTRIBUTES;

typedef struct {
    USAGE Usage, UsagePage;
    USHORT InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
    USHORT Reserved[17];
    USHORT NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices;
    USHORT NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices;
    USHORT NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
} HIDP_CAPS;

typedef struct {
    USAGE UsagePage; UCHAR ReportID; BOOLEAN IsAlias;
    USHORT BitField, LinkCollection;
    USAGE LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute, HasNull; UCHAR Reserved;
    USHORT BitSize, ReportCount;
    USHORT Reserved2[5];
    ULONG UnitsExp, Units;
    LONG LogicalMin, LogicalMax, PhysicalMin, PhysicalMax;
    union {
        struct { USAGE UsageMin, UsageMax; USHORT StringMin, StringMax, DesignatorMin, DesignatorMax, DataIndexMin, DataIndexMax; } Range;
        struct { USAGE Usage, Reserved1; USHORT StringIndex, Reserved2, DesignatorIndex, Reserved3, DataIndex, Reserved4; } NotRange;
    } u;
} HIDP_VALUE_CAPS;

typedef struct {
    USAGE UsagePage; UCHAR ReportID; BOOLEAN IsAlias;
    USHORT BitField, LinkCollection;
    USAGE LinkUsage, LinkUsagePage;
    BOOLEAN IsRange, IsStringRange, IsDesignatorRange, IsAbsolute;
    ULONG Reserved[10];
    union {
        struct { USAGE UsageMin, UsageMax; USHORT StringMin, StringMax, DesignatorMin, DesignatorMax, DataIndexMin, DataIndexMax; } Range;
        struct { USAGE Usage, Reserved1; USHORT StringIndex, Reserved2, DesignatorIndex, Reserved3, DataIndex, Reserved4; } NotRange;
    } u;
} HIDP_BUTTON_CAPS;

#define HidP_Input 0

typedef VOID (__stdcall *GETGUID)(LPGUID);
typedef BOOLEAN (__stdcall *GETATTR)(HANDLE, HIDD_ATTRIBUTES *);
typedef BOOLEAN (__stdcall *GETPP)(HANDLE, PHIDP_PREPARSED_DATA *);
typedef BOOLEAN (__stdcall *FREEPP)(PHIDP_PREPARSED_DATA);
typedef NTSTATUS_HID (__stdcall *GETCAPS)(PHIDP_PREPARSED_DATA, HIDP_CAPS *);
typedef NTSTATUS_HID (__stdcall *GETVCAPS)(int, HIDP_VALUE_CAPS *, PUSHORT, PHIDP_PREPARSED_DATA);
typedef NTSTATUS_HID (__stdcall *GETBCAPS)(int, HIDP_BUTTON_CAPS *, PUSHORT, PHIDP_PREPARSED_DATA);
typedef NTSTATUS_HID (__stdcall *GETVAL)(int, USAGE, USHORT, USAGE, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
typedef NTSTATUS_HID (__stdcall *GETSVAL)(int, USAGE, USHORT, USAGE, PLONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
typedef NTSTATUS_HID (__stdcall *GETUSAGES)(int, USAGE, USHORT, USAGE *, PULONG, PHIDP_PREPARSED_DATA, PCHAR, ULONG);

static GETGUID pGuid; static GETATTR pAttr; static GETPP pPP; static FREEPP pFree;
static GETCAPS pCaps; static GETVCAPS pVCaps; static GETBCAPS pBCaps; static GETVAL pVal; static GETSVAL pSVal;
static GETUSAGES pUsages;

static int Probe(FILE *f, const char *path, int reports, unsigned vid)
{
    HANDLE h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HIDD_ATTRIBUTES a;
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    HIDP_VALUE_CAPS vc[16];
    HIDP_BUTTON_CAPS bc[8];
    USHORT nv = 16, nb = 8, i;
    char buf[256];
    int r;
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { fprintf(f, "open %s failed %lu\n", path, GetLastError()); return 0; }
    }
    memset(&a, 0, sizeof(a)); a.Size = sizeof(a);
    if (!pAttr(h, &a)) { fprintf(f, "GetAttributes failed %lu\n", GetLastError()); CloseHandle(h); return 0; }
    if (vid && a.VendorID != vid) { CloseHandle(h); return 0; }
    fprintf(f, "device %s\n  VID %04x PID %04x ver %04x\n", path, a.VendorID, a.ProductID, a.VersionNumber);
    if (!pPP(h, &pp)) { fprintf(f, "  GetPreparsedData failed %lu\n", GetLastError()); CloseHandle(h); return 1; }
    memset(&caps, 0, sizeof(caps));
    fprintf(f, "  GetCaps %08lx: usage %02x:%02x in %u out %u feat %u, %u button caps, %u value caps, %u data indices\n",
            pCaps(pp, &caps), caps.UsagePage, caps.Usage, caps.InputReportByteLength, caps.OutputReportByteLength,
            caps.FeatureReportByteLength, caps.NumberInputButtonCaps, caps.NumberInputValueCaps, caps.NumberInputDataIndices);
    fprintf(f, "  GetValueCaps %08lx (%u)\n", pVCaps(HidP_Input, vc, &nv, pp), nv);
    for (i = 0; i < nv; i++)
        fprintf(f, "   value %u: page %02x usage %02x%s bits %u count %u abs %u null %u logical %ld..%ld physical %ld..%ld units %08lx exp %lu\n",
                i, vc[i].UsagePage, vc[i].IsRange ? vc[i].u.Range.UsageMin : vc[i].u.NotRange.Usage, vc[i].IsRange ? " (range)" : "",
                vc[i].BitSize, vc[i].ReportCount, vc[i].IsAbsolute, vc[i].HasNull, vc[i].LogicalMin, vc[i].LogicalMax,
                vc[i].PhysicalMin, vc[i].PhysicalMax, vc[i].Units, vc[i].UnitsExp);
    fprintf(f, "  GetButtonCaps %08lx (%u)\n", pBCaps(HidP_Input, bc, &nb, pp), nb);
    for (i = 0; i < nb; i++)
        fprintf(f, "   buttons %u: page %02x usages %u..%u range %u\n", i, bc[i].UsagePage,
                bc[i].IsRange ? bc[i].u.Range.UsageMin : bc[i].u.NotRange.Usage,
                bc[i].IsRange ? bc[i].u.Range.UsageMax : bc[i].u.NotRange.Usage, bc[i].IsRange);
    for (r = 0; r < reports; r++) {
        DWORD got = 0;
        if (!ReadFile(h, buf, caps.InputReportByteLength, &got, NULL)) {
            fprintf(f, "  read %d failed %lu\n", r, GetLastError());
            break;
        }
        fprintf(f, "  report %d (%lu):", r, got);
        for (i = 0; i < got && i < 32; i++) fprintf(f, " %02x", (UCHAR)buf[i]);
        fprintf(f, "\n   ");
        for (i = 0; i < nv; i++) {
            ULONG v = 0; LONG sv = 0;
            USAGE u = vc[i].IsRange ? vc[i].u.Range.UsageMin : vc[i].u.NotRange.Usage;
            NTSTATUS_HID s1 = pVal(HidP_Input, vc[i].UsagePage, 0, u, &v, pp, buf, got);
            NTSTATUS_HID s2 = pSVal(HidP_Input, vc[i].UsagePage, 0, u, &sv, pp, buf, got);
            fprintf(f, " %02x=%lu[%08lx]/%ld[%08lx]", u, v, s1, sv, s2);
        }
        if (nb) {
            USAGE us[32]; ULONG n = 32;
            NTSTATUS_HID s3 = pUsages(HidP_Input, bc[0].UsagePage, 0, us, &n, pp, buf, got);
            fprintf(f, " buttons[%08lx]:", s3);
            for (i = 0; i < n && s3 == 0x00110000; i++) fprintf(f, " %u", us[i]);
        }
        fprintf(f, "\n");
    }
    pFree(pp);
    CloseHandle(h);
    return 1;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    int reports = 20, found = 0;
    unsigned vid = 0;
    HMODULE hid = LoadLibrary("HID.DLL");
    GUID g;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i;
    FILE *f = fopen("C:\\HIDTEST.TXT", "wt");
    char msg[200];
    UNREFERENCED_PARAMETER(inst); UNREFERENCED_PARAMETER(prev); UNREFERENCED_PARAMETER(show);
    if (cmd && *cmd) {
        char *p = cmd;
        reports = atoi(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p) vid = (unsigned)strtoul(p, NULL, 16);
        if (reports <= 0) reports = 20;
    }
    if (!f) return 2;
    if (!hid) { fprintf(f, "HID.DLL not loaded %lu\n", GetLastError()); fclose(f); return 2; }
    pGuid = (GETGUID)GetProcAddress(hid, "HidD_GetHidGuid");
    pAttr = (GETATTR)GetProcAddress(hid, "HidD_GetAttributes");
    pPP = (GETPP)GetProcAddress(hid, "HidD_GetPreparsedData");
    pFree = (FREEPP)GetProcAddress(hid, "HidD_FreePreparsedData");
    pCaps = (GETCAPS)GetProcAddress(hid, "HidP_GetCaps");
    pVCaps = (GETVCAPS)GetProcAddress(hid, "HidP_GetValueCaps");
    pBCaps = (GETBCAPS)GetProcAddress(hid, "HidP_GetButtonCaps");
    pVal = (GETVAL)GetProcAddress(hid, "HidP_GetUsageValue");
    pSVal = (GETSVAL)GetProcAddress(hid, "HidP_GetScaledUsageValue");
    pUsages = (GETUSAGES)GetProcAddress(hid, "HidP_GetUsages");
    if (!pGuid || !pAttr || !pPP || !pFree || !pCaps || !pVCaps || !pBCaps || !pVal || !pSVal || !pUsages) {
        fprintf(f, "a HID.DLL export is missing\n"); fclose(f); return 2;
    }
    pGuid(&g);
    set = SetupDiGetClassDevs(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { fprintf(f, "SetupDiGetClassDevs failed %lu\n", GetLastError()); fclose(f); return 2; }
    for (i = 0; ; i++) {
        char detailBuf[512];
        PSP_DEVICE_INTERFACE_DETAIL_DATA d = (PSP_DEVICE_INTERFACE_DETAIL_DATA)detailBuf;
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &ifd)) break;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(set, &ifd, d, sizeof(detailBuf), NULL, NULL)) {
            fprintf(f, "interface %lu: detail failed %lu\n", i, GetLastError());
            continue;
        }
        found += Probe(f, d->DevicePath, reports, vid);
    }
    SetupDiDestroyDeviceInfoList(set);
    fprintf(f, "%d device(s) probed\n", found);
    fclose(f);
    sprintf(msg, "HIDTEST: %d device(s) probed. Report: C:\\HIDTEST.TXT", found);
    MessageBox(NULL, msg, "HIDTEST", MB_OK);
    return 0;
}
