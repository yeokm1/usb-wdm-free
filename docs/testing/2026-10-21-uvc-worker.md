# Synthetic STREAM.SYS M1 worker preflight, 2026-10-21

This is an original, isolated NT2000+ synthetic capture experiment. It
matches only `ROOT\UVCSIM1`, supplies fixed320x240 YUY2 frames at10fps,
and has no USB/Brio transport. Host tests and compilation are not camera
capture, native callback-rundown or Windows98 streaming proof. The first
Windows2000 native installation remained pending before frame delivery.
A revised initialization handoff completes installation but reports initial
Code10 and a required reboot; no post-reboot status or capture pass was
obtained. Both negative stages are recorded below.

The new files `uvcwork.c/.h`, `uvcm1.c`, `uvcm1.rc`, `UVCSIM1.INF`,
`m1build.cmd`, `m1gate.ps1`, `m1inst.c/.cmd`, `worktest.c/.cmd`,
`workshim.h` and `m1test.c/.cmd` use this repository's original policy,
adapter and generator. Only published documentation/public headers were
used; no kit samples or Microsoft/vendor binary code were read. The old
unsafe `uvcrun.c` is not linked. The original M0 SYS is preserved under
`out/video/uvc98/m0/`, SHA256
`5D3B0AD60722496C54A8BCC32C536CEC1CAA4A7217337751B17FBDBAD7F38EF8`.

## Actual engine and ownership boundary

Default Stream Class synchronization stays enabled, with no ISR. Device,
stream, cancel and class-timer callbacks share the class gate. Admission,
opaque-address cancellation lookup, retirement and notifications all take
place in that domain. Callbacks enqueue and return; none waits, maps or
joins. The private passive thread maps one already-owned single-MDL READ
or generates a retained frame. It publishes job completion before the
class timer delivers it. There is one device timer, rearmed only after
its current invocation, with a bounded four-completion pump.

LOW initialization starts the private thread while INITIALIZE remains
owned and the class device queue is not ready. A local thread handle is
published only after successful creation; even an unspecified nonNULL
failure output is not treated as a joinable handle. The initial design
waited for the first timer to complete INIT and stalled natively. The
revised LOW-to-HIGH handoff and atomic cancellation outcome below replace
that dependency; they have advanced native initialization but have not
resolved the later Code10/failed-start cleanup boundary.

UNINITIALIZE closes admission and cancels/drains owned requests. The
private thread exits; a separate FDO-associated work item actually waits
for its handle and closes it. The work item is freed before publishing
Joined, its last engine access. A failed join/close never publishes Joined
and leaves teardown pending. The final class timer does not rearm. It
observes join, no pending timer, completed bootstrap and its sole callback
gate occupancy, clears the class anchor, frees private resources, and
completes UNINITIALIZE as its last class access. Ordinary completions use
the documented combined complete-and-mark-ready operation; final
UNINITIALIZE completes without reopening its queue.

The general class synchronization documentation is the basis for treating
the class timer as part of this serialized callback domain; the timer API
page alone does not establish it. It also does not state whether timers
continue while power/remove/UNINITIALIZE is pending. Native testing must
check that boundary. Timer suppression would leave this design pending,
not justify freeing the worker/engine early. There are no USB completion
or URB lifetime claims here.

## Initial preflight checks, before the native handoff revision

`worktest.cmd`:486 checks run the actual Bootstrap/Timer/RunJob/Join code
under injected NT operations. They cover normal frame delivery, queued
and mapping cancellation, delayed mapping plus CLOSE, immediate initial
timer, failed/during-create cancellation, failed join, and notification
reentry into CLOSE/UNINITIALIZE. Completion protects SRB/header/IRP/MDL
storage as no-access. Join publication immediately protects the entire
engine; final notification protects the class anchor. Actual code returns
without touching the protected objects. A failed thread-create mock
writes a poisoned nonNULL output handle; no join is attempted on it.

`m1test.cmd`:124 checks compile the actual miniport dispatch with injected
engine admission and notification. They check sync-enabled registration,
initialization failure/pending, fixed format/pin callbacks, all state and
flush routing, power/remove/uninitialize routing, duplicate ownership and
rejected admission. Immediate completion protects the entire SRB page,
catching caller-side access after notification. These injected tests do
not model native STREAM.SYS, IRQL, SMP scheduling or timer cancellation.

`m1build.cmd free` and `checked` compile/link with warnings as errors.
The exact import gate permits24 free imports from NTOSKRNL/STREAM.SYS;
checked adds RtlAssert and HAL.KeGetCurrentIrql. This is an explicit
NT2000+ prototype allowlist, not target export/runtime proof. The gate
also rejects USB IDs and 9x INF sections. Launchers reject all nonzero
process statuses, including negative exception codes. `port64.ps1` also
compiles adapter/core/worker/M1 against WDK7.1 amd64 headers; no x64 link
or installation is claimed.

Preflight output SHA256:

| File | SHA256 |
|---|---|
| free uvcm1.sys | B240A673B6344621DE558F57A910D591A201BC1B841DB14397EC7C8E19A05223 |
| checked uvcm1.sys | 74E95D468C88500F124343C6613FF835A77459B9FD6F6F2465F3ACF0E14BE291 |
| M1INST.EXE | 3A40428E0D7F2BF3A3422F53E0EFF7FC3864CDE68C5F353906C1BA1D922C30B5 |
| M1CAP.EXE | CD2DD231977DEDCAB822FCCB2736720110F51CDBCCBE7FA2C9B74D9FF9DAF402 |

The targeted installer is a renamed copy of this repository's original
M0 installer, now limited to ROOT\UVCSIM1 and A:\UVCSIM1.INF. It cannot
install or remove the M0 root devnode or a USB camera. The miniport reuses
original M0 media metadata through a renamed include; unused old dispatch
and entry routines are discarded by the linker. M0 source/INF/resource
and its preserved tested binary retain their earlier behavior.

Both new user-mode helpers verify complete report writes, flush and close;
a save failure is visible in the GUI. GUI exit status does not imply an
installation/capture pass; NewDev/CM results and the actual read report
determine the verdict. M1CAP is an optional direct KS checker to separate
synthetic READ delivery from VfW's earlier YUY2 drawing failure. It selects
only capture interfaces whose devnode has the exact ROOT\UVCSIM1 hardware
ID, opens its fixed YUY2 pin with KsCreatePin, requests three changing
153600-byte frames, then STOP and close. It exports checksums, not pixels.
Each pending overlapped IOCTL has a5s wait followed by CancelIo and a3s
retirement wait. The initial API call or handle close can still block if
the native driver/class wedges; this is not an unconditional API timeout.
If cancellation does not retire, it logs failure and
retains all pending storage/handles instead of freeing memory still owned
by the IRP. Such a guest must be treated as a negative run and closed.
The helper's compilation is not proof the native KS calls succeed.

User-mode contracts: [KsCreatePin](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/nf-ks-kscreatepin),
[connection structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ns-ks-kspin_connect),
[READ_STREAM](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ni-ks-ioctl_ks_read_stream),
and [stream header](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/ns-ks-ksstream_header).
The same writable header array is supplied as the read IOCTL's input and
output, with a separate fixed payload allocation. The public user-mode
ks.h declaration of KsCreatePin returns a Win32 DWORD error, unlike the
kernel NTSTATUS declaration.

## First native gate: initialization pending, no capture pass

The prepared guest is a fresh E:\usb-wdm-free\wave2\w2kuvc1.qcow2 overlay
of the repository's Windows2000 base, with TCG, one Pentium3 CPU,512MB,
standard VGA, no NIC/USB, localhost HMP44338 and no display window.
TEMP/TMP and the writable virtual-floppy exchange are on E. After audio
gave an explicit quiet window, PID41268 booted. It was briefly HMP-paused
for a separate host wait-probe, then resumed only after that probe ended.
There was no physical audio or camera run in this guest.

Initial exchange mounting used vvfat's disk geometry on a floppy device;
Windows correctly offered to format it. The offer was declined. Replacing
that virtual medium with `fat:floppy:rw:` fixed the exchange without
formatting. No physical disk/device was attached.

M1INST created/registered the exact ROOT\UVCSIM1 node successfully,
error0. Windows showed the normal unsigned-driver prompt for the M1
synthetic source; it was accepted without changing policy. NewDev then
remained pending for over two minutes. Its completion/status line never
appeared. The desktop and Run dialog remained responsive, QEMU stderr
was empty, and the saved report ended after root registration. This does
not prove the exact kernel stage: LOW bootstrap, thread creation and the
initialization-time timer need separate instrumentation. There is no CM
problem0, completed installation, RUN, READ, STOP or removal pass from
this run. M1CAP was not started against the pending device.

Normal Shut Down reported the installer unresponsive. Choosing End Now
allowed Windows to show "Windows is shutting down", then a blank teal
screen. No safe-off confirmation appeared within the bounded wait, so
QEMU was quit through HMP. This was a forced close after a shutdown
attempt, not a clean guest-shutdown proof. PID41268 and HMP44338 are
closed, and the audio agent was notified before its next run. The overlay
and evidence are preserved; there is no camera passthrough to release.

Evidence copied unchanged into ignored `out/video/uvc98/m1-native/`:

| Artifact | SHA256 |
|---|---|
| initial-pending.txt | E90A0795E3C370C0E357C839C5DCDA398C3089EF2412579D35DF435C6875ECB2 |
| w2kuvc1-install2.png (unsigned prompt) | 9B2E2B1E2465F03D454AC84C7CDA445661F6CB4B1ABC079866FF0D7AD18DBC66 |
| w2kuvc1-installed.png (still pending) | EC025726FC39DC588DD8916FA858C6573F3D6D7F35082CBE28BEC9370B6497FA |
| w2kuvc1-shutdown-pending.png (unresponsive installer) | 0CB6684CD3524B16110D427B25393B6E573B669D905A805B77CB89ABC76FE58E |
| w2kuvc1-shutdown-last.png (blank before forced quit) | 1A1730379607983EEB1C41F72357A62B76C68A7D6362EB21DAF50C667C3EF0B1 |

The planned sequence after fixing initialization remains exact KS reads,
STOP/pin close, reopen, targeted removal and shutdown. A working frame
path and pending UNINITIALIZE timer behavior remain untested. The native
negative takes precedence over all host preflight passes.

## VM-only stage trace and initialization handoff

The explicit `m1build.cmd checked vmtrace` mode writes a separate
`out/video/uvc98/m1trace/checked` package. `uvctrace.h` uses the published
[WRITE_PORT_UCHAR](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-write_port_uchar)
DDI against QEMU's explicitly configured isa-debugcon port0xE9. This is
VM-only diagnostic code, not a physical-machine resource assignment. The
normal macros remove all port writes; normal import gates forbid the
extra HAL import. No Microsoft binary memory/code was inspected.

Fresh overlay w2kuvct, PID30736, checked trace SYS SHA256
`8F09424BC73D8926C9B953BD3651364F603F6E8A3B35EA6A4DF2AE241C2DEED7`,
reached LOW, successful PsCreateSystemThread, bootstrap publication and
worker entry. It armed the initial timer but emitted no TIMER_ENTRY or
INIT_COMPLETE through the bounded run/shutdown attempt. The original
trace is `out/video/uvc98/m1-native/initial-trace.txt`, SHA256
`96C8D5AE3687035C70E7484FF2FF080A523F87DCA76D9389EC7599F4A7598E78`.
The guest was forced closed after normal shutdown did not finish. This
establishes the observed boundary; the timer API documentation does not
promise dispatch while INITIALIZE is pending.

The revised LOW bootstrap performs setup, then uses the documented
LowToHigh transition as its last engine access. The sole device priority
routine changes phase; it does not create independent concurrent LOW/HIGH
jobs. HIGH publishes runtime readiness, arms the pump and completes INIT
as its last class access. Failure or cancellation before setup commits
signals, joins and closes any created thread at PASSIVE in LOW, then
hands failure to HIGH without depending on the initial timer. A failed
join/close retains the engine/INIT and does not claim cleanup.

Atomic PENDING/CANCELLED/COMMITTED/FAILED outcomes close the LOW-to-HIGH
cancellation race. Cancellation may win before the setup decision. Once
successful setup commits, later cancellation loses and successful
initialization wins; losing Cancel makes no further bootstrap mutation.
Failure is likewise frozen before HIGH. This is an explicit driver policy,
not a claim that all requests must complete cancelled after a cancel
callback. The [priority DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/nf-strmini-streamclasscallatnewpriority)
specifies the sole device routine and LOW-to-HIGH service handoff; the
[priority enumeration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/ne-strmini-_stream_priority)
and general class serialization contract provide the selected gate.

Updated actual-engine host checks pass711, including immediate HIGH
execution/free before LOW returns, INIT-notification reentry into Cancel
and full UNINITIALIZE/free, before/after-commit cancellation, immutable
failure, allocation/item/thread creation failures and passive bootstrap
wait/close failures. Dispatch checks remain124 PASS. Free/checked and
VMtrace link/import gates plus amd64 object compilation pass. These are
host guards, not native PnP success. The latest source additionally caps
timer-entry logging to the first eight entries and adds received-SRB and
completion-status fields for the next diagnostic; that instrumentation
has not yet had another guest run.

## Revised handoff native run: initial Code10, no capture

Fresh w2kuvch used checked trace SYS SHA256
`3780E9D42B1A19EA8803A140BBD77A5FA9878E978C1FFA6BF7937A376692159B`,
PID31368, the same bounded no-USB/no-NIC headless TCG profile. This run
actually emitted LOW_TO_HIGH, BOOTSTRAP_HIGH_ENTRY, INIT_COMPLETE and
recurring TIMER_ENTRY. NewDev returned success/error0/reboot1. CM reported
service uvcm1, flags01802501, problem10. It is an initial Code10 result,
not a started-device or frame-delivery pass.

The unchanged guest received the requested native Restart. It reached a
black720x400 display and did not return to the desktop within about one
minute. There was only one driver-entry trace, so no post-reboot CM/KS
status was obtained. Do not describe Code10 as persistent after a
successful reboot. The guest was then HMP-quit; PID31368 and44338 are
closed, and the audio owner received the released slot. No READ/RUN,
STOP/pin close, targeted removal or UNINITIALIZE success is claimed.

Uncapped diagnostic logging produced6796 timer lines in this run. It is
not timing/performance evidence and could affect behavior. The next
diagnostic caps those lines and traces only our received SRB commands and
completion statuses to locate the start failure. This does not justify
changing metadata or class-service behavior speculatively. Initial free
and both diagnostic SYS artifacts and original sources remain preserved
under ignored `out/video/uvc98/m1-native/` alongside both negative runs.

## Capped request trace: GET_STREAM_INFO guard failure

Fresh w2kuvcs, PID12904, used VM-only checked SYS SHA256
`8985C941603298CC13221AA0ED118E6FAFA72B2A5F6743B4F6082D100F22327A`.
Normal/free/checked/trace gates, amd64 compilation and711/124 host checks
passed before this run. Timer logging stopped after eight entries. Own
SRB-command/status logging showed INITIALIZE0x107 completingSUCCESS, then
GET_STREAM_INFO0x100 with a nonNULL class buffer, a zero union field read
after our output reset, and expected descriptor size0x78. Our guard returned C0000023
(STATUS_BUFFER_TOO_SMALL); INITIALIZATION_COMPLETE never appeared.
NewDev returned success/error0/reboot1 with initial CMproblem10 again.
No READ, RUN or filter-open test was performed.

The [request-block member contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/ns-strmini-_hw_stream_request_block)
defines NumberOfBytesToTransfer for data READ/WRITE requests. The
[GET_STREAM_INFO contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/srb-get-stream-info)
says its buffer has the size registered during INITIALIZE in
ConfigInfo.StreamDescriptorSize. Independent public-header review also
found that NumberOfBytesToTransfer and ActualBytesTransferred are union
aliases: M1 reset the latter before checking/tracing the former. The
observed zero was therefore self-clobbered. It does not establish the
incoming Windows value or a zero-byte allocation. Our initial attribution
of zero to Windows was incorrect and is withdrawn. The original M0 saves
the incoming value before resetting output, so its old control path is
different. The concrete M1 failure is its own guard/input-clobber logic.

The prepared M1-only correction records that registered budget in its
private device extension, retains NULL/insufficient-budget checks, and
does not use the irrelevant transfer field. Its targeted regression
supplies both zero and nonzero union inputs with the valid registered
budget, checks the descriptor and a trailing canary, and rejects absent/undersized budgets
even if transfer bytes isFFFFFFFF. This change has not yet been compiled
or rerun natively at this checkpoint. Diagnostic source now logs the
incoming union value before resetting it and labels the post-reset value
explicitly. The original M0 guard/source stays
separate for comparison; no claim is made that its old guard is correct.

Unlike the preceding black Restart attempt, this capped run's normal
Shut Down reached the explicit safe-to-turn-off screen. QEMU was quit
afterwards; PID12904/listener44338 are closed and the audio slot released.
That is clean guest shutdown evidence, not a separate worker-unload or
UNINITIALIZE success proof. The original capped trace/status, failed SYS
and safe-off screenshot are preserved in ignored
`out/video/uvc98/m1-native/` as `srb-code10-trace.txt`,
`srb-code10-status.txt`, `srb-code10.sys` and `w2kuvcs-last.png`.

## Contract references

- [Class/minidriver interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/stream-class-and-minidriver-interface): nonreentrant class entrypoints across CPUs.
- [Synchronization](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/synchronization-examples): default no-ISR dispatch level and unsynchronized LOW versus serialized HIGH.
- [CallAtNewPriority](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/nf-strmini-streamclasscallatnewpriority): bounded LOW initialization handoff.
- [ScheduleTimer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/nf-strmini-streamclassscheduletimer): one device timer, NULL stream and microsecond delay.
- [Combined completion/ready](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/nf-strmini-streamclasscompleterequestandmarkqueueready): a single ordinary class handoff.
- [System worker threads](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/system-worker-threads) and [IoFreeWorkItem](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iofreeworkitem): finite native work item versus persistent private thread and freeing the dequeued join item.
- [Actual SRB adapter record](2026-10-21-uvc-port.md): single-MDL mapping and guarded class-memory ownership limits.

## Budget fix in the guest: start, idle removal, and the pin 87 negative (21 Oct, late)

Run w2kuvcg (guest 47076, Windows 2000 SP4, no USB, TCG, one vCPU, overlay
`E:\usb-wdm-free\wave2\w2kuvcg.qcow2`), SYS
C0799AEAB343F27A4EAE117A6583A3C29387FFF2D68B2DB856A36127817C5EAA (the
registered-budget correction, VM trace build). M1INST created ROOT\UVCSIM1:
NewDev succeeded, no reboot, CM problem 0. The trace shows INIT pending, then
LOW->HIGH, INIT_COMPLETE SUCCESS, GET_STREAM_INFO with incoming union 0x78
and registered budget 0x78, and SUCCESS. Idle targeted removal passed: the
device was absent afterwards (1168), and SURPRISE_REMOVAL 0x10E and
UNINITIALIZE 0x109 completed SUCCESS. That run's M1CAP got KsCreatePin
**87** before any SRB_OPEN_STREAM, RUN or READ reached the miniport. Evidence:
`out/video/uvc98/m1-native/budget-*` and `w2kuvcg-*.png`.

## Pin 87 found: KSPRIORITY subclass 0; first synthetic frames through M1 (22 Oct)

M1CAP now first asks the filter, through the public KSPROPSETID_Pin
properties, what it publishes for pin 0, and then creates the pin.
The answers: one pin type; dataflow OUT (2); communication BOTH (3); one possible
instance; interface KSINTERFACESETID_Standard/STANDARD_STREAMING; medium
KSMEDIUMSETID_Standard/0; one 296-byte KS_DATARANGE_VIDEO (YUY2,
VIDEOINFO, sample 153600). DATAINTERSECTION reached the miniport
(SRB 0x10C SUCCESS) and returned 152 bytes, **byte for byte equal** to
M1CAP's hand-built connect format. So interface, medium and format were not
the cause. A one-change-at-a-time matrix then created the pin as soon as
`KSPIN_CONNECT.Priority.PrioritySubClass` was 1 instead of 0
(KSPRIORITY_NORMAL class unchanged). Read+write access alone still got 87.
The fixed checker keeps a subclass-0 create as a logged negative control and
connects with subclass 1. **The miniport was never at fault for 87.**

Run w2kuvck (same guest, a new overlay `E:\usb-wdm-free\wave2\w2kuvck.qcow2`
backed by w2kuvcg, same TCG profile, launched by `w2kuvck-launch.ps1`), the same
SYS C0799AEA...5EAA, M1INST 3A40428E...AAA1, final M1CAP
1BCC0387C5A0AE24A6D406F655F851341F67AA0ADE2AC43BE94AB4F6438DE9A6
(VC6 /W3 /WX, `m1cap.cmd`): install CM problem 0, then M1CAP printed
**PASS: three complete changing synthetic frames**. Details: control
subclass 0 = 87; subclass 1 = 0; ACQUIRE, PAUSE and RUN ok; three READs of
153600 bytes with FNV-1a C9CCA205, CAE63B05 and F7188C05; STOP ok; pin and
filter closed. The miniport trace shows OPEN_STREAM, the state SRBs, three
READ_SRB SUCCESS, CLOSE_STREAM 0x102 SUCCESS, and then, after
M1INST Remove, SURPRISE_REMOVAL 0x10E and UNINITIALIZE 0x109 SUCCESS. The
device was absent afterwards (1168). Normal Windows shutdown reached "safe to turn off"
before HMP quit. This is the **first frame path through the project
STREAM.SYS miniport**, but the frames are synthetic worker frames: there is
no USB, no camera and no UVC payload yet. It is not a uvc98 capture claim. The
frame content is the worker's deterministic pattern, and only its change
from frame to frame is checked.

Evidence: `E:\usb-wdm-free\evidence\w2k-m1cap\` (`m1cap-final.txt`, the
variant-matrix report `w2kuvck-m1cap-v2.txt`, the first property report
`w2kuvck-m1cap-v1.txt`, the debugcon trace, `sim1log.txt`, screenshots
`w2kuvck-cap3.png`, `w2kuvck-removed.png` and `w2kuvck-safeoff.png`, and exchange-file hashes).
The guest keyboard layout is US Dvorak, and `E:\usb-wdm-free\wave1\hmp.py --type`
maps it (HMP_PORT=44338).
