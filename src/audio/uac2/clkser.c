/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original guest clock reporter for an external reference (CLKSER).
 * CLKSER.EXE SECONDS PERIOD_MS REPORT.TXT
 * Writes one line per period to COM1 (115200 8N1) with the guest's QPC and
 * GetTickCount; a host receiver stamps each line on arrival with its own
 * monotonic clock. No audio, USB or timer-resolution calls. The original
 * COM1 state and timeouts are restored before exit. Exit 0 means the run
 * completed, never that any clock is good.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *report;
static HANDLE port = INVALID_HANDLE_VALUE;
static DCB saved;
static COMMTIMEOUTS savedTimeouts;
static int haveSaved, failed;

static void Note(const char *text)
{
    if (report && (fputs(text, report) < 0 || fflush(report))) failed = 1;
}
/* One whole line, or failure: a short or failed write ends the run. */
static int Send(const char *line)
{
    DWORD n = 0, len = (DWORD)strlen(line);
    if (!WriteFile(port, line, len, &n, NULL) || n != len) {
        char text[96];
        sprintf(text, "ERROR: COM1 write failed (%lu of %lu bytes, error %lu)\n",
            (unsigned long)n, (unsigned long)len, (unsigned long)GetLastError());
        Note(text); failed = 1; return 0;
    }
    Note(line);
    return 1;
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    LARGE_INTEGER freq, start, now, last;
    DWORD tick0, tick, period;
    double seconds, elapsed = 0;
    unsigned long seq = 0, frozen = 0;
    DCB dcb; COMMTIMEOUTS timeouts;
    char line[160];
    int result = 0;
    UNREFERENCED_PARAMETER(instance); UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command); UNREFERENCED_PARAMETER(show);
    if (__argc != 4) return 2;
    seconds = atof(__argv[1]); period = (DWORD)atol(__argv[2]);
    if (seconds <= 0 || seconds > 600 || period < 10 || period > 1000) return 2;
    report = fopen(__argv[3], "wt");
    if (!report) return 3;
    Note("CLKSER: guest QPC/GetTickCount lines to COM1 for an external clock.\n");
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        Note("ERROR: performance counter unavailable.\n"); result = 2; goto done;
    }
    port = CreateFileA("\\\\.\\COM1", GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_EXISTING, 0, NULL);
    if (port == INVALID_HANDLE_VALUE) {
        sprintf(line, "ERROR: COM1 open failed, error %lu\n", (unsigned long)GetLastError());
        Note(line); result = 2; goto done;
    }
    saved.DCBlength = sizeof(saved);
    if (!GetCommState(port, &saved) || !GetCommTimeouts(port, &savedTimeouts)) {
        Note("ERROR: COM1 state query failed.\n"); result = 2; goto done;
    }
    haveSaved = 1;
    dcb = saved;
    dcb.BaudRate = CBR_115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT; dcb.fBinary = TRUE; dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE;
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.WriteTotalTimeoutConstant = 2000; /* a stuck port ends the run */
    if (!SetCommState(port, &dcb) || !SetCommTimeouts(port, &timeouts)) {
        Note("ERROR: COM1 configuration failed.\n"); result = 2; goto done;
    }
    QueryPerformanceCounter(&start); last = start; tick0 = GetTickCount();
    sprintf(line, "BEGIN freq %.0f qpc %.0f tick %lu period %lu seconds %.3f\n",
        (double)freq.QuadPart, (double)start.QuadPart, (unsigned long)tick0,
        (unsigned long)period, seconds);
    if (!Send(line)) { result = 2; goto done; }
    while (elapsed < seconds) {
        Sleep(period);
        if (!QueryPerformanceCounter(&now) || now.QuadPart < last.QuadPart) {
            Note("ERROR: performance counter failed or moved backward.\n"); result = 2; break;
        }
        /* Frozen-clock watchdog: QPC that stops across many sleeps. */
        if (now.QuadPart == last.QuadPart) {
            if (++frozen > 100) { Note("ERROR: QPC frozen.\n"); result = 2; break; }
        } else frozen = 0;
        tick = GetTickCount();
        last = now;
        elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        sprintf(line, "S %lu qpc %.0f tick %lu\n", ++seq, (double)now.QuadPart,
            (unsigned long)(DWORD)(tick - tick0));
        if (!Send(line)) { result = 2; break; }
    }
    if (!result) {
        sprintf(line, "END samples %lu qpc_ms %.3f tick_ms %lu\n", seq, elapsed * 1000,
            (unsigned long)(DWORD)(GetTickCount() - tick0));
        if (!Send(line)) result = 2;
    }
done:
    if (port != INVALID_HANDLE_VALUE) {
        if (haveSaved) {
            DCB check; check.DCBlength = sizeof(check);
            if (!SetCommState(port, &saved) || !SetCommTimeouts(port, &savedTimeouts) ||
                !GetCommState(port, &check) || check.BaudRate != saved.BaudRate ||
                check.ByteSize != saved.ByteSize || check.Parity != saved.Parity ||
                check.StopBits != saved.StopBits) {
                Note("ERROR: COM1 state restore/readback failed.\n"); failed = 1;
            } else Note("COM1 state restored and read back.\n");
        }
        CloseHandle(port);
    }
    if (report && fclose(report)) failed = 1;
    return failed ? 3 : result;
}
