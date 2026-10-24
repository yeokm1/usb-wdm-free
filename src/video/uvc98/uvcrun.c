/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original NT2000+ synthetic STREAM.SYS worker, no USB or class timer.
 * Normal callbacks are PASSIVE with TurnOffSynchronization=TRUE.
 * Cancel/timeout may be DISPATCH; they share the bounded request lock.
 * Class owns SRBs, headers and locked MDLs until our completion notification.
 */
#include <strmini.h>
#include "uvcrun.h"

#define SIM_INTERVAL 1000000
#define SIM_QUEUE_LIMIT 8
#define PHASE_QUEUED 1
#define PHASE_WORKER 2
#define PHASE_COMPLETING 3

static void Complete(SIM_ENGINE *engine, PHW_STREAM_REQUEST_BLOCK srb,
                     NTSTATUS status, ULONG bytes)
{
    SIM_REQUEST *request = (SIM_REQUEST *)srb->SRBExtension;
    PHW_STREAM_OBJECT stream = srb->StreamObject;
    KIRQL old;
    KeAcquireSpinLock(&engine->QueueLock, &old);
    request->Phase = PHASE_COMPLETING;
    if (request->Cancelled) status = STATUS_CANCELLED;
    if (!NT_SUCCESS(status)) bytes = 0;
    if (status == STATUS_CANCELLED) engine->Cancelled++;
    engine->LastReadStatus = status;
    engine->LastBytes = bytes;
    if (bytes) engine->Frames++;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (request->Header) {
        request->Header->DataUsed = bytes;
        if (bytes) {
            request->Header->Duration = SIM_INTERVAL;
            request->Header->PresentationTime.Time = engine->Time.QuadPart;
            request->Header->PresentationTime.Numerator = 1;
            request->Header->PresentationTime.Denominator = 1;
            request->Header->OptionsFlags = KSSTREAM_HEADER_OPTIONSF_TIMEVALID |
                KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;
        }
    }
    srb->ActualBytesTransferred = bytes;
    srb->Status = status;
    /* Never access srb/request/header/mapped memory after this handoff. */
    StreamClassStreamNotification(StreamRequestComplete, stream, srb);
    /* Close/uninitialize waits for this count before class extensions may
     * be released, including a concurrent DPC cancellation completion. */
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (!--engine->InFlight) KeSetEvent(&engine->ReadsIdle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&engine->QueueLock, old);
}

static PHW_STREAM_REQUEST_BLOCK Pop(SIM_ENGINE *engine)
{
    PHW_STREAM_REQUEST_BLOCK srb = engine->Head;
    if (srb) {
        SIM_REQUEST *request = (SIM_REQUEST *)srb->SRBExtension;
        engine->Head = request->Next;
        if (!engine->Head) engine->Tail = NULL;
        engine->Queued--;
        request->Next = NULL;
        request->Phase = PHASE_WORKER;
        srb->TimeoutCounter = srb->TimeoutOriginal;
    }
    return srb;
}

static VOID Worker(PVOID context)
{
    SIM_ENGINE *engine = (SIM_ENGINE *)context;
    LARGE_INTEGER period;
    period.QuadPart = -SIM_INTERVAL;
    for (;;) {
        NTSTATUS wait = KeWaitForSingleObject(&engine->Stop, Executive, KernelMode, FALSE, &period);
        PHW_STREAM_REQUEST_BLOCK srb = NULL;
        KIRQL old;
        if (wait != STATUS_TIMEOUT) break;
        KeAcquireSpinLock(&engine->QueueLock, &old);
        if (engine->State == KSSTATE_RUN && !engine->Closing && !engine->Removed &&
            !engine->PowerBlocked && engine->OpenStream) {
            engine->Frame++;
            engine->Time.QuadPart += SIM_INTERVAL;
            srb = Pop(engine);
            if (srb) KeClearEvent(&engine->WorkerIdle);
        }
        KeReleaseSpinLock(&engine->QueueLock, old);
        if (srb) {
            SIM_REQUEST *request = (SIM_REQUEST *)srb->SRBExtension;
            ULONG bytes = UvcSimGenerate(request->Mapped, UVCSIM_BYTES, engine->Frame);
            Complete(engine, srb, bytes == UVCSIM_BYTES ? STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE, bytes);
            KeSetEvent(&engine->WorkerIdle, IO_NO_INCREMENT, FALSE);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS SimInitialize(SIM_ENGINE *engine)
{
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    RtlZeroMemory(engine, sizeof(*engine));
    KeInitializeMutex(&engine->Operation, 0);
    KeInitializeSpinLock(&engine->QueueLock);
    KeInitializeEvent(&engine->Stop, NotificationEvent, FALSE);
    KeInitializeEvent(&engine->ReadsIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&engine->WorkerIdle, NotificationEvent, TRUE);
    engine->State = KSSTATE_STOP;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&engine->Thread, THREAD_ALL_ACCESS, &attributes,
                                  NULL, NULL, Worker, engine);
    if (!NT_SUCCESS(status)) { engine->Thread = NULL; engine->Removed = TRUE; }
    return status;
}

static void Drain(SIM_ENGINE *engine)
{
    PHW_STREAM_REQUEST_BLOCK srb;
    KIRQL old;
    for (;;) {
        KeAcquireSpinLock(&engine->QueueLock, &old);
        srb = Pop(engine);
        if (srb) ((SIM_REQUEST *)srb->SRBExtension)->Cancelled = TRUE;
        KeReleaseSpinLock(&engine->QueueLock, old);
        if (!srb) break;
        Complete(engine, srb, STATUS_CANCELLED, 0);
    }
    KeWaitForSingleObject(&engine->ReadsIdle, Executive, KernelMode, FALSE, NULL);
}

NTSTATUS SimOpen(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream)
{
    KIRQL old;
    NTSTATUS status = STATUS_SUCCESS;
    KeWaitForSingleObject(&engine->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (engine->Removed || engine->PowerBlocked || engine->Closing || !engine->Thread)
        status = STATUS_DEVICE_NOT_READY;
    else if (engine->OpenStream) status = STATUS_DEVICE_BUSY;
    else {
        engine->OpenStream = stream;
        engine->State = KSSTATE_STOP;
        engine->Time.QuadPart = 0;
        engine->Frame = 0;
    }
    KeReleaseSpinLock(&engine->QueueLock, old);
    KeReleaseMutex(&engine->Operation, FALSE);
    return status;
}

NTSTATUS SimClose(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream)
{
    KIRQL old;
    NTSTATUS status = STATUS_SUCCESS;
    KeWaitForSingleObject(&engine->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (engine->OpenStream != stream && !engine->Removed) status = STATUS_INVALID_PARAMETER;
    else { engine->Closing = TRUE; engine->State = KSSTATE_STOP; }
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (NT_SUCCESS(status)) {
        Drain(engine);
        KeWaitForSingleObject(&engine->WorkerIdle, Executive, KernelMode, FALSE, NULL);
        KeAcquireSpinLock(&engine->QueueLock, &old);
        engine->OpenStream = NULL;
        engine->Closing = FALSE;
        KeReleaseSpinLock(&engine->QueueLock, old);
    }
    KeReleaseMutex(&engine->Operation, FALSE);
    return status;
}

NTSTATUS SimState(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream, KSSTATE state)
{
    KIRQL old;
    NTSTATUS status = STATUS_SUCCESS;
    if (state > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&engine->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (engine->OpenStream != stream || engine->Removed || engine->Closing ||
        engine->PowerBlocked) status = STATUS_DEVICE_NOT_READY;
    else engine->State = state;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (NT_SUCCESS(status) && state != KSSTATE_RUN) {
        if (state == KSSTATE_STOP) Drain(engine);
        KeWaitForSingleObject(&engine->WorkerIdle, Executive, KernelMode, FALSE, NULL);
    }
    KeReleaseMutex(&engine->Operation, FALSE);
    return status;
}

NTSTATUS SimGetState(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream, KSSTATE *state)
{
    KIRQL old;
    NTSTATUS status;
    KeAcquireSpinLock(&engine->QueueLock, &old);
    status = engine->OpenStream == stream ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    *state = (KSSTATE)engine->State;
    KeReleaseSpinLock(&engine->QueueLock, old);
    return status;
}

void SimRead(SIM_ENGINE *engine, PHW_STREAM_REQUEST_BLOCK srb)
{
    SIM_REQUEST *request = (SIM_REQUEST *)srb->SRBExtension;
    NTSTATUS status = STATUS_SUCCESS;
    PMDL mdl;
    KIRQL old;
    PKSSTREAM_HEADER header = NULL;
    RtlZeroMemory(request, sizeof(*request));
    srb->ActualBytesTransferred = 0;
    KeAcquireSpinLock(&engine->QueueLock, &old);
    engine->Reads++;
    engine->LastBuffers = srb->NumberOfBuffers;
    if (!engine->InFlight++) KeClearEvent(&engine->ReadsIdle);
    KeReleaseSpinLock(&engine->QueueLock, old);
    /* KsProbeStreamIrp documents payload MDLs in Irp->MdlAddress and
     * headers in SystemBuffer. Mapping already incorporates byte offset.
     * Class owns locking/unlocking; never change the header Data pointer. */
    mdl = srb->Irp ? srb->Irp->MdlAddress : NULL;
    if (srb->NumberOfBuffers != 1 || !srb->CommandData.DataBufferArray || !mdl || mdl->Next)
        status = STATUS_NOT_SUPPORTED;
    else {
        header = srb->CommandData.DataBufferArray;
        engine->LastHeaderSize = header->Size;
        engine->LastExtent = header->FrameExtent;
        engine->LastMdlBytes = MmGetMdlByteCount(mdl);
        if (header->Size < sizeof(KSSTREAM_HEADER) || header->FrameExtent < UVCSIM_BYTES ||
            header->FrameExtent > MmGetMdlByteCount(mdl) || header->OptionsFlags)
            status = STATUS_INVALID_BUFFER_SIZE;
        else {
            request->Mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
            if (!request->Mapped) status = STATUS_INSUFFICIENT_RESOURCES;
            else request->Header = header;
        }
    }
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (NT_SUCCESS(status)) {
        if (engine->OpenStream != srb->StreamObject || engine->Removed || engine->Closing ||
            engine->PowerBlocked || engine->State < KSSTATE_PAUSE) status = STATUS_CANCELLED;
        else if (engine->Queued >= SIM_QUEUE_LIMIT) status = STATUS_DEVICE_BUSY;
        else {
            request->Phase = PHASE_QUEUED;
            srb->TimeoutCounter = 0;
            if (engine->Tail) ((SIM_REQUEST *)engine->Tail->SRBExtension)->Next = srb;
            else engine->Head = srb;
            engine->Tail = srb;
            engine->Queued++;
        }
    }
    if (!NT_SUCCESS(status)) engine->Rejected++;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (!NT_SUCCESS(status)) Complete(engine, srb, status, 0);
}

void SimCancel(SIM_ENGINE *engine, PHW_STREAM_REQUEST_BLOCK srb)
{
    SIM_REQUEST *request = (SIM_REQUEST *)srb->SRBExtension;
    PHW_STREAM_REQUEST_BLOCK current, previous = NULL;
    BOOLEAN complete = FALSE;
    KIRQL old;
    KeAcquireSpinLock(&engine->QueueLock, &old);
    if (request->Phase == PHASE_QUEUED) {
        for (current = engine->Head; current; current = ((SIM_REQUEST *)current->SRBExtension)->Next) {
            if (current == srb) {
                if (previous) ((SIM_REQUEST *)previous->SRBExtension)->Next = request->Next;
                else engine->Head = request->Next;
                if (engine->Tail == srb) engine->Tail = previous;
                engine->Queued--;
                request->Next = NULL;
                request->Phase = PHASE_COMPLETING;
                request->Cancelled = TRUE;
                complete = TRUE;
                break;
            }
            previous = current;
        }
    } else if (request->Phase == PHASE_WORKER) request->Cancelled = TRUE;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (complete) Complete(engine, srb, STATUS_CANCELLED, 0);
}

void SimPower(SIM_ENGINE *engine, DEVICE_POWER_STATE state)
{
    KIRQL old;
    KeWaitForSingleObject(&engine->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    engine->PowerBlocked = state != PowerDeviceD0;
    if (engine->PowerBlocked) engine->State = KSSTATE_STOP;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (engine->PowerBlocked) {
        Drain(engine);
        KeWaitForSingleObject(&engine->WorkerIdle, Executive, KernelMode, FALSE, NULL);
    }
    KeReleaseMutex(&engine->Operation, FALSE);
}

void SimShutdown(SIM_ENGINE *engine)
{
    KIRQL old;
    KeWaitForSingleObject(&engine->Operation, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    engine->Removed = TRUE;
    engine->State = KSSTATE_STOP;
    engine->Closing = TRUE;
    KeReleaseSpinLock(&engine->QueueLock, old);
    if (engine->Thread) {
        KeSetEvent(&engine->Stop, IO_NO_INCREMENT, FALSE);
        ZwWaitForSingleObject(engine->Thread, FALSE, NULL);
        ZwClose(engine->Thread);
        engine->Thread = NULL;
    }
    Drain(engine);
    KeAcquireSpinLock(&engine->QueueLock, &old);
    engine->OpenStream = NULL;
    KeReleaseSpinLock(&engine->QueueLock, old);
    KeReleaseMutex(&engine->Operation, FALSE);
}
