/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * NETPERF.EXE - original guest-side TCP throughput peer for the USB
 * Ethernet drivers on real hardware (98 SE to 10). The guest is the
 * server, so a peer behind a firewall that refuses inbound connections
 * can still test: the peer connects out (netperf.py) and asks for one
 * of these, a line of text per connection:
 *   SEND <seconds> [<block>]   the guest sends the pattern stream
 *   RECV <seconds>             the guest receives and checks it, then
 *                              answers one RESULT line
 *   CPU <seconds>              the guest's CPU load over that time
 *   LAST                       the results of the last SEND and RECV
 * Every connection runs on its own thread, so a SEND and a RECV at once
 * load both directions. The stream is a fixed 65521-byte table
 * (t[k] = (k * 37 + 11 + (k >> 9)) & 255) repeated, so either side
 * checks it with one compare per block. CPU load: GetSystemTimes where
 * kernel32 has it (XP SP1 and later), else on 98 SE/ME the performance
 * statistic KERNEL\CPUUsage under HKEY_DYN_DATA; elsewhere (2000) it
 * reads "cpu=n/a". A send blocked for 5 s ends the run. Usage:
 *   NETPERF [port [sockbuf]]   (default 51130 and 262144; a sockbuf of 0
 *   leaves the stack's default socket buffers); log in C:\NETPERF.TXT
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIOD 65521UL
#define BLOCK_MAX 65536UL

static UCHAR table[PERIOD + BLOCK_MAX];
static CRITICAL_SECTION logLock;
static FILE *logFile;
static char lastSend[256] = "none", lastRecv[256] = "none";

typedef BOOL (WINAPI *GET_SYSTEM_TIMES)(LPFILETIME, LPFILETIME, LPFILETIME);
static GET_SYSTEM_TIMES getSystemTimes;
static BOOL nineCpu;              /* 98 SE/ME: the HKEY_DYN_DATA statistic */
#define CPU_NONE 0xffffffffUL

static void Log(const char *format, ...)
{
    va_list args;
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    va_start(args, format);
    _vsnprintf(line, sizeof(line) - 1, format, args);
    line[sizeof(line) - 1] = 0;
    va_end(args);
    EnterCriticalSection(&logLock);
    printf("%02u:%02u:%02u %s\n", t.wHour, t.wMinute, t.wSecond, line);
    if (logFile) {
        fprintf(logFile, "%02u:%02u:%02u %s\n", t.wHour, t.wMinute, t.wSecond, line);
        fflush(logFile);
    }
    LeaveCriticalSection(&logLock);
}

/* ---- CPU load ---- */

typedef struct CPU_MARK { ULONGLONG Idle, Total; DWORD Tick; } CPU_MARK;
static BOOL nineStarted;

static ULONGLONG Ft(const FILETIME *f) { return ((ULONGLONG)f->dwHighDateTime << 32) | f->dwLowDateTime; }

/* 98 SE/ME: reading StartStat starts the statistic; each StatData read
 * gives the load since the previous read, in percent. */
static DWORD NineCpu(void)
{
    HKEY key;
    DWORD value = 0, size = sizeof(value), type;
    const char *name = "KERNEL\\CPUUsage";
    if (!nineStarted) {
        if (RegOpenKeyExA(HKEY_DYN_DATA, "PerfStats\\StartStat", 0, KEY_READ, &key) == ERROR_SUCCESS) {
            RegQueryValueExA(key, name, NULL, &type, (LPBYTE)&value, &size);
            RegCloseKey(key);
        }
        nineStarted = TRUE;
        size = sizeof(value);
    }
    value = 0;
    if (RegOpenKeyExA(HKEY_DYN_DATA, "PerfStats\\StatData", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExA(key, name, NULL, &type, (LPBYTE)&value, &size);
        RegCloseKey(key);
    }
    return value;
}

static void CpuMark(CPU_MARK *m)
{
    FILETIME idle, kernel, user;
    m->Tick = GetTickCount();
    m->Idle = m->Total = 0;
    if (getSystemTimes && getSystemTimes(&idle, &kernel, &user)) {
        m->Idle = Ft(&idle);
        m->Total = Ft(&kernel) + Ft(&user);  /* kernel time includes idle */
    }
}

/* Load in tenths of a percent between two marks (NT); on 9x the caller
 * samples NineCpu once a second instead. */
static DWORD CpuBusy(const CPU_MARK *a, const CPU_MARK *b)
{
    ULONGLONG total = b->Total - a->Total, idle = b->Idle - a->Idle;
    if (!total) return 0;
    return (DWORD)(((total - idle) * 1000) / total);
}

/* ---- the sampler: 9x load once a second, averaged over a run ---- */

static volatile LONG nineSum, nineCount;

static DWORD WINAPI Sampler(PVOID unused)
{
    UNREFERENCED_PARAMETER(unused);
    for (;;) {
        DWORD v;
        Sleep(1000);
        v = NineCpu();
        InterlockedExchangeAdd((PLONG)&nineSum, (LONG)v);
        InterlockedIncrement((PLONG)&nineCount);
    }
}

typedef struct LOAD { CPU_MARK Mark; LONG Sum, Count; } LOAD;

static void LoadStart(LOAD *l)
{
    CpuMark(&l->Mark);
    l->Sum = InterlockedExchangeAdd((PLONG)&nineSum, 0);
    l->Count = InterlockedExchangeAdd((PLONG)&nineCount, 0);
}

/* Tenths of a percent. */
static DWORD LoadEnd(const LOAD *l)
{
    CPU_MARK now;
    LONG sum, count;
    if (!getSystemTimes && !nineCpu) return CPU_NONE;
    if (getSystemTimes) {
        CpuMark(&now);
        return CpuBusy(&l->Mark, &now);
    }
    sum = InterlockedExchangeAdd((PLONG)&nineSum, 0) - l->Sum;
    count = InterlockedExchangeAdd((PLONG)&nineCount, 0) - l->Count;
    return count > 0 ? (DWORD)(sum * 10 / count) : 0;
}

/* "12.3" or "n/a". */
static const char *CpuText(DWORD busy, char *text)
{
    if (busy == CPU_NONE) return "n/a";
    sprintf(text, "%lu.%lu", busy / 10, busy % 10);
    return text;
}

/* ---- connections ---- */

static int ReadLine(SOCKET s, char *line, int size)
{
    int n = 0;
    while (n < size - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') line[n++] = c;
    }
    line[n] = 0;
    return n;
}

static void SendText(SOCKET s, const char *text)
{
    int left = (int)strlen(text);
    while (left > 0) {
        int r = send(s, text, left, 0);
        if (r <= 0) return;
        text += r; left -= r;
    }
}

static void Send(SOCKET s, DWORD seconds, DWORD block, const char *peer)
{
    ULONG offset = 0, bytes = 0, high = 0;
    DWORD start = GetTickCount(), end, ms, err = 0, busy, limit = 5000;
    char cpu[16];
    LOAD load;
    if (block < 512 || block > BLOCK_MAX) block = 32768;
    /* a peer that stops reading ends the run within 5 s */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&limit, sizeof(limit));
    LoadStart(&load);
    for (;;) {
        int r;
        if (GetTickCount() - start >= seconds * 1000) break;
        r = send(s, (const char *)table + offset, (int)block, 0);
        if (r <= 0) { err = WSAGetLastError(); break; }
        offset = (offset + (ULONG)r) % PERIOD;
        if (bytes + (ULONG)r < bytes) high++;
        bytes += (ULONG)r;
    }
    end = GetTickCount();
    ms = end - start;
    busy = LoadEnd(&load);
    shutdown(s, 1);
    _snprintf(lastSend, sizeof(lastSend) - 1,
              "SEND peer=%s bytes=%lu wraps=%lu ms=%lu kbps=%lu cpu=%s err=%lu",
              peer, bytes, high, ms,
              ms ? (DWORD)(((double)high * 4294967296.0 + bytes) * 8.0 / ms) : 0,
              CpuText(busy, cpu), err);
    Log("%s", lastSend);
}

static void Receive(SOCKET s, DWORD seconds, DWORD rcvbuf, const char *peer)
{
    UCHAR *buffer = malloc(BLOCK_MAX);
    ULONG offset = 0, bytes = 0, high = 0, mismatch = 0, firstBad = 0xffffffffUL;
    DWORD start = GetTickCount(), ms, err = 0, busy;
    char result[300], cpu[16];
    LOAD load;
    UNREFERENCED_PARAMETER(seconds);
    UNREFERENCED_PARAMETER(rcvbuf);
    if (!buffer) { SendText(s, "RESULT no memory\n"); return; }
    LoadStart(&load);
    for (;;) {
        int r = recv(s, (char *)buffer, BLOCK_MAX, 0);
        if (r <= 0) { if (r < 0) err = WSAGetLastError(); break; }
        if (memcmp(buffer, table + offset, (size_t)r)) {
            int i;
            for (i = 0; i < r; i++)
                if (buffer[i] != table[(offset + (ULONG)i) % PERIOD]) {
                    if (firstBad == 0xffffffffUL) firstBad = bytes + (ULONG)i;
                    mismatch++;
                }
        }
        offset = (offset + (ULONG)r) % PERIOD;
        if (bytes + (ULONG)r < bytes) high++;
        bytes += (ULONG)r;
    }
    ms = GetTickCount() - start;
    busy = LoadEnd(&load);
    free(buffer);
    _snprintf(result, sizeof(result) - 1,
              "RECV peer=%s bytes=%lu wraps=%lu ms=%lu kbps=%lu cpu=%s mismatch=%lu firstbad=%ld err=%lu",
              peer, bytes, high, ms,
              ms ? (DWORD)(((double)high * 4294967296.0 + bytes) * 8.0 / ms) : 0,
              CpuText(busy, cpu), mismatch, (long)firstBad, err);
    result[sizeof(result) - 1] = 0;
    strncpy(lastRecv, result, sizeof(lastRecv) - 1);
    Log("%s", result);
    strcat(result, "\n");
    SendText(s, result);
}

typedef struct LINK_CONN { SOCKET S; char Peer[32]; } LINK_CONN;

static DWORD WINAPI Connection(PVOID parameter)
{
    LINK_CONN *c = parameter;
    char line[128], verb[16], text[300], cpu[16];
    DWORD a = 0, b = 0;
    int on = 1;
    setsockopt(c->S, IPPROTO_TCP, TCP_NODELAY, (char *)&on, sizeof(on));
    if (ReadLine(c->S, line, sizeof(line)) < 0) goto done;
    verb[0] = 0;
    sscanf(line, "%15s %lu %lu", verb, &a, &b);
    Log("%s: %s", c->Peer, line);
    if (!strcmp(verb, "SEND")) {
        Send(c->S, a ? a : 10, b, c->Peer);
    } else if (!strcmp(verb, "RECV")) {
        Receive(c->S, a, b, c->Peer);
    } else if (!strcmp(verb, "CPU")) {
        LOAD load;
        DWORD busy;
        LoadStart(&load);
        Sleep((a ? a : 10) * 1000);
        busy = LoadEnd(&load);
        _snprintf(text, sizeof(text) - 1, "CPU seconds=%lu busy=%s\n", a ? a : 10, CpuText(busy, cpu));
        text[sizeof(text) - 1] = 0;
        Log("%s", text);
        SendText(c->S, text);
    } else if (!strcmp(verb, "LAST")) {
        _snprintf(text, sizeof(text) - 1, "%s\n%s\n", lastSend, lastRecv);
        text[sizeof(text) - 1] = 0;
        SendText(c->S, text);
    } else {
        SendText(c->S, "ERROR unknown verb\n");
    }
done:
    closesocket(c->S);
    free(c);
    return 0;
}

int main(int argc, char **argv)
{
    WSADATA wsa;
    SOCKET listener;
    struct sockaddr_in at;
    unsigned port = argc > 1 ? (unsigned)atoi(argv[1]) : 51130;
    ULONG k;
    int size = argc > 2 ? atoi(argv[2]) : 256 * 1024, on = 1;
    char host[128];
    HOSTENT *he;

    InitializeCriticalSection(&logLock);
    logFile = fopen("C:\\NETPERF.TXT", "at");
    for (k = 0; k < PERIOD + BLOCK_MAX; k++) table[k] = (UCHAR)((k % PERIOD) * 37 + 11 + ((k % PERIOD) >> 9));
    getSystemTimes = (GET_SYSTEM_TIMES)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemTimes");
    nineCpu = !getSystemTimes && (GetVersion() & 0x80000000UL) != 0;
    if (nineCpu) { NineCpu(); CreateThread(NULL, 0, Sampler, NULL, 0, NULL); }
    if (WSAStartup(MAKEWORD(2, 0), &wsa)) { Log("WSAStartup failed"); return 2; }
    if (!gethostname(host, sizeof(host)) && (he = gethostbyname(host)) != NULL) {
        int i;
        for (i = 0; he->h_addr_list[i]; i++)
            Log("address %s", inet_ntoa(*(struct in_addr *)he->h_addr_list[i]));
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) { Log("socket failed %d", WSAGetLastError()); return 2; }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    /* Large socket buffers, inherited by accepted sockets: the window
     * then follows the link, not the stack's default. */
    if (size > 0) {
        setsockopt(listener, SOL_SOCKET, SO_RCVBUF, (char *)&size, sizeof(size));
        setsockopt(listener, SOL_SOCKET, SO_SNDBUF, (char *)&size, sizeof(size));
    }
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_port = htons((u_short)port);
    if (bind(listener, (struct sockaddr *)&at, sizeof(at)) || listen(listener, 8)) {
        Log("bind/listen on %u failed %d", port, WSAGetLastError());
        return 2;
    }
    Log("NETPERF listening on port %u, socket buffers %d (cpu by %s)", port, size,
        getSystemTimes ? "GetSystemTimes" : nineCpu ? "PerfStats" : "nothing");
    for (;;) {
        struct sockaddr_in from;
        int fromLen = sizeof(from);
        SOCKET s = accept(listener, (struct sockaddr *)&from, &fromLen);
        LINK_CONN *c;
        HANDLE t;
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        c = malloc(sizeof(*c));
        if (!c) { closesocket(s); continue; }
        c->S = s;
        _snprintf(c->Peer, sizeof(c->Peer) - 1, "%s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        c->Peer[sizeof(c->Peer) - 1] = 0;
        t = CreateThread(NULL, 0, Connection, c, 0, NULL);
        if (t) CloseHandle(t); else { closesocket(s); free(c); }
    }
}
