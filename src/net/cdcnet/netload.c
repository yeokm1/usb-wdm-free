/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded Winsock load/removal test; no console or Internet peer.
 * IP MIB declarations come from the installed Platform SDK's iprtrmib.h.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iprtrmib.h>
#include <stdio.h>

#define WORKERS 64
static volatile LONG stop;
static LONG udpOk[WORKERS], udpError[WORKERS];
static LONG tcpBytes, tcpError, tcpMismatch;

static DWORD WINAPI Upload(PVOID parameter)
{
    unsigned index = (unsigned)parameter, i;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in peer;
    ULONG nonblocking = 1;
    char bytes[1400];
    if (s == INVALID_SOCKET) { udpError[index]++; return 0; }
    memset(&peer, 0, sizeof(peer)); peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr("10.0.2.2"); peer.sin_port = htons(44343);
    memset(bytes, (char)index, sizeof(bytes));
    ioctlsocket(s, FIONBIO, &nonblocking);
    for (i = 0; i < 512 && !InterlockedExchangeAdd((PLONG)&stop, 0); i++) {
        if (sendto(s, bytes, sizeof(bytes), 0, (struct sockaddr *)&peer, sizeof(peer)) == sizeof(bytes))
            InterlockedIncrement(&udpOk[index]);
        else InterlockedIncrement(&udpError[index]);
        Sleep(1);
    }
    closesocket(s); return 0;
}

static DWORD WINAPI Download(PVOID unused)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in peer;
    UCHAR bytes[4096];
    DWORD timeout = 3000;
    int count, i;
    UNREFERENCED_PARAMETER(unused);
    if (s == INVALID_SOCKET) { tcpError = WSAGetLastError(); return 0; }
    memset(&peer, 0, sizeof(peer)); peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr("10.0.2.2"); peer.sin_port = htons(44344);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    if (connect(s, (struct sockaddr *)&peer, sizeof(peer))) {
        tcpError = WSAGetLastError(); closesocket(s); return 0;
    }
    while (!InterlockedExchangeAdd((PLONG)&stop, 0)) {
        count = recv(s, (char *)bytes, sizeof(bytes), 0);
        if (count <= 0) { tcpError = count ? WSAGetLastError() : -1; break; }
        for (i = 0; i < count; i++)
            if (bytes[i] != (UCHAR)(((tcpBytes + i) * 37 + 11) & 255)) tcpMismatch++;
        InterlockedExchangeAdd(&tcpBytes, count);
    }
    closesocket(s); return 0;
}

static void Counters(FILE *log)
{
    typedef DWORD (WINAPI *GET_TABLE)(PMIB_IFTABLE, PULONG, BOOL);
    HMODULE dll = LoadLibraryA("iphlpapi.dll");
    GET_TABLE get;
    ULONG size = 0, i;
    PMIB_IFTABLE table;
    if (!dll) return;
    get = (GET_TABLE)GetProcAddress(dll, "GetIfTable");
    if (!get || get(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) { FreeLibrary(dll); return; }
    table = HeapAlloc(GetProcessHeap(), 0, size);
    if (table && !get(table, &size, FALSE)) {
        for (i = 0; i < table->dwNumEntries; i++) {
            MIB_IFROW *r = &table->table[i];
            if (r->dwType != 6) continue;
            fprintf(log, "IF index=%lu oper=%lu txPackets=%lu txErrors=%lu txDiscards=%lu rxPackets=%lu\n",
                r->dwIndex, r->dwOperStatus, r->dwOutUcastPkts + r->dwOutNUcastPkts,
                r->dwOutErrors, r->dwOutDiscards, r->dwInUcastPkts + r->dwInNUcastPkts);
        }
    }
    if (table) HeapFree(GetProcessHeap(), 0, table);
    FreeLibrary(dll);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    FILE *log = fopen("A:\\NETLOAD.TXT", "wt");
    HANDLE thread[WORKERS + 1];
    WSADATA wsa;
    DWORD i, waited, threads = 0;
    LONG sent = 0, errors = 0;
    UNREFERENCED_PARAMETER(instance); UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command); UNREFERENCED_PARAMETER(show);
    if (!log) return 2;
    fprintf(log, "Bounded removal/load experiment: 64 UDP workers, 512 datagrams each; TCP pattern\n");
    if (WSAStartup(MAKEWORD(2, 0), &wsa)) { fclose(log); return 2; }
    Counters(log); fflush(log);
    thread[threads] = CreateThread(NULL, 0, Download, NULL, 0, NULL);
    if (thread[threads]) threads++;
    Sleep(2000); /* establish verified receive traffic before transmit load */
    for (i = 0; i < WORKERS; i++) {
        thread[threads] = CreateThread(NULL, 0, Upload, (PVOID)i, 0, NULL);
        if (thread[threads]) threads++;
    }
    for (i = 0; i < 30; i++) {
        Sleep(1000);
        fprintf(log, "second=%lu TCP bytes=%ld error=%ld mismatch=%ld\n", i + 1, tcpBytes, tcpError, tcpMismatch);
        fflush(log);
    }
    InterlockedExchange((PLONG)&stop, 1);
    for (i = 0; i < threads; i++) {
        waited = WaitForSingleObject(thread[i], 10000);
        fprintf(log, "thread=%lu joined=%lu\n", i, waited);
        if (waited != WAIT_OBJECT_0) {
            fflush(log); fclose(log);
            MessageBoxA(NULL, "FAIL: worker failed to finish", "CDCNET load test", MB_OK);
            ExitProcess(3); /* never free Winsock/shared state under a worker */
        }
        CloseHandle(thread[i]);
    }
    for (i = 0; i < WORKERS; i++) { sent += udpOk[i]; errors += udpError[i]; }
    fprintf(log, "UDP accepted=%ld errors=%ld; TCP bytes=%ld error=%ld mismatch=%ld; workers=%lu joined\n",
        sent, errors, tcpBytes, tcpError, tcpMismatch, threads);
    Counters(log); WSACleanup();
    fprintf(log, "DONE (removal may intentionally interrupt traffic)\n"); fclose(log);
    MessageBoxA(NULL, tcpBytes && !tcpMismatch && threads == WORKERS + 1 ?
        "DONE: verified traffic, all 65 workers joined; see A:\\NETLOAD.TXT" :
        "FAIL: see A:\\NETLOAD.TXT", "CDCNET load test", MB_OK);
    return 0;
}
