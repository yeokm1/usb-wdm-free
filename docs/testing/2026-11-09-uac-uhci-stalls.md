# uac2 and uac1 under 98 SE's own UHCI: streams that stop, the cause and the fix, 2026-11-09

The open item "`uac1` 0.1.1.0 ... Under 98 SE's own UHCI ... some streams
stopped completing buffers" ([open items](open-items.md),
[uac1-formats](2026-11-09-uac1-formats.md)): A-law play at 44.1 kHz, mu-law
`ramp` and the adaptive source's recording at 48 kHz each completed 1 or
0 of 8 buffers, the next stream of the same device passing. `uac2` shares
the transport (`uacusb.c`), so a Full Speed `uac2` device on a 98 SE
machine with USB 1.1 ports only meets it too. This record reads failed
streams' counters and a trace, and runs the fix, design
[uac2.md](../design/uac2.md) section 26.13, before and after. A second
part (2026-11-10, "The revision on 98 SE" below) runs 26.13 as revised
(items 4, 5 and 8) on the same stack, as found and with host stalls
added; a third (2026-11-10, "ME's, 2000's and NUSB's UHCI") runs that
build under ME's and 2000's own UHCI drivers and NUSB's. Development legs in QEMU (TCG) with the simulators, not a release
and not "Tested".

## Rig

- **Emulator:** the usb4-emu fork `cb36497` (`3e7f6bf5...`), `pc`
  machine, one vCPU, 256 MB, `piix3-usb-uhci` only. Development host A
  was loaded by other work during every leg.
- **Base:** `w98p` (98 SE as installed, its own UHCI driver only), a fresh
  overlay per leg; the device plugged after the boot, installed from a
  folder on `DevicePath`, a cold start, plugged again (98 SE's first-plug
  wedge, 36.41 in xHCI98's records, cost several plugs, as before).
- **Devices:** `uacemu.py --profile fs` (`uac2`, Full Speed: asynchronous
  playback with a feedback endpoint, recording; 8 URBs of 4 ms queued) and
  `uac1emu.py` profiles `adsrc` (an adaptive source with a synch OUT
  endpoint), `alaw`, `mulaw`, `headset`, the `uac1-formats` kit's copies.
- **Steps per round:** `UACWAV play 2 16 48000 5`, `play 2 16 44100 3`,
  `rec <ch> 16 48000 3`, `rec <ch> 16 44100 3` (as the device has them);
  after a failed step (in the after legs, after every step) the device's
  driver key is exported (`regedit /e` of the `MEDIA` class) and the
  stream counters `Uac2UsbStats` and `Uac2UsbStatsIn` decoded (`stats.py`,
  the words of `UAC_USB_STATS`).
- **Builds:**

| Build | `uac2wdm.sys` | `uac1wdm.sys` | From |
|---|---|---|---|
| released | `b3c4d555...c0b9` (0.1.10.0, `Uac2Build` 26) | `2251016c...f222` (0.1.0.1: `free/` has not been rebuilt for 0.1.1.0) | `free/` at `2fea99e` |
| `uac1` 0.1.1.0 | - | `5e1e4eff...e306` (build 2, the `uac1-formats` legs' image) | `294c891` |
| the fix | `035acb6f1d10c0aea4c116d8008255ef71dee810efb9480bc44c2f7fef0ed15e` (`Uac2Build` 27) | `674f2eaafb530f02f31672e9d44feb396c20782e2d043f634e53329b04b3b7aa` (build 3) | `98270a5`, wdmx86 free, tree clean |
| the fix, `uac1` without item 7's last step | - | `8cfaae46...efb` | `04bf305` |
| the first build of 26.13 | `a0b3f8fe...ddc2` | - | `cb6d547` |
| its trace build | `989913e8...b743` | - | `cb6d547`, `build.cmd wdmx86 trace` (test-only) |
| the revision (2026-11-10) | `cf243b47ad5e60353d57a2276749c32a7948d0c45ccdd94a9e5b84ca5f2f5345` (`Uac2Build` 28) | `568c81a586c58f65853ae69f065ff07acd97ca48428bff8b6e3fc20b9c957fa3` (build 4) | `dca05e0` (the code of `main` at `a27d075`), wdmx86 free, tree clean |

  The INFs are the released ones, except with the revision: there the
  source INFs at `dca05e0`, which differ from the released ones only in
  the device and disk names and the header comment (the names without
  "(experiment)"). `UACWAV.EXE` `1cc0f1b7...` (`8c1eb43`).

## The cause

**`uac2`, before** (leg `uac2-w98-own-b1u2`, the released 0.1.10.0, `fs`,
six rounds): playback 12 of 12, recording at 48 kHz 6 of 6, at 44.1 kHz 5
of 6. The failed recording completed none of its buffers; its counters:
384 data URBs, 250 of them failed as a whole, every one with URB status
`C0000A00` (`USBD_STATUS_BAD_START_FRAME`), `LastStatus` `C000009C`. 250
is design 25.8's run limit for 4 ms URBs: the driver ended the stream. The
simulator polled the endpoint for 0.49 s of that stream and not again
until the client closed it 13 s later.

**`uac1`, before** (`adsrc`, recording, four rounds each): 0.1.0.1 (leg
`uac1-w98-own-b1`; its synch endpoint is never sent anything) 5 of 8, the
3 failures each 250 data URBs refused (`C0000A00`); 0.1.1.0 (leg
`uac1-w98-own-b2`) 1 of 8: four failures the same 250 refusals, and
three that ended within 26 data URBs of their start because 8 synch URBs
in a row came back failed (26.12's run limit; the URB successful with
failed packets, or lost whole, `C0000B00`), no data URB failed.

**The trace** (leg `uac2-w98-own-t1u2`, the first build of 26.13, which
resynced only at a refusal): that build's streams went on where the
release's ended (in a 5 s play 8 resyncs and no end), but each resync came
after about 1.3 s with no packet on the bus. The trace showed why: after
a stall the stack went on placing ASAP URBs right after the last frame it
had scheduled, by then behind the bus (start frame `0x31275` for a URB
submitted at `0x312d1`; later `0x312a5` with the bus at `0x3180c`), each
came back lost whole (`C0000B00`, every packet `C000000F`), and the lag
grew until the stack refused a start frame, about a frame list (1024
frames) later. At the refusal the held slots were back within 70 ms; the
URBs at explicit frames then played.

## The fix

Design 26.13 (`744bc33` to `98270a5`): after a URB refused for its start
frame, or a data URB lost whole that the stack had placed behind the bus
frame read at its submission, the worker holds every slot until none is
with the stack (at most 1 s), reads the bus frame and submits each pipe's
slots at `current + 8 + k * F`, then ASAP again; 4 resyncs with no good
data URB between them end the stream. For `uac1`'s adaptive source a synch
URB with some packets delivered ends 26.12's run of failures, and once Ff
has been delivered a failed synch URB is a lost interval that never ends
the stream. Counters' words 95
to 97: the resyncs, the frame the last read, how many a URB behind the bus
began. Host tests: `uac2` `test.cmd` (`desctest.c` 10094 assertions, the
resync frames, spans, the behind test on the trace's frames; every suite
PASS), `uac1` `test.cmd` (565533 checks PASS); builds wdmx86 free (`uac2`,
`uac1`), wdm64 free (`uac2`, `c08c8656...e72a`, unsigned) and wdmx86
trace (`db993afd...d5`), each with its gates, at `98270a5`. Reviewed and
revised.

## Legs

| Leg | Build | Device | play 48k | play 44.1k | rec 48k | rec 44.1k | Notes |
|---|---|---|---|---|---|---|---|
| `uac2-w98-own-b1u2` | released | `fs` | 6/6 | 6/6 | 6/6 | 5/6 | the failure above |
| `uac1-w98-own-b1` | 0.1.0.1 | `adsrc` | - | - | 3/4 | 2/4 | 3 failures: 250 refusals each |
| `uac1-w98-own-b2` | 0.1.1.0 | `adsrc` | - | - | 1/4 | 0/4 | 4 x 250 refusals, 3 x synch run |
| `uac2-w98-own-a1u2` | first build of 26.13 (`cb6d547`, `a0b3f8fe...`) | `fs` | 1/2 | 1/1 | 1/1 | 1/1 | stopped for the trace: the failed play completed 7 of 8 buffers in 15 s, no stream end, 8 resyncs, about 1.3 s with no packet before each |
| `uac2-w98-own-t1u2` | its trace build | `fs` | 2/2 | 2/2 | 2/2 | 1/1 | stopped once read: one play with 3 resyncs, the trace above |
| `uac1-w98-own-a4` | the fix at `04bf305` | `adsrc` | - | - | 2/3 | 2/3 | stopped for item 7's last step: the 2 failures ended at 26.12's run after 51 and 53 synch URBs delivered (9 and 11 failed in a row, URB status `00000B00`), no data URB failed |
| `uac1-w98-own-a5` | the fix (`98270a5`) | `adsrc` | - | - | 4/4 | 4/4 | no stream ended; one resync (3 refused data URBs) in a stream that completed |
| `uac2-w98-own-a6u2` | the fix (`98270a5`) | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | no stream ended; one resync (2 refused feedback URBs) in a stream that completed |
| `uac2-w98-own-a7u2` | the fix (`98270a5`) | `fs` | 6/6 | 6/6 | 4/6 | 6/6 | no stream ended; 3 streams resynced (1 to 22 times); the 2 failures below |
| `uac2-w98-own-b3u2` | released, run at the same time as `a7u2` | `fs` | 5/6 | 6/6 | 4/6 | 5/6 | 4 failures, each 250 refusals |
| `uac1-w98-own-a5` | the fix (`98270a5`) | `alaw` | 4/4 | 4/4 | 4/4 | 4/4 | no stream ended |

In all: `uac2` with `fs` 43 of 48 streams before (every failure the driver
ending the stream at 250 refusals) and 46 of 48 after (none ended by the
driver); `uac1` `adsrc` recordings 5 of 8 (0.1.0.1) and 1 of 8 (0.1.1.0)
before, 8 of 8 after, `alaw` 16 of 16 after. The two failures after: both
came in the two minutes in which the release, in the other guest, failed
too (host stalls that hit both guests). In one the recording's counters
read the stream running to its end with no failed URB and 288768 bytes
captured (3 s), yet `UACWAV` received none of its buffers in 13 s; in the
other the stream resynced 22 times (132 data URBs refused) and captured 192
bytes: the simulator was polled for about 80 ms every 1.7 s and sent about
78 packets each time, which did not reach the driver, and the stack gave
the recording's URBs placed behind the bus back as successful with empty
packets, which the lag rule (made from lost URBs) does not see. Neither is
read further here (below, "Left").

Not tied to this fix, under this stack in these legs: devices WinMM did
not list after a first plug (the released `uac1` 0.1.0.1 runs no A-law,
so its `alaw` device is a null function, stage `0x40`, as designed; a
`headset` under 0.1.0.1 started, stage `0x0C`, and was not listed, once),
and 98 SE's first-plug wedge.

## The revision on 98 SE (2026-11-10)

26.13 as revised after review (`ac0e5bc` to `dca05e0`, merged at
`a27d075`): only a data URB that carried audio (a capture URB: at least
one PCM frame delivered) clears the resync limit's run and the lag test;
a successful capture URB that delivered nothing, placed behind the bus
frame read before its submission, starts a resync as a URB lost whole
does (items 4 and 5); the worker sends its frame requests and does not
wait on them (item 8). Same rig and steps as above, the revision's build,
a fresh overlay a leg, two guests at a time; development host A was
loaded by other work (six other QEMUs, CPU about 88 % at the start).

The legs as found gave the new rules nothing to do: no stream resynced.
So that they would be exercised, the other legs added host stalls of the
kind the two failures after the fix came with: from the device's
listing on, a script suspended the leg's QEMU process for a random 150
to 600 ms every 2 to 6 s (the "150-600" legs), or 300 to 1000 ms every 2
to 5 s ("300-1000"), the simulator running on (`stallinj.py`, each
stall logged; the table counts the stalls up to the leg's last stream).

| Leg | Stalls | Device | play 48k | play 44.1k | rec 48k | rec 44.1k | Resyncs |
|---|---|---|---|---|---|---|---|
| `uac2-w98-own-d1u2` | none | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | none |
| `uac2-w98-own-d2u2` | none | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | none |
| `uac1-w98-own-d1` | none | `adsrc` | - | - | 4/4 | 4/4 | none |
| `uac1-w98-own-d1` | none | `alaw` | 4/4 | 4/4 | 4/4 | 4/4 | none |
| `uac2-w98-own-i1u2` | 150-600: 213, 81 s in all | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 12 plays 1 or 2 each, at refusals; 9 recordings 1 each, behind the bus |
| `uac2-w98-own-i3u2` | 300-1000: 214, 144 s | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 10 plays 1 or 2, at refusals; 10 recordings 1 each, behind the bus |
| `uac1-w98-own-i2` | 150-600: 59, 21 s | `adsrc` | - | - | 4/4 | 4/4 | 7 recordings, 9 in all, 7 behind the bus |
| `uac1-w98-own-i4` | 300-1000: 67, 42 s | `adsrc` | - | - | 4/4 | 4/4 | 8 recordings, 9 in all, 7 behind the bus |
| `uac1-w98-own-i4` | 300-1000: 151, 99 s (from 8 s after the listing) | `alaw` | 4/4 | 4/4 | 4/4 | 4/4 | 8 plays, 12 in all, at refusals; 6 recordings 1 each, behind the bus |

In all, with the revision: `uac2` `fs` 48 of 48 streams as found (46 of 48
after the fix above), 48 of 48 with stalls; `uac1` `adsrc` 8 of 8 as
found (8 of 8 above) and 16 of 16 with stalls; `alaw` 16 of 16 as found
(16 of 16 above) and 16 of 16 with stalls. No stream ended at the driver
(`LastStatus` 0 in every stream's counters) and every recording delivered
all its buffers; neither failure after the fix above came back. 98 SE's
first-plug wedge struck four times (each leg's harness reset the guest
and went on).

What the counters (words 95 to 97, read after every step) show:

- **The empty capture URB's rule engaged.** Of the 48 recordings with
  stalls, 40 resynced, 38 of them begun at least once by a URB behind the
  bus (`ResyncBehind`). In 31 of those 38 no data URB failed at all
  (`DataUrbFailed` 0; it counts every data URB back with a failed URB
  status, a URB lost whole included), so the URB behind the bus was a
  successful capture URB that delivered nothing: item 5's new case, the
  one the build before the revision did not see. Each of the 31 resynced
  once (one `adsrc` recording twice) and went on to record every buffer.
  The other resyncs began at refused data URBs (3 each, `C0000A00`) or,
  for `adsrc`, at refused synch URBs. Playback never resynced by lag:
  every play's resync began at a refused URB, data or feedback
  (`ResyncBehind` 0), as in the legs above.
- **The frame requests the worker does not wait on worked.** The lag test
  uses only a submission frame that item 5's request returned (a slot
  submitted without one cannot begin a resync by lag), so each
  `ResyncBehind` above is one such request back and used; every resync
  read its frame (`ResyncFrame` set) and ran, and no stream ended at the
  1 s hold. No word counts a request cancelled at 100 ms or a frame read
  afresh past 64 ms, so whether those paths ran is not read.
- `uac1`'s adaptive source: failed synch URBs (`BadFeedback`) 1 or 2 a
  stream as found, 1 to 15 with stalls, none ending a stream (item 7).

## ME's, 2000's and NUSB's UHCI (2026-11-10)

The same build as "The revision on 98 SE" (`uac2wdm.sys` `cf243b47...`,
`uac1wdm.sys` `568c81a5...`, the source INFs at `dca05e0`), the same
simulators, steps, rounds and host-stall injector, under three more USB
1.1 stacks. Development host A was loaded by other work throughout.

- **ME's own UHCI:** `men` (ME as installed, `piix3-usb-uhci` only), the
  0.1.9 matrix's base, a fresh overlay a leg, the `pc` machine as above.
  `uac2` only: `uac1` is 98 SE's alone (its INF and `STAGE.TXT`), so it
  has no ME or 2000 leg.
- **2000's own UHCI:** the 0.1.9 NT matrix's `w2k-win` base (2000 with
  Windows' own stack: `usb-ehci` and `piix3-usb-uhci` on q35, the Full
  Speed device on the UHCI controller), one vCPU, the usb4-emu fork as
  above. `UACCI.INF` by its DefaultInstall, the INF staged, the first
  plug, the restart Setup asks for (on 2000 a shutdown and a cold start),
  then the rounds; after each step the `MEDIA` class key exported
  (`regedit /e`, the counters decoded as on 98 SE).
- **NUSB's UHCI on 98 SE:** the 0.1.9 matrix's `w98n2` base (98 SE with
  NUSB 3.3, its EHCI and UHCI drivers installed), the device on the UHCI
  controller (NUSB's UHCI driver).

| Leg | Stack | Stalls | Device | play 48k | play 44.1k | rec 48k | rec 44.1k | Resyncs |
|---|---|---|---|---|---|---|---|---|
| `uac2-me-uh-d1` | ME | none | `fs` | 6/6 | 6/6 | 6/6 | 5/6 | 1 play (a refusal); the failure below |
| `uac2-me-uh-mi1` | ME | 150-600: 285, 103 s | `fs` | 6/6 | 6/6 | 6/6 | 5/6 | 11 plays (13), at refusals; 9 recordings 1 each, behind the bus; the failure below |
| `uac2-me-uh-mi3` | ME | 300-1000: 305, 192 s | `fs` | 6/6 | 6/6 | 6/6 | 5/6 | 8 plays (13), at refusals; 11 recordings (13), behind the bus; the failure below |
| `uac2-w2k-win` (`11101558`) | 2000 | none | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | none |
| `uac2-w2k-win` (`11101647`) | 2000 | 150-600: 130, 48 s | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 8 plays 1 each, at refusals; 8 recordings 1 each, behind the bus |
| `uac2-w98-uhc-d1` | NUSB | none | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 1 play, 1 recording (behind the bus) |
| `uac2-w98-uhc-ni1` | NUSB | 150-600: 230, 84 s | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 8 plays (14), at refusals; 7 recordings (8), behind the bus |
| `uac1-w98-uhc-d1` | NUSB | none | `adsrc` | - | - | 4/4 | 4/4 | 1 recording of the leg's 16 (`adsrc` and `alaw`), behind the bus |
| `uac1-w98-uhc-d1` | NUSB | none | `alaw` | 4/4 | 4/4 | 4/4 | 4/4 | (above) |
| `uac1-w98-uhc-ni2` | NUSB | none (see below) | `adsrc` | - | - | 4/4 | 4/4 | none |
| `uac2-w98-uhc-ni3` | NUSB | 300-1000: 237, 155 s | `fs` | 6/6 | 6/6 | 6/6 | 6/6 | 11 plays (14), at refusals; 10 recordings (12), behind the bus |
| `uac1-w98-uhc-ni5` | NUSB | 150-600: 291, 109 s (each device from its listing) | `adsrc` | - | - | 4/4 | 4/4 | 13 of the leg's 16 recordings (14 in all), behind the bus |
| `uac1-w98-uhc-ni5` | NUSB | (as above) | `alaw` | 4/4 | 4/4 | 4/4 | 4/4 | 7 plays (9), at refusals; recordings above |

In all, every stream that opened completed all its buffers: no stream
ended at the driver (`LastStatus` 0 in every stream's counters), with
and without stalls, under all three stacks. `uac2` `fs`: ME 69 of 72
streams (the three failures are not streams: below), 2000 48 of 48,
NUSB 72 of 72 (24 as found, 48 with stalls); `uac1` under NUSB:
`adsrc` 16 of 16 as found and 8 of 8 with stalls, `alaw` 16 of 16 as
found and 16 of 16 with stalls. With stalls the rules of 26.13 ran as on 98 SE: plays resynced
only at refused URBs (`ResyncBehind` 0), recordings at a URB behind the
bus, most with no data URB failed (ME 9 of 9 and 9 of 11, 2000 7 of 8,
NUSB 7 of 7 and 10 of 10 for `uac2`, 12 of 13 for `uac1`), so the empty capture URB's rule engaged on each stack;
every resync read its frame (`ResyncFrame` set), and no stream reached
the 1 s hold's end or the resync limit.

**ME: a recording at 44.1 kHz that did not open, once in each ME leg.**
`UACWAV rec 1 16 44100 3` came back `open=8` (`MMSYSERR_NOTSUPPORTED`)
3 times in 18, the other 15 recorded every buffer; nothing reached the
device (the capture counters unchanged). The driver key read
`Uac2M0Stage` 30 (the pin's `NewStream`) with `Uac2M0Status` `80000011`
(`STATUS_DEVICE_BUSY`): the one-stream gate of the capture direction was
still held. `Uac2Streams` shows ME's audio stack creating and closing
several capture pins at each 44.1 kHz open (attempts climbing by 2 to 6
a step, created by 1 to 2), and in 3 of the 15 that passed the stream
ran at 48 kHz (ME's own rate conversion over the 48 kHz pin). The gate
is released when the stream object is destroyed, which a closed pin's
final release queues to a work item (`uacstream.h`, `Release`), so a
pin created right after the close of the one before can meet the gate
still held. That fits the readings; it is not proven, it was not seen
on 98 SE, 2000 or NUSB, and the driver was not changed here (a wait for
a closing stream in `Create` would have to drop the operation mutex the
cleanup's `Close` hook takes). Under the same stacks every other open
succeeded.

**2000** needed the first plug's wizard to finish before Setup's
restart: the base shows a Found New Hardware wizard for an unknown PCI
device first, and the first try restarted before the USB device's own
wizard came, so the device had no driver (void, rerun; the harness now
waits until `uac2wdm` is the device's service). `UACWAV list` took two
5-minute timeouts after the restart while 2000 set up its kernel audio
stack, as the matrix recorded.

**NUSB, `uac1`:** in `uac1-w98-uhc-ni2` the injector's start condition
was given wrongly and it stalled the guest during the `alaw` install, not
the rounds, so its `adsrc` rounds count as a second leg as found, and its
`alaw` device was never installed (two first-plug wedges, then the
install step failed); `uac1-w98-uhc-ni5` ran it again with the start
condition right. 98 SE's first-plug wedge struck the NUSB legs seven
times in all (each leg's harness reset the guest and went on).

## Left

- The lag rule for recordings: read with the revision (above), not with
  a trace. The two recordings that failed after the fix above (one ran
  at the driver while `UACWAV` got no buffer; one resynced 22 times and
  got 192 bytes, the simulator's data not reaching the driver, QEMU's
  `usb-redir` isochronous IN path suspected) did not come back, as found
  or with stalls; their causes are not read.
- On real USB 1.1 ports with real devices (the owner's bench). ME's,
  2000's and NUSB's UHCI in QEMU: run (above).
- ME: the 44.1 kHz recording refused at the capture gate (`open=8`, 3
  in 18, above): fixed by reading on 2026-11-11 (design
  [uac2.md](../design/uac2.md) section 27: an open waits for a closing
  stream's queued destroy, `Uac2Waits` counts it); under ME's own UHCI
  before it 6 of 20 refused, after it 20 of 20 with 15 opens waiting
  (xHCI98 `runs/run-49.md`, "49.8"); 98 SE's round owed.
- NUSB on Windows 95: no image with NUSB on 95 exists here; owed.
- `uac1`'s `mulaw`, `mpoout`, `pcm8in`, `floatin` and `headset` under this
  stack with the fix (not run here; `adsrc` and `alaw` were).
- One guest that went black and stopped answering just after a `uac2`
  `fs` play under this stack with the release (`uac2-w98-own-b0u2`, its
  first stream; the stream had played its 5 s): not seen again in the
  legs above; cause not read.
- The devices WinMM did not list after a first plug (above): not tied to
  the stalls; open as in [uac1-formats](2026-11-09-uac1-formats.md).
- `free/`'s `uac1wdm.sys` is still 0.1.0.1 while `main`'s source is 0.1.1.0
  (`uac1wdm.rc`'s version strings read "0.1.1.0" since `20a3b56`; the
  INF's `DriverVer` still reads 0.1.0.1): the release owes both.
- With the revision: the frame request cancelled at 100 ms and the frame
  read afresh past 64 ms (26.13 item 8) have no counter, so whether they
  ran in these legs is not read.

## Evidence

`E:\xhci98-vm\p48\aud98\kit\` (development host A): `ev\results.jsonl`
(one line per step), `ev\<leg>-d<n>.jsonl` and `.pcap` (the simulators'
logs and captures), `g\<leg>\serial-*.log` (the steps' output and the
registry exports), `g\uac2-w98-own-t1u2\e9-b2.log` (the trace),
`scripts\stats.py`, `shots\`, `pids.txt`. The revision's legs:
`E:\xhci98-vm\p48\aud3\kit\`, laid out the same, with
`g\<leg>\stallinj.log` (each stall added) and `scripts\stallinj.py`. ME's, 2000's and NUSB's
UHCI: `E:\xhci98-vm\p48\uhci\kit\` (ME and NUSB, laid out the same)
and `E:\xhci98-vm\p48\uhci\ntkit\` (2000: `legs\<set>\serial-a.log`,
`steps.txt`, `result.json`, `stallinj.log`, `shots\`; `ev\` the
simulator's logs; `legs.py` `run_stall`).
