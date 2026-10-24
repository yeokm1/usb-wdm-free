/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * NETTEST.EXE - original guest-side GUI test for CDCNET. It needs no
 * program on the host: everything it talks to is QEMU's own user-mode
 * network (slirp) at 10.0.2.2.
 *   1. ICMP echo to the gateway at sizes chosen for the USB packet
 *      boundaries: payloads 22, 86 and 1430 give 64-, 128- and 1472-byte
 *      Ethernet frames (exact 64-byte multiples: the ECM/NCM ZLP and the
 *      RNDIS one-byte terminator), 1472 gives a full 1514-byte frame.
 *   2. TFTP (RFC 1350, octet mode) download of CDCNET.BIN from QEMU's
 *      built-in TFTP server (-netdev user,tftp=<dir>), every byte checked
 *      against the deterministic pattern the staging script writes.
 * Results go to A:\NETTEST.TXT; a message box shows PASS or FAIL.
 * ICMP ABI facts: WDK 7.1 ipexport.h/icmpapi.h; Windows' icmp.dll is
 * called through its public API only.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FILE_BYTES (1024UL * 1024UL)
#define ROUNDS 2

typedef struct OPTIONS { UCHAR Ttl, Tos, Flags, Size; PUCHAR Data; } OPTIONS;
typedef struct REPLY {
    ULONG Address, Status, RoundTripTime;
    USHORT DataSize, Reserved;
    PVOID Data;
    OPTIONS Options;
} REPLY;
typedef HANDLE (WINAPI *CREATE_ICMP)(VOID);
typedef BOOL (WINAPI *CLOSE_ICMP)(HANDLE);
typedef DWORD (WINAPI *SEND_ICMP)(HANDLE, ULONG, PVOID, WORD, OPTIONS *, PVOID, DWORD, DWORD);

static UCHAR Pattern(ULONG i) { return (UCHAR)((i * 37 + 11 + (i >> 9)) & 255); }

static int Ping(FILE *log)
{
    static const WORD sizes[] = { 32, 22, 86, 1430, 1472, 0, 600 };
    HMODULE dll = LoadLibraryA("icmp.dll");
    CREATE_ICMP create;
    CLOSE_ICMP close;
    SEND_ICMP echo;
    HANDLE handle;
    static UCHAR data[1472], output[1472 + 256];
    int i, k, good = 0, total = 0;
    DWORD start;
    if (!dll) { fprintf(log, "icmp.dll unavailable: %lu\n", GetLastError()); return 0; }
    create = (CREATE_ICMP)GetProcAddress(dll, "IcmpCreateFile");
    close = (CLOSE_ICMP)GetProcAddress(dll, "IcmpCloseHandle");
    echo = (SEND_ICMP)GetProcAddress(dll, "IcmpSendEcho");
    if (!create || !close || !echo) { FreeLibrary(dll); return 0; }
    handle = create();
    if (handle == INVALID_HANDLE_VALUE) { FreeLibrary(dll); return 0; }
    for (i = 0; i < (int)sizeof(data); i++) data[i] = (UCHAR)(i ^ 0x5a);
    /* The first echo also waits for DHCP to finish: up to 90 s. */
    start = GetTickCount();
    while (GetTickCount() - start < 90000) {
        DWORD n = echo(handle, inet_addr("10.0.2.2"), data, 32, NULL, output, sizeof(output), 2000);
        if (n && ((REPLY *)output)->Status == 0) break;
        Sleep(1000);
    }
    fprintf(log, "first gateway reply after %lu ms\n", GetTickCount() - start);
    for (k = 0; k < (int)(sizeof(sizes) / sizeof(sizes[0])); k++) {
        for (i = 0; i < 3; i++) {
            DWORD n = echo(handle, inet_addr("10.0.2.2"), data, sizes[k], NULL, output, sizeof(output), 3000);
            REPLY *reply = (REPLY *)output;
            int ok = n && reply->Status == 0 && reply->DataSize == sizes[k] &&
                     (!sizes[k] || (reply->Data && memcmp(reply->Data, data, sizes[k]) == 0));
            total++; good += ok;
            fprintf(log, "ICMP payload %4u (frame %4u) #%d: %s status=%lu\n", sizes[k],
                    sizes[k] + 42 < 60 ? 60 : sizes[k] + 42, i + 1, ok ? "ok" : "FAIL",
                    n ? reply->Status : GetLastError());
        }
    }
    close(handle); FreeLibrary(dll);
    fprintf(log, "ICMP %d/%d replies verified\n", good, total);
    return good == total;
}

static int Tftp(FILE *log, int round)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in server, from;
    static UCHAR packet[600];
    char request[64];
    int length, fromLength, tries;
    ULONG received = 0, block = 0, retransmits = 0;
    DWORD timeout = 3000, start = GetTickCount();
    if (s == INVALID_SOCKET) { fprintf(log, "TFTP socket error %d\n", WSAGetLastError()); return 0; }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr("10.0.2.2");
    server.sin_port = htons(69);
    /* RRQ: opcode 1, file name, mode. */
    request[0] = 0; request[1] = 1;
    strcpy(request + 2, "CDCNET.BIN");
    strcpy(request + 2 + 11, "octet");
    length = 2 + 11 + 6;
    if (sendto(s, request, length, 0, (struct sockaddr *)&server, sizeof(server)) != length) goto fail;
    for (;;) {
        int n, i;
        fromLength = sizeof(from);
        for (tries = 0; tries < 6; tries++) {
            n = recvfrom(s, (char *)packet, sizeof(packet), 0, (struct sockaddr *)&from, &fromLength);
            if (n >= 4) break;
            /* Timeout: repeat the last request or acknowledgement. */
            retransmits++;
            if (!block) sendto(s, request, length, 0, (struct sockaddr *)&server, sizeof(server));
            else sendto(s, request, 4, 0, (struct sockaddr *)&server, sizeof(server));
        }
        if (tries == 6) { fprintf(log, "TFTP timeout at block %lu\n", block + 1); goto fail; }
        if (packet[1] == 5) { fprintf(log, "TFTP error %u: %s\n", packet[3], packet + 4); goto fail; }
        if (packet[1] != 3) continue;
        if (((ULONG)packet[2] << 8 | packet[3]) != ((block + 1) & 0xffff)) {
            /* A duplicate of the previous block: acknowledge it again. */
            if (block) sendto(s, request, 4, 0, (struct sockaddr *)&from, fromLength);
            continue;
        }
        n -= 4;
        for (i = 0; i < n; i++)
            if (packet[4 + i] != Pattern(received + i)) {
                fprintf(log, "TFTP data mismatch at byte %lu\n", received + i);
                goto fail;
            }
        received += n;
        block++;
        server = from;               /* the transfer port */
        request[0] = 0; request[1] = 4; request[2] = packet[2]; request[3] = packet[3];
        sendto(s, request, 4, 0, (struct sockaddr *)&from, fromLength);
        if (n < 512) break;
    }
    closesocket(s);
    fprintf(log, "TFTP round %d: %lu bytes verified in %lu blocks, %lu ms, %lu retransmits\n",
            round, received, block, GetTickCount() - start, retransmits);
    return received == FILE_BYTES;
fail:
    fprintf(log, "TFTP round %d failed after %lu bytes, Winsock error %d\n", round, received, WSAGetLastError());
    closesocket(s);
    return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WSADATA wsa;
    FILE *log = fopen("A:\\NETTEST.TXT", "wt");
    int ping = 0, transfer = 1, i, rounds = ROUNDS;
    char message[128];
    FILE *cfg = fopen("A:\\NETTEST.CFG", "rt");     /* optional: a round count */
    if (cfg) {
        if (fscanf(cfg, "%d", &rounds) != 1 || rounds < 1 || rounds > 1000) rounds = ROUNDS;
        fclose(cfg);
    }
    UNREFERENCED_PARAMETER(instance); UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command); UNREFERENCED_PARAMETER(show);
    if (!log) { MessageBoxA(NULL, "Cannot create A:\\NETTEST.TXT", "CDCNET test", MB_OK); return 2; }
    fprintf(log, "CDCNET guest test: gateway ICMP + QEMU TFTP, no host program\n");
    if (!WSAStartup(MAKEWORD(2, 0), &wsa)) {
        ping = Ping(log);
        fflush(log);
        for (i = 1; i <= rounds; i++) { transfer &= Tftp(log, i); fflush(log); }
        WSACleanup();
    } else transfer = 0;
    fprintf(log, "%s\n", ping && transfer ? "PASS" : "FAIL");
    fclose(log);
    sprintf(message, ping && transfer ? "PASS: ICMP sizes and %d x %lu TFTP bytes verified" :
            "FAIL: see A:\\NETTEST.TXT", rounds, FILE_BYTES);
    MessageBoxA(NULL, message, "CDCNET guest test", MB_OK);
    return ping && transfer ? 0 : 1;
}
