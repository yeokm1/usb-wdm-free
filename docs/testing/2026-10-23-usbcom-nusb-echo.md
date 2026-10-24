# usbcom: the 64 KiB echo under NUSB's EHCI, 2026-10-23

The owner asked (2026-10-23) for a closer reading of the 98 SE leg that
failed a 64 KiB echo now and then under NUSB 3.3's EHCI driver
([guest legs](2026-10-22-usbcom98-legs.md), "Below the driver"). This
record finds where the data went wrong, and it fixes that in `USBCOM.SYS`
by keeping one bulk OUT transfer in flight instead of four. Every run here
is QEMU (TCG, `usb-ehci`) with this package's simulated CDC-ACM device. No
real controller or board was used, so nothing here is a "Tested" result.

Kit `E:\usb-wdm-free\usbcom9x`, guest `n` (98 SE + NUSB 3.3, the device on
`usb-ehci`, High Speed, Uno model), ports 49170-49179, one guest at a time,
smp 1, 256 MB. The host was loaded by other agents' guests, so timings are
noisy. Branch `usbcom-nusb`.

## Before the fix: about half the echoes failed

`series.ps1 -N 30` runs `COMTEST COM3 echo 65536` once per run, opening the
port each time:

| Run | Build | Simulator | 64 KiB echo |
|---|---|---|---|
| n4 (a) | trace | as in the legs | 19 of 30 PASS |
| n4 (b) | trace | `--no-bulk32` (QEMU does not combine bulk IN packets) | 9 of 30 PASS |
| n6 | trace | as in the legs, QEMU EHCI trace events on | 7 of 10 PASS |

That is a higher failure rate than the legs' one in six, on a busier host.
The program's own readings separate two failures:

- "got 65536, first difference at *n*, 16 extra" (29 runs): one 4096-byte
  block came back twice.
- "got 61440" (3 runs): one block never came back.

**Combining is not the cause.** With `--no-bulk32`, which turns QEMU's
combining of bulk IN packets off, it failed more often, not less.

## Where it goes wrong: QEMU cancels and replays an EHCI queue

The simulator's log shows the duplicate directly: the same OUT block,
**under the same usbredir packet id**, reaches the device twice. Between the
two there is a `cancel_late`, a cancel from QEMU for a packet the device had
already completed. (The legs' record said "two packet ids"; that was wrong.
The id is the qTD's address, and it is the same.)

QEMU's own trace (`-trace usb_ehci_guest_bug`) names the event. In run n6 it
logged "guest updated active qTD" 3 times, once per failed echo. At that
point QEMU's EHCI model finds that the first packet it holds for a queue no
longer matches the qTD at the queue head's current pointer. It then cancels
every packet of the queue and fetches the qTDs again. A usb-redir device
completes packets asynchronously, so a cancelled OUT may already have been
delivered: the device had answered it, which is the `cancel_late`. Fetched
again, it is sent again, and the device gets the block twice. A cancelled IN whose answer is already on its way back
is lost, which is the 61440 case.

This takes two parts:

1. **NUSB's EHCI driver rewrites queue state while a qTD is active.** NUSB
   3.3's EHCI driver is the Windows 2000 `usbehci.sys` stack, and it does
   this while several transfers are queued. Whether that is allowed depends
   on what it writes (the EHCI specification lets software append to a live
   queue). These runs record only QEMU's verdict.
2. **QEMU's EHCI model replays a queue it has cancelled.** A real
   controller does not take back a transaction it has already completed on
   the bus, so it has nothing to replay.

The data is lost below the driver: in QEMU's EHCI model, triggered by NUSB's
queue updates. `usb-redir` only carries the replayed packet; it does not
create it. NUSB's qTD handling is not proven wrong. A short packet is not
involved: every failed block was a full 4096-byte transfer.

## The fix in `USBCOM.SYS`: one bulk OUT transfer in flight

`usbcom.h` `UC_OUT_URBS` goes from 4 to 1. Each OUT transfer still carries
up to 4096 bytes. The next one starts from the completion, so the cost is
one completion gap per 4 KiB. In these High Speed VM runs a 64 KiB echo took
as long as before. Throughput at Full Speed, and on real hardware, is not
measured. Bulk IN is unchanged: eight 4096-byte reads.

| Run | Build | Checks | Result |
|---|---|---|---|
| n7 | trace | 20 × 64 KiB echo | 20 of 20 PASS |
| n8 | **free** | `full` 58/58, pull while open and replug 6/6, 30 × 64 KiB echo | all PASS |
| x26 | **free**, under xHCI98 (regression) | `full` 58/58, pull 6/6, 16 × 64 KiB echo | all PASS; a 64 KiB echo took 43-50 ms, as before |

All 50 echoes under NUSB passed. QEMU still logs "guest updated active
qTD" (57 in n7, 77 in n8). But the simulator saw **no** `cancel_late` in
n7 or n8, against 8 and 11 in the two n4 series. With one OUT in flight,
QEMU's queue cancels no longer catch a packet the device has already
answered. A 64 KiB echo took about 430 ms, as before.

An echo test cannot prove the absence of a race. It now did not occur in
50 runs, where before the fix it failed in 35 of 70.

## Control: Windows' own EHCI driver on the same QEMU

Guest `p`: XP SP3 (an overlay of the kit-shared `vm\wxp.qcow2`), smp 2,
512 MB, the same `usb-ehci`, the same simulator (defaults, flush packet on)
and the same QEMU trace. `USBCDC.INF` binds Windows' `usbser.sys`, and
XP's own `usbehci.sys` drives the controller. `COMTEST -noe9 -log COM1 COM3
echo 65536` ran 23 times; COM1 went to a host file. The series was stopped
at 23 by a usage pause.

- 23 of 23 PASS, 93-171 ms each.
- QEMU logged no "guest updated active QH/qTD" event in these 23 runs, and
  the simulator saw no `cancel_late`.

The trigger was seen only with NUSB 3.3's EHCI driver. This control compares
the two stacks: the OS, the serial driver and the VM's resources also
differ, and the number of OUT transfers in flight may differ (XP's was not
measured), so it does not by itself isolate NUSB's EHCI driver.

## REAL CHECK NEEDED (owner's bench)

Whether a real EHCI controller under NUSB 3.3 ever duplicates or loses a
block with the old four-OUT build. That QEMU's model causes it is a reading
of these traces, not a result on hardware, and a pass on one PC would not
prove it. The fix's effect on real hardware, including throughput, is also
unverified. Steps:

1. 98 SE with NUSB 3.3 on a PC with a real EHCI controller, and an Uno R3 or
   Pico behind a High Speed hub.
2. Install the previous build (`USBCOM.SYS` with four OUT transfers: the
   `working` branch before `usbcom-nusb`) and run
   `COMTEST COMn echo 65536` 30 times, with the board running a plain
   echo sketch at 115200.
3. Repeat with this branch's build.
4. Record the pass counts and, for each failure, COMTEST's "first
   difference" and "extra" line.

## Also on this branch: `--no-flush-zlp` for the 2000 legs

The wdm-sweep agent found that on Windows 2000, after the simulator sends
its flush zero-length packet following a full 64-byte echo, no further IN
request from 2000's `usbser.sys` appeared during the failing read. That is
the 65-byte echo failure in [usbcdc-vm](2026-10-21-usbcdc-vm.md). A copy of
the simulator without that packet passed the echoes of 1, 63, 64, 65, 128
and 1537 bytes. `acmemu.py --no-flush-zlp`
(off by default) now sends no flush packet, and 2000's legs use it. Whether
a real Uno R3 or Pico firmware sends such a packet after a full-packet
echo is a REAL CHECK for the owner: a USB analyser, or Linux's `usbmon`,
on an echo of exactly 64 bytes.

## Evidence

`E:\usb-wdm-free\usbcom9x\evidence\`:

- the 0xE9 logs `n-ehci-e9-n4.log`, `n-ehci-e9-n7-out1.log`,
  `n-ehci-e9-n8-free-out1.log` and `x-xhci-e9-x26-free-out1.log`;
- QEMU's traces `qtrace-n6.log` (queue and packet events), `qtrace-n7.log`
  and `qtrace-n8.log` (guest-bug events);
- the simulator's logs `n4-uno-hs.jsonl`, `n4-nobulk32.jsonl`,
  `n7-out1.jsonl` and `n8-free-out1.jsonl`;
- the XP control `p-xp-com1-p1.log`, `p1-xp-series.txt`, `qtrace-p1.log` and
  `p1-uno-hs.jsonl`;
- the series results `n4a-series-default.txt`, `n4b-series-nobulk32.txt`,
  `n6-series.txt`, `n7-series-out1.txt` and `n8-free-series-out1.txt`.
