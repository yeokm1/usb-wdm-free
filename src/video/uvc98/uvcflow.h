/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original asynchronous synthetic-stream policy; no Windows objects/USB.
 * All operations require one external lock. Request/stream addresses are
 * opaque. Output storage must not alias the flow or ownership table.
 * One persistent worker calls Next/Finish. It never waits in notification.
 */
#ifndef UVC_FLOW_H
#define UVC_FLOW_H
#include "uvcown.h"
#define UVFLOW_NORMAL_LIMIT 24u
#define UVFLOW_INTERVAL_MS 100u
enum UVFLOW_COMMAND {
    UVFLOW_OPEN=1, UVFLOW_SET_STATE, UVFLOW_GET_STATE, UVFLOW_CLOSE,
    UVFLOW_POWER_DOWN, UVFLOW_POWER_UP, UVFLOW_UNINITIALIZE,
    UVFLOW_FLUSH, UVFLOW_END_FLUSH
};
enum UVFLOW_STATE { UVFLOW_STOP=0, UVFLOW_ACQUIRE, UVFLOW_PAUSE, UVFLOW_RUN };
enum UVFLOW_RESULT {
    UVFLOW_OK=0, UVFLOW_INVALID, UVFLOW_CLOSED, UVFLOW_FULL,
    UVFLOW_CANCELLED, UVFLOW_MAPPING, UVFLOW_NOT_READY, UVFLOW_BUSY,
    UVFLOW_DUPLICATE, UVFLOW_EXHAUSTED
};
enum UVFLOW_ACTION { UVFLOW_WAIT=0, UVFLOW_READ, UVFLOW_CONTROL, UVFLOW_EXIT };
typedef struct UVFLOW_SLOT {
    const void *Stream;
    unsigned int Command, State, AdmissionDone, MappingFailed;
} UVFLOW_SLOT;
typedef struct UVFLOW_WORK {
    UVOWN_TOKEN Token;
    const void *Request, *Stream;
    unsigned int Action, Command, State, Frame, Tick;
} UVFLOW_WORK;
typedef struct UVFLOW_COMPLETION {
    const void *Request, *Stream;
    unsigned int Command, State, Result, Bytes, Final;
} UVFLOW_COMPLETION;
typedef struct UVFLOW {
    UVOWN_TABLE Own;
    UVFLOW_SLOT Slot[UVOWN_CAPACITY];
    const void *Stream;
    UVOWN_TOKEN Active, Uninitialize;
    UVFLOW_WORK Running;
    unsigned int State, PowerBlocked, Flushing, Closing, Removing, Faulted;
    unsigned int Busy, Notifying, ExitIssued, FinalIssued;
    unsigned int Deadline, Frame;
} UVFLOW;
void UvFlowInitialize(UVFLOW *flow);
/* Ordinary requests leave teardown capacity. CLOSE can use slot31;
 * UNINITIALIZE can use slot32, closes both gates, and is never cancelled.
 * Teardown begins blocking/cancelling reads at admission, not at execution.
 * DUPLICATE leaves existing ownership intact: a caller MUST NOT complete
 * a second rejected callback on that same already-owned class request.
 */
int UvFlowControl(UVFLOW *flow, const void *request, const void *stream,
                  unsigned int command, unsigned int state);
/* Register before mapping; Ready must eventually be called even if mapping
 * fails/cancellation arrives. The worker never touches an unfinished admission.
 */
int UvFlowRead(UVFLOW *flow, const void *request, const void *stream,
               UVOWN_TOKEN *token);
int UvFlowReady(UVFLOW *flow, UVOWN_TOKEN token, int mappingSucceeded);
int UvFlowCancel(UVFLOW *flow, const void *request);
/* Tick is monotonic milliseconds modulo32, sampled at least every2^31ms
 * while RUN. Frame scheduling never catches up with a burst after a delay.
 * Next has no side effects on class memory. WAIT includes admission/PAUSE/
 * deadline waits. EXIT retains UNINITIALIZE: it is NOT a completion ticket.
 */
int UvFlowNext(UVFLOW *flow, unsigned int now, UVFLOW_WORK *work);
/* Finish retires ownership BEFORE returning the completion ticket, sets a
 * notification gate, and never calls external code. Caller writes status and
 * notifies outside the lock, then calls Returned after ordinary notification.
 * Reentrant callbacks may enqueue/cancel but cannot run another worker step.
 */
int UvFlowFinish(UVFLOW *flow, UVFLOW_WORK work, unsigned int result,
                 unsigned int bytes, UVFLOW_COMPLETION *completion);
int UvFlowReturned(UVFLOW *flow);
/* External joiner only, after worker exit AND separately proven callback
 * rundown. False flags fail closed. These flags are caller obligations, NOT
 * a portable proof of Stream Class/FDO lifetime. Final ticket has no Returned:
 * notification may destroy all device-private storage.
 */
int UvFlowAfterJoin(UVFLOW *flow, int workerJoined, int callbacksDrained,
                    UVFLOW_COMPLETION *completion);
#endif
