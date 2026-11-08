# uac2 0.1.10.0: Full Speed feedback in 3 and 4 bytes, 2026-11-07

The legs of [uac2.md](../design/uac2.md) section 23: a Full Speed UAC 2.0
device whose explicit feedback endpoint is USB 2.0's 3 bytes (10.14) and
one whose endpoint is 4 bytes (16.16 samples per frame, the form Windows
10's own UAC 2.0 driver reads), on 98 SE and XP, under xHCI98 3.4.1.0 and
under the systems' USB 2.0 stacks. Development legs in QEMU (TCG) with the
simulator, not a release and not "Tested".

## Rig

- **Build:** branch `w47-wdm6-uac` `3d1a5cb`, `build.cmd wdmx86 free`:
  `uac2wdm.sys` `90930794807d49bd...` (0.1.10.0), `uac2wdm.inf`
  `0c8944fd8c1bbd52...`; `build.cmd wdm64 free` `f06bff1ede7080f0...`
  (unsigned: the x64 image needs the owner's signature before an x64
  leg or release). Host tests: `test.cmd`, `rtest.cmd` (98579 checks),
  `streamtest.cmd` pass. Control: the released 0.1.8.26
  (`free/audio/uac2/w2k-xp`, `d677357681a4afb7...`).
- **Simulator:** `uacemu.py` of branch `w47-ref2` (`66d99ee`, `ecce6e38...`),
  profile `fs` (Full Speed; playback 2 channels 16-bit, capture 1 channel
  16-bit; clock 44.1 and 48 kHz), `--fs-feedback 3` (configuration
  `aa0220f1...`) or `--fs-feedback 4` (`989cb6b4...`), with
  `--fb-ppm 15000`: the device asks 1.5% more than nominal, so a driver
  that follows the feedback sends 49 frames in about 72% of the packets at
  48 kHz (48.72 a frame) and 45 in about 76% at 44.1 kHz, where nominal
  is 48 or 44 and 45.
- **Emulator:** the usb4-emu fork `cb36497` (`3e7f6bf5...`, `usb-redir`,
  the isochronous real-time fix).
- **98 SE:** an overlay of a flattened copy of the 2026-11-06 98 SE guest
  of [uac2-swap](2026-11-06-uac2-swap.md) (98 SE, NUSB 3.3's EHCI,
  `USBHUB.SYS`, xHCI98 3.4.1.0, `uac2` 0.1.8.26), `pc` machine, one vCPU;
  `qemu-xhci`, `usb-ehci` and a `piix3-usb-uhci` added for these legs
  (its wizard took NUSB's updated driver). `uac2wdm.sys` 0.1.10.0 copied
  over the installed one and into the install source; the `fs` device
  installed with the 0.1.10.0 INF (Have Disk).
- **XP:** XP SP3 x86, an overlay of the shared q35 XP disk, two vCPUs;
  xHCI98 3.4.1.0 (`INSTALL.BAT /console /yes`, exit 3, cold start),
  `UACCI.INF` (DefaultInstall) and the 0.1.10.0 `uac2wdm.inf` staged with
  `SetupCopyOEMInf`; each first plug through Found New Hardware
  (Continue Anyway).
- **USB 2.0 stacks and Full Speed.** QEMU's `usb-ehci` has no companion
  controller and no transaction translator, so a Full Speed device never
  runs on it: the legs "under NUSB" ran on a UHCI controller (`piix3-usb-uhci`)
  under NUSB 3.3's stack on 98 SE, and "under Windows' stack" on XP's
  own UHCI. A real EHCI port behind a High Speed hub is not covered.
- **Steps per leg** (`UACWAV.EXE` `b15d4238...`, built from this tree):
  `play 2 16 48000 5`, `play 2 16 44100 5`, `rec 1 16 48000 3`, a
  `play 2 16 48000 20` with the device unplugged after about 8 s,
  replug, `play 2 16 48000 3`. The device's clock stays at 48 kHz in those
  (the mixer resamples 44.1); a second plug with `--rates 44100` (the
  device offers 44.1 kHz only) runs the stream at 44.1 kHz: `play 2 16
  44100 5`, `play 2 16 48000 5`, `rec 1 16 44100 3`.

## Results

Packets are the simulator's count of the OUT packets of each play, by
length (192 bytes = 48 frames, 196 = 49; 176 = 44, 180 = 45).

| System, stack | Feedback | Plays, record, unplug, replug | 48 kHz packets (49 frames) | 44.1 kHz device (45 frames) |
|---|---|---|---|---|
| 98 SE, xHCI98 3.4.1.0 | 3 bytes | all PASS; record 1 kHz tone (999 Hz); the unplugged play ended with its buffers returned, the replugged device played | 4930: 1444 / 3486 (70.7%) | 4925: 1236 / 3689 (74.9%); record PASS |
| 98 SE, xHCI98 3.4.1.0 | 4 bytes | all PASS, the same | 4939: 1453 / 3486 (70.6%) | 4925: 1236 / 3689; record PASS |
| 98 SE, UHCI under NUSB 3.3 | 3 bytes | all PASS | 4908: 1424 / 3484 (71.0%) | 4904: 1211 / 3693; record PASS |
| 98 SE, UHCI under NUSB 3.3 | 4 bytes | all PASS | 4910: 1420 / 3490 (71.1%) | 4912: 1218 / 3694; record PASS |
| XP SP3, xHCI98 3.4.1.0 | 3 bytes | all PASS | 4963: 1454 / 3509 (70.7%) | not run |
| XP SP3, xHCI98 3.4.1.0 | 4 bytes | all PASS | 4973: 1459 / 3514 (70.7%) | not run |
| XP SP3, Windows' UHCI | 3 and 4 bytes | plays FAIL (0/8 buffers; the device got about 30 packets, then nothing); record, unplug and replug PASS | - | not run |

The share of 49-frame packets is the feedback at work: a driver at the
nominal rate sends none, and the share is below 72% only by the first
packets of each stream, sent before the first feedback value.

**Controls.**

- 98 SE, xHCI98 3.4.1.0, the released 0.1.8.26: with the 4-byte endpoint
  the play FAILs (0/8 buffers; the stream stops after 89 packets, the
  driver asked 3 bytes of a 4-byte answer); with the 3-byte endpoint it
  plays (4929 packets, 3482 of 49 frames). 0.1.8.26 asks 3 bytes per
  Full Speed feedback packet; the 4-byte case is what the change fixes.
- XP, Windows' UHCI, the released 0.1.8.26, 3-byte endpoint: the play
  FAILs the same way (0/8, 32 packets). So the 3-byte failure on XP's
  UHCI predates this change; the cause of the failures with 0.1.10.0 and
  any part this change has in them (the 4-byte case had no control) are
  not established (QEMU's UHCI with `usb-redir` or the driver: not read
  further here).

**Open (not settled).** On 98 SE three times a guest stopped in ring 0
(the clock stopped, the pointer test tool's lines stopped, the processor
in `ff0270ed` and `c00xxxxx`), each within minutes of a cold start and
the first plug of a device on xHCI98's port. Two were during Add New
Hardware for an `absemu.py` touch screen, with no `uac2` device present,
so the guest can stop without `uac2`. The first was at the first play of
the `fs` device with 0.1.10.0 loaded (the play's start line reached the
serial port, the simulator saw no stream start); whether it shares the
others' cause is not established, and it did not recur in the later
cold start that ran every 98 SE leg above. All three were with the added
UHCI controller present; the starts without it (the `absptr` legs of
2026-11-07) had none. The debug channel of the released xHCI98 is silent.

## Not covered

- 2000, ME, XP x64, Vista and later (the x64 image is unsigned).
- A Full Speed device on a real EHCI port (transaction translator).
- The 44.1 kHz device on XP.
- A real Full Speed UAC 2.0 device with either endpoint size.
