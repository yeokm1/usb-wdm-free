# uac2 playback level baselines for the engine's move, 2026-11-17

Design record section 28.15 (`docs/design/uac2.md`): before the playback
level engine leaves `uacwdm.cpp`, the shipped build's Feature Unit
requests are recorded in a guest, so that the moved code
(`src/audio/common/uaclevel.c`) can be replayed against them
(`src/audio/uac2/leveltest.c`, `src/audio/uac2/testdata/levels`). These
are development legs in QEMU (TCG) with the simulator, not "Tested"; no
real device was used.

## What ran

- Driver: the shipped build, release 0.1.11's `uac2wdm.sys` 0.1.12.0 for
  2000 and XP, as `free/audio/uac2/w2k-xp` holds it at main's tip
  (`934db4cf...`; no audio source changed after that release), with its
  INF and the co-installer: the co-installer's DefaultInstall, the INF
  staged (`devcon dp_add`), each new devnode taken by the Found New
  Hardware wizard (the driver is unsigned: "Continue Anyway").
- Guest: XP SP3 x86, a fresh overlay of the q35 base, one vCPU, stock QEMU
  11.1.0 (`usb-redir`), Windows' own EHCI (`usb-ehci`, no xHCI). Every
  `.lvl` but `cx31993m-first.lvl` was taken with the simulator on EHCI
  port 1; `cx31993m-first.lvl` is the start inside the install of a
  devnode on port 2, before the port was fixed (a device plugged on
  another port is another devnode, with no driver until installed).
- Simulator: `uacemu.py` at `1142c5f` (its control log carries each
  request's data stage), one process per plug, so a replug starts from
  the simulator's power-up state, as a real device does.
- Steps: `UACWAV vol` and `mute` (the mixer API the Volume Control uses),
  `UACWAV play` (2 channels, 16 bits, 48 kHz, 3 s) for a RUN, unplug and
  replug; the MEDIA class keys (`Uac2VolMap`, `Uac2Level`) read after
  the steps.
- Conversion: `levbase.py` lists the Feature Unit requests of the
  simulator's log; the events in each `.lvl` come from the harness steps
  and the values read after them.

## What WDMAud sent

- A stereo volume change from the mixer API reaches the driver as two
  KS SETs, channel 0 then channel 1 (`Change` grows by 2 a step); a mute
  change as one. A step to the value the mixer already holds sends
  nothing (the first `vol 65535` after arrival, where the device's level
  was already the top: no SET, the volume stays unowned).
- At a replug, after the start has restored the saved level, WDMAud sets
  its saved mixer state again: 4 SETs, at the values the driver already
  holds, so no request.

## Profiles and results

"Identical" below means identical for the assertions each file holds:
every request (direction, request, selector, channel, entity, length,
every byte written) in order and no request left over, and, where the
file has them, the `Change`, `Uac2VolMap` (232 bytes) and `Uac2Level`
checks. `cx31993m-first.lvl` holds request checks only. The IN answers
are the recorded ones, and two SETs of one mixer step are checked
together (the trace cannot tell which of them sent which request). It
is not a proof of preservation beyond those assertions.

| Profile | Simulator | What it covers | Replay |
|---|---|---|---|
| `cx31993m-first.lvl` | `--profile cx31993m --vol-range -32512:3072:256 --vol-cur 3072` | the start inside the wizard's install: the two RANGEs, the two CURs at +12 dB, both lowered to 0 dB (section 20.3) | identical |
| `cx31993m.lvl` | the same | first arrival (lowering), volume to -8, -26, then -8/-16 dB (balance) per channel, mute on and off (software: no device mute), a play, a replug restoring -8/-16 dB, one more step to -3 dB, a play | identical: 2 starts, 14 SETs, every request and value, `Uac2VolMap` (232 bytes) and `Uac2Level` at each check |
| `hs24-full.lvl` | `--profile hs24 --fu full --vol-range -15360:3072:128 --vol-cur-at 0=1536 --vol-cur-at 1=-10240 --vol-cur-at 2=-10240` | master and channel mute and volume on Feature Unit 2; the start lowers the +6 dB master to 0 dB and leaves the -40 dB channels; the first owned volume sets the channels (extras) to 0 dB; master steps, balance in software, the device mute, a play, a replug restoring level and mute, one more step | identical: 2 starts, 12 SETs |
| `hs24-master.lvl` | `--profile hs24 --fu master --pid 0202 --vol-cur -2560` | master mute and volume only, -10 dB at power-up | identical: 2 starts, 16 SETs |
| `hs24-channels.lvl` | `--profile hs24 --fu channels --pid 0203 --vol-cur -24576` | mute and volume per channel only, both at the -96 dB minimum at power-up | identical: 2 starts, 16 SETs |
| `x4full.lvl` | `--profile x4full` | the captured X4 layout: master mute and volume on Feature Unit 1 after Mixer Unit 7, eight channel volumes as extras; pair 0's requests only | identical: 2 starts, 14 SETs, 45 requests |

The X4 makes two pairs (section 15.3): pair 1 drives Feature Unit 14 on
the second playback terminal, at the same starts. Its 14 requests are
left out of `x4full.lvl`, and so are the two `Uac2VolMap` readings it
wrote last (both pairs write that one value); the file is pair 0's. The
X4's vendor interface (`MI_01`) has no driver in the guest and was
disabled so that its wizard would not take the harness's keystrokes.

Each `.lvl` after the first two was taken with the MEDIA class keys read
after every step, so each step's events follow from its own `Change`.

## The preservation gate of the wiring

The playback path of `uacwdm.cpp` calls `uaclevel.c` (11d5afd); the
retained implementation builds with `UAC_LEVEL=retained`. The gate ran
the same harness scripts with the wired image (`uac2wdm.sys` wdmx86 free
from 11d5afd, `4f56175f...`) and with the shipped image (`934db4cf...`)
in the same XP SP3 guest, the image swapped by copying it over the
installed file with no device present and restarting, the saved level
deleted before each leg (a first arrival each time). Each before/after
pair was run from the same state: the shipped and wired legs of a
profile each started after a leg on the same devnode that ended with the
same mixer step, because WDMAud keeps and sets its own mixer state: the
first captures above, taken on fresh devnodes, are not that state, so
the gate compares new captures with each other.

Compared: every Feature Unit request of both plugs, of every pair, in
order, with the data written and the answers (`levbase.py`); and every
`Uac2VolMap` (232 bytes) and `Uac2Level` read after each step.

| Profile | Case | Requests, first plug / replug | Requests | Values |
|---|---|---|---|---|
| cx31993m | the steps of `cx31993m.lvl` | 16 / 10 | equal | equal |
| hs24 `--fu full` | the steps of `hs24-full.lvl` | 19 / 14 | equal | equal |
| hs24 `--fu master` | the steps of `hs24-master.lvl` | 10 / 5 | equal | equal |
| hs24 `--fu channels` | the steps of `hs24-channels.lvl` | 20 / 10 | equal | equal but `Change` |
| x4full | both pairs (pair 1's Feature Unit 14 included) | 33 / 28 | equal | equal but `Change` |
| hs24 `--fu full` | a failed request (`--vol-cur-stall 2`) | 21 / 15 | equal | equal but `Change` |
| hs24 `--fu full` | `Uac2VolAboveZero` = 1 | 18 / 12 | equal | equal |

"Equal but `Change`": every request and every other byte were equal;
`Change`, the count of KS SETs the driver accepted, differed. What the
saved logs show, checkpoint by checkpoint (`Change` and `Writes` from
the `Uac2VolMap` read after each step; `VolumeUnit` in the same record
says which pair wrote it):

| Case | Checkpoint where they differ | `Change` shipped / wired | Every other checkpoint | `Writes` |
|---|---|---|---|---|
| hs24 `--fu channels` | the first plug (an arrival, no mixer step) | 4 / 2 | the same increment in both (2 per stereo step, 1 per mute step, 0 at a play and the replug's own 4 / 4) | equal at every checkpoint |
| x4full, pair 0 (`VolumeUnit` 1 in every reading) | the replug (an arrival, no mixer step) | 4 / 8 | the same increment in both | equal at every checkpoint |
| hs24 `--fu full`, failed request | the first plug | 2 / 4 | the same increment in both | equal at every checkpoint |

So each difference arises at one arrival only and is constant after
it. **The logs do not show the SETs themselves.** The harness log
records each mixer call (`UACWAV`: one call per step, with the values
before, asked and after), not the KS SETs WDMAud makes for it, and
nothing at an arrival, where WDMAud sets its saved mixer state on its
own; the events in the `.lvl` files were derived from the `Change`
deltas by the converter (a mute SET assumed for each count beyond a
stereo step's two). That the difference is a difference in accepted
SETs was therefore inferred from the counter itself, with the code
(both implementations add 1 to `Change` for every accepted SET,
redundant ones included, and for nothing else) as the only support. A
handler trace was then taken (below, "The `Change` count traced").

The failed-request and above-zero cases were also captured as baselines
(`hs24-stall.lvl`, `hs24-above0.lvl`) and replay through `uaclevel.c`
identically.

## The `Change` count traced

To see the SETs themselves, the trace flavour (`wdmx86`) traces every
accepted KS SET in both `LevelProperty` paths (pair, volume or mute,
channel, value, `Change` after it) and every `PowerLocal` transition,
to the debug console. `Change` is sampled for the trace after the level
lock is released, so SETs racing each other could show duplicated or
reordered values; the trace counts the handler's accepted entries in
these runs, it is not a serialized history of the counter (in these
runs the values ran 1, 2, 3, ... without a gap or repeat). A trace
build's own locks and console output also change its timing. Two trace images were built from the same source:
the retained implementation (`UAC_LEVEL=retained`, the shipped level
code; not the shipped binary, which has no trace) and the wired one
(the default). The hs24 `--fu channels` case was run with each, the
same harness steps as the gate, on the same devnode (driver key 0020)
in the same guest, the image swapped as above; the `Uac2VolMap` read
after each step against the SETs traced since the step before:

| Checkpoint | Retained: SETs traced | Retained: `Change` | Wired: SETs traced | Wired: `Change` |
|---|---|---|---|---|
| first plug (arrival) | 4 (the volume pair, -2.0 dB, sent twice) | 4 | 4 (the same) | 4 |
| `vol 65535` | 2 | 6 | 2 | 6 |
| `vol 32768` | 2 | 8 | 2 | 8 |
| `vol 6554` | 2 | 10 | 2 | 10 |
| `vol 32768 16384` | 2 | 12 | 2 | 12 |
| `mute 1` | 1 | 13 | 1 | 13 |
| `mute 0` | 1 | 14 | 1 | 14 |
| play | 0 | 14 | 0 | 14 |
| replug (arrival) | 2 | 2 | 2 | 2 |
| `vol 49152` | 2 | 4 | 2 | 4 |
| play | 0 | 4 | 0 | 4 |

The Feature Unit requests of the two runs were equal (20 and 10, both
plugs).

At every checkpoint of both runs `Change` equals the number of SETs
the handler traced (and the trace's own `Change` after each SET runs
1, 2, 3, ... from each start): the counter counts accepted SETs and
nothing else, in both implementations. In this pair of runs WDMAud sent
the arrival's volume pair twice to both images (4 and 4), the same
values each time; so this traced pair shows no `Change` difference at
all, and it does not reproduce the gate's 4 and 2.

What this settles about the gate's three exceptions is therefore
narrow. The trace shows, for the channels case's steps, that both
handlers count exactly the SETs they accept and that WDMAud's arrival
may send the same pair once or twice. That the gate's channels
difference (4 and 2), the X4's (4 and 8 at the replug) and the failed
request's (2 and 4) are each such a difference in WDMAud's arrival
SETs remains **inferred** from the counter and the code, as above:
none of those three runs was traced, and the X4 and failed-request
cases were not traced at all.

## D3 and the return to D0

The preservation contract's D3 and RUN (design 28.15) compared the
shipped and the wired image through a device-power D3 and D0 of the
emulated devnode in the same guest (no physical device).

**The route to D3.** A system standby was tried first: the q35 board's
S3 and S4 enabled (`-global ICH9-LPC.disable_s3=0`, `disable_s4=0`, both
read back as 0 from the running machine). XP still offered no sleep
state: `SetSuspendState(FALSE, TRUE, FALSE)` returned at once and the
guest kept running, and `powercfg /hibernate off` answered "System does
not support hibernation". This was asked with no USB device attached:
a system that offers no sleep state at all cannot be made to sleep by
attaching one, so it was not tried with the simulator. Why XP takes
this machine as unable to sleep was not found. Disabling and enabling
the device is not a D3 cycle and was not used. The device's D3 was
reached instead through PortCls's inactivity timer, as Microsoft's
driver kit documentation describes it ("Audio Device Class Inactivity
Timer Implementation"; the
`PowerSettings` key of the driver key: `ConservationIdleTime` and
`PerformanceIdleTime` 10 s, `IdlePowerState` 3, REG_BINARY), which
sends the device to D3 after 10 s with no activity and back to D0 at
the next request (a mixer call or a stream open). The trace builds
show each transition reaching `PowerLocal` (device state 4, D3; 1, D0):
D3 about 10 s after each step, D0 at the next, so every step below
after the first began from D3. The simulated device was not reset or
enumerated again (its log has one configuration and no descriptor read
after the start); whether the bus suspended its port is not in the
log.

**The leg** (hs24 `--fu full`, driver key 0018, the saved level
deleted first): plug; a stereo volume step (`32768 16384`: the volume
owned, the channel controls normalised as extras); mute on (the mute
owned); a play; 45 s idle (D3); a play (D0, then RUN); mute off; 45 s
idle (D3); a play (D0, then RUN); unplug. The MEDIA class keys read
after each step. Run with the retained and the wired trace images and
then, from the same mixer state, with the wired (`4f56175f...`) and
the shipped (`934db4cf...`) images.

What every run showed (the trace runs from their trace, every run from
the simulator's log and the class keys):

- **The unknown-state rereads.** After each return to D0 the next
  apply read the six controls again (GET CUR of the master volume and
  mute and of both channels' volume and mute) before deciding what to
  write: at each mixer step and at each play's RUN (`LevelStale`, the
  state unknown). The simulator held its values across the D3, so
  every reread found what the driver had last written and nothing was
  written after a D0 (`Writes` unchanged by the plays).
- **The owned volume and mute** (-6 dB and -12 dB, `Owned` 3, mute on
  through the first D3 and off through the second) and **the extras**
  (both channel volumes at 0 dB, both channel mutes off) stood after
  each cycle, read back as such.
- The saved `Uac2Level` read the same before and after each D3, D0
  and RUN.

| Comparison | Requests (all steps, in order, data and answers) | `Uac2Level`, `Uac2VolMap` (232 bytes), stats words 65 to 93, at each of the 9 readings |
|---|---|---|
| wired (`4f56175f...`) against shipped (`934db4cf...`), same mixer state | equal (13 at the plug, then 6 or 7 a step, 0 while idle) | equal but `Change` (offset 64): shipped 4, 6, 7, 7, 7, 7, 8, 8, 8; wired 2, 4, 5, 5, 5, 5, 6, 6, 6, a constant 2 from the plug |
| wired trace against wired | equal | equal |
| retained trace against wired trace | equal from the mute step on; at the plug and the first volume step they differ because the starting mixer state did: WDMAud's arrival SETs set -2.5 dB in the retained run (the state an earlier run left) and -6 and -12 dB in the wired one, so the first volume step wrote in the one and not in the other | equal from the mute step on but `Writes`, one more in the retained run (that write) |

The `Change` difference of the wired and shipped pair is again at the
arrival only and constant after it (the free images trace nothing; in
the traced pairs WDMAud sent that arrival's SETs once to each image).
The `Uac2VolMap` read after a D3 still says the controls are known: the
record is written at an apply, not at a power change, so the
invalidation shows only in the rereads that follow.

## Not covered

- 98 SE: not run, before or after. This rig drives XP guests (the
  harness types into a US-Dvorak XP desktop and installs with `devcon`);
  a 98 SE leg needs its own install path (no `devcon`, the 98 SE Add New
  Hardware wizard for each devnode) and its own harness, which this run
  did not build, with one guest at a time. The engine is the same source
  on every system, compiled per family.
- D3 and the return to D0 by a system standby: XP offered no sleep
  state in this rig (above, "D3 and the return to D0"); the device's D3
  was reached through PortCls's inactivity timer instead. A device that
  comes back from D3 with other levels than it had (so that the driver
  rewrites its owned controls) was not exercised: the simulator keeps
  its state across the D3. That case is covered by `leveltest`'s
  vectors only.
- `uac1`: no guest leg (its `leveltest` build has vectors only).
- `cx31993m-first.lvl` (the start inside an install) was not captured
  again; its requests are those of the first arrival of `cx31993m.lvl`.
