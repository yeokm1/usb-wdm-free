# UAC2 guest pipeline and cleanup audit, 2026-10-21

This review separates the guest C7 driver from the concurrent QEMU/libusbK
host experiments. No VM or physical device was operated by this agent.
The installed C7 artifact `out/audio/uac2/w7-kiso4-installed.sys` remains
SHA256 `c7a172067c3d9b540c84d965664ab9bdca90ac435a1374b0759ef85d7e61126e`.
The normal build artifact `out/audio/uac2/wdmx86/free/uac2wdm.sys` remains
`d201b924e0bc7e293c2882a75fda0f99be94a1449b1e7bc6ed379f905f6a1910`.
Neither was overwritten or restaged by this audit.

## Host and guest errors measure different boundaries

Existing `w7-kiso4-*` evidence under ignored `out/audio/uac2/` records:

| Boundary | Measured result |
|---|---|
| Guest stop/close stats | Submitted389416, completed386340 scheduled bytes; DataUrbs507, FeedbackUrbs253; PacketErrors9; GoodFeedback1961, BadFeedback37, keepalives10; LastStatus0 and aggregate LastUsbStatus0 |
| QEMU guest-facing trace | 24 OUT requests completed with **status0/length0**, corresponding to24 empty host-ring events; other OUT completions include16007 at24bytes,13 at28bytes and4 at20bytes |
| Actual KISO callback record | OUT callback455 of500 reports24 failed physical packets, first24 of32; remaining8 successful. Feedback raw callbacks report0 failed packets. These24 physical failures are not established to be the same24 transfers as the earlier guest-facing empty-ring events. |
| QEMU aggregate host timing | OUT submit-to-callback maximum21.1014ms, callback-gap maximum15.3936ms; host first-submit-to-final-callback2006.6996ms |

The source reports scheduled PCM timeline progress after successful
guest-URB completion. It does not possess a later physical-host completion
acknowledgement. A host trace reporting success to the guest cannot be
converted into a physical error by guessing from OUT packet length.
Microsoft's [URB_ISOCH_TRANSFER contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usb/ns-usb-_urb_isoch_transfer)
states that per-packet Length is for IN and is not used for OUT. The guest
correctly checks per-packet Status and aggregate IRP/URB status instead.

Thus **guest9 does not refute physical24**, and making the guest count OUT
Length0 as an error would be an unsupported change. Host failure propagation
and ring scheduling require the host-side evidence, which another agent owns.
Conversely, strict WinMM completion and nonzero submitted PCM do not certify
physical delivery or audio quality. C7's combined PacketErrors counter also
does not identify whether its9 errors were data or feedback; its
LastUsbStatus records aggregate URB failures, not the first partial-packet
failure. No diagnostic ABI was changed to invent an attribution.

## Buffering and ownership review

The current guest submits four32-packet high-speed OUT URBs, nominally16ms
of scheduled PCM, plus two8-packet feedback URBs. The worker retires data in
submission sequence, handles completed feedback first, then reposts idle
slots. Fill reads the submitted ring cursor; Progress advances the completed
cursor and notifications. Scheduled lost packets advance the timeline and
are not replayed. Stop suppresses progress for flush cancellations.

No source evidence in this review justifies blindly increasing queue depth
or changing feedback tolerance. Guest queue depth, QEMU host-ring capacity
and libusbK's own pipeline are different quantities. The observed host timing
is relevant to starvation diagnosis but does not prove a guest queue bug.

The observed ownership paths remain: completion publishes status/phase under
the transport spinlock, the passive worker alone retires/frees the IRP, Stop
closes repost and cancels lower-owned requests, then waits for actual
completion and thread termination before freeing slot storage. No timeout
permits freeing an IRP/URB still owned below. This source review is not a
replacement for active-removal, stress or verifier runs.

## Definite watchdog correction

The C7 source used adjustable `KeQuerySystemTime` for the200ms feedback
freshness deadline. A forward clock correction can create a false timeout;
a backward correction can hide a genuinely expired deadline. The new source
uses [KeQueryInterruptTime](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kequeryinterrupttime)
for all four observations/reads. Microsoft documents it in100ns units and
independent of system-time corrections; the existing DDK declares it.

`uacwatch.h` contains the actual worker's small predicate. It preserves the
strict `elapsed > 2000000`100ns boundary and avoids unsigned underflow on a
backward observation. No stats schema, slot counts, data cadence, feedback
acceptance, cancellation ownership or USB control sequence changed. x86's
allowlist replaces KeQuerySystemTime with KeQueryInterruptTime; x64's kit
inlines both routines, so its import list remains unchanged.

This does not repair WHPX guest-clock drift or host scheduling. In the
existing run, reported guest GetTick duration differed from host/QPC timing;
interrupt time can also reflect virtual clock behavior. The correction
removes wall-clock adjustability, not every possible virtual timing defect.

## Paired runtime experiment prepared after host timer-policy test

The policy-only physical run produced clean raw host packets during its
short active interval (100 OUT callbacks/3200 packets and27 feedback
callbacks/864 packets), but the guest stopped with `LastStatus=C00000B5`.
Its snapshot included362 good feedback values, last `0x0005FFCF`/4bytes,
38 bad feedback values and8 packet errors. The last value is within the
planner's accepted48kHz high-speed range. This does not establish which
path produced the timeout: worker expiry, a failed USB completion or a
failed stop-time control request can affect `LastStatus`.

`clockpair.ps1` prepares baseline versus monotonic transport relinks with
identical retained D201 main/descriptor/rate/resource objects. It compiles
only `uacusb.c` in each variant, records all input/output hashes, checks
input immutability and runs eight native link/import gates. Exact C7 objects
were unavailable, so these are explicitly not literal C7 relinks. The
published-stream cleanup correction below is not folded into this pair.
Root reviewed the script and the clock-only source delta was preserved in
the E: experiment manifest. The paired x86 free SYS identities are:

| Variant | SHA256 |
|---|---|
| Adjustable baseline | 1D9D97097AB1D7DB1C541A2D854403AC66420E5B0AF2942362E40F3F07047306 |
| Monotonic | 35F03961554237CC5C89D8C31A972C991DAE9A21D02960D722E4339D86DBF912 |

The separate Win7 baseline overlay preserved C7 and kept the same host
QEMU/libusb, process-local timer honor, queue settings, C:-local player and
ejected floppy. Its verified1D9D driver failed the client deadline with
8submitted/0completed/4862samples, GetTick110ms/QPC11726ms, but the stop
snapshot had `LastStatus=0`. It therefore did not reproduce the recorded
transport timeout. The monotonic variant remains unstaged; this comparison
is paused rather than treating an untested change as a fix.

The player's counter begins before its first `waveOutWrite`, which may
perform USB startup work. A large total QPC interval with a short physical
ISO window does not locate a steady-stream stall. The next phase-helper
experiment separately stamps each write while retaining the strict gates.
Native host playback has meanwhile passed twice with full zero-error raw
coverage in the second run; see [native evidence](2026-10-21-uac2-native.md).

## Final stream release and driver-image lifetime

The existing queued path is:

```text
final Stream::Release
  -> native IoQueueWorkItem -> Cleanup -> DestroyPassive
     -> ~UacStream -> Wave::Close -> Drain(TRUE)
        -> UacUsbStop -> join actual USB worker
     -> release group / free ring / clear gate / owner->Release
        -> ~Wave (descriptor free only; requires usb==NULL)
     -> IoFreeWorkItem / explicit FDO dereference
  -> return through native worker's device/image lifetime guard
```

The explicit FDO reference remains live during the final Wave owner release.
The queued native item has an additional device lifetime guard through
callback return. [Microsoft permits freeing the item inside its callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iofreeworkitem).
There is no custom UAC2 DriverUnload wrapper joining this cleanup worker.
The unsafe Win98 research dispatcher's specific self-unload/join assumption
therefore does not describe this native queued path.

The weaker path was a **PASSIVE final Release executing DestroyPassive
inline**. If final release is induced reentrantly during a USB-worker
notification, inline Close can attempt to stop/join that same USB worker.
No recorded run proves that PortCls took this path. Published
[Notify](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportwavecyclic-notify)
permits any IRQL, but does not provide the strong caller-thread exclusion
needed to rely on inline destruction. Direct final-device dereference from
the inline epilogue also lacks a queued item's callback-return guard.

With parent coordination, successful streams now **always queue final
destruction through their preallocated native IoWorkItem**, including at
PASSIVE. A new internal `published` flag distinguishes construction from
exposed streams. It becomes true only after the DMA buffer, group and work
item exist and hooks are assigned, before output publication. Before that
boundary, failure cleanup remains synchronous at PASSIVE with no published
callbacks, so failed NewStream does not leave its one-stream gate busy.
This is lifetime hardening, not an explanation or fix for physical24 losses.

## Isolated verification and artifact boundary

`watchchk.ps1` defaults all new artifacts to
`E:/usb-wdm-free/wave2/uac-watchdog-check/`. It invalidates old success markers
before any failure-prone step, records source input hashes, baseline own
objects/resource/record/SYS hashes and a copy of each baseline build record,
then verifies that inputs stayed unchanged during checks.

- **500013** direct deadline checks pass: exact boundary,100000 positions
  across32-bit limbs, near unsigned64 maximum, backward observations and
  examples distinguishing forward/backward wall-clock corrections. The
  source gate requires all four clock calls to use interrupt time.
- The direct actual-header stream test retains allocation-failure, ring,
  position,5000 normalization and COM tests. Its new regression drops the
  final DMA reference **inside CompleteBytes -> Notify at PASSIVE**. Close
  must not run inline; queued cleanup executes after the simulated callback
  returns, closes/releases owner exactly once and preserves explicit plus
  native callback-return FDO guards. The DISPATCH final-release case remains.
  Kernel object/queue primitives are mocked; this is not kernel concurrency
  or hot-unplug proof.
- All four native source units are rebuilt from current source for
  wdmx86/wdm64 free/checked, using the package's recorded toolchain flags.
  Only the unchanged baseline version resource is reused. All four native
  compile/relink/import gates pass. Root independently reran the combined
  watchdog and published-stream cleanup checks in
  `E:/usb-wdm-free/wave2/uac-watchdog-independent`: all four native gates,
  500013 deadline cases and the actual-header stream regression pass.
- GCC16.1.0 `-O2` with undefined-behavior trap instrumentation independently
  passes the500013 deadline checks.

The combined experimental x86 free relink is
`bdb817e0d548b78647867412a58bf71123260b2dffa21cf502bebaf977432f05`.
It is **not installed**, retains old package version metadata and is not a
release or a separately validated device binary. Root review precedes any
restage while the C7 host A/B tests continue.

The portable control/mixer helpers should remain unintegrated during this
playback investigation. They perform no USB I/O and cannot remedy queueing,
host failure propagation or final-release lifetimes. Future integration
requires kernel arithmetic/import review, serialized control requests,
device capability validation and OS property contracts; it should not add
unrelated hardware writes to a controlled playback comparison.
