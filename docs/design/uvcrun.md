# Synthetic STREAM.SYS RUN experiment: ownership design

This is a **partially implemented policy redesign**, following review of the unwired
`src/video/uvc98/uvcrun.c/.h` draft. The tested M0 remains STOP-only. The
draft must not be linked into the tested package until these requirements
are implemented and checked. There is no USB or real-camera claim here.

`uvcflow.c/.h` now implements the portable asynchronous policy below, using
the actual ownership table. `flowtest.cmd` exercises notification reentry and
teardown gates. It is excluded from the M0 kernel build; the Windows worker,
callback rundown and external joiner were initially unimplemented. Passing these
policy tests is not permission to link the original unsafe `uvcrun.c`.

The [actual-SRB adapter](../testing/2026-10-21-uvc-port.md) now adds bounded
single-MDL mapping and retire-before-notify delivery, with883 guarded host
checks and x86/amd64 object compilation. It remains outside M0. The proposed
next gate is default class serialization with passive retained jobs and
class HIGH/timer delivery, rather than assuming arbitrary concurrent
callbacks are drained by a counter. Details and unresolved handoffs are in
that record.

The isolated [M1 worker preflight](../testing/2026-10-21-uvc-worker.md) now
implements a different, synchronization-ON route in `uvcwork.c` and
`uvcm1.c`, leaving M0 and the unsafe draft separate. It has actual engine
and dispatch host guards plus x86 link/amd64 compile checks. The first
Windows2000 install remained pending before READ/RUN. A revised LOW-to-HIGH
handoff advances to INIT completion and timer dispatch, but initial Code10
and a black-screen reboot prevent capture/cleanup proof. Failed-start
cleanup and timer-during-UNINITIALIZE remain unresolved. The
synchronization-OFF discussion below is historical policy motivation;
it is not the synchronization contract selected by M1.

## Why the first worker draft cannot be used

The frame worker notifies `StreamRequestComplete`, then retires InFlight
and signals WorkerIdle. If that notification synchronously calls back into
Close or Stop, a synchronous callback that waits on those counters waits
on its own completion stack. Moving a mutex unlock does not solve this.
An admission-stage cancellation can also be lost, and a Cancel callback
already waiting for QueueLock must not dereference class-owned extension
storage after another thread notifies completion and frees that storage.

## Proposed control flow

Use one persistent passive worker per initialized device, with
`TurnOffSynchronization=TRUE`. OPEN, state changes, CLOSE and power changes
become owned control SRBs. Their callbacks enqueue and return; they do not
wait for frame generation or another completion callback. A reentrant
control request therefore returns to the current notification. The worker
processes that control only after the notification returns.

Use bounded read and control queues and an independent fixed ownership
table keyed by opaque SRB addresses. The table records kind, admission,
queue/worker ownership and cancellation without dereferencing the SRB.
All table operations require the same external spin lock. Cancellation
looks up ownership first; a missing entry is a no-op. Admission publishes
ownership before dropping the lock to map a buffer, then rechecks the
cancel latch and the documented IRP cancel state before queueing.

Retire a request from the ownership table while holding the lock before
calling any completion notification. Copy needed notification arguments
first. Never access SRB, extension, header or mapped payload after the
notification. An already waiting cancellation callback must find no owned
entry and return without touching that memory. Do not assume a Phase field
inside potentially freed SRBExtension provides this protection.

The worker must complete cancelled or rejected reads even while PAUSED.
It must not wait for RUN to return a cancelled buffer. Normal valid PAUSE
reads may remain queued. Frame production at RUN must use a deadline,
rather than producing an additional frame on every control/cancel wakeup.

CLOSE blocks new reads for that stream, waits for admission mapping to
retire asynchronously, cancels queued reads, then clears the stream and
completes the owned CLOSE SRB. The worker must touch no class stream
storage after that handoff. GET_STATE must be ordered consistently with
queued SET_STATE. Queue capacity must reserve room for teardown; a full
ordinary control queue cannot prevent CLOSE or UNINITIALIZE progress.

## Final device teardown

UNINITIALIZE closes new admission and is retained rather than completed in
its callback. Drain queued reads and controls, finish any active admission,
then exit the persistent worker. A preallocated FDO-associated IoWorkItem
can join that thread and complete UNINITIALIZE last. It must not access
class-private engine storage after notification. IoWorkItem/MDL mapping
make this a Windows 2000-or-later experiment; they do not establish a 98
implementation.

This is not yet a proof of rundown. The implementation must account for
callbacks already entering or waiting for the lock, callbacks arriving
after the admission gate closes, cancellation concurrent with final
handoff, and control requests queued reentrantly by notifications. A raw
queued-request count or an event observed at zero is insufficient. Record
the published class/FDO callback-lifetime contract used for the final
handoff; a device reference must not silently be assumed to preserve a
separately allocated class-private buffer. Do not complete UNINITIALIZE
while any admitted callback can still access that buffer.

## Buffer and verification requirements

The first bounded path accepts one KS header and one payload MDL. Follow
[the buffer-contract audit](../testing/2026-10-21-uvc-stream-buffer-contract.md):
map the MDL explicitly, check mapping failure and extent, avoid applying
the byte offset twice, and leave class locking/unlocking/freeing alone.
Reject unsupported chains and layouts before writing any image bytes.

The portable ownership core must be exercised as actual implementation,
including cancel during admission, duplicate/full-table rejection, closed
admission, retire then late cancel, and read/control distinctions. Worker
tests must explicitly inject synchronous completion-to-CLOSE/STOP and
completion-to-UNINITIALIZE reentry. Add queue-full teardown, cancellation
while paused, failed mapping and delayed admission cases. Only then wire
the draft, rebuild/import-check, and test a fresh headless guest. Preserve
the known M0 binary and distinguish its hash from a future RUN build.

## Executable portable dispatch policy

`UvFlowControl` admits an opaque request and copies its validated command
facts to driver-owned slots. STOP/ACQUIRE, CLOSE, flush and power-down close
read admission and cancel existing reads immediately. The worker processes
controls in order; GET_STATE therefore observes preceding SET_STATE. PAUSE
keeps ordinary read buffers queued but cancellation/mapping failures remain
selectable. An ADMITTING read is not worker-selectable until its mapping
callback publishes `UvFlowReady`, including failed or cancelled admission.

`UvFlowNext` selects one work ticket. It never invokes class code or waits.
`UvFlowFinish` consults owned canonical work, latches cancellation, updates
policy, retires the request, and returns a completion ticket. The adapter
must write class status/header fields and notify outside its lock. While that
notification is active, reentrant callbacks can enqueue teardown/cancel, but
another worker step returns BUSY. After an ordinary notification returns,
the single persistent worker calls `UvFlowReturned`. This requires persistent
device-policy storage until UNINITIALIZE's final notification; it is not a
license to touch a completed SRB or stream extension.

Duplicate admission has a distinct result and preserves original ownership;
the adapter must not complete a rejected duplicate of an already-owned SRB.
Both slots and serial values reserve progress for final teardown. Ordinary
admission stops at 24 entries. Other teardown can use up to 30, CLOSE up to
31, and UNINITIALIZE the final slot. Generation reservations likewise prevent
ordinary work consuming the last serial needed for teardown. No generation
wrap is permitted. A failed teardown leaves admission faulted until a
successful close, rather than reopening RUN silently.

RUN frame selection uses one 100-ms deadline in this fixed-10fps synthetic
policy. Unrelated controls/cancel wakes do not reset it or produce a frame.
A delayed wake selects at most one frame and schedules from that wake, without
a catch-up burst. Tick arithmetic is modulo 32 bits and assumes observation
gaps below half the counter range while RUN; the Windows clock/power adapter
must enforce that precondition. This does not implement UVC clock recovery.

UNINITIALIZE closes both gates, cancels prior work, waits asynchronously for
all read admissions/ordinary completions, and yields an **EXIT ticket**.
That ticket cannot be completed using `UvFlowFinish`. The worker must exit.
Only an external joiner may call `UvFlowAfterJoin`, with separately established
worker-joined and callbacks-drained facts. Either false fact returns BUSY.
The final ticket is retired before notification and requires no subsequent
`Returned` or access to policy storage. The portable booleans are **external
obligations**, not evidence that a Windows implementation has established
callback/FDO/class-extension lifetime. They must never be hardcoded true.

Remaining kernel gates include callback entry/rundown around rejected requests
and mapping; stale cancellation versus reused SRB addresses; the documented
class-private/FDO lifetime at UNINITIALIZE; preallocated joiner/work-item
ownership; real spin-lock/IRQL ordering; and exact-once notification mapping.
The original unsafe worker is still unwired. See the
[policy run record](../testing/2026-10-21-uvc-flow.md).
