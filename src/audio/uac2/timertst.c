/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original Win32 clock diagnostic. No audio, USB or timer-resolution calls.
 * TIMERTST.EXE [/quiet] REPORT.TXT ; automatic exit after20 QPC seconds.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static FILE *report;
static int writeFailed;
static void Log(const char *format,...)
{
    va_list args;
    va_start(args,format);
    if(vfprintf(report,format,args)<0) writeFailed=1;
    va_end(args);
    if(fflush(report)) writeFailed=1;
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    LARGE_INTEGER frequency,start,now,last;
    DWORD firstTick,currentTick;
    double elapsed=0,next=1,gap,maxGap=0;
    unsigned long sleeps=0;
    int quiet=0,arg=1,result=0,closed=0;
    HWND window=NULL;
    MSG message;
    char title[160];
    UNREFERENCED_PARAMETER(instance);UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command);UNREFERENCED_PARAMETER(show);
    if(__argc>1 && !strcmp(__argv[1],"/quiet")) {quiet=1;arg++;}
    if(__argc!=arg+1) return 2;
    report=fopen(__argv[arg],"wt");
    if(!report) return 3;
    Log("TIMERTST: 20 QPC seconds; Sleep10 loop; no audio/USB/MM timer changes.\n");
    Log("Diagnostic only. Exit0 means completed, never audio or clock quality PASS.\n");
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 ||
       !QueryPerformanceCounter(&start)) {
        Log("ERROR: performance counter unavailable.\n");result=2;goto done;
    }
    last=start;firstTick=GetTickCount();currentTick=firstTick;
    Log("qpc_frequency=%.0f initial_GetTickCount=%lu\n",
        (double)frequency.QuadPart,(unsigned long)firstTick);
    if(!quiet) {
        window=CreateWindowA("STATIC","Clock diagnostic running (20 QPC seconds)",
            WS_OVERLAPPEDWINDOW|WS_VISIBLE,120,120,620,120,NULL,NULL,
            GetModuleHandle(NULL),NULL);
        if(!window) {Log("ERROR: diagnostic window creation failed.\n");result=2;goto done;}
    }
    while(elapsed<20 && !writeFailed) {
        while(PeekMessage(&message,NULL,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT) closed=1;
            TranslateMessage(&message);DispatchMessage(&message);
        }
        if(closed || (window && !IsWindow(window))) {
            Log("CANCELLED: diagnostic window closed.\n");result=1;break;
        }
        Sleep(10);sleeps++;
        if(!QueryPerformanceCounter(&now) || now.QuadPart<last.QuadPart) {
            Log("ERROR: performance counter failed or moved backward.\n");
            result=2;break;
        }
        currentTick=GetTickCount();
        gap=(double)(now.QuadPart-last.QuadPart)*1000/(double)frequency.QuadPart;
        if(gap>maxGap) maxGap=gap;
        elapsed=(double)(now.QuadPart-start.QuadPart)*1000/(double)frequency.QuadPart/1000;
        last=now;
        if(elapsed>=next || elapsed>=20) {
            Log("qpc_ms=%.3f tick_ms=%lu sleeps=%lu max_sleep_loop_ms=%.3f\n",
                elapsed*1000,(unsigned long)(DWORD)(currentTick-firstTick),sleeps,maxGap);
            sprintf(title,"Timer diagnostic: QPC %.0f ms, GetTickCount %lu ms",
                elapsed*1000,(unsigned long)(DWORD)(currentTick-firstTick));
            if(window) SetWindowTextA(window,title);
            next=elapsed+1;
        }
    }
    if(!result && !writeFailed) {
        Log("COMPLETE qpc_ms=%.3f tick_ms=%lu sleeps=%lu max_sleep_loop_ms=%.3f\n",
            elapsed*1000,(unsigned long)(DWORD)(currentTick-firstTick),sleeps,maxGap);
        Log("GetTickCount is unsigned wrap-safe delta; QPC scale is independently reported.\n");
    }
done:
    if(window && IsWindow(window)) DestroyWindow(window);
    if(fclose(report)) writeFailed=1;
    return writeFailed?3:result;
}
