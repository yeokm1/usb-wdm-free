/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * PTRTEST.EXE - guest reading tool for absptr's legs (docs/design/
 * absptr.md, test plan). Samples the system pointer every 50 ms for N
 * seconds and writes one line per change (position or buttons) to COM1
 * and to a report file, so a test host that moves an absolute pointing
 * device to known places can read back where Windows put the cursor:
 *
 *   t=<ms> x=<x> y=<y> b=<L><M><R> scr=<width>x<height>
 *
 * b is three digits, 1 for a button held (left, middle, right, read
 * with GetAsyncKeyState). A "mark" line is written at the start and
 * every 5 s, so a reader can tell the tool is alive.
 *
 *   PTRTEST [seconds] [report file]     default 120 s, C:\PTRTEST.TXT
 *
 * Win32 only, 98 SE to 7. Original code. Reads no device directly.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE gCom = INVALID_HANDLE_VALUE;
static FILE *gOut;

static void Emit(const char *line)
{
    DWORD n;
    if (gCom != INVALID_HANDLE_VALUE)
        WriteFile(gCom, line, (DWORD)strlen(line), &n, NULL);
    if (gOut) {
        fputs(line, gOut);
        fflush(gOut);
    }
}

static int Down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char line[160], path[MAX_PATH];
    DWORD seconds = 120, start, now, lastMark = 0;
    POINT last, p;
    int lastB = -1, b;
    char *arg = cmd;
    (void)inst; (void)prev; (void)show;

    strcpy(path, "C:\\PTRTEST.TXT");
    while (*arg == ' ')
        arg++;
    if (*arg) {
        seconds = (DWORD)strtoul(arg, &arg, 10);
        while (*arg == ' ')
            arg++;
        if (*arg) {
            strncpy(path, arg, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
        }
    }
    if (seconds == 0)
        seconds = 120;
    gOut = fopen(path, "w");
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
    last.x = last.y = -1;
    start = GetTickCount();
    sprintf(line, "PTRTEST start seconds=%lu scr=%dx%d\r\n", seconds,
            GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    Emit(line);
    for (;;) {
        now = GetTickCount() - start;
        if (now >= seconds * 1000)
            break;
        GetCursorPos(&p);
        b = Down(VK_LBUTTON) * 100 + Down(VK_MBUTTON) * 10 + Down(VK_RBUTTON);
        if (p.x != last.x || p.y != last.y || b != lastB) {
            sprintf(line, "t=%lu x=%ld y=%ld b=%03d scr=%dx%d\r\n", now, p.x, p.y, b,
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
            Emit(line);
            last = p;
            lastB = b;
        }
        if (now - lastMark >= 5000) {
            sprintf(line, "mark t=%lu x=%ld y=%ld b=%03d\r\n", now, p.x, p.y, b);
            Emit(line);
            lastMark = now;
        }
        Sleep(50);
    }
    Emit("PTRTEST end\r\n");
    if (gOut)
        fclose(gOut);
    if (gCom != INVALID_HANDLE_VALUE)
        CloseHandle(gCom);
    return 0;
}
