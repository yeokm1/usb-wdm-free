/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Executes production dispatcher with real Win32 threads and mocked WDM
 * object/event/lock primitives. This tests ownership, not NTKERN availability.
 */
#include "umclean.h"
static LONG Checks,Allocations,Callbacks,Unloaded;
static int FailThread,FailPool;
static DWORD Tls;
static HANDLE Entered,Continue;
static PDEVICE_OBJECT ExpectedDevice;
static VOID (*Entry)(PVOID);
static PVOID EntryContext;
static int SelfMode;
static PDRIVER_OBJECT ActiveDriver;
#define CHECK(x) do {InterlockedIncrement(&Checks);if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);ExitProcess(1);}} while(0)
void MockEvent(KEVENT *event,BOOLEAN signaled) {
    event->Kind=1;event->Handle=CreateEvent(NULL,TRUE,signaled,NULL);CHECK(event->Handle!=NULL);
}
void *MockAllocate(SIZE_T size) {
    void *p;if(FailPool) return NULL;p=malloc(size);if(p) InterlockedIncrement(&Allocations);return p;
}
void MockFree(void *p) {InterlockedDecrement(&Allocations);free(p);}
void ObReferenceObject(void *p) {InterlockedIncrement(&((MOCK_OBJECT*)p)->Refs);}
void ObDereferenceObject(void *p) {
    MOCK_OBJECT *o=(MOCK_OBJECT*)p;
    if(!InterlockedDecrement(&o->Refs) && o->Kind==2) {CloseHandle(o->Handle);free(o);}
}
PETHREAD KeGetCurrentThread(void) {return (PETHREAD)TlsGetValue(Tls);}
DWORD MockWait(void *object) {
    MOCK_OBJECT *o=(MOCK_OBJECT*)object;
    if(o->Kind==2 && o==KeGetCurrentThread()) {
        /* Expected negative contract case, not evidence about NTKERN: an
         * unload invoked synchronously on its own worker would self-join. */
        CHECK(SelfMode);ExitProcess(77);
    }
    return WaitForSingleObject(o->Handle,INFINITE);
}
static DWORD WINAPI Launch(void *context) {
    TlsSetValue(Tls,context);Entry(EntryContext);CHECK(0);return 0;
}
NTSTATUS PsCreateSystemThread(HANDLE *handle,ULONG access,void *attributes,
    void *process,void *client,VOID (*entry)(PVOID),PVOID context) {
    MOCK_OBJECT *o;
    (void)access;(void)attributes;(void)process;(void)client;
    if(FailThread) return STATUS_INSUFFICIENT_RESOURCES;
    o=(MOCK_OBJECT*)calloc(1,sizeof(*o));CHECK(o!=NULL);o->Kind=2;o->Refs=1;
    Entry=entry;EntryContext=context;
    o->Handle=CreateThread(NULL,0,Launch,o,CREATE_SUSPENDED,NULL);CHECK(o->Handle!=NULL);
    *handle=o;ResumeThread(o->Handle);return STATUS_SUCCESS;
}
static VOID Previous(PDRIVER_OBJECT driver) {
    (void)driver;CHECK(Callbacks==258);CHECK(Allocations==0);
    CHECK(ExpectedDevice->Refs==1);InterlockedIncrement(&Unloaded);
}
typedef struct TASK {UM_CLEAN_ITEM *Item;ULONG Number;} TASK;
static VOID Callback(PDEVICE_OBJECT device,PVOID context) {
    TASK *task=(TASK*)context;ULONG number=task->Number;
    CHECK(device==ExpectedDevice);
    /* Production must not access either allocation after this point. */
    UmCleanFree(task->Item);free(task);
    if(number==0) {
        SetEvent(Entered);
        if(SelfMode) {ActiveDriver->DriverUnload(ActiveDriver);CHECK(0);}
        CHECK(WaitForSingleObject(Continue,5000)==WAIT_OBJECT_0);
    }
    InterlockedIncrement(&Callbacks);
}
static DWORD WINAPI ReleaseBlock(void *unused) {
    (void)unused;Sleep(50);CHECK(Unloaded==0);SetEvent(Continue);return 0;
}
int main(int argc,char **argv) {
    MOCK_DRIVER driver;MOCK_OBJECT device;ULONG i;HANDLE unblock;
    UM_CLEAN_ITEM *unused;
    (void)argv;SelfMode=argc>1;
    if(!SelfMode) {
        char path[MAX_PATH],command[MAX_PATH+40];STARTUPINFO startup;
        PROCESS_INFORMATION process;DWORD result;
        CHECK(GetModuleFileName(NULL,path,sizeof(path))!=0);
        sprintf(command,"\"%s\" /self-unload",path);
        ZeroMemory(&startup,sizeof(startup));startup.cb=sizeof(startup);
        CHECK(CreateProcess(NULL,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
        CHECK(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0);
        CHECK(GetExitCodeProcess(process.hProcess,&result));CHECK(result==77);
        CloseHandle(process.hProcess);CloseHandle(process.hThread);
    }
    Tls=TlsAlloc();CHECK(Tls!=TLS_OUT_OF_INDEXES);
    ZeroMemory(&device,sizeof(device));device.Kind=3;device.Refs=1;ExpectedDevice=&device;
    driver.DriverUnload=NULL;CHECK(!NT_SUCCESS(UmCleanInitialize(&driver)));
    driver.DriverUnload=Previous;FailThread=1;
    CHECK(!NT_SUCCESS(UmCleanInitialize(&driver)));CHECK(driver.DriverUnload==Previous);
    FailThread=0;CHECK(NT_SUCCESS(UmCleanInitialize(&driver)));CHECK(driver.DriverUnload!=Previous);
    ActiveDriver=&driver;
    CHECK(!NT_SUCCESS(UmCleanInitialize(&driver))); /* no duplicate dispatcher */
    CHECK(!UmCleanAllocate(NULL));FailPool=1;CHECK(!UmCleanAllocate(&device));FailPool=0;
    unused=UmCleanAllocate(&device);CHECK(unused!=NULL);CHECK(device.Refs==2);
    UmCleanFree(unused);CHECK(device.Refs==1);
    Entered=CreateEvent(NULL,TRUE,FALSE,NULL);Continue=CreateEvent(NULL,TRUE,FALSE,NULL);
    for(i=0;i<258;i++) {
        TASK *task=(TASK*)malloc(sizeof(*task));CHECK(task!=NULL);
        task->Item=UmCleanAllocate(&device);task->Number=i;CHECK(task->Item!=NULL);
        UmCleanQueue(task->Item,Callback,task);
        if(i==0) CHECK(WaitForSingleObject(Entered,5000)==WAIT_OBJECT_0);
        if(SelfMode) Sleep(INFINITE); /* child exits77 in self-join detector */
    }
    /* Unload begins while the first callback is blocked and 257 are queued.
     * It may return only after the callback and actual worker termination. */
    unblock=CreateThread(NULL,0,ReleaseBlock,NULL,0,NULL);CHECK(unblock!=NULL);
    driver.DriverUnload(&driver);CHECK(Unloaded==1);CHECK(Callbacks==258);
    CHECK(!UmCleanAllocate(&device));WaitForSingleObject(unblock,INFINITE);
    CloseHandle(unblock);CloseHandle(Entered);CloseHandle(Continue);TlsFree(Tls);
    printf("PASS: %ld cleanup dispatcher checks, 258 self-freeing callbacks and unload join\n",Checks);
    return 0;
}
