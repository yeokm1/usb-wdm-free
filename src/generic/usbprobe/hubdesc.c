/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original read-only Windows 2000 hub descriptor diagnostic, using published
 * SetupAPI/USB hub IOCTL documentation and ntddk/inc/usbioctl.h declarations.
 * No DDK/WDK samples. No reset/configuration/vendor/string requests.
 */
#define WIN32_LEAN_AND_MEAN
#pragma warning(push)
#pragma warning(disable:4200 4201)
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <objbase.h>
#include <initguid.h>
#include <usbioctl.h>
#pragma warning(pop)
#include <stdio.h>
#include <string.h>

#define HUB_LIMIT 64
#define PORT_LIMIT 127
#define CONFIG_LIMIT 4096
#define PATH_LIMIT 4096
static FILE *report;
static char directory[MAX_PATH], filename[MAX_PATH];
static unsigned char detailBytes[PATH_LIMIT], connectionBytes[1024];
static unsigned char requestBytes[sizeof(USB_DESCRIPTOR_REQUEST) + CONFIG_LIMIT];
static unsigned char deviceBytes[18];
static unsigned int targetVendor = 0x0b95, targetProduct = 0x1790;
static int quiet;

static int Io(HANDLE h, DWORD code, void *buffer, DWORD bytes, DWORD *returned)
{
    OVERLAPPED ov;
    BOOL ok;
    DWORD error, wait;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    ok = DeviceIoControl(h, code, buffer, bytes, buffer, bytes, returned, &ov);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        wait = WaitForSingleObject(ov.hEvent, 10000);
        if (wait != WAIT_OBJECT_0) {
            fprintf(report, "IOCTL timed out/wait failed %lu; cancelling\n", wait); fflush(report);
            CancelIo(h);
            if (WaitForSingleObject(ov.hEvent, 5000) != WAIT_OBJECT_0) {
                fprintf(report, "Cancellation did not drain; exiting diagnostic\n"); fflush(report);
                ExitProcess(4); /* Never reuse pending request storage. */
            }
            ok = FALSE; error = ERROR_TIMEOUT;
        } else { ok = GetOverlappedResult(h, &ov, returned, FALSE); error = ok ? 0 : GetLastError(); }
    }
    CloseHandle(ov.hEvent);
    if (!ok) fprintf(report, "IOCTL %08lX failed error %lu\n", code, error);
    return ok != FALSE;
}
static int Save(const char *name, const void *bytes, unsigned int length)
{
    FILE *f;
    int good;
    if (strlen(directory) + strlen(name) >= sizeof(filename)) return 0;
    strcpy(filename, directory); strcat(filename, name);
    f = fopen(filename, "wb");
    if (!f) { fprintf(report, "Cannot create %s\n", name); return 0; }
    good = fwrite(bytes, 1, length, f) == length;
    if (fclose(f)) good = 0;
    fprintf(report, "%s: %s, %u bytes\n", name, good ? "saved" : "WRITE FAILED", length);
    return good;
}
static int Descriptor(HANDLE hub, ULONG port, unsigned int type,
    unsigned int length, DWORD *actual)
{
    PUSB_DESCRIPTOR_REQUEST request = (PUSB_DESCRIPTOR_REQUEST)requestBytes;
    DWORD returned = 0, prefix = sizeof(USB_DESCRIPTOR_REQUEST);
    if ((type != 1 && type != 2) || length > CONFIG_LIMIT) return 0;
    memset(requestBytes, 0, sizeof(requestBytes));
    request->ConnectionIndex = port;
    request->SetupPacket.bmRequest = 0x80; request->SetupPacket.bRequest = 6;
    request->SetupPacket.wValue = (USHORT)(type << 8); /* index zero only */
    request->SetupPacket.wIndex = 0; request->SetupPacket.wLength = (USHORT)length;
    if (!Io(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, request,
        prefix + length, &returned)) return 0;
    if (returned < prefix || returned > prefix + length) {
        fprintf(report, "Invalid descriptor IOCTL byte count %lu\n", returned); return 0;
    }
    *actual = returned - prefix; return 1;
}
static int Capture(HANDLE hub, ULONG port)
{
    DWORD actual;
    unsigned int total, o, size, good = 1;
    const unsigned char *p;
    PUSB_DESCRIPTOR_REQUEST request = (PUSB_DESCRIPTOR_REQUEST)requestBytes;
    if (!Descriptor(hub, port, 1, 18, &actual) || actual != 18) {
        fprintf(report, "Device descriptor unavailable/short\n"); return 0;
    }
    memcpy(deviceBytes, request->Data, 18);
    if (deviceBytes[0] != 18 || deviceBytes[1] != 1 ||
        (deviceBytes[8] | ((unsigned int)deviceBytes[9] << 8)) != targetVendor ||
        (deviceBytes[10] | ((unsigned int)deviceBytes[11] << 8)) != targetProduct) {
        fprintf(report, "Descriptor identity mismatch; refusing capture\n"); return 0;
    }
    fprintf(report, "GET_DESCRIPTOR device bcdUSB=%02X%02X configs=%u\n", deviceBytes[3], deviceBytes[2], deviceBytes[17]);
    if (!Descriptor(hub, port, 2, CONFIG_LIMIT, &actual) || actual < 9) {
        fprintf(report, "Configuration descriptor unavailable/short\n"); return 0;
    }
    p = request->Data; total = p[2] | ((unsigned int)p[3] << 8);
    if (p[0] != 9 || p[1] != 2 || total < 9 || total > actual || total > CONFIG_LIMIT) {
        fprintf(report, "Invalid configuration returned=%lu total=%u\n", actual, total); return 0;
    }
    fprintf(report, "GET_DESCRIPTOR config0 total=%u returned=%lu interfaces=%u\n", total, actual, p[4]);
    for (o = 0; o < total; o += size) {
        if (total - o < 2 || (size = p[o]) < 2 || size > total - o) {
            fprintf(report, "Malformed configuration at offset %u\n", o); return 0;
        }
        if (p[o + 1] == 4) {
            if (size < 9) return 0;
            fprintf(report, "Interface %u alt%u class %02X/%02X/%02X endpoints%u\n",
                p[o+2], p[o+3], p[o+5], p[o+6], p[o+7], p[o+4]);
        } else if (p[o + 1] == 5) {
            unsigned int packet;
            if (size < 7) return 0;
            packet = p[o+4] | ((unsigned int)p[o+5] << 8);
            fprintf(report, "Endpoint %02X type%u rawMaxPacket=%04X bytes=%u interval=%u\n",
                p[o+2], p[o+3] & 3, packet, packet & 2047, p[o+6]);
        } else if (p[o + 1] == 0x30) fprintf(report, "SuperSpeed endpoint companion present at offset%u\n", o);
    }
    if (!Save(quiet ? "DEVICE.BIN" : "AXDEV.BIN", deviceBytes, 18)) good = 0;
    if (!Save(quiet ? "CONFIG.BIN" : "AXCFG.BIN", p, total)) good = 0;
    return good;
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA iface;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
    USB_NODE_INFORMATION node;
    PUSB_NODE_CONNECTION_INFORMATION connection;
    HANDLE hub;
    DWORD needed, returned, index, port, error, pathLength;
    unsigned int matches = 0, captured = 0, hubs = 0;
    GUID hubGuid = GUID_CLASS_USBHUB; /* VC6 SetupAPI takes mutable LPGUID. */
    char *slash;
    (void)instance; (void)previous; (void)show;
    if (command && *command) {
        char extra;
        if (sscanf(command, "%x:%x %c", &targetVendor, &targetProduct, &extra) != 2 ||
            targetVendor > 65535 || targetProduct > 65535 || !targetVendor || !targetProduct) return 2;
        quiet = 1; /* Explicit target mode writes a report without a UI prompt. */
    }
    pathLength = GetModuleFileNameA(NULL, directory, sizeof(directory));
    if (!pathLength || pathLength >= sizeof(directory)) return 2;
    directory[pathLength] = 0;
    slash = strrchr(directory, '\\'); if (!slash) return 2; slash[1] = 0;
    if (strlen(directory) + 11 >= sizeof(filename)) return 2;
    strcpy(filename, directory); strcat(filename, "HUBDESC.TXT");
    report = fopen(filename, "w"); if (!report) return 2;
    fprintf(report, "Original HUBDESC read-only diagnostic; target VID/PID %04X:%04X only\n", targetVendor, targetProduct);
    fprintf(report, "No strings, MAC, vendor controls, configuration changes, resets or driver installation\n");
    fprintf(report, "Limits: 64 hubs,127 ports/hub,4096 config bytes,10s IOCTL+5s cancellation\n");
    fprintf(report, "Old connection IOCTL LowSpeed cannot distinguish full/high speed\n"); fflush(report);
    set = SetupDiGetClassDevsA(&hubGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { fprintf(report, "SetupDiGetClassDevs error%lu\n", GetLastError()); goto done; }
    for (index = 0; index < HUB_LIMIT; ++index) {
        memset(&iface, 0, sizeof(iface)); iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &hubGuid, index, &iface)) {
            error = GetLastError(); if (error != ERROR_NO_MORE_ITEMS) fprintf(report, "Enum interface error%lu\n", error); break;
        }
        ++hubs; needed = 0;
        SetupDiGetDeviceInterfaceDetailA(set, &iface, NULL, 0, &needed, NULL);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) || needed > sizeof(detailBytes)) {
            fprintf(report, "Hub%lu path size refused %lu\n", index, needed); continue;
        }
        memset(detailBytes, 0, sizeof(detailBytes)); detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detailBytes;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &iface, detail, sizeof(detailBytes), &needed, NULL)) continue;
        detailBytes[sizeof(detailBytes)-1] = 0;
        hub = CreateFileA(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (hub == INVALID_HANDLE_VALUE) { fprintf(report, "Hub%lu open error%lu\n", index, GetLastError()); continue; }
        memset(&node, 0, sizeof(node)); node.NodeType = UsbHub; returned = 0;
        if (Io(hub, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &returned) && returned >= sizeof(node)) {
            unsigned int ports = node.u.HubInformation.HubDescriptor.bNumberOfPorts;
            fprintf(report, "Hub%lu ports%u\n", index, ports); fflush(report);
            if (ports > PORT_LIMIT) ports = PORT_LIMIT;
            for (port = 1; port <= ports; ++port) {
                memset(connectionBytes, 0, sizeof(connectionBytes)); connection = (PUSB_NODE_CONNECTION_INFORMATION)connectionBytes;
                connection->ConnectionIndex = port; returned = 0;
                if (!Io(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION, connection, sizeof(connectionBytes), &returned) ||
                    returned < sizeof(USB_NODE_CONNECTION_INFORMATION)) continue;
                if (connection->ConnectionStatus != DeviceConnected || connection->DeviceDescriptor.idVendor != targetVendor ||
                    connection->DeviceDescriptor.idProduct != targetProduct) continue;
                ++matches;
                fprintf(report, "MATCH hub%lu port%lu status%u LowSpeed%u configuration%u pipes%lu\n",
                    index, port, connection->ConnectionStatus, connection->LowSpeed, connection->CurrentConfigurationValue, connection->NumberOfOpenPipes);
                if (matches == 1 && Capture(hub, port)) ++captured;
            }
        }
        CloseHandle(hub);
    }
    SetupDiDestroyDeviceInfoList(set);
done:
    fprintf(report, "RESULT hubs%u matches%u captured%u\n", hubs, matches, captured); fclose(report);
    if (!quiet) MessageBoxA(NULL, captured == 1 && matches == 1 ? "Target descriptors saved beside HUBDESC.EXE. See HUBDESC.TXT." :
        "Descriptor capture incomplete. See HUBDESC.TXT beside HUBDESC.EXE.", "HUBDESC read-only result", MB_OK);
    return captured == 1 && matches == 1 ? 0 : 1;
}
