# STREAM.SYS synthetic camera M0, 2026-10-21

This is an original test-only root device, `ROOT\UVCSIM98`, separate from
future USB UVC binding. No USB device or real camera is accessed. It
advertises one fixed320x240 YUY2 capture pin,153600bytes/frame,10fps,
allows STOP/ACQUIRE/PAUSE, and rejects RUN/read delivery. It is not a
working camera or a release package. The visible name explicitly says
`SIMULATED YUY2 test source (STOP only)`.

The original files are `src/video/uvc98/uvcmini.c`, `UVCSIM.INF`,
`uvcmini.rc`, `build.cmd`, `minigate.ps1`, `minitest.c/.cmd` and targeted
`uvcinst.c/inst.cmd`. The minidriver uses Windows2000 DDK public
`strmini.h`, `ks.h`, `ksmedia.h`; the installer uses public SetupAPI/NewDev
contracts. No DDK samples, Microsoft/vendor binary code or disassembly
were used. The experimental INF has no USB identifier match and is not
copied under `free/`.

`build.cmd free` and `build.cmd checked` pass using named repository-local
MSVC6 and Windows2000 DDK tools. The output is x86 native subsystem4.00.
Its only three imports are StreamClassRegisterAdapter,
StreamClassDeviceNotification and StreamClassStreamNotification from
STREAM.SYS. The gate checks them against recorded98SE/ME export-name and
ordinal fixtures; this establishes import availability, not runtime support
on those operating systems. `minitest.cmd` compiles the actual original
miniport source against mock class notifications and passes bounds,
intersection, format, single-pin ownership, state-rejection and balanced
completion/ready-notification tests. RUN has not been synthesized by an
elapsed timer or unsupported callback assumption.

Actual run: fresh `E:\usb-wdm-free\wave2\w2kmini.qcow2` overlay backed by
`vm/w2k.qcow2`, QEMU pc/pentium3/oneCPU/512MB, standard VGA, no NIC, no USB,
headless HMP44333. TEMP/TMP and exchange files are on E. WHPX stalled at
early kernel startup before any project installation; that disposable run
was stopped and the overlay booted successfully with TCG. The Windows2000
guest uses Dvorak; all interaction used explicit QEMU HMP keyboard/mouse
commands and native screendumps.

The original targeted installer created/registered the exact synthetic
root devnode successfully, then NewDev installed `A:\UVCSIM.INF` with
success/error0/reboot0. CM status reported service`uvcmini`, flags0180200B,
problem0. Tested SYS SHA256:
`5D3B0AD60722496C54A8BCC32C536CEC1CAA4A7217337751B17FBDBAD7F38EF8`.
Tested installer EXE SHA256:
`8AB6158AC99D877AAE5CBBF8C33B7E42C673BF646FAEBF87E568B0349FA939CF`.
`out/video/uvc98/w2k-mini-installed.txt` SHA256:
`60A483D02ACA296A7C9E222DD49CE8561F88AC99644CC08C8CEC9AD693F03C38`.

CamCheck discovered the system's Microsoft WDM Image Capture mapper.
Its Source dialog explicitly listed our SIMULATED device, and its Format
dialog showed320x240, YUY2,153600bytes. Evidence screenshots:
`out/video/uvc98/w2k-mini-source.png`, `w2k-mini-format.png`.
Preview returned error439, unable to draw this data format; no frame or
preview success is claimed. RUN is intentionally unimplemented, and old
VfW YUY2 rendering may be an additional issue to isolate later. The Source
and Format dialogs establish actual class/proxy/mapper discovery and
format negotiation, independent of successful streaming.

After stopping/closing CamCheck, the original installer removed only the
exact synthetic root devnode successfully and confirmed it absent.
`out/video/uvc98/w2k-mini-removed.txt` SHA256:
`9A16590F64B274F6E5FA200E4081B086FA86050199D1722B222CC32413A536B9`.
The guest then received ACPI powerdown. There was no streaming teardown,
USB-removal or physical-camera test in this run.

The next contract is original bounded frame production with explicit
request ownership and close/uninitialize rundown. Public synchronization
documentation states that disabling class synchronization puts normal
request callbacks at PASSIVE_LEVEL while Cancel/Timeout/Timer remain at
DISPATCH_LEVEL. This permits a passive worker joined before completing
Close, provided request cancellation and buffer mapping are established.
The StreamClassScheduleTimer documentation alone does not specify close
cancellation/drain, so no lifetime guarantee was invented from that API.

Primary references:

- [Stream Class/minidriver interface](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/stream-class-and-minidriver-interface)
- [HW_STREAM_REQUEST_BLOCK and request ownership](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/ns-strmini-_hw_stream_request_block)
- [HW_STREAM_OBJECT](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/ns-strmini-_hw_stream_object)
- [SRB_GET_STREAM_INFO](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/srb-get-stream-info)
- [Stream categories](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/stream-categories)
- [Minidriver synchronization and callback IRQL](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/minidriver-synchronization)
- [StreamClassScheduleTimer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/nf-strmini-streamclassscheduletimer)

Windows2000 did not react to the ACPI button. Its own Shut Down dialog
completed to "It is now safe to turn off your computer"; only then was
QEMU quit. `out/video/uvc98/w2k-mini-safe-off.png` records the clean guest
shutdown. The synthetic devnode had already been removed. PID6916 and
HMP44333 are closed.

## Unwired frame-worker draft

`uvcgen.c/.h` now contains an original bounded grayscale YUY2 test image,
explicit SIMULATED text and a moving marker. `gentest.cmd` passes guarded
full-frame tests, failure-without-write checks, YUY2 chroma/luma bounds,
label/marker and frame-index wrapping. It accesses no camera and produces
no USB payloads.

`uvcrun.c/.h` is a separate **unwired, unapproved draft**. It is excluded
from `build.cmd`, the tested M0 SYS and every VM run. Independent review
identified synchronous completion reentry deadlocks and a cancellation
admission race. It must not be linked or tested as a working stream engine
until its completion/control/rundown design is corrected and reviewed.
The existing M0 source and tested binary are preserved under
`out/video/uvc98/m0/`. No frame-delivery result is claimed from the draft.

The buffer contract audit now identifies a documented route for a future
single-buffer prototype: payload MDLs are in IRP.MdlAddress, while stream
headers are in IRP.AssociatedIrp.SystemBuffer. Map only the class-owned,
already locked payload MDL with MmGetSystemAddressForMdlSafe, validate
MDL byte count >= FrameExtent >= frame bytes, reject MDL chains, leave
Header.Data unchanged, and never unlock/free/unmap the class-owned MDL.
The safe mapping API requires Windows2000 or later, so this does not
establish Windows98 streaming support. See
[the scoped buffer audit](2026-10-21-uvc-stream-buffer-contract.md) and
[KsProbeStreamIrp](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/nf-ks-ksprobestreamirp).

Generator visual check: `GENTEST.EXE out/video/uvc98/synthetic-frame.yuy`
exports the actual C-generated first frame after all guarded tests pass.
The raw153600-byte YUY2 frame has SHA256
`EEC96E7425E60BC27FDAA0173815C5A76CB4BDD574DD49D95A5CC52FA061CC3F`.
A studio-luma-to-grayscale PNG inspection confirms the SIMULATED label,
bars and marker (`out/video/uvc98/synthetic-frame.png`, SHA256
`8585CE8A08420940A25A768E677322841E066D5DC4D5742B68C159DAB53DA7B2`).
This is a host generator check, not a captured Windows camera frame.

## Unwired ownership core

`uvcown.c/.h` is a separate portable ownership table, excluded from the M0
build and all guest runs. It stores opaque request addresses without
dereferencing class memory. Under one caller-held external lock, admission
precedes class-memory inspection, cancellation finds a registered address,
and retirement removes ownership before the caller notifies Stream Class.
Read/control kinds have separate admission gates; serial tokens reject
stale table handles and saturate instead of wrapping. Output tokens and
entry snapshots must use separate caller-owned storage that does not alias
the table.

Cancelled admission has an explicit handoff policy: if cancellation arrives
while mapping/admission is in progress, `UvOwnQueue` returns CANCELLED and
leaves the entry ADMITTING. After admission work ends, that callback must
retire the entry and complete it directly. It must not wait for RUN or the
worker, because `UvOwnNext` only selects QUEUED entries. Already queued
cancelled reads remain selectable while PAUSED. The sequence tests exercise
both paths, including absence from the worker selection before direct
retirement and a late cancellation after retirement.

`owntest.cmd` passes actual-source admission/cancellation/retirement,
full-table, duplicate-address, independent-gate, FIFO, stale-token and
serial-exhaustion cases plus100000 deterministic sequence operations.
These tests establish the portable state transitions; they do not prove
kernel callback rundown or address-reuse safety after class notification.
The complete asynchronous control/reentry/rundown requirements remain in
[the worker design](../design/uvcrun.md). The existing unsafe worker draft
remains unwired until an implementation satisfies those requirements and
their reentry tests. No live-camera or kernel frame-delivery result is
claimed.

## Subsequent portable dispatch policy

`uvcflow.c/.h` now executes the asynchronous scheduling policy atop that
ownership table. `flowtest.cmd` passes361 policy checkpoints, including
completion-to-STOP/CLOSE/UNINITIALIZE reentry, delayed admissions,
cancelled PAUSE reads, queue/serial reservation and clock-wrap deadlines.
It does not call Windows or supply the final callback-rundown proof.
UNINITIALIZE remains owned until an external joiner establishes both
worker exit and callback rundown. The original unsafe worker remains
unwired and the M0 binary/build are unchanged. See the
[policy run record](2026-10-21-uvc-flow.md).
