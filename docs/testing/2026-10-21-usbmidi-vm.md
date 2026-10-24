# USB MIDI project-driver experiment, 2026-10-21

This is an unreleased, exact-simulator Windows 2000 x86 experiment. It is
separate from the Windows 7 in-box-driver run in
[the portable-core log](2026-10-21-usbmidi-core.md). No physical MIDI device
was opened and no Microsoft driver code or sample source was read.

## Implementation and build

`src/audio/usbmidi/usbmidi.cpp` implements an original PortCls MIDI miniport;
`umusb.c` supplies actual USB bulk transfers through the function PDO.
The portable event codec and descriptor parser are compiled into the driver.
The local `midiemu.py` USB-redirection peer calls the independently built
original C codec DLL. It advertises explicitly simulated FFFF:0001 IDs,
AudioControl interface 0, MIDIStreaming interface 1 and one MIDI1 cable.
It is neither a QEMU built-in MIDI device nor a hardware emulation claim.

Build from the repository root with `src\audio\usbmidi\build.cmd free`
or `checked`; both passed MSVC6/Windows 2000 DDK `/W3 /WX` builds and
the own-PE import gate. The free binary has 38 allowlisted named imports
from NTOSKRNL, HAL and PortCls. The toolchains are named under `tools/`;
the package shares only the repository's `src/audio/common/punknown.h` shim.
`probe.cmd` builds the explicit-port WinMM helper; `host.ps1` builds the
simulator codec DLL. `stage.ps1` emits the install INF in ASCII/CRLF.
No build or runtime dependency points at another repository.

Tested SYS SHA-256 (11488 bytes), unchanged throughout these VM runs:
`ae666c6c84926bf688bc399aff8b459f553f8c7e6add5f18448acdb729505849`.
Simulator codec SHA-256:
`b61df47bf64a3effeed04770e8cccf6caf5813152f3e826decf4092362b94ae3`.

The driver uses bounded 4096-byte byte rings and a passive USB worker.
IRP timeout cancels and then drains completion before freeing its storage.
Render STOP serializes against RUN/reopen and waits for old dequeued/output
requests to retire. Elevated final COM release defers passive destruction
through a preallocated work item with a retained FDO reference. PnP removal
drains the worker before forwarding to PortCls. An independent source review
checked these paths; it does not establish runtime safety. Power resume,
general device matching, multiple cables and UMP are unimplemented.

Primary contracts used include USB MIDI 1.0, the installed DDK headers,
Microsoft's published IMiniportMidi/IMiniportMidiStream, PcNewServiceGroup,
PcRegisterSubdevice, IoWorkItem and USB URB documentation. In particular,
SetState is PASSIVE_LEVEL, Read/Write support DISPATCH_LEVEL, a partial
Write accepts a multiple of four bytes, and KSDATARANGE_MUSIC Channels is
zero for an external port. No DDK samples were used.

## First actual project-driver run

Headless QEMU i386, machine `pc,acpi=off`, Pentium III without APIC,
256 MiB, Cirrus VGA, UHCI, no NIC and no physical passthrough; base
`vm/w2k.qcow2`, throwaway `E:\usb-wdm-free\wave2\w2kmidi.qcow2`.
All evidence below is under `E:\usb-wdm-free\wave2\`.

The baseline bound the composite parent to `usbhub` and its grouped MI_00
function to native `usbaudio`. Manual Have Disk using directory `A:\`
selected the project device. Windows reported "This device is working
properly" (`w2kmidi-status.png`). The earlier full INF filename selection
was rejected; directory selection and CRLF staging changed together, so
their individual effects were not isolated.

SetupAPI/own HKR metadata identified service `usbmidi`, class key 0011 and
MidiStage 10/status 0 (`w2kmidi-project-diag.png`). WinMM exposed the project
input/output names. Two invocations of `MIDITEST 0 0` passed:

```
Received=355 expected=355 short=32 long=1 errors=0 outputDone=1
Final received=355 errors=0
PASS; cleanup=complete
```

This includes 32 note messages and a 259-byte SysEx, exact byte comparison,
actual output MHDR_DONE, and checked reset/unprepare/close. Screenshots:
`w2kmidi-first-loop.png`, `w2kmidi-logtest2.png`. Explicit log
`w2kmidi-a/midi1.txt` is 190 bytes; helper SHA-256 in that stage is
`be667dba10eb114d18548c58ef2a2b71509f0b8aa810461db7ffff0b8b89e2cb`.
Packet lengths/hashes are in `w2kmidi.jsonl`.

Idle removal returned the normal unsafe-removal dialog and left the guest
responsive. Own registry export `w2kmidi-a/mstats.reg` recorded Tx/Rx 952
USB bytes, 12/16 URBs, zero bad events/overruns, 2942 idle IN timeouts and
final DEVICE_DATA_ERROR (C000009C) on removal. The two MIDI tests each use
476 USB event bytes. The final removal error is retained as negative
evidence; neither active removal nor error-free teardown is established.

Replug exposed an INF defect: whole-device IDs also matched the composite
parent, replacing `usbhub` and suppressing its MI child. The INF now matches
only revision-specific and generic FFFF:0001 **MI_00** IDs, never the parent.
The SYS was not rebuilt for this INF fix. The initial guest was shut down
to the safe-to-power-off screen and QEMU/model PIDs 30652/8684 exited.

## Fresh function-only retest

Fresh overlays `w2kmidif.qcow2` had display/logon startup difficulties
before the project package was installed. They were closed. A further
fresh `w2kmidig.qcow2` booted without the model, then hot attachment gave
the expected native parent/function baseline (`w2kmidig-native2.png`).

The helper's explicit `install` command calls the documented
UpdateDriverForPlugAndPlayDevicesA for exactly the simulated MI_00 ID and
`A:\USBMIDI.INF`. Its first attempt unexpectedly restarted the guest before
an install verdict was captured. No stop code was captured; the cause is
unresolved, so that attempt is not a pass. After disabling guest automatic
crash restart, a second attempt returned success=1/error=0/reboot=0.
Metadata confirmed parent `usbhub`, child `usbmidi`, MidiStage 5/status 0
(`w2kmidig-project2.png`). `MIDITEST 0 0 A:\MIDIG1.TXT` passed all 355 bytes
with complete cleanup (`w2kmidif-a/midig1.txt`). The helper SHA-256 was
`bc76531a86963d9c94cfdeaa5b06750bde4a925ca3e306b6eb13ab750818c6da`.

Idle removal remained responsive. Replug correctly retained `usbhub` on
the parent and `usbmidi` on MI_00, fixing the parent false match. However,
the new function instance required a restart and had no MidiStage value,
so automatic live restart and repeat traffic have **not** passed.
Evidence: `w2kmidig-replugready.png`, `w2kmidig-replugdiag3.png`,
`w2kmidig.jsonl`, `w2kmidig-replug.jsonl`.
Old function export `w2kmidif-a/mgstat.reg` recorded 476 bytes each way,
6/8 URBs, zero bad events/overruns, 579 idle IN timeouts and the same final
C000009C on removal. After the explicitly requested guest restart, the
replugged MI_00 reached MidiStage 5/status 0 while its parent stayed on
`usbhub` (`w2kmidig-restartdiag2.png`). A second explicit-file run passed
all 355 bytes, outputDone=1, errors=0 and complete cleanup:
`w2kmidif-a/midig2.txt`, `w2kmidig-restartloop.png`. Thus restart recovery
worked, while live hot-replug remains unpassed.

Final UI shutdown reached the safe-to-power-off screen
(`w2kmidig-safeoff.png`), then the monitor quit QEMU PID 37388. The model
PID 40144 logged `disconnected` and exited. Process/listener checks found
no owned guest/helper left and neither 44337 nor 44412 listening. Earlier
attempt PIDs 37560/29384 and first fresh model 36428 were already closed.
The removed-device backend had retained its socket, so model 36428 was
explicitly stopped after validating its command line; JSON lines were
flushed throughout. No QEMU window was opened.

The experiment proves an actual project driver carried MIDI bytes on this
one simulated Windows 2000/UHCI path. It does not establish physical MIDI,
cross-OS support, hot-replug recovery, suspend/resume, queue-pressure safety
or production readiness. Nothing was copied into `free/` or released.

## Bounded hot-replug contract review

[PcInitializeAdapterDriver](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-pcinitializeadapterdriver)
documents add/remove and IRP dispatch initialization. It does not explicitly
state whether it assigns DriverUnload; the Windows 2000 header declaration
also leaves that unspecified. Absence from that documentation does not prove
that the function leaves DriverUnload null. Neither prototype overrides
DriverUnload. Measuring only whether the returned pointer is non-null would
answer that narrow question without examining system binary code.

MIDI's wrapper stops the USB worker and releases its extension-owned Mini
reference before forwarding REMOVE to the saved PortCls dispatch. Each
mini/stream's retained FDO/work-item reference is released at final object
destruction. Whether PortCls releases all remaining COM references during
that REMOVE path was not measured. A reference-retention cycle is a possible
explanation, not a demonstrated defect and not grounds for a speculative
release or unload patch.

[RegisterServiceGroup](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportmidi-registerservicegroup)
follows COM reference ownership. The published
[service-group contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/service-sink-and-service-group-objects)
describes returning groups from Init/NewStream and registering before
starting hardware; those uses in this driver are intentional.
The current helper did not query CM_Get_DevNode_Status, so the exact CM
problem/status of the restart-required function is **not captured**. A
future diagnostic build should collect that, DriverUnload presence and
Add/Remove/final-destruction counters before distinguishing pending removal,
failed unload, or instance-install policy. The preserved SYS above remains
the baseline; no source-contract review changed it.

## Diagnostic build 2

The later requested diagnostic-only build is INF version 0.0.0.2 with
service-key MidiDiagBuild=0x00010002. Its free SYS SHA-256 is
`f1877f4d23c15585fdeae8de01fb117ee83793ecf508bd4f530e7a03945d6cfc`.
The earlier SYS/INF remain at
`E:\usb-wdm-free\wave2\midi-baseline-ae666c` with the original hash.
Free/checked `/W3 /WX` builds and import gates passed. ZwOpenKey is the
single new intentional import for PASSIVE_LEVEL own-service-key writes.
The helper now queries
[CM_Get_DevNode_Status](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status)
using the Windows 2000 header ABI and can write `diag logfile` explicitly.
It reports CONFIGRET, flags and problem, which is meaningful only when
DN_HAS_PROBLEM is set.

Counters are per loaded driver image, not persistent cumulative totals.
They count Add entry, REMOVE entry before PortCls forwarding, and final
Mini destructor completion after USB/group cleanup. A Boolean captures
whether DriverUnload is present immediately after PcInitializeAdapterDriver;
no address or system implementation is inspected. Diagnostics write only
the driver's service key, including after REMOVE when a retained FDO must
not be dereferenced for extension metadata. The registry-path argument is
deep-copied into bounded static storage at DriverEntry. USB, reference
ownership, dispatch forwarding and unload behavior are otherwise unchanged.

Fresh `w2kmidid.qcow2` used the same headless Windows 2000/UHCI setup,
attaching the simulator after desktop startup. Installation returned
success=1/error=0/reboot=0. `w2kmidid-a/dpre.txt` captured:

- Parent CM return 0, flags 0180600A, problem 0, service `usbhub`.
- MI_00 CM return 0, flags 0180200A, problem 0, service `usbmidi`, stage 5.
- Diagnostic build 00010002; Add=1, Remove=0, Destroy=0, UnloadPresent=1.

Thus PcInitializeAdapterDriver supplied a non-null unload callback on this
actual OS. `dloop.txt` passed 355 bytes, outputDone=1, errors=0 and complete
cleanup. After idle removal, `dremove.txt` recorded Add=1, Remove=1,
Destroy=1, UnloadPresent=1. Final Mini destruction completed its USB and
service-group cleanup; saved work-item/FDO release immediately follows in
DestroyPassive. A retained Mini reference cycle is not supported by this
run's evidence.

Replug still requested restart. After declining restart, `dreplug.txt`
recorded parent status unchanged and MI_00 CM return 0, flags **01802500**,
problem **1 (CM_PROB_NOT_CONFIGURED)**, with DN_HAS_PROBLEM set. It was
neither measured Code 14 nor failed Add. Counters remained 1/1/1, proving
that a second Add callback had not run.

Only installation metadata was copied from `C:\WINNT\setupapi.log` to
`w2kmidid-a/setupall.txt`. Its replug section records copying `usbmidi.sys`
and then states that reboot is required because files are in use or treated
as in use, followed by successful full installation. This is concrete
evidence of file-queue reboot gating before the new Add call. It does not
establish why that file remained in use or whether the image unloaded.

The current build has no VERSIONINFO resource. The official
[CopyFiles contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-copyfiles-directive)
explains that a target without file version information is considered older
for version checks. A future original version-resource packaging test is
appropriate. COPYFLG_NO_OVERWRITE would strand old files during upgrades
and was not applied as a general fix. No unload/refcount behavior patch was
made on this evidence.

The diagnostic guest was shut down to `w2kmidid-safeoff.png`, then QEMU
7152 and model 8300 exited; its predecessor model 33644 had been explicitly
stopped after removal. Text logs and CM verdicts remain in `w2kmidid-a`.
The diagnostic helper SHA-256 was
`b1c31603cd45e882a07472046b4e391e60f18fff8bdadb88efab49769875264d`.

## Version-resource follow-up

Original `usbmidi.rc` now supplies FILEVERSION/PRODUCTVERSION 0.0.0.2,
matching the INF, through the named repository-local MSVC6 RC compiler.
Normal CopyFiles policy is retained; NO_OVERWRITE is not used.
The marker is MidiDiagBuild=00010003, distinguishing this resource-bearing
build. Free/checked builds and their import gates passed. Windows' host
version-info API reports file/product versions 0.0.0.2. Free SYS SHA-256:
`12deb89458ac22f028cce5c64181099a4c0d6aa3ace7ce23262e4b7a50d9a207`.
The previous tested diagnostic free SYS/INF are retained at
`E:\usb-wdm-free\wave2\midi-diag-f1877f`; both new flavors are retained at
`E:\usb-wdm-free\wave2\midi-versioned-12deb8`.

The fresh versioned test `w2kmidiv.qcow2` started without USB attached,
then attached the simulator at UHCI port 1. Before an installation verdict
or an observed unsigned-driver acceptance, the guest unexpectedly restarted.
After that restart it displayed STOP 0000001E (C0000005, BFE03A90, 0, 0),
KMODE_EXCEPTION_NOT_HANDLED, with no module named on screen
(`w2kmidiv-rebootdesktop.png`). Guest AutoReboot had been disabled, and this
fault remained visible. Neither system binary code nor a memory dump was
read. Whether the project driver had loaded at the fault is not established;
the cause is unresolved. Version metadata is verified on the host, but
**its reconnect effect has not been validated**. The requested bounded
cycle stopped here, preserving the overlay and `w2kmidiv.jsonl`.
QEMU PID 30680 was quit from the faulted state; model 41292 logged
disconnected and exited, leaving neither 44337 nor 44412 listening.

The earlier successful reconnect-after-reboot and diagnostic cycles used
UHCI port 1 initially and automatically selected port 2 for replacement,
therefore created a **new Windows device instance**. They are not evidence
of same-port reconnect. A future versioned retest must explicitly request
port 1 after confirmed QEMU and guest removal, and record that topology.

### Subsequent loader-integrity evidence

To recover only own installation metadata without restarting the USB
function, a child `w2kmidivmeta.qcow2` was backed by the preserved failed
overlay and booted **without any USB device/model attached**. Windows
reported `\SystemRoot\system32\drivers\portcls.sys` could not be loaded,
status **C0000221** (`w2kmidivmeta-boot.png`). Dismissing the loader dialog
never yielded a usable desktop; Windows shut down instead. Consequently
the installed own SYS identity and persisted diagnostic stages could not
be recovered by this bounded normal-guest read. No Microsoft file, dump or
disassembly was read and no repair was attempted. This OS-loader-integrity
failure prevents attributing STOP 1E or evaluating the versioned package
from this overlay. A future retest must independently establish a clean
guest before attaching the function. Both failed parent and metadata child
are retained. Metadata-only QEMU PID 7732 was closed; no guest/model or
44337/44412 listener remained.
