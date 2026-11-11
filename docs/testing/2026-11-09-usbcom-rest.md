# `usbcom`'s bridges: the chip legs on xHCI98 3.4.2.0, ME under EHCI, and the 64 KiB IN loss under NUSB, 2026-11-09

Open items, "47.19 USB serial bridges in `usbcom`, 98 SE and ME", Left:
the chip legs on the current xHCI98, ME under an EHCI stack, the 64 KiB
IN loss under NUSB (the real chips, bench 45-E.14, stay owed). Every leg
here ran against `chipemu.py`, this package's simulated bridges; no
physical bridge was used and none is claimed.

## Build

`USBCOM.SYS` trace build `18d39fb7...` of branch `w48-wdmb` (`usbcom`'s
sources last changed at `427acad`); `build.cmd w98 trace`, gates and host
tests PASS.

## The chip legs on xHCI98 3.4.2.0

98 SE, xHCI98 3.4.2.0 (`3990e17d`), the 0.1.9 9x matrix's harness copied
to `E:\xhci98-vm\p48\wdmb\kit` (QEMU `cb36497`, TCG, one vCPU, base
`w98x`), leg `usbcom-w98-xh-c1`, the package staged by `DevicePath`, a
fresh simulator per plug, the FTDI High Speed parts at High Speed and
the rest at Full Speed, as in the 2026-10-29 legs
([usbcom-chips](2026-10-29-usbcom-chips.md)):

| Part | `COMTEST full` (first port) | Other ports, `quick` | More |
|---|---|---|---|
| FT-X | 66/66 | - | pull while open and back 15 s later (`hold`) 6/6; `quick` after the replug 4/4; `quick` after a restart 4/4 |
| FT232H (High Speed) | 66/66 | - | |
| FT4232H (High Speed) | 66/66 | COM6, COM7, COM8 PASS | |
| PL2303 H | 66/66 | - | |
| PL2303 HX | 66/66 | - | |
| PL2303 HXD | 66/66 | - | |
| PL2303 TA | 66/66 | - | |
| PL2303 TB | 66/66 | - | |
| PL2303 G | 66/66 | - | |
| CP2105 | 65/65 | COM13 (interface 1) PASS | |
| CP2108 | 65/65 | COM15, COM16 PASS | the fourth port (COM17) not run: the harness's `COMTEST list` tries COM1 to COM16 only, as on 2026-10-29 |

Every part passed every check on xHCI98 3.4.2.0, the 64 KiB echo of the
`full` run included.

## ME under an EHCI stack

Not run, and not runnable on this rig: ME has no EHCI driver of its own,
and NUSB, the only EHCI stack the 98 SE legs use, does not install on ME
(design [absptr.md](../design/absptr.md) and [ccid.md](../design/ccid.md)
record the same). ME's bridges at High Speed run under
xHCI98 only; ME under an EHCI controller is a bench item (a real EHCI
controller with a driver that installs on ME), recorded as owed.

## The 64 KiB IN loss under NUSB

The 2026-10-29 legs lost bytes in one 64 KiB echo in each `full` run on
four parts under NUSB 3.3's EHCI driver, with the simulator's log showing
every byte sent ([usbcom-chips](2026-10-29-usbcom-chips.md), "Below the
driver"). Here, on a fresh overlay of `w98n2` (98 SE, NUSB 3.3, QEMU's
`usb-ehci`), leg `comloss-w98-nusb-l1`: per part, 20 runs of `COMTEST
COMn echo 65536`, the simulator logging every bulk-IN request and answer
and every cancellation (in time, or late: for a packet already
answered), and QEMU run with its trace events `usb_ehci_guest_bug` and
`usb_ehci_packet_action`:

| Part | 64 KiB echoes | Late cancellations | QEMU "guest updated active qTD" |
|---|---|---|---|
| FT-X (at High Speed here) | 20 of 20 PASS | 0 | 0 |
| FT232H (High Speed) | 19 of 20 PASS; run 20: "got 57376, first difference at 53120" (8160 bytes, two 4096-byte transfers' worth of data, missing) | 0 | 1, in run 20, at the answers the guest lost |
| CP2105 | void: the leg passed `COMTEST` flags its `echo` mode does not take (it printed its usage); not rerun | - | - |

Where the bytes went, run 20 of the FT232H:

- The simulator received 65536 bytes and sent all 65536 back (its log:
  16 bulk-OUT transfers of 4096 bytes; the answers' data, less the two
  status bytes per packet, 65536).
- QEMU combines the guest's queued bulk-IN transfers into one usb-redir
  request (4096 to 28672 bytes here) and splits the answer back over
  them. The answers around offset 53120 were a 4096-byte answer and an
  8192-byte one for the same transfer descriptor address, back to back.
- Between them QEMU's trace has the run's only "guest updated active
  qTD". In QEMU's EHCI model (`hw/usb/hcd-ehci.c`, `ehci_state_fetchqtd`),
  when the descriptor at the queue's current address does not match what
  QEMU holds for its first queued packet (`ehci_verify_qtd`: the
  descriptor address, the next and alternate pointers, the token, the
  first buffer pointer), QEMU cancels the whole queue
  (`ehci_cancel_queue`) and fetches the descriptor again. A packet the
  device has already answered (QEMU's finished state) is not cancelled on
  the wire, so no cancellation reaches the simulator; QEMU tries to write
  it back first (`ehci_writeback_async_complete_packet`), but refuses
  when the queue head or descriptor has changed, and then frees it. Its
  data is then never completed to the guest, and the guest's transfer can
  run again with the next data. That would lose data with no late
  cancellation logged, as here and in the 2026-10-29 logs.
- 69 "guest updated active QH" lines over the leg (a queue head changed;
  QEMU resets the queue) went with passing runs; the one qTD case went
  with the one loss.

**Reading (a supported explanation, not a proof):** one loss in 40
echoes, every byte sent by the simulator, no cancellation of an answered
packet, the same descriptor address answered twice back to back, and the
leg's only "guest updated active qTD" at that point: this fits QEMU's EHCI
model dropping the completion of bulk-IN packets already answered when
its check of the guest's descriptor fails. Not shown: the dropped
packets' states and write-back history (the trace events used do not
carry them), which check failed (a changed field, or the queue's address
moved), and which of NUSB's descriptor updates set it off. The CP2105's
repeats were void, so its October loss (a 4 KiB block) is not read here;
the 2026-10-23 OUT replay ([usbcom-nusb-echo](2026-10-23-usbcom-nusb-echo.md))
went with the same QEMU warnings. Nothing found points at `USBCOM.SYS`'s
receive path, which logged no error, reset or resubmission.

**Fix:** none in `usbcom` (no fault found in it), and none proposed: an
emulator change is not obvious, since QEMU already writes back finished
packets where it can and refuses only when the guest has changed the
descriptor, which it might have reused. Owed: a packet-level reading in
the emulator (QEMU's packet states and write-backs with the descriptor
addresses), owed to usb4-emu's QEMU, and the same echo on a real EHCI
controller with NUSB.

Evidence: `E:\xhci98-vm\p48\wdmb\kit` (`ev\results.jsonl`, the simulator
logs `ev\comloss-w98-nusb-l1-d*.jsonl`, QEMU's trace
`ev\comloss-qemu-trace-95200.log`, a sampler of its line counts
`ev\comloss-trace-sampler.txt`, the leg script `scripts\comloss.py`);
`usbcom-w98-xh-c1` in the same `results.jsonl`.
