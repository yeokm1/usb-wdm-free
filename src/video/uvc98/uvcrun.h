/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
#ifndef UVC_SIM_RUN_H
#define UVC_SIM_RUN_H
#include "uvcgen.h"
typedef struct _SIM_REQUEST {
    PHW_STREAM_REQUEST_BLOCK Next;
    PUCHAR Mapped;
    PKSSTREAM_HEADER Header;
    ULONG Phase;
    BOOLEAN Cancelled;
} SIM_REQUEST;
typedef struct _SIM_ENGINE {
    KMUTEX Operation;
    KSPIN_LOCK QueueLock;
    KEVENT Stop;
    KEVENT ReadsIdle, WorkerIdle;
    HANDLE Thread;
    PHW_STREAM_OBJECT OpenStream;
    PHW_STREAM_REQUEST_BLOCK Head, Tail;
    LONG State;
    ULONG Queued, Frame, InFlight;
    LARGE_INTEGER Time;
    BOOLEAN Removed, PowerBlocked, Closing;
    /* Diagnostic snapshots only; not physical-camera counters. */
    ULONG Reads, Rejected, Frames, Cancelled;
    NTSTATUS LastReadStatus;
    ULONG LastBytes, LastExtent, LastMdlBytes, LastBuffers, LastHeaderSize;
} SIM_ENGINE;
NTSTATUS SimInitialize(SIM_ENGINE *engine);
NTSTATUS SimOpen(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream);
NTSTATUS SimClose(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream);
NTSTATUS SimState(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream, KSSTATE state);
NTSTATUS SimGetState(SIM_ENGINE *engine, PHW_STREAM_OBJECT stream, KSSTATE *state);
void SimRead(SIM_ENGINE *engine, PHW_STREAM_REQUEST_BLOCK request);
void SimCancel(SIM_ENGINE *engine, PHW_STREAM_REQUEST_BLOCK request);
void SimPower(SIM_ENGINE *engine, DEVICE_POWER_STATE state);
void SimShutdown(SIM_ENGINE *engine);
#endif
