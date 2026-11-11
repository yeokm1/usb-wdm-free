# usbmidi 0.1.9 on 98 SE under xHCI98, `multi`: the fatal exception watch, 2026-11-09

The open item "`usbmidi` on 98 SE under xHCI98, `multi`: one fatal
exception (watch)" ([open items](open-items.md)): one VMM fatal exception
0E (`0028:C000A4B0`, `VMM(01) + 000094B0`) during the first `MIDITEST`
of the [9x matrix](2026-11-07-matrix-9x.md)'s leg b, in 9 legs of the
`multi` profile. This record tries to reproduce it with the matrix's own
leg, six times. Development legs in QEMU (TCG) with the simulator, not a
release and not "Tested".

**Outcome: not reproduced** in 6 more legs (16 `MIDITEST` loopbacks, every
one PASS). With the matrix's, the fault stands at 1 in 15 legs of
`multi`; nothing in these legs points at the driver. It stays a watch
item.

## Rig

- **Driver:** `usbmidi` 0.1.9 as released (`free/audio/usbmidi/w98se`):
  `usbmidi.sys` `14437317030c2a7a5ea0aec9b8e5de49028c1f7e38d701cfdae71dd865c74245`;
  the leg's test copy of `usbmidi.inf` (the released lines plus the
  simulator's ids, as in the matrix).
- **Emulator:** the usb4-emu fork `cb36497` (`3e7f6bf5...`), `pc` machine,
  one vCPU; `qemu-xhci`, `usb-ehci`, `piix3-usb-uhci`.
- **Base:** the matrix's `w98x` (98 SE, NUSB 3.3, xHCI98 3.4.2.0
  `3990e17d`), read-only, a fresh overlay per leg.
- **Simulator:** `midiemu.py --profile multi --speed full` (two MIDI
  in/out pairs) at `bf92407`, as in the matrix, on `qemu-xhci`.
- **Leg** (the matrix's `usbmidi` leg unchanged): stage the INF on
  `DevicePath`, plug, answer the install, cold start; `MIDITEST` loopback
  on each pair; unplug and replug, `MIDITEST`; restart, `MIDITEST`.
- **Host:** development host A was loaded by other work (94% CPU, 11
  QEMU processes at one reading), so the guests ran slower than in the
  matrix, which is the condition the fault was first met under.

## Results (legs `usbmidi-w98-xh-m1` to `-m6`)

| Step | m1 | m2 | m3 | m4 | m5 | m6 |
|---|---|---|---|---|---|---|
| install, two MIDI in and two out listed | PASS | PASS | PASS (after 2 arrival wedges, reset) | PASS | PASS | PASS |
| `MIDITEST` loopback, pair 0 and pair 1 | PASS, PASS | PASS, PASS | PASS, PASS | PASS, PASS | PASS, PASS | PASS, PASS |
| `MIDITEST` after a replug | known MMSYSERR_ALLOCATED | known | known | known | known | known |
| `MIDITEST` after a restart | harness | harness | PASS | PASS | PASS | PASS |
| fatal exception or fault screen | none | none | none | none | none | none |

- "known": 98 SE answers every open with MMSYSERR_ALLOCATED after a
  replug until a restart (the documented limitation).
- "harness": the typed command line reached the guest garbled and
  `MIDITEST` printed its usage line, as in the matrix's r5.
- The arrival wedges (98 SE's first plug of a boot, task 36.41) and three
  hangs at the final shutdown (m4 to m6; the matrix's known shutdown hang)
  showed no fault screen.

## Evidence

`E:\xhci98-vm\p48\wdma\uvc\` (development host A): `ev\results.jsonl`
(legs `usbmidi-w98-xh-m1` to `-m6`), `ev\usbmidi-w98-xh-m*-d*.jsonl` (the
simulator's logs), `g-midi-m*.out`, `shots\usbmidi-w98-xh-m*`,
`pids.txt`.
