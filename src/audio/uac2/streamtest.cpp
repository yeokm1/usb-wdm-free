/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct host contract tests; kernel services are deterministic test doubles. */
#define PC_NEW_NAMES
#define _NTHAL_
#include <excpt.h>
#include <ntdef.h>
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#include <initguid.h>
#include <portcls.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
inline void *operator new(size_t,void *p){return p;}
#include "uacstream.h"
static LONG allocations=0, groups=0, failAllocation=-1;
static BOOLEAN failGroup=FALSE,failWork=FALSE;
static KIRQL testIrql=0;
static LONG deviceRefs=0, workItems=0;
struct _IO_WORKITEM { PDEVICE_OBJECT device; PIO_WORKITEM_ROUTINE callback; PVOID context; };
static PIO_WORKITEM queued=NULL;
extern "C" PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT d){if(failWork)return NULL;PIO_WORKITEM w=(PIO_WORKITEM)calloc(1,sizeof(*w));if(w){++workItems;w->device=d;}return w;}
extern "C" void IoFreeWorkItem(PIO_WORKITEM w){--workItems;free(w);}
extern "C" void IoQueueWorkItem(PIO_WORKITEM w,PIO_WORKITEM_ROUTINE fn,WORK_QUEUE_TYPE,PVOID c){assert(testIrql<=DISPATCH_LEVEL&&!queued);w->callback=fn;w->context=c;++deviceRefs;queued=w;}
extern "C" void FASTCALL ObfReferenceObject(PVOID){++deviceRefs;}
extern "C" void FASTCALL ObfDereferenceObject(PVOID){--deviceRefs;}
extern "C" KIRQL KeGetCurrentIrql(){return testIrql;}
extern "C" PVOID NTAPI ExAllocatePoolWithTag(POOL_TYPE,SIZE_T n,ULONG){if(failAllocation==0)return NULL;if(failAllocation>0)--failAllocation;++allocations;return calloc(1,n);}
extern "C" void NTAPI ExFreePool(PVOID p){--allocations;free(p);}
extern "C" LONG FASTCALL InterlockedIncrement(PLONG p){return ++*p;}
extern "C" LONG FASTCALL InterlockedDecrement(PLONG p){return --*p;}
extern "C" LONG FASTCALL InterlockedExchange(PLONG p,LONG n){LONG old=*p;*p=n;return old;}
extern "C" LONG FASTCALL InterlockedCompareExchange(PLONG p,LONG n,LONG c){LONG old=*p;if(old==c)*p=n;return old;}
extern "C" void NTAPI KeInitializeSpinLock(PKSPIN_LOCK p){*p=0;}
extern "C" KIRQL FASTCALL KfAcquireSpinLock(PKSPIN_LOCK p){assert(!*p);*p=1;return 0;}
extern "C" void FASTCALL KfReleaseSpinLock(PKSPIN_LOCK p,KIRQL){assert(*p);*p=0;}
class Group:public IServiceGroup{
 LONG refs;
public: Group():refs(1){++groups;}
 STDMETHODIMP QueryInterface(REFIID,PVOID *o){*o=this;AddRef();return 0;}
 STDMETHODIMP_(ULONG) AddRef(){return ++refs;}
 STDMETHODIMP_(ULONG) Release(){LONG n=--refs;if(!n){--groups;delete this;}return n;}
 STDMETHODIMP_(void) RequestService(){}
 STDMETHODIMP AddMember(PSERVICESINK){return 0;}
 STDMETHODIMP_(void) RemoveMember(PSERVICESINK){}
 STDMETHODIMP_(void) SupportDelayedService(){}
 STDMETHODIMP_(void) RequestDelayedService(ULONGLONG){}
 STDMETHODIMP_(void) CancelDelayedService(){}
};
extern "C" NTSTATUS NTAPI PcNewServiceGroup(PSERVICEGROUP *p,PUNKNOWN){if(failGroup){*p=NULL;return STATUS_INSUFFICIENT_RESOURCES;}*p=new Group;return 0;}
class Owner:public IUnknown{
public: LONG refs;
 Owner():refs(1){}
 STDMETHODIMP QueryInterface(REFIID,PVOID *o){*o=this;AddRef();return 0;}
 STDMETHODIMP_(ULONG) AddRef(){return ++refs;}
 STDMETHODIMP_(ULONG) Release(){return --refs;}
};
static ULONG starts=0, stops=0, closes=0, notices=0;
static BOOLEAN inNotify=FALSE,inProgress=FALSE,notifyReturned=FALSE;
static PDMACHANNEL releaseInNotify=NULL;
static NTSTATUS Start(PVOID,UacStream*){++starts;return STATUS_SUCCESS;}
static void Stop(PVOID,UacStream*){++stops;}
static void Close(PVOID,UacStream*){assert(!inNotify&&!inProgress&&deviceRefs==2);++closes;}
static void Notify(PVOID,PSERVICEGROUP){
 ++notices;
 if(releaseInNotify){assert(testIrql==PASSIVE_LEVEL&&inProgress);inNotify=TRUE;PDMACHANNEL last=releaseInNotify;releaseInNotify=NULL;ULONG before=closes;last->Release();assert(queued&&closes==before&&deviceRefs==2);inNotify=FALSE;notifyReturned=TRUE;}
}
static void RunQueued(){
 assert(queued&&!inNotify&&!inProgress&&testIrql==PASSIVE_LEVEL);
 PIO_WORKITEM work=queued;queued=NULL;PIO_WORKITEM_ROUTINE callback=work->callback;
 PDEVICE_OBJECT device=work->device;PVOID context=work->context;
 callback(device,context); /* callback may free the work item itself */
 assert(deviceRefs==1);--deviceRefs; /* native callback-return lifetime guard */
}
int main(){
 KSDATAFORMAT_WAVEFORMATEX f;memset(&f,0,sizeof(f));
 f.DataFormat.FormatSize=FIELD_OFFSET(KSDATAFORMAT_WAVEFORMATEX,WaveFormatEx)+sizeof(WAVEFORMATEX);
 f.DataFormat.MajorFormat=KSDATAFORMAT_TYPE_AUDIO;f.DataFormat.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;f.DataFormat.Specifier=KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
 f.WaveFormatEx.wFormatTag=WAVE_FORMAT_PCM;f.WaveFormatEx.nChannels=2;f.WaveFormatEx.nSamplesPerSec=48000;f.WaveFormatEx.nAvgBytesPerSec=192000;f.WaveFormatEx.nBlockAlign=4;f.WaveFormatEx.wBitsPerSample=16;
 assert(UacStream::ValidFormat(&f.DataFormat));
 for(ULONG i=0;i<f.DataFormat.FormatSize;++i){ULONG full=f.DataFormat.FormatSize;f.DataFormat.FormatSize=i;assert(!UacStream::ValidFormat(&f.DataFormat));f.DataFormat.FormatSize=full;}
 f.WaveFormatEx.nAvgBytesPerSec=96000;assert(!UacStream::ValidFormat(&f.DataFormat));f.WaveFormatEx.nAvgBytesPerSec=192000;
 Owner owner;LONG active=0;PMINIPORTWAVECYCLICSTREAM stream=NULL;PDMACHANNEL dma=NULL;PSERVICEGROUP service=NULL;
 UAC_STREAM_HOOKS hooks={NULL,Start,Stop,Close,Notify};
 PDEVICE_OBJECT device=(PDEVICE_OBJECT)&owner;
 for(LONG failure=0;failure<4;++failure){failAllocation=failure<2?failure:-1;failGroup=failure==2;failWork=failure==3;assert(UacStream::Create(&owner,&active,&f.DataFormat,&stream,&dma,&service,&hooks,device)==STATUS_INSUFFICIENT_RESOURCES);assert(!active&&owner.refs==1&&!allocations&&!groups&&!stream&&!dma&&!service&&!deviceRefs&&!workItems&&!closes);}
 failAllocation=-1;failGroup=failWork=FALSE;
 assert(!UacStream::Create(&owner,&active,&f.DataFormat,&stream,&dma,&service,&hooks,device));assert(active==1&&owner.refs==2&&allocations==2&&groups==1);
 UacStream *s=(UacStream*)stream;
 PMINIPORTWAVECYCLICSTREAM second=NULL;PDMACHANNEL d2=NULL;PSERVICEGROUP g2=NULL;
 assert(UacStream::Create(&owner,&active,&f.DataFormat,&second,&d2,&g2,&hooks,device)==STATUS_DEVICE_BUSY);
 assert(!second&&!d2&&!g2);
 dma->SetBufferSize(19);assert(dma->BufferSize()==16);
 PUCHAR ring=(PUCHAR)dma->SystemAddress();UCHAR input[20],out[24];for(i=0;i<20;++i)input[i]=(UCHAR)i;
 dma->CopyTo(ring,input,16);memset(out,0xcc,sizeof(out));s->FillBytes(out,20);for(i=0;i<20;++i)assert(out[i]==(UCHAR)(i%16));assert(out[20]==0xcc);
 dma->CopyTo(ring+12,input,8);assert(ring[12]==12&&ring[15]==15);
 assert(!stream->SetState(KSSTATE_RUN)&&starts==1);ULONG frame=0;assert(stream->SetNotificationFreq(10,&frame)==10&&frame==1920);
 s->CompleteBytes(1000,0);assert(!notices);s->CompleteBytes(1000,1);assert(notices==1);ULONG pos=0;assert(!stream->GetPosition(&pos)&&pos==0);
 s->CompleteBytes(5,0);assert(!stream->SetState(KSSTATE_PAUSE)&&stops==1);s->FillBytes(out,1);assert(out[0]==5);
 assert(!stream->SetState(KSSTATE_STOP));assert(!stream->GetPosition(&pos)&&pos==0);for(i=0;i<16;++i)assert(ring[i]==0);
 const LONGLONG values[]={0,1,11,12,192000,4294967296i64,177088743107611695i64};
 for(i=0;i<sizeof(values)/sizeof(values[0]);++i){LONGLONG n=values[i],expected=(n/12)*625+((n%12)*625)/12;assert(!stream->NormalizePhysicalPosition(&n)&&n==expected);}
 for(i=0;i<5000;++i){LARGE_INTEGER random;random.HighPart=(LONG)((ULONG)rand()%0x02752546);random.LowPart=((ULONG)rand()<<17)^((ULONG)rand()<<2)^(ULONG)rand();LONGLONG n=random.QuadPart,expected=(n/12)*625+((n%12)*625)/12;assert(!stream->NormalizePhysicalPosition(&n)&&n==expected);}
 LONGLONG invalid=-1;assert(!NT_SUCCESS(stream->NormalizePhysicalPosition(&invalid)));invalid=177088743107611696i64;assert(!NT_SUCCESS(stream->NormalizePhysicalPosition(&invalid)));
 service->Release();stream->Release();assert(active==1);testIrql=DISPATCH_LEVEL;dma->Release();assert(active==1&&queued&&owner.refs==2&&closes==0&&deviceRefs==2);testIrql=PASSIVE_LEVEL;RunQueued();assert(!active&&owner.refs==1&&!allocations&&!groups&&closes==1&&!deviceRefs&&!workItems);
 /* PASSIVE final reference drops inside the actual CompleteBytes->Notify
  * call stack. Close must not execute inline (would join its USB caller).
  * The production header queues it; simulate native execution after return. */
 assert(!UacStream::Create(&owner,&active,&f.DataFormat,&stream,&dma,&service,&hooks,device));
 s=(UacStream*)stream;service->Release();stream->Release();releaseInNotify=dma;
 inProgress=TRUE;s->CompleteBytes(1920,0);inProgress=FALSE;
 assert(notifyReturned&&queued&&closes==1&&owner.refs==2&&active&&deviceRefs==2);
 RunQueued();assert(closes==2&&!active&&owner.refs==1&&!allocations&&!groups&&!deviceRefs&&!workItems);
 puts("PASS: stream format/allocation/ring/position ownership; deferred DISPATCH and PASSIVE-in-Notify final release; native callback-return FDO guard and exactly-once close");return 0;
}
