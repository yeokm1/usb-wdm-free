/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original synthetic worker. NOT linked into M0; no USB transport.
 * Default Stream Class synchronization, no ISR: callbacks/timer DISPATCH.
 * LOW bootstrap and private worker never mutate class-gated ownership.
 * Final class timer does not rearm and hands UNINITIALIZE back LAST.
 */
#include "uvcwork.h"
#include "uvctrace.h"
#ifdef UVWORK_TEST
/* Published ZwWaitForSingleObject signature (ntifs.h), for the host shim's
 * join; the kernel build joins through the Exited event (below). */
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle,BOOLEAN Alertable,
    PLARGE_INTEGER Timeout);
#include "workshim.h"
#else
/* Windows 98 SE: NTKERN exports neither IoAllocateWorkItem/IoQueueWorkItem/
 * IoFreeWorkItem nor ZwWaitForSingleObject (link probe against the 98 DDK
 * wdm.lib, 2026-10-22), and waiting on a thread object faults there (usbmidi
 * 98 SE record, 2026-10-22). So the join item is an executive work item
 * (ExQueueWorkItem, in every WDM system) behind the IoXxxWorkItem names, and
 * the join waits for an Exited event the worker sets as its last act; on WDM
 * 1.10 systems (2000 and later) it then also waits for the thread object, so
 * the image cannot unload under the thread. Unlike IoQueueWorkItem, an
 * executive item holds no device reference while it runs: Join's publication
 * of Joined is its last engine access, and only its return remains. */
typedef struct UVJOINITEM {
    WORK_QUEUE_ITEM Item;
    PIO_WORKITEM_ROUTINE Routine;
    PVOID Context;
    PDEVICE_OBJECT Device;
} UVJOINITEM;
static VOID UvwExItem(PVOID p)
{
    UVJOINITEM *j=(UVJOINITEM *)p;
    j->Routine(j->Device,j->Context); /* may free j: no access after */
}
static PIO_WORKITEM UvwAllocItem(PDEVICE_OBJECT d)
{
    UVJOINITEM *j=(UVJOINITEM *)ExAllocatePoolWithTag(NonPagedPool,sizeof(*j),'jwvU');
    if(j) { RtlZeroMemory(j,sizeof(*j)); j->Device=d; }
    return (PIO_WORKITEM)j;
}
static VOID UvwFreeItem(PIO_WORKITEM i) { ExFreePool(i); }
static VOID UvwQueueItem(PIO_WORKITEM i,PIO_WORKITEM_ROUTINE r,WORK_QUEUE_TYPE q,PVOID c)
{
    UVJOINITEM *j=(UVJOINITEM *)i;
    j->Routine=r; j->Context=c;
    ExInitializeWorkItem(&j->Item,UvwExItem,j);
    ExQueueWorkItem(&j->Item,q);
}
#define IoAllocateWorkItem UvwAllocItem
#define IoFreeWorkItem UvwFreeItem
#define IoQueueWorkItem UvwQueueItem
#endif
#ifdef UVWORK_USB
/* M2: frames come from the camera below the class FDO instead of the
 * synthetic generator. All USB work runs in the private passive worker. */
#include "uvcusb.h"
#include "uvcdesc.h"
#include "uvcfmt.h"
#endif
#define JOB_MAP 1
#define JOB_FRAME 2
#define JOB_EXIT 3
#define JOB_USBSTOP 4 /* UVWORK_USB: stop the camera, then complete the control */
#define TIMER_US 10000
#define PUMP_BUDGET 4
#define WORK_TAG '1wvU'
#define BOOT_PENDING 0
#define BOOT_CANCELLED 1
#define BOOT_COMMITTED 2
#define BOOT_FAILED 3
struct UVWORK {
    UVPORT Port;
    UVWORK_ANCHOR *Anchor;
    PVOID Device;
    PDEVICE_OBJECT Fdo;
    PIO_WORKITEM JoinItem;
    PHW_STREAM_REQUEST_BLOCK Initialize;
    HANDLE Thread;        /* kernel build: a "thread running" marker only */
    KEVENT JobReady;
#ifndef UVWORK_TEST
    KEVENT Exited;        /* set by the worker as its last act */
    PVOID ThreadObject;   /* WDM 1.10+ only: referenced, waited, released */
    BOOLEAN WaitThread;   /* IoIsWdmVersionAvailable(1, 0x10) */
#endif
    LONG BootstrapOutcome, BootstrapDone, Done, Joined, JoinFailed; /* Interlocked only */
    NTSTATUS BootstrapStatus;
    unsigned int BootHandled, JoinQueued, Job, Busy, Exiting;
    unsigned int CallbackDepth, TimerArmed;
    UVOWN_TOKEN MapToken;
    PHW_STREAM_REQUEST_BLOCK MapRequest;
    UVPORT_MAPPING Mapping;
    UVPORT_WORK Work;
    unsigned int FrameBytes;
#ifdef UVWORK_USB
    PDEVICE_OBJECT Lower;
    UVU *Usb;
    ULONG FrameSequence;
    NTSTATUS UsbStatus;
    UVFMT Formats;        /* built from the camera's descriptors at bootstrap */
    int Selected;         /* Formats index opened by the stream, -1 none */
    int Streaming;        /* camera stream started for Selected */
    PUCHAR Scratch;       /* the camera's YUY2 frame, for RGB24 output */
    ULONG ScratchBytes;
    ULONG Pictures, Drops; /* KS_FRAME_INFO / DROPPEDFRAMES counts since open or STOP */
    ULONG SequenceBase;   /* the camera's frame sequence when this stream started */
    int EmptyOk;          /* this read's empty result is a camera timeout, not a failure */
#endif
#ifdef UVWORK_VM_TRACE
    unsigned int TraceTimers;
#endif
};
static VOID STREAMAPI Timer(PVOID context);
static VOID STREAMAPI BootstrapComplete(PVOID context);
static ULONG Tick(void)
{
    LARGE_INTEGER t;
    t.QuadPart=KeQueryInterruptTime();
    return RtlExtendedLargeIntegerDivide(t,10000,NULL).LowPart;
}
/* Only private passive worker uses this routine. Done publication is the
 * LAST engine access for a job; the worker then waits for another job.
 * Engine itself remains allocated until this thread has actually joined. */
static int RunJob(UVWORK *e)
{
#ifdef UVWORK_USB
    if(e->Job==JOB_EXIT) {
        /* Stream stop/close runs here, passive, before the thread exits. */
        if(e->Usb) { UvuClose(e->Usb); e->Usb=NULL; UVTRACE("USB_CLOSED\n"); }
        return 0;
    }
#else
    if(e->Job==JOB_EXIT) return 0;
#endif
    if(e->Job==JOB_MAP) UvPortMapBytes(e->MapRequest,e->MapToken,e->Port.FrameBytes,UvPortNtMap,NULL,&e->Mapping);
#ifdef UVWORK_USB
    else if(e->Job==JOB_USBSTOP) {
        /* STOP, CLOSE or power-down: the camera stops streaming now (the next
         * read restarts it) instead of running until UNINITIALIZE. The device
         * stays open: its descriptors back the advertised formats. */
        if(e->Usb) { UvuStop(e->Usb); e->FrameSequence=0; e->Streaming=0; UVTRACE("USB_STOPPED\n"); }
        e->FrameBytes=0; e->Pictures=e->Drops=0;
    }
    else {
        /* The first frame request after a STOP starts the selected format;
         * a failure is kept and later reads of that open complete empty. */
        if(e->Usb && !e->Streaming && NT_SUCCESS(e->UsbStatus) && e->Selected>=0) {
            NTSTATUS status=UvuStartFrame(e->Usb,e->Formats.Entry[e->Selected].FrameIndex);
            UVHEX("USB_START ",status);
            if(NT_SUCCESS(status)) {
                /* This session's baseline: frames from an earlier session
                 * are neither delivered nor counted as drops. */
                e->Streaming=1;
                e->FrameSequence=e->SequenceBase=UvuLatestSequence(e->Usb);
            } else e->UsbStatus=status;
        }
        /* The first frame may take a camera's start-up (exposure) time. */
        e->FrameBytes=0; e->EmptyOk=e->Streaming;
        if(e->Streaming) {
            const UVFMT_ENTRY *f=&e->Formats.Entry[e->Selected];
            ULONG wait=e->FrameSequence!=e->SequenceBase ? 1000 : 4000, before=e->FrameSequence;
            if(f->Kind==UVFMT_RGB24) {
                /* Design 6: RGB24 synthesised from the camera's YUY2 frame,
                 * here at PASSIVE_LEVEL in the private worker. */
                if(e->Scratch && f->SourceBytes<=e->ScratchBytes &&
                   UvuCopyLatest(e->Usb,e->Scratch,f->SourceBytes,&e->FrameSequence,wait)==f->SourceBytes) {
                    UvfYuy2ToRgb24(e->Scratch,e->Work.Binding.Data,f->Width,f->Height);
                    e->FrameBytes=f->FrameBytes;
                }
            } else e->FrameBytes=UvuCopyLatest(e->Usb,e->Work.Binding.Data,e->Port.FrameBytes,&e->FrameSequence,wait);
            /* Frames the camera completed but no read took (overwritten in
             * the latest-frame buffer) were scheduled and lost: drops. */
            if(e->FrameSequence-before>1) e->Drops+=e->FrameSequence-before-1;
        }
        if(e->FrameBytes==e->Port.FrameBytes) e->Pictures++; else e->Drops++;
#ifdef UVWORK_VM_TRACE
        if(e->Usb) {
            UVU_STATS st; UvuGetStats(e->Usb,&st);
            UVHEX("USB_COPY_BYTES ",e->FrameBytes); UVHEX("USB_FRAMES ",st.Frames);
            UVHEX("USB_DROPPED ",st.Dropped); UVHEX("USB_URBS ",st.Urbs);
            UVHEX("USB_FAILED_URBS ",st.FailedUrbs); UVHEX("USB_GAPS ",st.Gaps);
            UVHEX("USB_PACKETS ",st.Packets); UVHEX("USB_EMPTY ",st.Empty);
            UVHEX("USB_PKT_FAILED ",st.Failed); UVHEX("USB_INVALID ",st.Invalid);
            UVHEX("USB_ALT ",st.Alternate); UVHEX("USB_PAYLOAD ",st.PayloadBytes);
            UVHEX("USB_FRAMEBYTES ",st.FrameBytes); UVHEX("USB_SPEED ",st.Speed);
            UVHEX("USB_STRIDE ",st.Stride); UVHEX("USB_LAST ",st.LastStatus);
            UVHEX("USB_FIRSTFAIL ",st.FirstFailStatus); UVHEX("USB_FIRSTFAIL_USBD ",st.FirstFailUsbd);
            UVHEX("USB_SUBMITTED ",st.Submitted);
            UVHEX("USB_QUIRK ",st.Quirk); UVHEX("USB_BW_NEED ",st.BandwidthNeed);
            UVHEX("USB_REQ_FRAME ",st.ReqFrame); UVHEX("USB_DEV_FRAME ",st.DevFrame);
            UVHEX("USB_DEV_FRAMEBYTES ",st.DevFrameBytes); UVHEX("USB_COMMIT_PAYLOAD ",st.CommitPayload);
            UVHEX("USB_RESOLVED_W ",st.ResolvedWidth); UVHEX("USB_RESOLVED_H ",st.ResolvedHeight);
            UVHEX("USB_EOFS ",st.Eofs); UVHEX("USB_FIDFLIPS ",st.FidFlips);
            UVHEX("USB_DROP_BAD ",st.DropBad); UVHEX("USB_DROP_SIZE ",st.DropSize);
            UVHEX("USB_LASTDROP_USED ",st.LastDropUsed); UVHEX("USB_ERRPKTS ",st.ErrorPackets);
            UVHEX("USB_OVERFLOWS ",st.Overflows);
        }
#endif
    }
#else
    else e->FrameBytes=UvcSimGenerate(e->Work.Binding.Data,UVCSIM_BYTES,e->Work.Flow.Frame);
#endif
    InterlockedExchange(&e->Done,1);
    return 1;
}
static VOID Worker(PVOID context)
{
    UVWORK *e=(UVWORK *)context;
    UVTRACE("WORKER_ENTRY\n");
    for(;;) {
        KeWaitForSingleObject(&e->JobReady,Executive,KernelMode,FALSE,NULL);
        if(!RunJob(e)) break;
    }
#ifndef UVWORK_TEST
    KeSetEvent(&e->Exited,IO_NO_INCREMENT,FALSE); /* last engine access */
#endif
    PsTerminateSystemThread(STATUS_SUCCESS);
}
#ifdef UVWORK_TEST
#define JoinThread(e) ZwWaitForSingleObject((e)->Thread,FALSE,NULL)
#define CloseThread(e) ZwClose((e)->Thread)
#else
static NTSTATUS JoinThread(UVWORK *e)
{
    KeWaitForSingleObject(&e->Exited,Executive,KernelMode,FALSE,NULL);
    if(e->ThreadObject) return KeWaitForSingleObject(e->ThreadObject,Executive,KernelMode,FALSE,NULL);
    {   LARGE_INTEGER t; t.QuadPart=-100000; /* 10 ms: let it leave our code */
        KeDelayExecutionThread(KernelMode,FALSE,&t); }
    return STATUS_SUCCESS;
}
static NTSTATUS CloseThread(UVWORK *e)
{
    if(e->ThreadObject) { ObDereferenceObject(e->ThreadObject); e->ThreadObject=NULL; }
    return STATUS_SUCCESS;
}
#endif
/* A separate system work item joins the private thread. Free the dequeued
 * item BEFORE publishing Joined; publication is the last engine access.
 * Its native queue callback lifetime guard covers the remaining return. */
static VOID Join(PDEVICE_OBJECT device,PVOID context)
{
    UVWORK *e=(UVWORK *)context;
    PIO_WORKITEM item=e->JoinItem;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(device);
    status=JoinThread(e);
    if(status!=STATUS_SUCCESS) {
        e->JoinItem=NULL; IoFreeWorkItem(item);
        InterlockedExchange(&e->JoinFailed,1); return; /* no joined fact */
    }
    status=CloseThread(e);
    if(status!=STATUS_SUCCESS) {
        e->JoinItem=NULL; IoFreeWorkItem(item);
        InterlockedExchange(&e->JoinFailed,1); return;
    }
    e->Thread=NULL; e->JoinItem=NULL;
    IoFreeWorkItem(item);
    InterlockedExchange(&e->Joined,1);
}
#ifdef UVWORK_TEST
int UvwTestRunOne(UVWORK_ANCHOR *a) { return RunJob(a->Engine); }
#endif
static void ExitWorker(UVWORK *e)
{
    e->Exiting=1;
    if(!e->Thread) { InterlockedExchange(&e->Joined,1); return; }
    if(!e->JoinQueued) {
        e->JoinQueued=1; e->Job=JOB_EXIT; e->Busy=1;
        KeSetEvent(&e->JobReady,IO_NO_INCREMENT,FALSE);
        IoQueueWorkItem(e->JoinItem,Join,DelayedWorkQueue,e);
    }
}
/* Caller is class-serialized HIGH failure completion or the FINAL timer;
 * no timer is armed/rearmed after this function. No other class callback
 * runs concurrently, LOW bootstrap
 * has no engine access after LowToHigh handoff, and worker is joined. */
static void Destroy(UVWORK *e)
{
    PDEVICE_OBJECT fdo=e->Fdo;
#ifdef UVWORK_USB
    if(e->Usb) { UvuClose(e->Usb); e->Usb=NULL; } /* failed start: never streamed */
    if(e->Scratch) { ExFreePool(e->Scratch); e->Scratch=NULL; }
#endif
    e->Anchor->Engine=NULL;
    if(e->JoinItem) IoFreeWorkItem(e->JoinItem); /* never queued in no-thread path */
    ExFreePool(e);
    ObDereferenceObject(fdo);
}
static VOID STREAMAPI Bootstrap(PVOID context)
{
    UVWORK *e=(UVWORK *)context;
    OBJECT_ATTRIBUTES a;
    HANDLE thread=NULL;
    UVTRACE("LOW_ENTRY\n");
#ifdef UVWORK_TEST
    InitializeObjectAttributes(&a,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
#else
    UNREFERENCED_PARAMETER(a);
#endif
    if(InterlockedCompareExchange(&e->BootstrapOutcome,0,0)==BOOT_CANCELLED) e->BootstrapStatus=STATUS_CANCELLED;
    else {
#ifdef UVWORK_TEST
        e->BootstrapStatus=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,&a,NULL,NULL,Worker,e);
#else
        /* No OBJ_KERNEL_HANDLE (2000+ only): LOW runs in a system worker
         * thread, and the handle is closed at once. */
        e->BootstrapStatus=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,NULL,NULL,NULL,Worker,e);
        if(NT_SUCCESS(e->BootstrapStatus)) {
            if(e->WaitThread &&
               !NT_SUCCESS(ObReferenceObjectByHandle(thread,SYNCHRONIZE,NULL,KernelMode,&e->ThreadObject,NULL)))
                e->ThreadObject=NULL; /* join then waits Exited and 10 ms */
            ZwClose(thread);
        }
#endif
        UVSTATUS(e->BootstrapStatus);
        if(NT_SUCCESS(e->BootstrapStatus)) e->Thread=thread;
#ifdef UVWORK_USB
        /* Passive: read the camera's descriptors now so GET_STREAM_INFO can
         * advertise its own formats. No camera, no device. */
        if(NT_SUCCESS(e->BootstrapStatus)) {
            NTSTATUS status=UvuOpen(e->Lower,&e->Usb);
            UVHEX("USB_OPEN ",status);
            if(NT_SUCCESS(status) && !UvfBuild(UvuDescriptors(e->Usb),&e->Formats))
                status=STATUS_NOT_SUPPORTED;
            UVHEX("USB_FORMATS ",e->Formats.Count);
            if(NT_SUCCESS(status)) {
                unsigned int i;
                for(i=0;i<e->Formats.Count;i++)
                    if(e->Formats.Entry[i].Kind==UVFMT_RGB24 && e->Formats.Entry[i].SourceBytes>e->ScratchBytes)
                        e->ScratchBytes=e->Formats.Entry[i].SourceBytes;
                if(e->ScratchBytes) {
                    e->Scratch=(PUCHAR)ExAllocatePoolWithTag(NonPagedPool,e->ScratchBytes,WORK_TAG);
                    if(!e->Scratch) status=STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            if(!NT_SUCCESS(status)) e->BootstrapStatus=status;
        }
#endif
    }
    /* Successful setup commits atomically against cancellation. Once it
     * commits, later Cancel loses: initialization already succeeded, even
     * if its class notification has not yet been delivered. */
    if(NT_SUCCESS(e->BootstrapStatus) &&
       InterlockedCompareExchange(&e->BootstrapOutcome,BOOT_COMMITTED,BOOT_PENDING)==BOOT_PENDING) {
        UVTRACE("BOOTSTRAP_COMMITTED\n");
    } else {
        NTSTATUS status;
        /* Freeze failure too: cancellation may win before this decision,
         * but cannot mutate any immutable outcome awaiting HIGH. */
        InterlockedCompareExchange(&e->BootstrapOutcome,BOOT_FAILED,BOOT_PENDING);
        if(InterlockedCompareExchange(&e->BootstrapOutcome,0,0)==BOOT_CANCELLED)
            e->BootstrapStatus=STATUS_CANCELLED;
        /* INITIALIZE is still pending: class timers cannot be assumed to
         * run. This LOW callback must perform the passive failure cleanup
         * itself, before handing failure back to the class gate. */
        if(e->Thread) {
            e->Job=JOB_EXIT; KeSetEvent(&e->JobReady,IO_NO_INCREMENT,FALSE);
            status=JoinThread(e);
            if(status!=STATUS_SUCCESS) { InterlockedExchange(&e->JoinFailed,1); return; }
            status=CloseThread(e);
            if(status!=STATUS_SUCCESS) { InterlockedExchange(&e->JoinFailed,1); return; }
            e->Thread=NULL;
        }
    }
    InterlockedExchange(&e->BootstrapDone,1);
    UVTRACE("BOOTSTRAP_DONE_PUBLISHED\nLOW_TO_HIGH\n");
    /* Documented LOW->HIGH transition of the sole device routine, not a
     * second independent callback. LAST access: callback may run inline. */
    StreamClassCallAtNewPriority(NULL,e->Device,LowToHigh,BootstrapComplete,e);
}
static VOID STREAMAPI BootstrapComplete(PVOID context)
{
    UVWORK *e=(UVWORK *)context;
    PHW_STREAM_REQUEST_BLOCK s=e->Initialize;
    LONG outcome=InterlockedCompareExchange(&e->BootstrapOutcome,0,0);
    UVTRACE("BOOTSTRAP_HIGH_ENTRY\n");
    if(outcome!=BOOT_COMMITTED) {
        ASSERT(!e->Thread);
        s->ActualBytesTransferred=0; s->Status=e->BootstrapStatus;
        UVTRACE("INIT_COMPLETE_FAILURE\n");
        Destroy(e); UvPortNtSerialNotify(s,NULL,1,NULL); return;
    }
    e->BootHandled=1; e->Initialize=NULL;
    s->Status=STATUS_SUCCESS; s->ActualBytesTransferred=0;
    e->TimerArmed=1;
    UVTRACE("TIMER_ARM_INITIAL\n");
    StreamClassScheduleTimer(NULL,e->Device,TIMER_US,Timer,e);
    UVTRACE("INIT_COMPLETE\n");
    UvPortNtSerialNotify(s,NULL,1,NULL); /* LAST engine/class access */
}
NTSTATUS UvwInitialize(UVWORK_ANCHOR *a, PHW_STREAM_REQUEST_BLOCK s,PDEVICE_OBJECT fdo)
{
    UVWORK *e;
    UVTRACE("WORK_INIT_ENTRY\n");
    if(!a || !s || !fdo || a->Engine) return STATUS_INVALID_PARAMETER;
    e=(UVWORK *)ExAllocatePoolWithTag(NonPagedPool,sizeof(*e),WORK_TAG);
    if(!e) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(e,sizeof(*e)); UvPortInitialize(&e->Port);
    e->JoinItem=IoAllocateWorkItem(fdo);
    if(!e->JoinItem) { ExFreePool(e); return STATUS_INSUFFICIENT_RESOURCES; }
    ObReferenceObject(fdo); e->Fdo=fdo; e->Anchor=a; e->Device=s->HwDeviceExtension;
#ifdef UVWORK_USB
    e->Lower=a->Lower; e->Selected=-1;
#endif
    e->Initialize=s; s->TimeoutCounter=0; a->Engine=e;
    KeInitializeEvent(&e->JobReady,SynchronizationEvent,FALSE);
#ifndef UVWORK_TEST
    KeInitializeEvent(&e->Exited,NotificationEvent,FALSE);
    e->WaitThread=IoIsWdmVersionAvailable(1,0x10);
#endif
    /* Class device queue remains not-ready while INITIALIZE is pending. */
    UVTRACE("LOW_SCHEDULE\n");
    StreamClassCallAtNewPriority(NULL,e->Device,Low,Bootstrap,e);
    return STATUS_PENDING;
}
int UvwControl(UVWORK_ANCHOR *a,PHW_STREAM_REQUEST_BLOCK s,unsigned int command,unsigned int state)
{
    int r;
    if(!a || !a->Engine || !a->Engine->BootHandled || a->Engine->Initialize) return UVFLOW_NOT_READY;
    ++a->Engine->CallbackDepth;
    r=UvPortControl(&a->Engine->Port,s,command,state);
    --a->Engine->CallbackDepth; return r;
}
#ifdef UVWORK_USB
const UVFMT *UvwFormats(UVWORK_ANCHOR *a)
{ return a && a->Engine && a->Engine->BootHandled ? &a->Engine->Formats : NULL; }
/* OPEN_STREAM, class-serialized, worker idle for this stream: record the
 * opened format so the next start and every READ use its exact size. */
int UvwSelected(UVWORK_ANCHOR *a)
{ return a && a->Engine ? a->Engine->Selected : -1; }
int UvwSelectFormat(UVWORK_ANCHOR *a,int index)
{
    UVWORK *e=a?a->Engine:NULL;
    if(!e || index<0 || (unsigned int)index>=e->Formats.Count) return 0;
    e->Selected=index; e->UsbStatus=STATUS_SUCCESS;
    e->Port.FrameBytes=e->Formats.Entry[index].FrameBytes;
    e->Pictures=e->Drops=0;
    return 1;
}
void UvwFrameCounts(UVWORK_ANCHOR *a,ULONG *pictures,ULONG *drops,ULONG *frameBytes)
{
    UVWORK *e=a?a->Engine:NULL;
    *pictures=*drops=*frameBytes=0;
    if(!e) return;
    *pictures=e->Pictures+e->Drops; *drops=e->Drops; *frameBytes=e->Port.FrameBytes;
}
#endif
int UvwOwnsInitialize(UVWORK_ANCHOR *a,const void *s)
{ return a && a->Engine && s && a->Engine->Initialize==s; }
int UvwRead(UVWORK_ANCHOR *a,PHW_STREAM_REQUEST_BLOCK s)
{
    UVOWN_TOKEN token;
    int r;
    if(!a || !a->Engine || !a->Engine->BootHandled || a->Engine->Initialize) return UVFLOW_NOT_READY;
    ++a->Engine->CallbackDepth;
    r=UvPortRead(&a->Engine->Port,s,&token);
    --a->Engine->CallbackDepth; return r;
}
void UvwCancel(UVWORK_ANCHOR *a,const void *s)
{
    UVWORK *e=a?a->Engine:NULL;
    if(!e) return;
    if(s==e->Initialize) {
        /* Losing CAS must not modify the committed result. */
        InterlockedCompareExchange(&e->BootstrapOutcome,BOOT_CANCELLED,BOOT_PENDING);
        return;
    }
    ++e->CallbackDepth;
    UvPortCancel(&e->Port,s);
    --e->CallbackDepth;
}
static int MapJob(UVWORK *e)
{
    unsigned int i;
    for(i=0;i<UVOWN_CAPACITY;++i) {
        UVOWN_ENTRY *entry=&e->Port.Flow.Own.Entry[i];
        if(entry->Request && entry->Kind==UVOWN_READ && entry->Phase==UVOWN_ADMITTING &&
            !e->Port.Flow.Slot[i].AdmissionDone) {
            e->MapRequest=e->Port.Binding[i].Request; e->MapToken=e->Port.Binding[i].Token;
            e->Job=JOB_MAP; e->Busy=1;
            KeSetEvent(&e->JobReady,IO_NO_INCREMENT,FALSE); return 1;
        }
    }
    return 0;
}
static void Deliver(UVWORK *e,UVPORT_WORK work,unsigned int bytes)
{
    UVPORT_COMPLETION c;
#ifdef UVWORK_USB
    /* Design 7 / DDK "Video Stream Extended Headers": a capture pin's
     * stream header carries a KS_FRAME_INFO (StreamHeaderMediaSpecific). */
    PKSSTREAM_HEADER h=work.Binding.Read ? work.Binding.Header : NULL;
    if(h && h->Size>=sizeof(KSSTREAM_HEADER)+sizeof(KS_FRAME_INFO)) {
        PKS_FRAME_INFO fi=(PKS_FRAME_INFO)(h+1);
        fi->ExtendedHeaderSize=sizeof(KS_FRAME_INFO);
        fi->dwFrameFlags=KS_VIDEO_FLAG_FRAME|KS_VIDEO_FLAG_I_FRAME;
        fi->PictureNumber=(LONGLONG)(e->Pictures+e->Drops);
        fi->DropCount=(LONGLONG)e->Drops;
    }
#endif
    if(UvPortFinish(&e->Port,work,UVFLOW_OK,bytes,&c)==UVFLOW_OK) {
        UvPortDeliver(&c,UvPortNtSerialNotify,NULL);
        UvPortReturned(&e->Port); /* engine is private and UNINIT not yet handed off */
    }
}
static VOID STREAMAPI Timer(PVOID context)
{
    UVWORK *e=(UVWORK *)context;
    UVPORT_WORK work; UVPORT_COMPLETION c;
    unsigned int n,joined,drained;
#ifdef UVWORK_VM_TRACE
    if(e->TraceTimers<8) { ++e->TraceTimers; UVTRACE("TIMER_ENTRY\n"); }
#endif
    ASSERT(e->TimerArmed && KeGetCurrentIrql()==DISPATCH_LEVEL);
    e->TimerArmed=0; ++e->CallbackDepth;
    if(!InterlockedCompareExchange(&e->BootstrapDone,0,0)) goto rearm;
    if(e->Exiting) {
        if(InterlockedCompareExchange(&e->Joined,0,0)) {
            joined=e->Thread==NULL;
            /* Runtime facts plus the published default class nonreentrancy
             * contract: only this timer is in the gate, no timer pending,
             * LOW bootstrap published its final engine access, worker joined.
             * This is not a counter proof for synchronization-OFF callbacks. */
            drained=e->CallbackDepth==1 && !e->TimerArmed &&
                InterlockedCompareExchange(&e->BootstrapDone,0,0) && !e->Port.Flow.Notifying;
            if(UvPortAfterJoin(&e->Port,joined,drained,&c)==UVFLOW_OK) {
                Destroy(e); UvPortDeliver(&c,UvPortNtSerialNotify,NULL); return;
            }
        }
    } else {
        if(e->Busy && InterlockedCompareExchange(&e->Done,0,0)) {
            InterlockedExchange(&e->Done,0); e->Busy=0;
            if(e->Job==JOB_MAP) UvPortReady(&e->Port,&e->Mapping);
            else {
                /* A started camera that timed out completes the read empty;
                 * a failed start still fails it (UVPORT.AllowEmpty). */
#ifdef UVWORK_USB
                e->Port.AllowEmpty=e->EmptyOk;
#endif
                Deliver(e,e->Work,e->FrameBytes);
                e->Port.AllowEmpty=0;
            }
        }
        for(n=0;n<PUMP_BUDGET && !e->Busy;++n) {
            if(MapJob(e)) break;
            if(UvPortNext(&e->Port,Tick(),&work)!=UVFLOW_OK || work.Flow.Action==UVFLOW_WAIT) break;
            if(work.Flow.Action==UVFLOW_EXIT) { ExitWorker(e); break; }
            if(work.Flow.Action==UVFLOW_READ) {
                UVOWN_ENTRY entry;
                UvOwnSnapshot(&e->Port.Flow.Own,work.Flow.Token,&entry);
                if(entry.Cancelled || e->Port.Flow.Slot[work.Flow.Token.Slot].MappingFailed) Deliver(e,work,0);
                else {
                    e->Work=work; e->Job=JOB_FRAME; e->Busy=1;
                    KeSetEvent(&e->JobReady,IO_NO_INCREMENT,FALSE);
                }
            }
#ifdef UVWORK_USB
            else if(e->Usb && work.Flow.Action==UVFLOW_CONTROL &&
                (work.Flow.Command==UVFLOW_CLOSE || work.Flow.Command==UVFLOW_POWER_DOWN ||
                 (work.Flow.Command==UVFLOW_SET_STATE && work.Flow.State==UVFLOW_STOP))) {
                /* Passive camera stop first; the control completes when done. */
                e->Work=work; e->Job=JOB_USBSTOP; e->Busy=1;
                KeSetEvent(&e->JobReady,IO_NO_INCREMENT,FALSE);
            }
#endif
            else Deliver(e,work,0);
        }
    }
    /* Exactly one future class timer. It remains armed until joined, then
     * the CURRENT timer performs final notification without rearming. */
rearm:
    --e->CallbackDepth; e->TimerArmed=1;
    StreamClassScheduleTimer(NULL,e->Device,TIMER_US,Timer,e);
}
