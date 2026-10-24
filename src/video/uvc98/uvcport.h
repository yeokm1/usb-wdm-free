/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original STREAM.SYS request adapter. Not linked into the M0 miniport.
 */
#ifndef UVC_PORT_H
#define UVC_PORT_H
#include <strmini.h>
#include "uvcflow.h"
#include "uvcgen.h"
typedef PUCHAR (*UVPORT_MAP)(PMDL mdl, PVOID context);
typedef VOID (*UVPORT_NOTIFY)(PHW_STREAM_REQUEST_BLOCK request,
    PHW_STREAM_OBJECT stream, int deviceRequest, PVOID context);
typedef struct UVPORT_BINDING {
    UVOWN_TOKEN Token;
    PHW_STREAM_REQUEST_BLOCK Request;
    PHW_STREAM_OBJECT Stream;
    PKSSTREAM_HEADER Header;
    PUCHAR Data;
    unsigned int Read;
} UVPORT_BINDING;
typedef struct UVPORT {
    UVFLOW Flow;
    UVPORT_BINDING Binding[UVOWN_CAPACITY];
    unsigned int FrameBytes; /* exact READ payload; UVCSIM_BYTES unless M2 selects a format */
    unsigned int AllowEmpty; /* M2: a read with no frame (camera timeout) completes empty, not failed */
} UVPORT;
typedef struct UVPORT_MAPPING {
    UVOWN_TOKEN Token;
    PHW_STREAM_REQUEST_BLOCK Request;
    PKSSTREAM_HEADER Header;
    PUCHAR Data;
    unsigned int Valid, Cancelled;
} UVPORT_MAPPING;
typedef struct UVPORT_WORK {
    UVFLOW_WORK Flow;
    UVPORT_BINDING Binding;
} UVPORT_WORK;
typedef struct UVPORT_COMPLETION {
    UVFLOW_COMPLETION Flow;
    UVPORT_BINDING Binding;
} UVPORT_COMPLETION;

/* Initialize before publication. Every function taking a UVPORT pointer
 * requires the SAME external lock. No allocation, waiting or notification
 * occurs while holding that lock. One persistent passive worker only.
 * Caller must independently prove callback/worker lifetime through final
 * UNINITIALIZE; this module does NOT establish rundown or SRB-address ABA.
 */
void UvPortInitialize(UVPORT *port);
/* command/state already validated by dispatch; incoming class SRB valid.
 * DUPLICATE must never be completed by the rejecting callback. Other
 * rejection is caller-owned and not a completion ticket from this module.
 */
int UvPortControl(UVPORT *port, PHW_STREAM_REQUEST_BLOCK request,
    unsigned int command, unsigned int state);
int UvPortRead(UVPORT *port, PHW_STREAM_REQUEST_BLOCK request,
    UVOWN_TOKEN *token);
/* Outside lock, ONLY after successful UvPortRead. ADMITTING ownership keeps
 * SRB/header/IRP/MDL alive; caller eventually publishes Ready even on failure.
 * Mapper handles already-locked MDL, never locks/unlocks/frees/unmaps it.
 * This helper never trusts Header.Data or applies an MDL offset twice.
 */
void UvPortMap(PHW_STREAM_REQUEST_BLOCK request, UVOWN_TOKEN token,
    UVPORT_MAP mapper, PVOID context, UVPORT_MAPPING *mapping);
/* Same, requiring FrameExtent >= frameBytes instead of UVCSIM_BYTES. */
void UvPortMapBytes(PHW_STREAM_REQUEST_BLOCK request, UVOWN_TOKEN token,
    unsigned int frameBytes, UVPORT_MAP mapper, PVOID context, UVPORT_MAPPING *mapping);
int UvPortReady(UVPORT *port, const UVPORT_MAPPING *mapping);
/* Opaque address only; no SRB/extension access. Caller lifetime/ABA proof
 * must cover callbacks already entering or waiting for the external lock. */
int UvPortCancel(UVPORT *port, const void *request);
int UvPortNext(UVPORT *port, unsigned int tick, UVPORT_WORK *work);
/* Worker writes only its copied binding Data, before Finish. Finish uses
 * canonical owned request/token; clears binding and retires before delivery.
 * No class-memory access after Deliver. Returned touches only persistent
 * driver-owned storage, whose lifetime caller must retain until worker exit.
 */
int UvPortFinish(UVPORT *port, UVPORT_WORK work, unsigned int result,
    unsigned int bytes, UVPORT_COMPLETION *completion);
int UvPortReturned(UVPORT *port);
int UvPortAfterJoin(UVPORT *port, int workerJoined, int callbacksDrained,
    UVPORT_COMPLETION *completion);
/* Outside lock. Exact ticket produced by successful Finish/AfterJoin only.
 * Notify may synchronously reenter or release ALL class objects. It is the
 * last class access. Choose exactly the hook for the driver's synchronization
 * mode; synchronization ON also requires class callbacks/HIGH priority gate.
 * Delivery is once-only by caller; tickets must not be replayed.
 */
void UvPortDeliver(const UVPORT_COMPLETION *completion, UVPORT_NOTIFY notify,
    PVOID context);
/* Real kernel mapping/notification hooks, separately compile-checked. */
PUCHAR UvPortNtMap(PMDL mdl, PVOID context);
VOID UvPortNtNotify(PHW_STREAM_REQUEST_BLOCK request,
    PHW_STREAM_OBJECT stream, int deviceRequest, PVOID context);
VOID UvPortNtSerialNotify(PHW_STREAM_REQUEST_BLOCK request,
    PHW_STREAM_OBJECT stream, int deviceRequest, PVOID context);
#endif
