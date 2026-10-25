/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original exact-PID, temporary timer-request-policy diagnostic.
 * HOSTPOLICY.EXE PID E:\absolute\REPORT.TXT
 * No timer-resolution calls, USB, injection or persistent/global changes.
 * API provenance and operator lifecycle: docs/testing/2026-10-21-host-waits.md
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Public Win32 ABI, checked against installed processthreadsapi.h. */
typedef struct {ULONG Version,ControlMask,StateMask;} POLICY_STATE;
typedef BOOL (WINAPI *POLICY_API)(HANDLE,int,LPVOID,DWORD);
typedef char PolicyAbiSize[sizeof(POLICY_STATE)==12?1:-1];
#define POLICY_CLASS 4
#define POLICY_IGNORE_TIMER 4UL
static FILE *policyReport;
static int reportFailure,pendingRestore;
static HANDLE target;
static POLICY_API policyGet,policySet;
static POLICY_STATE original;

static void Log(const char *format,...)
{
    va_list args;
    va_start(args,format);if(vfprintf(policyReport,format,args)<0)reportFailure=1;va_end(args);
    if(fflush(policyReport))reportFailure=1;
    va_start(args,format);vprintf(format,args);va_end(args);fflush(stdout);
}
static int Alive(void)
{
    DWORD status=WaitForSingleObject(target,0);
    if(status==WAIT_TIMEOUT)return 1;
    Log("ERROR target not live wait=%lu error=%lu\n",status,status==WAIT_FAILED?GetLastError():0);
    return 0;
}
static int Query(POLICY_STATE *state,const char *stage)
{
    ZeroMemory(state,sizeof(*state));state->Version=1;
    if(!policyGet(target,POLICY_CLASS,state,sizeof(*state))) {
        Log("ERROR GetProcessInformation stage=%s error=%lu\n",stage,GetLastError());return 0;
    }
    Log("POLICY stage=%s version=%lu control=%08lX state=%08lX\n",stage,state->Version,state->ControlMask,state->StateMask);
    if(state->Version!=1) {Log("ERROR unsupported state version\n");return 0;}
    return 1;
}
static int Resolve(void)
{
    HMODULE kernel=GetModuleHandleA("kernel32.dll");
    if(!kernel) {Log("ERROR kernel32 module unavailable\n");return 0;}
    policyGet=(POLICY_API)GetProcAddress(kernel,"GetProcessInformation");
    policySet=(POLICY_API)GetProcAddress(kernel,"SetProcessInformation");
    if(!policyGet || !policySet) {Log("ERROR required documented APIs unavailable; no policy applied\n");return 0;}
    return 1;
}
static int Apply(void)
{
    POLICY_STATE desired,actual;
    if(!Alive() || !Query(&original,"snapshot") || reportFailure)return 0;
    desired=original;
    desired.ControlMask|=POLICY_IGNORE_TIMER;
    desired.StateMask&=~POLICY_IGNORE_TIMER;
    /* Same full state apart from bit4; no execution-speed policy request. */
    pendingRestore=1;
    if(!policySet(target,POLICY_CLASS,&desired,sizeof(desired))) {
        Log("ERROR SetProcessInformation honor error=%lu\n",GetLastError());return 0;
    }
    if(!Query(&actual,"honor"))return 0;
    if(actual.ControlMask!=desired.ControlMask || actual.StateMask!=desired.StateMask) {
        Log("ERROR honor readback mismatch\n");return 0;
    }
    if(reportFailure)return 0;
    Log("HONOR_ACTIVE: only timer-ignore bit changed; send Enter after audio stops, BEFORE target exits\n");
    return !reportFailure;
}
static int Restore(void)
{
    POLICY_STATE actual;
    if(!pendingRestore)return 1;
    if(!Alive()) {Log("ERROR target exited; original policy cannot be verified\n");return 0;}
    if(!policySet(target,POLICY_CLASS,&original,sizeof(original))) {
        Log("ERROR SetProcessInformation restore error=%lu; state unknown\n",GetLastError());return 0;
    }
    if(!Query(&actual,"restored"))return 0;
    if(actual.ControlMask!=original.ControlMask || actual.StateMask!=original.StateMask) {
        Log("ERROR original-state restore readback mismatch; state unknown\n");return 0;
    }
    pendingRestore=0;
    Log("RESTORED: original policy readback matched; target still live\n");
    return 1;
}
#ifndef HOSTPOLICY_TEST
int main(int argc,char **argv)
{
    DWORD pid,length;
    unsigned long parsed;
    int result=0,c;
    char path[MAX_PATH],*end;
    if(argc!=3)return 2;
    if(!argv[1][0] || strspn(argv[1],"0123456789")!=strlen(argv[1]))return 2;
    parsed=strtoul(argv[1],&end,10);
    if(*end || !parsed || parsed==0xffffffffUL)return 2;
    pid=(DWORD)parsed;
    length=GetFullPathNameA(argv[2],sizeof(path),path,NULL);
    if(!length || length>=sizeof(path) || (path[0]!='E' && path[0]!='e') || path[1]!=':' || path[2]!='\\')return 2;
    policyReport=fopen(path,"wt");if(!policyReport)return 4;
    Log("HOSTPOLICY target_pid=%lu; operator must verify exact executable identity separately\n",pid);
    if(reportFailure) {result=4;goto done;}
    target=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_SET_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if(!target) {Log("ERROR OpenProcess=%lu\n",GetLastError());result=3;goto done;}
    if(!Resolve() || !Apply()) {result=3;goto done;}
    /* Explicit operator-controlled lifetime: no normal EOF path skips restore. */
    do {c=getchar();} while(c!=EOF && c!='\n' && c!='\r');
    if(c==EOF) {Log("ERROR stdin ended before Enter; restoring now\n");result=1;}
done:
    if(!Restore())result=3;
    if(pendingRestore) {Log("RETRY original-state restoration\n");if(!Restore())result=3;}
    Log("EXIT result=%d policy_restore_unconfirmed=%d\n",result,pendingRestore);
    if(target && !CloseHandle(target)) {Log("ERROR CloseHandle=%lu\n",GetLastError());result=3;}
    if(fclose(policyReport))reportFailure=1;
    return reportFailure?4:result;
}
#endif
