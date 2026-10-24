# USB MIDI 1.0 portable core, 2026-10-21

This records the original portable core under `src/audio/usbmidi/`, following
the package's audio-class source/release convention. The initial preparation
had no kernel driver. Subsequent native-driver simulation is recorded below;
the later original PortCls/USB experiment and its limits are recorded in
[the Windows 2000 project-driver log](2026-10-21-usbmidi-vm.md).

## Reference and source boundary

The normative reference is USB-IF's [USB MIDI Devices 1.0](https://www.usb.org/sites/default/files/midi10.pdf),
release 1.0, November 1, 1999, sections 4 and 6, Tables 4-1/4-2 and
Appendix B. The unchanged 180066-byte PDF is downloaded under ignored
`docs/references/midi10.pdf`; SHA-256:
`6ce79fec1053a9d74bd7178ae01d4373701395714080e34b7bbeb9837e62cf33`.
It has 43 PDF pages. Its internal-use-only reproduction license and other
notices remain intact. This is an internal reference, not a redistributed
part of a package. No MIDI implementation from Linux, Microsoft samples,
vendor drivers or another repository was copied/adapted for this core.
The original source is marked GPL-2.0-only.

The reference has two concrete descriptor inconsistencies. Table 6-4's
OUT-jack size formula omits a byte; Appendix B.4.4 lists a nine-byte
one-input OUT jack and its final `iJack` at offset eight. The parser uses
seven fixed bytes plus two per input pin. Section 6.1.2.1 counts the MS
header plus Jack/Element descriptors for `wTotalLength`; Appendix B's
`0x0041` also includes endpoint descriptors. The parser accepts either
exact bounded interpretation, rather than accepting an arbitrary length.

## Implemented contract

- `umpkt.c` expands MIDI running status into complete USB events and
  encodes channel messages, defined System Common, SysEx starts/continues/
  ends, and interleaved real-time bytes. State is separate per cable.
  Undefined real-time bytes remain single-byte events; undefined System
  Common cancels pending/running status and is rejected. A new non-real-
  time status abandons an incomplete SysEx. Reset explicitly drops an
  incomplete message on stream closure. There is no silent flush that
  invents an end-of-SysEx byte.
- Decoding accepts only one four-byte event on an allowed endpoint cable,
  rejects reserved CINs and inconsistent status/data/SysEx forms, and
  returns only meaningful payload bytes. CIN F preserves raw single-byte
  mode. Padding is ignored on input and zeroed on output. This stateless
  decoder does not assert cross-packet SysEx continuity.
- `msdesc.c` validates all descriptor boundaries in a supplied complete
  configuration blob, then selects one explicit interface/alternate with
  Audio class, MIDIStreaming subclass and MIDI1 protocol/header. It
  extracts up to 30 distinct bulk endpoints, up to 16 cables per endpoint,
  embedded jack associations and packet capacities. It rejects duplicate
  IDs/addresses, missing or external/reversed associations, unknown jack
  sources, invalid source pins, malformed counts and inconsistent totals.
  Result is zero on failure. Endpoint descriptors of seven or nine bytes
  are accepted; nine-byte refresh/sync fields must be zero.

These routines allocate no memory and have no Windows/kernel imports.
The parser's bounded subset excludes Elements, Transfer endpoints,
vendor interfaces/quirks and MIDI2/UMP. It does not validate the separate
AC collection topology or actual bus-speed packet-size limits. A future
transport must validate those before selecting interfaces, carry split
USB transfers until a complete event exists, implement queue ownership/
backpressure and cancel/drain on removal. No CH345/Uno compatibility,
Windows installation, WinMM routing or physical loopback is claimed.

## Verification

Run `src/audio/usbmidi/test.ps1`; the script names the repository-local
MSVC 6.0 toolchain and performs no downloads or driver installation.
Compiled with `/W3 /WX /O2`, using the minimum C compiler intended for the
older target systems. Test artifacts are ignored under `out/audio/usbmidi`.
The recorded run passed **203668 checks**.

Tests cover every channel status/cable, running status expansion, defined
System Common and raw real-time, real-time interleaving throughout SysEx
payload lengths 0..300, abandoned messages and invalid input. SysEx
round-trips compare recovered bytes against the original byte stream.
The synthetic descriptor fixture describes an embedded/external DIN pair;
it is original test data, not a hardware capture. Both documented total-
length interpretations succeed. Explicit malformed association/source/
pin/address/type/length cases fail. All 21248 single-byte descriptor
mutations run, with output invariants checked on success and zero output
on failure. Every fixture truncation and short event is additionally
placed against a Windows `PAGE_NOACCESS` boundary to catch over-reads.

No VM or helper process was started for this host-only test.

## Prepared simulation and guest helper

`midiemu.py` is an original, test-only USB MIDI1 Full Speed loopback
exporter using QEMU's existing `usb-redir` transport. It does not add a
native QEMU MIDI model. The AudioControl collection and MIDIStreaming
interface, one cable per bulk direction, are simulated; local `FFFF:0001`
identifiers make no claim to a registered product. Manufacturer/product
strings explicitly identify simulation. Protocol facts are from SPICE's
[USB redirection protocol 0.7](https://github.com/SPICEorg/usbredir/blob/master/usb-redirection-protocol.txt).
There is no third-party implementation copied into the model.

The model calls the owned C decoder and encoder via `ctypes`, requiring
the import-free x64 `umcodec.dll` produced by `host.ps1`. There is no
Python codec fallback. It validates cable zero, reconstructs complete
messages from raw/running-status input, and returns canonical event
packets. A one MiB loopback bound and 256 pending-read bound are enforced;
malformed or unsupported test traffic fails the model explicitly. Reset,
configuration changes and MS alternate selection cancel pending reads
and clear buffered codec state. Only localhost is bound; `--log` creates
a new JSONL trace and records the loaded codec hash, transfer lengths and
hashes. No physical USB device is opened.

Preparation, without starting any guest:

```powershell
& src/audio/usbmidi/host.ps1
python src/audio/usbmidi/midisimtest.py
& src/audio/usbmidi/probe.cmd
```

The eight Python host tests pass, covering descriptor identity/bounds,
C-codec short/running-status and SysEx reconstruction, malformed packets,
reserved cable rejection, cancellation, five reset cycles, ordered
short-packet loopback and pending-request pressure. A future guest run
can launch the server with
`python src/audio/usbmidi/midiemu.py --log E:/usb-wdm-free/midi-sim.jsonl`
and attach QEMU `usb-redir` through its localhost TCP port 44412. The
selected QEMU USB bus and guest driver must be recorded for that run.
This preparation does not establish QEMU or guest enumeration success.

`MIDITEST.EXE` is a separate x86 WinMM helper, built with MSVC6 `/W3 /WX`
and subsystem 4.0, dynamically linked to the target's MSVCRT and WinMM.
`MIDITEST list` only enumerates ports. `MIDITEST inputId outputId` requires
explicit port numbers and an actual physical/simulated loopback; it sends
32 Note On events and a 259-byte noncommercial-ID SysEx, checks the exact
355 received bytes, callback error count, 32 short-message callbacks,
at least one long callback and real output `MHDR_DONE` within five seconds.
Cleanup stops/resets input, resets output, unprepares every prepared
header and closes both handles. Headers/data are static and retained
through process exit if a driver refuses release; callbacks never call
WinMM APIs. Final byte/error checks occur after cleanup callback draining.

The host list-only smoke run showed no MIDI input and the Microsoft GS
Wavetable Synth output. No output port was opened or MIDI sent on the
host. At this preparation stage end-to-end verification was still unrun;
the later guest results are recorded below. Build artifact SHA-256 values:

- `MIDITEST.EXE`: `c0b0243c4677cb6c6fc271d2d80d2c0ad9f03fc388446359dc772f252dc2b09d`.
- `umcodec.dll`: `b61df47bf64a3effeed04770e8cccf6caf5813152f3e826decf4092362b94ae3`.

No VM was started for this preparation. The list-only host process exited;
the simulation listener has not been started and no helper is left running.

## Recorded Windows 7 x86 simulation run

An actual headless QEMU run used a fresh
`E:\usb-wdm-free\wave2\w7midi.qcow2` overlay of `vm/w786.qcow2`:
`qemu-system-x86_64`, machine `pc`, WHPX with `kernel-irqchip=off`,
`pentium3`, two vCPUs, 1024 MiB RAM, standard VGA, no NIC, a
`piix3-usb-uhci` controller and `usb-redir` on its UHCI bus. The model
listened on localhost 44412; the guest monitor was localhost 44337.
All VM/TEMP scratch was on E:. No physical USB device was attached.

Windows installed its native drivers without a project INF. Its restart
request was honored, but the warm restart stayed at Starting Windows for
over 90 seconds, with no new USB requests after initial descriptors.
That attempt was closed through QEMU's monitor; a cold launch of the same
overlay, choosing Start Windows Normally, reached the desktop without any
source/model/CPU/memory changes. This is a recorded boot limitation, not
a successful warm-restart test or an identified MIDI-driver defect.

`MIDITEST list` then reported:

```text
IN 0: MIDI1 loopback - SIMULATED
OUT 0: Microsoft GS Wavetable Synth
OUT 1: MIDI1 loopback - SIMULATED
```

Two separate direct-console `MIDITEST 0 1` invocations passed:

```text
Received=355 expected=355 short=32 long=1 errors=0 outputDone=1
Final received=355 errors=0
PASS; cleanup=complete
```

The first ran directly from A:, and the second after copying the same
binary into the guest's user folder. Both used the simulator and its
loaded C-codec DLL, whose SHA-256 in the actual JSONL connect event matches
`b61df47bf64a3effeed04770e8cccf6caf5813152f3e826decf4092362b94ae3`.
The two direct runs' USB transfer bursts took approximately 0.23 and
0.24 seconds on the host clock. Each had 70 bulk OUT and 70 bulk IN
completions, totaling 604 USB bytes each way. That includes 476 bytes of
event packets for the test data (32 notes and 259 SysEx bytes), plus
128 bytes of native output-reset traffic during cleanup; it does not
mean 604 application MIDI bytes.

Guest WMI metadata for `USB%VID_FFFF%` identified USB Audio Device bound
through Microsoft's `wdma_usb.inf`, version `6.1.7601.17514`, and USB
Composite Device through Microsoft's `usb.inf`, same version. Registry
metadata also identified the composite service as `usbccgp`. Only metadata
was queried; no Microsoft binary code, disassembly or samples were read.

Two additional invocations redirected stdout, one to A: and one to the
guest's local disk, produced zero-byte files despite complete USB transfer
bursts. Their WinMM verdict is therefore not asserted. The console runs
above are the positive evidence. This logging anomaly remains unresolved;
do not depend on CMD redirection for unattended verdict collection from
this helper until it is fixed and retested.

Evidence is retained under `E:\usb-wdm-free\wave2\`:

- `w7midi.jsonl`: initial installation/warm-restart attempt;
  `w7midi2.jsonl`: cold-start and four transfer bursts, including the two
  direct-console passes. Every transfer records length and SHA-256.
- `w7midi-start.png`: successful driver-install notification and requested
  restart; `w7midi-cold.png`: recovery menu after the stopped warm boot.
- `w7midi-ports2.png`, `w7midi-direct2.png`,
  `w7midi-direct-repeat.png`: actual ports and two PASS verdicts.
- `w7midi-wmi.png`: native driver provider/version/INF metadata;
  `w7midi-file.png`: zero-byte local redirected log.
- Model/QEMU `.err` logs are empty for the recorded cold-start run.

Finally `shutdown /s /t 0` cleanly powered off the guest. QEMU PID 35844
and model PID 40228 exited; the earlier attempt's PIDs 37148/30708 had
already exited. Process/localhost-listener checks found neither remaining
guest/model nor listeners on 44337/44412. No QEMU window was opened.

This proves one simulated standard MIDI1 cable works end to end through
the Windows 7 x86 in-box driver, original C codec and WinMM helper. It
does **not** prove a project kernel MIDI driver, physical MIDI timing,
hardware loopback, quirk support, removal with open MIDI streams or any
other target OS.
