/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of actual adapter and public kit structures. No kernel run.
 */
#include "uvcport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__declspec(dllimport) PVOID __stdcall VirtualAlloc(PVOID,SIZE_T,ULONG,ULONG);
__declspec(dllimport) int __stdcall VirtualProtect(PVOID,SIZE_T,ULONG,PULONG);
__declspec(dllimport) int __stdcall VirtualFree(PVOID,SIZE_T,ULONG);
#define RW 4
#define NOACCESS 1
#define RESERVE_COMMIT 0x3000
typedef struct CLASS_MEMORY {
    HW_STREAM_REQUEST_BLOCK Request;
    KSSTREAM_HEADER Header;
    IRP Irp;
    MDL Mdl;
    HW_STREAM_OBJECT Stream;
} CLASS_MEMORY;
static unsigned long checks;
static UVPORT *port;
static CLASS_MEMORY *mem;
static PUCHAR data,dataBase,guard;
static unsigned int mapCalls,failMap,cancelMap,notifications,reentry,protectRead,protectPort;
static NTSTATUS expected;
static HW_STREAM_REQUEST_BLOCK control,closeRequest,finalRequest;
static void __cdecl Check(int value,unsigned int line,const char *expr)
{ ++checks; if(!value) { printf("FAIL line%u: %s\n",line,expr); exit(1); } }
#define CHECK(x) Check((x),__LINE__,#x)
static void Protect(PVOID p,ULONG n,ULONG flags)
{ ULONG old; CHECK(VirtualProtect(p,n,flags,&old)); }
static void Reset(void)
{
    Protect(port,16384,RW); Protect(mem,4096,RW);
    memset(mem,0,sizeof(*mem)); memset(&control,0,sizeof(control));
    memset(&closeRequest,0,sizeof(closeRequest)); memset(&finalRequest,0,sizeof(finalRequest));
    UvPortInitialize(port); mapCalls=failMap=cancelMap=notifications=reentry=protectRead=protectPort=0;
    expected=STATUS_SUCCESS;
    mem->Request.StreamObject=&mem->Stream; mem->Request.Irp=&mem->Irp;
    mem->Request.Command=SRB_READ_DATA; mem->Request.NumberOfBuffers=1;
    mem->Request.CommandData.DataBufferArray=&mem->Header;
    mem->Request.SRBExtension=guard; /* NEVER read this inaccessible pointer. */
    mem->Request.TimeoutOriginal=123;
    mem->Irp.MdlAddress=&mem->Mdl; mem->Mdl.ByteCount=UVCSIM_BYTES;
    mem->Mdl.ByteOffset=37; /* mapper result already includes it */
    mem->Header.Size=sizeof(mem->Header); mem->Header.FrameExtent=UVCSIM_BYTES;
    mem->Header.Data=(PVOID)0x12345678; /* untrusted user VA never dereferenced */
    control.StreamObject=closeRequest.StreamObject=&mem->Stream;
}
static PUCHAR Map(PMDL mdl,PVOID context)
{
    CHECK(context==port && mdl==&mem->Mdl); ++mapCalls;
    if(cancelMap) { CHECK(UvPortCancel(port,&mem->Request)==UVFLOW_OK); mem->Irp.Cancel=TRUE; }
    return failMap?NULL:data;
}
static VOID Notify(PHW_STREAM_REQUEST_BLOCK s,PHW_STREAM_OBJECT stream,int device,PVOID context)
{
    UVPORT_WORK w;
    CHECK(context==port); CHECK(s->Status==expected); ++notifications;
    CHECK(UvPortCancel(port,s)==UVFLOW_CLOSED); /* retired BEFORE callback */
    if(s==&mem->Request) {
        CHECK(!device && stream==&mem->Stream);
        CHECK(mem->Header.Data==(PVOID)0x12345678);
        if(expected==STATUS_SUCCESS) {
            CHECK(s->ActualBytesTransferred==UVCSIM_BYTES && mem->Header.DataUsed==UVCSIM_BYTES);
            CHECK(mem->Header.Duration==1000000 && mem->Header.OptionsFlags==KSSTREAM_HEADER_OPTIONSF_DURATIONVALID);
        } else CHECK(!s->ActualBytesTransferred && !mem->Header.DataUsed);
    }
    if(reentry) {
        CHECK(UvPortControl(port,reentry==1?&closeRequest:&finalRequest,
            reentry==1?UVFLOW_CLOSE:UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
        CHECK(UvPortNext(port,100,&w)==UVFLOW_BUSY);
    }
    if(protectRead) Protect(mem,4096,NOACCESS);
    if(protectPort) Protect(port,16384,NOACCESS);
    /* No class-memory access after this point, including caller Deliver. */
}
static void CompleteControl(unsigned int command,unsigned int state)
{
    UVPORT_WORK w; UVPORT_COMPLETION c;
    CHECK(UvPortControl(port,&control,command,state)==UVFLOW_OK);
    CHECK(!control.TimeoutCounter);
    CHECK(UvPortNext(port,0,&w)==UVFLOW_OK && w.Flow.Action==UVFLOW_CONTROL);
    CHECK(UvPortFinish(port,w,UVFLOW_OK,0,&c)==UVFLOW_OK);
    UvPortDeliver(&c,Notify,port); CHECK(UvPortReturned(port)==UVFLOW_OK);
}
static void Open(unsigned int state)
{ CompleteControl(UVFLOW_OPEN,0); CompleteControl(UVFLOW_SET_STATE,state); }
static UVOWN_TOKEN Admit(void)
{
    UVOWN_TOKEN t,dup;
    CHECK(UvPortRead(port,&mem->Request,&t)==UVFLOW_OK);
    CHECK(!mem->Request.TimeoutCounter);
    CHECK(UvPortRead(port,&mem->Request,&dup)==UVFLOW_DUPLICATE);
    CHECK(port->Flow.Own.Reads==1);
    return t;
}
static void Read(unsigned int bytes,NTSTATUS status)
{
    UVPORT_WORK w; UVPORT_COMPLETION c;
    CHECK(UvPortNext(port,100,&w)==UVFLOW_OK && w.Flow.Action==UVFLOW_READ);
    CHECK(mem->Request.TimeoutCounter==123);
    if(status==STATUS_SUCCESS) CHECK(UvcSimGenerate(w.Binding.Data,UVCSIM_BYTES,w.Flow.Frame)==UVCSIM_BYTES);
    /* Forged class pointers/commands in worker copy must not replace owned facts. */
    w.Binding.Header=(PKSSTREAM_HEADER)guard; w.Flow.Request=guard; w.Flow.Command=UVFLOW_UNINITIALIZE;
    CHECK(UvPortFinish(port,w,UVFLOW_OK,bytes,&c)==UVFLOW_OK);
    CHECK(port->Flow.Own.Reads==0 && !port->Binding[w.Flow.Token.Slot].Request);
    expected=status; UvPortDeliver(&c,Notify,port);
    CHECK(UvPortReturned(port)==UVFLOW_OK);
}
static void Malformed(void)
{
    unsigned int i; UVOWN_TOKEN t; UVPORT_MAPPING m;
    for(i=0;i<15;++i) {
        Reset(); Open(UVFLOW_RUN); t=Admit();
        switch(i) {
        case 0: mem->Request.NumberOfBuffers=0; break;
        case 1: mem->Request.NumberOfBuffers=2; break;
        case 2: mem->Request.Irp=NULL; break;
        case 3: mem->Request.CommandData.DataBufferArray=NULL; break;
        case 4: mem->Irp.MdlAddress=NULL; break;
        case 5: mem->Mdl.Next=&mem->Mdl; break;
        case 6: mem->Header.Size=sizeof(mem->Header)-1; break;
        case 7: mem->Header.FrameExtent=UVCSIM_BYTES-1; break;
        case 8: mem->Header.FrameExtent=UVCSIM_BYTES+1; break;
        case 9: mem->Header.OptionsFlags=1; break;
        case 10: mem->Mdl.ByteCount=UVCSIM_BYTES-1; break;
        case 11: failMap=1; break;
        case 12: break; /* null mapper */
        case 13: mem->Header.FrameExtent=0xffffffffUL; break;
        case 14: mem->Header.Size=0; break;
        }
        UvPortMap(&mem->Request,t,i==12?NULL:Map,port,&m); CHECK(!m.Valid);
        CHECK(UvPortReady(port,&m)==UVFLOW_OK);
        CHECK(UvPortReady(port,&m)==UVFLOW_INVALID); /* cannot publish twice */
        Read(0,STATUS_INVALID_BUFFER_SIZE);
        CHECK(mapCalls==(i==11?1U:0U));
    }
}
int __cdecl main(void)
{
    UVOWN_TOKEN t; UVPORT_MAPPING m; UVPORT_WORK w; UVPORT_COMPLETION c;
    unsigned int mode;
    port=(UVPORT *)VirtualAlloc(NULL,16384,RESERVE_COMMIT,RW);
    mem=(CLASS_MEMORY *)VirtualAlloc(NULL,4096,RESERVE_COMMIT,RW);
    guard=(PUCHAR)VirtualAlloc(NULL,4096,RESERVE_COMMIT,NOACCESS);
    dataBase=(PUCHAR)VirtualAlloc(NULL,((UVCSIM_BYTES+4095)/4096+1)*4096,RESERVE_COMMIT,RW);
    data=dataBase+((UVCSIM_BYTES+4095)/4096)*4096-UVCSIM_BYTES;
    CHECK(port && mem && guard && data && sizeof(*port)<=16384 && sizeof(*mem)<=4096);
    /* Exact frame ends at inaccessible page. Offset37 in MDL must not be added. */
    Protect(data+UVCSIM_BYTES,4096,NOACCESS);
    Reset(); Open(UVFLOW_RUN); t=Admit(); UvPortMap(&mem->Request,t,Map,port,&m);
    CHECK(m.Valid && m.Data==data && mapCalls==1); CHECK(UvPortReady(port,&m)==UVFLOW_OK);
    reentry=1; protectRead=1; Read(UVCSIM_BYTES,STATUS_SUCCESS);
    CHECK(UvPortNext(port,100,&w)==UVFLOW_OK && w.Flow.Command==UVFLOW_CLOSE);
    CHECK(UvPortFinish(port,w,UVFLOW_OK,0,&c)==UVFLOW_OK);
    reentry=protectRead=0; UvPortDeliver(&c,Notify,port); CHECK(UvPortReturned(port)==UVFLOW_OK);
    CHECK(!port->Flow.Stream); /* old class page is still NOACCESS */
    Malformed();
    for(mode=0;mode<3;++mode) {
        Reset(); Open(UVFLOW_PAUSE); t=Admit();
        if(mode==0) CHECK(UvPortCancel(port,&mem->Request)==UVFLOW_OK);
        if(mode==1) mem->Irp.Cancel=TRUE;
        if(mode==2) cancelMap=1;
        UvPortMap(&mem->Request,t,Map,port,&m); CHECK(m.Valid);
        CHECK(UvPortReady(port,&m)==UVFLOW_OK); Read(0,STATUS_CANCELLED);
    }
    /* Delayed mapping blocks close but never waits synchronously in callback. */
    Reset(); Open(UVFLOW_PAUSE); t=Admit();
    CHECK(UvPortControl(port,&closeRequest,UVFLOW_CLOSE,0)==UVFLOW_OK);
    CHECK(UvPortNext(port,100,&w)==UVFLOW_OK && w.Flow.Action==UVFLOW_WAIT);
    UvPortMap(&mem->Request,t,Map,port,&m); CHECK(UvPortReady(port,&m)==UVFLOW_OK);
    Read(0,STATUS_CANCELLED);
    CHECK(UvPortNext(port,100,&w)==UVFLOW_OK && w.Flow.Command==UVFLOW_CLOSE);
    CHECK(UvPortFinish(port,w,UVFLOW_OK,0,&c)==UVFLOW_OK);
    expected=STATUS_SUCCESS; UvPortDeliver(&c,Notify,port); CHECK(UvPortReturned(port)==UVFLOW_OK);
    /* A success cannot report more or less than the fixed frame extent. */
    for(mode=0;mode<2;++mode) {
        Reset(); Open(UVFLOW_RUN); t=Admit(); UvPortMap(&mem->Request,t,Map,port,&m);
        CHECK(UvPortReady(port,&m)==UVFLOW_OK); Read(mode?UVCSIM_BYTES+1:0,STATUS_INVALID_BUFFER_SIZE);
    }
    /* Completion-to-uninitialize reentry: no inline join or final completion. */
    Reset(); Open(UVFLOW_RUN); t=Admit(); UvPortMap(&mem->Request,t,Map,port,&m);
    CHECK(UvPortReady(port,&m)==UVFLOW_OK); reentry=2; protectRead=1;
    Read(UVCSIM_BYTES,STATUS_SUCCESS);
    CHECK(UvPortNext(port,100,&w)==UVFLOW_OK && w.Flow.Action==UVFLOW_EXIT);
    CHECK(UvPortFinish(port,w,UVFLOW_OK,0,&c)==UVFLOW_INVALID);
    CHECK(UvPortAfterJoin(port,0,0,&c)==UVFLOW_BUSY);
    CHECK(UvPortAfterJoin(port,1,0,&c)==UVFLOW_BUSY);
    CHECK(UvPortAfterJoin(port,0,1,&c)==UVFLOW_BUSY);
    CHECK(UvPortAfterJoin(port,1,1,&c)==UVFLOW_OK && c.Flow.Final);
    reentry=protectRead=0; protectPort=1; expected=STATUS_SUCCESS;
    UvPortDeliver(&c,Notify,port); /* final callback protects entire adapter */
    CHECK(VirtualFree(port,0,0x8000)); CHECK(VirtualFree(mem,0,0x8000));
    CHECK(VirtualFree(guard,0,0x8000)); CHECK(VirtualFree(dataBase,0,0x8000));
    printf("PASS: %lu actual-SRB adapter checks; guarded handoff, reentry, mapping and cancellation\n",checks);
    return 0;
}
