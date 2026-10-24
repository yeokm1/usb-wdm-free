/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Actual hostpolicy.c core, with all policy/target APIs mocked.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static HMODULE WINAPI MModule(LPCSTR);
static FARPROC WINAPI MProc(HMODULE,LPCSTR);
static DWORD WINAPI MWait(HANDLE,DWORD);
static DWORD WINAPI MError(void);
#define GetModuleHandleA MModule
#define GetProcAddress MProc
#define WaitForSingleObject MWait
#define GetLastError MError
#define HOSTPOLICY_TEST
#include "hostpolicy.c"
enum { OK,NO_MODULE,NO_GET,NO_SET,GET_INITIAL,GET_HONOR,GET_RESTORE,
       SET_HONOR,SET_RESTORE_ONCE,SET_RESTORE_ALWAYS,MISMATCH_HONOR,
       MISMATCH_RESTORE,REPORT_BEFORE,REPORT_AFTER,GONE };
static int mode,getCalls,setCalls,checks,failures,restoreFailures;
static POLICY_STATE current;
#define CHECK(x) do {++checks;if(!(x)){printf("TESTFAIL line%d %s\n",__LINE__,#x);++failures;}} while(0)
static DWORD WINAPI MError(void) {return 1234;}
static DWORD WINAPI MWait(HANDLE h,DWORD delay)
{CHECK(h==target);CHECK(delay==0);return mode==GONE?WAIT_OBJECT_0:WAIT_TIMEOUT;}
static BOOL WINAPI MGet(HANDLE h,int klass,LPVOID data,DWORD bytes)
{
    POLICY_STATE *s=(POLICY_STATE *)data;
    CHECK(h==target);CHECK(klass==4);CHECK(bytes==12);CHECK(s->Version==1);++getCalls;
    if((mode==GET_INITIAL && getCalls==1)||(mode==GET_HONOR && getCalls==2)||
       (mode==GET_RESTORE && getCalls>=3))return FALSE;
    *s=current;
    if((mode==MISMATCH_HONOR && getCalls==2)||(mode==MISMATCH_RESTORE && getCalls>=3))s->StateMask^=4;
    return TRUE;
}
static BOOL WINAPI MSet(HANDLE h,int klass,LPVOID data,DWORD bytes)
{
    POLICY_STATE *s=(POLICY_STATE *)data;
    CHECK(h==target);CHECK(klass==4);CHECK(bytes==12);CHECK(s->Version==1);++setCalls;
    /* No execution-speed or other unrelated bit may change. */
    CHECK((s->ControlMask&~4UL)==(current.ControlMask&~4UL));
    CHECK((s->StateMask&~4UL)==(current.StateMask&~4UL));
    if(mode==SET_HONOR && setCalls==1)return FALSE;
    if(mode==SET_RESTORE_ALWAYS && setCalls>=2)return FALSE;
    if(mode==SET_RESTORE_ONCE && setCalls==2) {++restoreFailures;return FALSE;}
    current=*s;
    if(mode==REPORT_AFTER && setCalls==1)reportFailure=1;
    return TRUE;
}
static HMODULE WINAPI MModule(LPCSTR name)
{CHECK(!strcmp(name,"kernel32.dll"));return mode==NO_MODULE?NULL:(HMODULE)7;}
static FARPROC WINAPI MProc(HMODULE module,LPCSTR name)
{
    CHECK(module==(HMODULE)7);
    if(!strcmp(name,"GetProcessInformation"))return mode==NO_GET?NULL:(FARPROC)MGet;
    CHECK(!strcmp(name,"SetProcessInformation"));return mode==NO_SET?NULL:(FARPROC)MSet;
}
static void BeginCase(int which,ULONG control,ULONG state)
{
    mode=which;getCalls=setCalls=restoreFailures=0;pendingRestore=reportFailure=0;
    policyGet=policySet=NULL;target=(HANDLE)9;
    current.Version=1;current.ControlMask=control;current.StateMask=state;
    policyReport=fopen("mock.txt","wt");CHECK(policyReport!=NULL);
    if(!policyReport)exit(2);
}
static void EndCase(void) {CHECK(!fclose(policyReport));}
int main(void)
{
    int i,applied,expected;
    ULONG c,s;
    /* Preserve all combinations of execution-speed and timer control/state. */
    for(c=0;c<8;c++)for(s=0;s<8;s++) {
        BeginCase(OK,c,s);CHECK(Resolve());CHECK(Apply());
        CHECK(current.ControlMask==(c|4));CHECK(current.StateMask==(s&~4UL));
        CHECK(pendingRestore);CHECK(Restore());CHECK(!pendingRestore);
        CHECK(current.ControlMask==c);CHECK(current.StateMask==s);
        CHECK(setCalls==2);CHECK(Restore());CHECK(setCalls==2);EndCase();
    }
    for(i=NO_MODULE;i<=NO_SET;i++) {
        BeginCase(i,1,1);CHECK(!Resolve());CHECK(!setCalls);CHECK(Restore());EndCase();
    }
    for(i=GET_INITIAL;i<=GONE;i++) {
        BeginCase(i,1,1);CHECK(Resolve());
        if(i==REPORT_BEFORE)reportFailure=1;
        applied=Apply();
        expected=(i==GET_RESTORE || i==SET_RESTORE_ONCE || i==SET_RESTORE_ALWAYS || i==MISMATCH_RESTORE);
        CHECK(applied==expected);
        if(i==GET_INITIAL || i==REPORT_BEFORE || i==GONE) {
            CHECK(setCalls==0);CHECK(!pendingRestore);CHECK(Restore());
        } else {
            int restored=Restore();
            if(i==GET_RESTORE || i==SET_RESTORE_ONCE || i==SET_RESTORE_ALWAYS || i==MISMATCH_RESTORE) {
                CHECK(!restored);CHECK(pendingRestore);
                mode=OK;CHECK(Restore());CHECK(!pendingRestore);
            } else {CHECK(restored);CHECK(!pendingRestore);}
            CHECK(current.ControlMask==1 && current.StateMask==1);
        }
        EndCase();
    }
    BeginCase(OK,1,1);CHECK(Resolve());CHECK(Apply());mode=GONE;
    CHECK(!Restore());CHECK(pendingRestore);CHECK(setCalls==1);EndCase();
    printf("%s: actual policy core %d checks, %d failures; no real process/API mutation\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
