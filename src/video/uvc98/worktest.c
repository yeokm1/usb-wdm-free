/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Deterministic host scheduler runs ACTUAL engine timer/bootstrap/job/join.
 * Injected operations are not a kernel, IRQL, SMP or STREAM.SYS runtime.
 */
#include "uvcwork.h"
#include "workshim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__declspec(dllimport) PVOID __stdcall VirtualAlloc(PVOID,SIZE_T,ULONG,ULONG);
__declspec(dllimport) int __stdcall VirtualProtect(PVOID,SIZE_T,ULONG,PULONG);
__declspec(dllimport) int __stdcall VirtualFree(PVOID,SIZE_T,ULONG);
typedef struct CLASS_MEMORY {
    HW_STREAM_REQUEST_BLOCK Read;
    KSSTREAM_HEADER Header;
    IRP Irp;
    MDL Mdl;
    HW_STREAM_OBJECT Stream;
} CLASS_MEMORY;
static UVWORK_ANCHOR *anchor;
static CLASS_MEMORY *cls;
static PHW_PRIORITY_ROUTINE low,high;
static PHW_TIMER_ROUTINE timer;
static PIO_WORKITEM_ROUTINE join;
static PVOID lowContext,highContext,timerContext,joinContext,pool;
static ULONG poolBytes,tick;
static PUCHAR frameBase,frame;
static HW_STREAM_REQUEST_BLOCK init,ctl,closeSrb,uninit;
static unsigned long checks;
static unsigned int itemFreed,refs,workerPending,terminated,created,inJoin;
static unsigned int initDone,readDone,closeDone,finalDone,notifyMode;
static unsigned int failCreate,failMap,cancelCreate,cancelMap,failJoin,inlineTimer;
static unsigned int inlineHigh,cancelBeforeCommit,cancelBeforeHigh,initNotifyMode;
static unsigned int failAllocate,failItem,failClose;
static NTSTATUS initStatus,readStatus;
static void __cdecl Check(int x,unsigned int line,const char *e)
{ ++checks; if(!x) { printf("FAIL line%u: %s\n",line,e); exit(1); } }
#define CHECK(x) Check((x),__LINE__,#x)
static void WorkerStep(void);
static void JoinStep(void);
static void Protect(PVOID p,ULONG n,ULONG f)
{ ULONG old; CHECK(VirtualProtect(p,n,f,&old)); }
static void Fire(void)
{
    PHW_TIMER_ROUTINE fn=timer; PVOID p=timerContext;
    CHECK(fn!=NULL); timer=NULL; timerContext=NULL; tick+=10; fn(p);
}
PVOID UvwFakeAllocate(POOL_TYPE type,SIZE_T n,ULONG tag)
{
    CHECK(type==NonPagedPool && tag!=0 && pool==NULL);
    if(failAllocate)return NULL;
    poolBytes=((ULONG)n+4095)&~4095UL;
    pool=VirtualAlloc(NULL,poolBytes,0x3000,4); CHECK(pool!=NULL); return pool;
}
VOID UvwFakeFree(PVOID p) { CHECK(p==pool); Protect(pool,poolBytes,1); }
PIO_WORKITEM UvwFakeItem(PDEVICE_OBJECT fdo)
{ CHECK(fdo==(PVOID)0x1111); return failItem?NULL:(PIO_WORKITEM)0x3333; }
VOID UvwFakeFreeItem(PIO_WORKITEM item) { CHECK(item==(PVOID)0x3333 && !itemFreed); itemFreed=1; }
VOID UvwFakeQueue(PIO_WORKITEM item,PIO_WORKITEM_ROUTINE fn,WORK_QUEUE_TYPE q,PVOID p)
{ CHECK(item==(PVOID)0x3333 && !itemFreed && !join && q==DelayedWorkQueue); join=fn; joinContext=p; }
VOID UvwFakeReference(PVOID p) { CHECK(p==(PVOID)0x1111); ++refs; }
VOID UvwFakeDereference(PVOID p) { CHECK(p==(PVOID)0x1111 && refs==1); --refs; }
VOID UvwFakeEvent(PKEVENT e,EVENT_TYPE type,BOOLEAN state)
{ CHECK(type==SynchronizationEvent && !state); memset(e,0,sizeof(*e)); }
LONG UvwFakeSet(PKEVENT e,KPRIORITY priority,BOOLEAN wait)
{ CHECK(e!=NULL && priority==0 && !wait && !workerPending); workerPending=1; return 0; }
NTSTATUS UvwFakeWait(PVOID p,KWAIT_REASON r,KPROCESSOR_MODE m,BOOLEAN a,PLARGE_INTEGER t)
{ (void)p;(void)r;(void)m;(void)a;(void)t; CHECK(0); return STATUS_UNSUCCESSFUL; }
NTSTATUS UvwFakeCreate(PHANDLE h,ACCESS_MASK access,POBJECT_ATTRIBUTES a,HANDLE process,PCLIENT_ID id,PKSTART_ROUTINE fn,PVOID p)
{
    CHECK(access==THREAD_ALL_ACCESS && a!=NULL && !process && !id && fn!=NULL && p==pool);
    if(failCreate) { *h=(HANDLE)0xbad; return STATUS_INSUFFICIENT_RESOURCES; }
    *h=(HANDLE)0x2222; created=1;
    if(cancelCreate) UvwCancel(anchor,&init);
    return STATUS_SUCCESS;
}
NTSTATUS UvwFakeTerminate(NTSTATUS s) { (void)s; CHECK(0); return STATUS_SUCCESS; }
NTSTATUS UvwFakeJoin(HANDLE h,BOOLEAN alert,PLARGE_INTEGER timeout)
{
    /* Passive bootstrap cleanup waits for the real JOB_EXIT path too. */
    if(!terminated && workerPending) { workerPending=0; CHECK(!UvwTestRunOne(anchor)); terminated=1; }
    CHECK(h==(HANDLE)0x2222 && !alert && !timeout && terminated);
    return failJoin?STATUS_INVALID_HANDLE:STATUS_SUCCESS;
}
NTSTATUS UvwFakeClose(HANDLE h)
{ CHECK(h==(HANDLE)0x2222 && terminated); return failClose?STATUS_INVALID_HANDLE:STATUS_SUCCESS; }
LONG UvwFakeExchange(PLONG p,LONG v)
{
    LONG old=*p; *p=v;
    if(inJoin && v==1) { CHECK(itemFreed); Protect(pool,poolBytes,1); }
    return old;
}
LONG UvwFakeCompare(PLONG p,LONG x,LONG c)
{
    LONG old;
    if(cancelBeforeCommit && x==2 && c==0) { cancelBeforeCommit=0; UvwCancel(anchor,&init); }
    old=*p; if(old==c) *p=x; return old;
}
VOID UvwFakePriority(PHW_STREAM_OBJECT s,PVOID dev,STREAM_PRIORITY pri,PHW_PRIORITY_ROUTINE fn,PVOID p)
{
    CHECK(!s && dev==anchor && !low && !high);
    if(pri==Low) { low=fn; lowContext=p; }
    else {
        CHECK(pri==LowToHigh); high=fn; highContext=p;
        if(cancelBeforeHigh) { cancelBeforeHigh=0; UvwCancel(anchor,&init); }
        if(inlineHigh) { inlineHigh=0; high=NULL; fn(p); }
    }
}
VOID UvwFakeTimer(PHW_STREAM_OBJECT s,PVOID dev,ULONG us,PHW_TIMER_ROUTINE fn,PVOID p)
{
    CHECK(!s && dev==anchor && us==10000 && !timer);
    timer=fn; timerContext=p;
    if(inlineTimer) { inlineTimer=0; Fire(); } /* final cleanup before bootstrap returns */
}
ULONGLONG UvwFakeTime(void) { return (ULONGLONG)tick*10000; }
LARGE_INTEGER UvwFakeDivide(LARGE_INTEGER n,ULONG d,PULONG rem)
{ LARGE_INTEGER q; q.QuadPart=n.QuadPart/d; if(rem) *rem=(ULONG)(n.QuadPart%d); return q; }
KIRQL UvwFakeIrql(void) { return DISPATCH_LEVEL; }
PUCHAR UvPortNtMap(PMDL mdl,PVOID unused)
{
    CHECK(mdl==&cls->Mdl && unused==NULL);
    if(cancelMap) UvwCancel(anchor,&cls->Read);
    return failMap?NULL:frame;
}
VOID UvPortNtSerialNotify(PHW_STREAM_REQUEST_BLOCK s,PHW_STREAM_OBJECT stream,int device,PVOID unused)
{
    (void)stream; CHECK(unused==NULL);
    if(s==&init) {
        CHECK(device); ++initDone; initStatus=s->Status;
        if(initNotifyMode==1) UvwCancel(anchor,&init);
        if(initNotifyMode==2) {
            CHECK(UvwControl(anchor,&uninit,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
            Fire(); WorkerStep(); JoinStep(); Fire();
        }
    }
    else if(s==&cls->Read) {
        CHECK(!device); ++readDone; readStatus=s->Status;
        if(s->Status==STATUS_SUCCESS) {
            CHECK(s->ActualBytesTransferred==UVCSIM_BYTES && cls->Header.DataUsed==UVCSIM_BYTES);
            CHECK(cls->Header.Data==(PVOID)0x12345678);
        } else CHECK(!s->ActualBytesTransferred && !cls->Header.DataUsed);
        if(notifyMode==1) CHECK(UvwControl(anchor,&closeSrb,UVFLOW_CLOSE,0)==UVFLOW_OK);
        if(notifyMode==2) CHECK(UvwControl(anchor,&uninit,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
        Protect(cls,4096,1); /* MUST NOT touch class data after completion */
    } else if(s==&closeSrb) { CHECK(device && s->Status==STATUS_SUCCESS); ++closeDone; }
    else if(s==&uninit) {
        CHECK(device && s->Status==STATUS_SUCCESS && !timer && itemFreed && !refs && terminated);
        ++finalDone; Protect(anchor,4096,1); /* final callback may release class anchor */
    } else CHECK(s==&ctl && s->Status==STATUS_SUCCESS);
}
static void Reset(void)
{
    if(pool) { CHECK(VirtualFree(pool,0,0x8000)); pool=NULL; }
    Protect(anchor,4096,4); Protect(cls,4096,4);
    memset(anchor,0,sizeof(*anchor)); memset(cls,0,sizeof(*cls));
    memset(&init,0,sizeof(init)); memset(&ctl,0,sizeof(ctl));
    memset(&closeSrb,0,sizeof(closeSrb)); memset(&uninit,0,sizeof(uninit));
    low=high=NULL; timer=NULL; join=NULL; tick=0;
    itemFreed=refs=workerPending=terminated=created=inJoin=0;
    initDone=readDone=closeDone=finalDone=notifyMode=0;
    failCreate=failMap=cancelCreate=cancelMap=failJoin=inlineTimer=0;
    inlineHigh=cancelBeforeCommit=cancelBeforeHigh=initNotifyMode=0;
    failAllocate=failItem=failClose=0;
    initStatus=readStatus=STATUS_PENDING;
    init.Command=SRB_INITIALIZE_DEVICE; init.HwDeviceExtension=anchor;
    uninit.Command=SRB_UNINITIALIZE_DEVICE; uninit.HwDeviceExtension=anchor;
    closeSrb.Command=SRB_CLOSE_STREAM; closeSrb.StreamObject=&cls->Stream;
    ctl.StreamObject=&cls->Stream; ctl.HwDeviceExtension=anchor;
    cls->Read.Command=SRB_READ_DATA; cls->Read.StreamObject=&cls->Stream;
    cls->Read.HwDeviceExtension=anchor; cls->Read.Irp=&cls->Irp;
    cls->Read.NumberOfBuffers=1; cls->Read.CommandData.DataBufferArray=&cls->Header;
    cls->Irp.MdlAddress=&cls->Mdl; cls->Mdl.ByteCount=UVCSIM_BYTES;
    cls->Header.Size=sizeof(cls->Header); cls->Header.FrameExtent=UVCSIM_BYTES;
    cls->Header.Data=(PVOID)0x12345678;
}
static void Begin(void)
{
    CHECK(UvwInitialize(anchor,&init,(PDEVICE_OBJECT)0x1111)==STATUS_PENDING && low && !timer && refs==1);
    CHECK(UvwOwnsInitialize(anchor,&init) && !UvwOwnsInitialize(anchor,&ctl));
}
static void LowStep(void)
{ PHW_PRIORITY_ROUTINE fn=low; PVOID p=lowContext; CHECK(fn!=NULL); low=NULL; fn(p); }
static void HighStep(void)
{ PHW_PRIORITY_ROUTINE fn=high;PVOID p=highContext;CHECK(fn!=NULL);high=NULL;fn(p); }
static void WorkerStep(void)
{ CHECK(workerPending); workerPending=0; if(!UvwTestRunOne(anchor)) terminated=1; }
static void JoinStep(void)
{
    PIO_WORKITEM_ROUTINE fn=join; PVOID p=joinContext;
    CHECK(fn!=NULL); join=NULL; inJoin=1; fn((PDEVICE_OBJECT)0x1111,p); inJoin=0;
    /* Joined/JoinFailed publication protected engine immediately. */
    Protect(pool,poolBytes,4);
}
static void Boot(void)
{
    Begin(); LowStep(); HighStep(); CHECK(initDone==1 && initStatus==STATUS_SUCCESS && created && timer);
    CHECK(!UvwOwnsInitialize(anchor,&init));
}
static void Open(unsigned int state)
{
    CHECK(UvwControl(anchor,&ctl,UVFLOW_OPEN,0)==UVFLOW_OK); Fire();
    CHECK(UvwControl(anchor,&ctl,UVFLOW_SET_STATE,state)==UVFLOW_OK); Fire();
}
static void Remove(void)
{
    CHECK(UvwControl(anchor,&uninit,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
    Fire(); CHECK(workerPending && join && !finalDone);
    WorkerStep(); CHECK(terminated); JoinStep(); Fire();
    Protect(anchor,4096,4); CHECK(finalDone==1 && anchor->Engine==NULL && !timer);
}
int __cdecl main(void)
{
    unsigned int i;
    anchor=(UVWORK_ANCHOR *)VirtualAlloc(NULL,4096,0x3000,4);
    cls=(CLASS_MEMORY *)VirtualAlloc(NULL,4096,0x3000,4);
    frameBase=(PUCHAR)VirtualAlloc(NULL,((UVCSIM_BYTES+4095)/4096+1)*4096,0x3000,4);
    frame=frameBase+((UVCSIM_BYTES+4095)/4096)*4096-UVCSIM_BYTES;
    CHECK(anchor && cls && frameBase && sizeof(*cls)<4096); Protect(frame+UVCSIM_BYTES,4096,1);
    /* Failed allocations never transfer INITIALIZE or publish callbacks. */
    Reset();failAllocate=1;
    CHECK(UvwInitialize(anchor,&init,(PDEVICE_OBJECT)0x1111)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!anchor->Engine && !low && !high && !timer && !refs && !initDone);
    Reset();failItem=1;
    CHECK(UvwInitialize(anchor,&init,(PDEVICE_OBJECT)0x1111)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!anchor->Engine && !low && !high && !timer && !refs && !initDone);
    /* Cancel before LOW: immediate HIGH destroys before LOW returns. */
    Reset(); Begin(); UvwCancel(anchor,&init); inlineHigh=1; LowStep();
    CHECK(initDone==1 && initStatus==STATUS_CANCELLED && !created && !anchor->Engine && itemFreed && !refs);
    /* Failed thread creation still cleans initial owned resources. */
    Reset(); Begin(); failCreate=1; LowStep(); CHECK(!timer && high); HighStep();
    CHECK(initDone==1 && initStatus==STATUS_INSUFFICIENT_RESOURCES && !anchor->Engine && itemFreed && !join && !workerPending);
    Reset();Begin();failCreate=cancelBeforeHigh=1;LowStep();HighStep();
    CHECK(initDone==1 && initStatus==STATUS_INSUFFICIENT_RESOURCES && !anchor->Engine);
    /* Cancellation during thread creation must join before INIT completion. */
    Reset(); Begin(); cancelCreate=1; LowStep(); CHECK(!initDone && !join && created && terminated && !timer);
    HighStep(); CHECK(initDone==1 && initStatus==STATUS_CANCELLED && !anchor->Engine);
    /* Cancellation wins immediately before successful setup commits. */
    Reset(); Begin(); cancelBeforeCommit=1; LowStep(); CHECK(terminated && !timer);
    HighStep(); CHECK(initDone==1 && initStatus==STATUS_CANCELLED && !anchor->Engine);
    /* Once setup commits, cancel before HIGH loses without changing outcome. */
    Reset(); Begin(); cancelBeforeHigh=1; LowStep(); HighStep();
    CHECK(initDone==1 && initStatus==STATUS_SUCCESS && anchor->Engine); Remove();
    /* Immediate successful HIGH and timer; reentrant Cancel cannot undo INIT. */
    Reset(); Begin(); inlineHigh=inlineTimer=1; initNotifyMode=1; LowStep();
    CHECK(initDone==1 && initStatus==STATUS_SUCCESS && timer); Remove();
    /* INIT notification can synchronously remove/free the whole engine. */
    Reset(); Begin(); inlineHigh=1; initNotifyMode=2; LowStep();
    Protect(anchor,4096,4); CHECK(initDone==1 && finalDone==1 && !anchor->Engine && !timer);
    /* A failed passive bootstrap join never publishes HIGH or timer/INIT. */
    Reset(); Begin(); cancelCreate=failJoin=1; LowStep();
    CHECK(!high && !timer && !initDone && anchor->Engine && refs==1);
    Reset(); Begin(); cancelCreate=failClose=1; LowStep();
    CHECK(!high && !timer && !initDone && anchor->Engine && refs==1);
    /* Mapping/generation handoff then reentrant CLOSE; guarded old class SRB. */
    Reset(); Boot(); Open(UVFLOW_RUN); CHECK(UvwRead(anchor,&cls->Read)==UVFLOW_OK);
    Fire(); WorkerStep(); Fire();
    for(i=0;i<20 && !workerPending;++i) Fire(); CHECK(workerPending);
    WorkerStep(); notifyMode=1; Fire(); CHECK(readDone==1 && readStatus==STATUS_SUCCESS && closeDone==1);
    Remove();
    /* Cancelled PAUSE mapping retires without RUN or a generation job. */
    Reset(); Boot(); Open(UVFLOW_PAUSE); CHECK(UvwRead(anchor,&cls->Read)==UVFLOW_OK);
    Fire(); cancelMap=1; WorkerStep(); Fire();
    CHECK(readDone==1 && readStatus==STATUS_CANCELLED && !workerPending); Remove();
    /* Mapping failure remains distinct and never generates a frame. */
    Reset(); Boot(); Open(UVFLOW_RUN); CHECK(UvwRead(anchor,&cls->Read)==UVFLOW_OK);
    Fire(); failMap=1; WorkerStep(); Fire();
    CHECK(readDone==1 && readStatus==STATUS_INVALID_BUFFER_SIZE); Remove();
    /* CLOSE with delayed admission cannot complete before retained mapper. */
    Reset(); Boot(); Open(UVFLOW_PAUSE); CHECK(UvwRead(anchor,&cls->Read)==UVFLOW_OK);
    Fire(); CHECK(UvwControl(anchor,&closeSrb,UVFLOW_CLOSE,0)==UVFLOW_OK); Fire(); CHECK(!closeDone && !readDone);
    WorkerStep(); Fire(); CHECK(readDone==1 && readStatus==STATUS_CANCELLED && closeDone==1); Remove();
    /* Successful frame notification queues UNINITIALIZE synchronously. */
    Reset(); Boot(); Open(UVFLOW_RUN); CHECK(UvwRead(anchor,&cls->Read)==UVFLOW_OK);
    Fire(); WorkerStep(); Fire(); for(i=0;i<20 && !workerPending;++i) Fire();
    CHECK(workerPending); WorkerStep(); notifyMode=2; Fire(); CHECK(readDone==1 && join && !finalDone);
    WorkerStep(); JoinStep(); Fire(); Protect(anchor,4096,4); CHECK(finalDone==1 && !anchor->Engine);
    /* Injected join failure MUST NOT publish Joined or free engine. */
    Reset(); Boot(); CHECK(UvwControl(anchor,&uninit,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
    Fire(); WorkerStep(); failJoin=1; JoinStep(); Fire(); CHECK(!finalDone && anchor->Engine && timer && refs==1);
    CHECK(VirtualFree(pool,0,0x8000)); CHECK(VirtualFree(anchor,0,0x8000));
    CHECK(VirtualFree(cls,0,0x8000)); CHECK(VirtualFree(frameBase,0,0x8000));
    printf("PASS: %lu actual-engine deterministic checks; class gate, bootstrap, join and notification guards\n",checks);
    return 0;
}
