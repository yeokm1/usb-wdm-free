/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original process-local wait diagnostic. No USB/audio/device access.
 * HOSTWAIT.EXE [/quiet] [/honor] E:\absolute\REPORT.TXT
 * API contracts: docs/testing/2026-10-21-host-waits.md
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 256
static FILE *report;
static int writeFailed;
static HWND window;
static LARGE_INTEGER frequency,last;
/* Public processthreadsapi.h ABI (modern installed MinGW headers), absent
 * from VC6. Resolve documented APIs by name; never inspect DLL contents.
 * ProcessPowerThrottling=4, CURRENT_VERSION=1, IGNORE_TIMER_RESOLUTION=4.
 */
typedef struct { ULONG Version,ControlMask,StateMask; } HOST_POWER;
typedef BOOL (WINAPI *HOST_PROCESS_INFO)(HANDLE,int,LPVOID,DWORD);
typedef char HostPowerSizeCheck[sizeof(HOST_POWER)==12?1:-1];
#define HOST_POWER_CLASS 4
#define HOST_IGNORE_TIMER 4UL
static HOST_PROCESS_INFO powerGet,powerSet;
static HOST_POWER savedPower;
static int powerRestorePending;
static void Log(const char *format,...)
{
    va_list args;va_start(args,format);
    if(vfprintf(report,format,args)<0) writeFailed=1;
    va_end(args);
    if(fflush(report)) writeFailed=1;
}
static int PowerQuery(HOST_POWER *state,const char *phase)
{
    BOOL ok;
    ZeroMemory(state,sizeof(*state));state->Version=1;
    ok=powerGet(GetCurrentProcess(),HOST_POWER_CLASS,state,sizeof(*state));
    if(!ok) {Log("ERROR GetProcessInformation phase=%s error=%lu\n",phase,GetLastError());return 0;}
    Log("POWER phase=%s version=%lu control=%08lX state=%08lX\n",phase,state->Version,state->ControlMask,state->StateMask);
    if(state->Version!=1) {Log("ERROR unsupported power state version\n");return 0;}
    return 1;
}
static int PowerHonor(void)
{
    HMODULE kernel;
    HOST_POWER desired,actual;
    kernel=GetModuleHandleA("kernel32.dll");
    if(!kernel) {Log("ERROR kernel32 module unavailable\n");return 0;}
    powerGet=(HOST_PROCESS_INFO)GetProcAddress(kernel,"GetProcessInformation");
    powerSet=(HOST_PROCESS_INFO)GetProcAddress(kernel,"SetProcessInformation");
    if(!powerGet || !powerSet) {Log("ERROR documented process information APIs unavailable\n");return 0;}
    if(!PowerQuery(&savedPower,"snapshot"))return 0;
    desired=savedPower;
    desired.ControlMask|=HOST_IGNORE_TIMER;
    desired.StateMask&=~HOST_IGNORE_TIMER;
    /* All other policy bits retain exactly their captured values. */
    powerRestorePending=1;
    if(!powerSet(GetCurrentProcess(),HOST_POWER_CLASS,&desired,sizeof(desired))) {
        Log("ERROR SetProcessInformation honor error=%lu; original-state restore required\n",GetLastError());return 0;
    }
    if(!PowerQuery(&actual,"honor"))return 0;
    if(actual.ControlMask!=desired.ControlMask || actual.StateMask!=desired.StateMask) {
        Log("ERROR honor policy readback differs from requested state\n");return 0;
    }
    return 1;
}
static int PowerRestore(void)
{
    HOST_POWER actual;
    if(!powerRestorePending)return 1;
    if(!powerSet(GetCurrentProcess(),HOST_POWER_CLASS,&savedPower,sizeof(savedPower))) {
        Log("ERROR SetProcessInformation restore error=%lu; policy state unknown\n",GetLastError());return 0;
    }
    if(!PowerQuery(&actual,"restored"))return 0;
    if(actual.ControlMask!=savedPower.ControlMask || actual.StateMask!=savedPower.StateMask) {
        Log("ERROR restored policy readback differs from snapshot; state unknown\n");return 0;
    }
    powerRestorePending=0;
    Log("POWER_RESTORED snapshot matched; no execution-speed policy change requested\n");
    return 1;
}
static int Compare(const void *a,const void *b)
{double x=*(const double *)a,y=*(const double *)b;return x<y?-1:x>y?1:0;}
static int Clock(LARGE_INTEGER *value)
{
    if(!QueryPerformanceCounter(value) || value->QuadPart<last.QuadPart) {
        Log("ERROR performance counter failed or moved backward\n");return 0;
    }
    last=*value;return 1;
}
static int Messages(void)
{
    MSG message;
    while(PeekMessage(&message,NULL,0,0,PM_REMOVE)) {
        if(message.message==WM_QUIT)return 0;
        TranslateMessage(&message);DispatchMessage(&message);
    }
    return !window || IsWindow(window);
}
/* Returns 0 success, 1 user cancellation, 2 API/clock error, 3 report error. */
static int Measure(const char *phase,int eventWait,HANDLE event)
{
    double samples[SAMPLES],sorted[SAMPLES],total=0;
    LARGE_INTEGER before,after;
    DWORD status=WAIT_TIMEOUT;
    unsigned i;
    char title[160];
    sprintf(title,"Host wait diagnostic: %s / %s",phase,eventWait?"event1ms":"Sleep1ms");
    if(window)SetWindowTextA(window,title);
    for(i=0;i<SAMPLES;i++) {
        if(writeFailed)return 3;
        if(!Messages()) {Log("CANCELLED window closed\n");return 1;}
        if(!Clock(&before))return 2;
        if(eventWait)status=WaitForSingleObject(event,1);else Sleep(1);
        if(!Clock(&after))return 2;
        if(eventWait && status!=WAIT_TIMEOUT) {
            Log("ERROR unsignalled event returned=%lu GetLastError=%lu\n",status,GetLastError());return 2;
        }
        samples[i]=(double)(after.QuadPart-before.QuadPart)*1000.0/(double)frequency.QuadPart;
        total+=samples[i];
    }
    memcpy(sorted,samples,sizeof(sorted));qsort(sorted,SAMPLES,sizeof(double),Compare);
    Log("SUMMARY phase=%s wait=%s n=%u min_ms=%.6f median_ms=%.6f p90_ms=%.6f p99_ms=%.6f max_ms=%.6f mean_ms=%.6f\n",
        phase,eventWait?"event1":"sleep1",(unsigned)SAMPLES,sorted[0],
        (sorted[127]+sorted[128])/2,sorted[230],sorted[253],sorted[SAMPLES-1],total/SAMPLES);
    /* Raw values permit independent recomputation; no file I/O timed above. */
    for(i=0;i<SAMPLES;i++)Log("SAMPLE phase=%s wait=%s index=%u ms=%.6f\n",phase,eventWait?"event1":"sleep1",i,samples[i]);
    return writeFailed?3:0;
}
static int Phase(const char *name,HANDLE event)
{int status=Measure(name,0,event);return status?status:Measure(name,1,event);}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    int quiet=0,honor=0,arg=1,result=0,active=0;
    UINT period=0;
    TIMECAPS caps;
    MMRESULT status;
    HANDLE event=NULL;
    DWORD length;
    char path[MAX_PATH];
    (void)previous;(void)command;(void)show;
    while(arg<__argc) {
        if(!strcmp(__argv[arg],"/quiet") && !quiet)quiet=1;
        else if(!strcmp(__argv[arg],"/honor") && !honor)honor=1;
        else break;
        arg++;
    }
    if(__argc!=arg+1)return 2;
    length=GetFullPathNameA(__argv[arg],sizeof(path),path,NULL);
    if(!length || length>=sizeof(path) || (path[0]!='E' && path[0]!='e') ||
        path[1]!=':' || path[2]!='\\')return 2;
    report=fopen(path,"wt");if(!report)return 3;
    Log("HOSTWAIT process_id=%lu samples_per_wait=%u quiet=%d honor=%d\n",GetCurrentProcessId(),(unsigned)SAMPLES,quiet,honor);
    Log("Only this process: Sleep(1), manual-reset unsignalled event wait(1). No audio/USB.\n");
    Log("No QEMU/GLib timing inference; Windows11 visibility/occlusion may affect requested resolution.\n");
    Log("Exit0=measurement completed, not timer accuracy or audio quality PASS.\n");
    if(!QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0 || !QueryPerformanceCounter(&last)) {
        Log("ERROR QPC unavailable\n");result=2;goto done;
    }
    Log("qpc_frequency=%.0f\n",(double)frequency.QuadPart);
    status=timeGetDevCaps(&caps,sizeof(caps));
    Log("timeGetDevCaps=%u min_ms=%u max_ms=%u\n",status,status?0:caps.wPeriodMin,status?0:caps.wPeriodMax);
    if(status || !caps.wPeriodMin || caps.wPeriodMin>caps.wPeriodMax) {result=2;goto done;}
    event=CreateEvent(NULL,TRUE,FALSE,NULL);
    if(!event) {Log("ERROR CreateEvent=%lu\n",GetLastError());result=2;goto done;}
    if(!quiet) {
        window=CreateWindowA("STATIC","Host wait diagnostic running",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            120,120,700,120,NULL,NULL,instance,NULL);
        if(!window) {Log("ERROR CreateWindow=%lu\n",GetLastError());result=2;goto done;}
    }
    result=Phase("baseline",event);if(result)goto done;
    if(honor && !PowerHonor()) {result=2;goto done;}
    period=caps.wPeriodMin;status=timeBeginPeriod(period);
    if(status==TIMERR_NOERROR)active=1;
    Log("timeBeginPeriod period_ms=%u result=%u\n",period,status);
    if(status) {result=2;goto done;}
    result=Phase("requested",event);
    /* End even on cancellation, QPC, report or wait failure. */
    status=timeEndPeriod(period);
    if(status==TIMERR_NOERROR)active=0;
    Log("timeEndPeriod period_ms=%u result=%u\n",period,status);
    if(status)Log("ERROR timer request release unconfirmed; cleanup will retry\n");
    if(status && result!=3)result=2;
    if(result)goto done;
    if(!PowerRestore()) {result=2;goto done;}
    result=Phase("after_end",event);
    if(!result)Log("COMPLETE all6waitdistributions; paired timer request ended\n");
done:
    if(active) {
        status=timeEndPeriod(period);
        if(status==TIMERR_NOERROR)active=0;
        Log("cleanup timeEndPeriod period_ms=%u result=%u\n",period,status);
        if(status)Log("ERROR timer request state unknown at process exit; API restoration not verified\n");
        if(status && result!=3)result=2;
    }
    if(!PowerRestore() && result!=3)result=2;
    if(event && !CloseHandle(event)) {Log("ERROR CloseHandle event=%lu\n",GetLastError());if(!result)result=2;}
    if(window && IsWindow(window))DestroyWindow(window);
    Log("EXIT result=%d timer_request_release_unconfirmed=%d power_restore_unconfirmed=%d\n",result,active,powerRestorePending);
    if(fclose(report))writeFailed=1;
    return writeFailed?3:result;
}
