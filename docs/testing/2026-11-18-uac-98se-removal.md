# uac2 and uac1 on Windows 98 SE: the fault and the stalls at a removal, and a bounded mitigation (2026-11-18)

The fatal exception 0E in KS at a removal while recording (open-items,
"`uac2`, `uac1` on 98 SE: open after the failed state's inactive legs";
leg 5 of the stage 2 guest legs), the idle-unplug stall under NUSB's
EHCI and the arrival wedges, read again on `main` (`eb34eff`) and on a
mitigation. Every device here is a simulated one (`uacemu.py`, `uac1emu.py` of
`eb34eff`, over `usb-redir`); no physical USB device was used, and
nothing here says how a real device or a real controller behaves. Every
guest is QEMU 11.1.0 (stock, TCG, headless, one processor) on a fresh
overlay of an installed base. These are development legs, not "Tested".

**Outcome.** Lifecycle markers of a diagnostic build observed, at four
`x4full` removals while recording under NUSB's EHCI, two orders after
the driver forwarded 98 SE's remove (sent with no surprise removal before
it) to PortCls: where a client's teardown of the pin and its filter came
before a logged final `Wave` release the remove returned; where it came
after, the KS fault screen or a stall followed (below, "Mechanism"). The
proposed explanation is a race between PortCls's remove and that
teardown; it is not established, and the same order for the other
devices, moments and idle removals is inferred. The mitigation (design
[uac2.md](../design/uac2.md) section 30; `50f0241`, its follow-up
`619370d`, and `c357172`) delays PortCls's remove at its first call into a
miniport while a stream, a create or a close is in progress or one ended
less than 250 ms ago, at most 5 s: a bounded timing rule, not a teardown
barrier. As `619370d` left it, a `uac2` and a `uac1` device removed
together stalled every time; markers in both drivers then observed the
`uac1` device powered down (D3) by PortCls during the other device's
remove and a client close pending inside its own remove, with `main`'s
code too, less often. `c357172` asks such a device back to D0 before its
remove and takes the hold only for a device in D0 with something left to
tear down; against `main`'s images on the same rig and day it was at least
as good on every row run ("The images of `c357172` against `main`'s",
below). `149c49b` corrects the stream count for a failed creation, and
its images were run again in a short confirmation. The figures that follow up to that section are of the earlier
images.
Of the devices that arrived (a trial whose plug wedged is not counted as
a removal), under NUSB's EHCI with the mitigation 49 of 49 removals with
a handle open and 19 of 19 idle removals passed (the `x4full`, `hs24` and
`uac1` devices); with `main`'s images 18 of 47 and 7 of 16 did, the rest
ending in the fault (9 and 5) or a stall (20 and 4); the `uac1` device
never failed, `x4full` and `hs24` mostly did. Arrival wedges: no
outstanding driver work was observed in the two wedges read with the
markers; an upper-layer arrival problem is suspected (below).

## Images

Built from clean trees with the packages' own `build.cmd`; every build
passed its gates. SHA-256:

| Image | Commit | SHA-256 |
|---|---|---|
| `uac2wdm.sys` `wdmx86` free (main) | `eb34eff` | `90aaf693ad74c4756458d240e11e05edb5acc781b153994c7d8a297b460f55d5` |
| `uac2wdm.sys` `wdmx86` trace (main) | `eb34eff` | `6dfdd82f195254ef7e4e7fa60229cdec5382c172b188ace586a0c2860f15df23` |
| `uac1wdm.sys` `wdmx86` free (main) | `eb34eff` | `250f1de058781bf34ab8ef416a2eeab8fe7410f7b16d37504d555c9beda5d93f` |
| `uac2wdm.sys` `wdmx86` free (fix) | `50f0241` | `c5e1028f55697521f092ea813ede635da31838cd1dc42894cd7e1bef0ead079e` |
| `uac2wdm.sys` `wdmx86` trace (fix) | `50f0241` | `598a05224650612f79d1d5adca7f6310876ac70adf2a95e0e53b9712435fcd7b` |
| `uac2wdm.sys` `wdm64` free (fix, built, not run) | `50f0241` | `f5136c9c7a60920b448c8ee3e1e33aef45b6ae54e069a412194c06bc17f81761` |
| `uac1wdm.sys` `wdmx86` free (fix) | `50f0241` | `fae47aeb3a0a8bff763b13b9d6cb1a079aa38f4292d978ac071adee36cc585d3` |
| `uac2wdm.sys` `wdmx86` free (intermediate step) | `3684529` | `ae133d826814c6957e39110ce3517a89147ea7546815dfb0e3cee50997e034a6` |
| `uac1wdm.sys` `wdmx86` free (intermediate step) | `3684529` | `ee5efa2e2a941a4db9332a8630a374c15f8b1f64be669f1ce4ad72133ee18ccb` |
| `uac2wdm.sys` `wdmx86` free | `c357172` | `6afa20af127af8b59f5217d6d27cd2d7e41aeeaa2791b0fba1b96c171cfb8788` |
| `uac2wdm.sys` `wdmx86` trace | `c357172` | `5e7dec77924aeb5634773fb99df93d7ccfc02d875aae855a069c83bad7c56775` |
| `uac2wdm.sys` `wdm64` free (built, not run) | `c357172` | `7b15d06f65d6ca2ad3932b9557ec2527260a797ccd6a4d62875a5f535247b546` |
| `uac1wdm.sys` `wdmx86` free | `c357172` | `b97d563b37258c65db06fb99badbbea71e2ad5e9eb70a635a72dc05a6e6f7e05` |
| `uac2wdm.sys` `wdmx86` free (the tree as it stands) | `149c49b` | `164867c8424a9b10d61f01e706bcbda8a12e8403521e81dff3af16e7111d517f` |
| `uac2wdm.sys` `wdmx86` trace | `149c49b` | `861272ab32d4af84eebba0e110534930f6485f2fd8ef6e9d4f2444f070f46c86` |
| `uac2wdm.sys` `wdm64` free (built, not run) | `149c49b` | `96c9f6c6fdc669ddad459167f1e65d08c5af05924808ecb89285090f72888c73` |
| `uac1wdm.sys` `wdmx86` free | `149c49b` | `91bab493d79ebcbd3188878db1b62d3adfddb77f2f996a7ffb20a9b7171fc9d4` |

The second round's diagnostic builds (markers in both drivers, on the
hold of `619370d`, on `main`'s code, and on each step of the change) were
built from commits between `fac3797` and `c357172`; none of their code is
in the tree at `c357172`.

The diagnostic builds (lifecycle markers in the trace flavour, and the
variants below) were built from this branch's earlier commits
(`2d3d564` to `cba89f8`); none of their code is in the tree at
`50f0241`. The INFs are `main`'s (`uac2wdm.inf` `7fe359ac...`,
`uac1wdm.inf` `e069fdb4...`).

## Rig

- **Bases.** 98 SE with NUSB 3.3 (`w98se.qcow2`) and 98 SE with xHCI98
  3.5.0.0 (`w98x35.qcow2`), each prepared once on an overlay: the
  controllers' wizards answered, the `x4full` and `hs24` devices
  (`uac2`) and `uac1`'s `hs` installed from `main`'s images, a test agent
  started at log-on; then read-only. Per image set a child base with that
  set's images copied over the installed ones (checked with `fc /b`).
  XP SP3: the q35 guest with the shipped `uac2` installed for `x4full`
  (an earlier legs' base), the image copied over the installed one in each
  boot and checked with `fc /b`. `pc` machine, `-cpu pentium3,-apic`,
  256 MB for 98 SE; the XP machine as its base was made.
- **Agent.** A small Win32 test program (scratch, not tracked) driven over
  the guest's serial port: WinMM `waveIn` open, start, stop, reset and
  close at 48 kHz, 16 bits, two channels, 100 ms buffers, a heartbeat
  every second, and each call's entry and return written to the serial
  port and to QEMU's debug console. It makes no WinMM call until told
  (a WinMM client present at an arrival is not part of what was read),
  and a fresh copy of it takes over after every trial.
- **A trial.** A new simulator, the plug (or, with "at power-on", the
  device plugged before the guest booted, so the first plug after a cold
  start is not part of the trial), 40 s, a device query, the moment's
  steps, then the simulator process stopped (the device leaves the bus),
  45 s watched (heartbeat, screen), the agent's close, a device query,
  the redirector removed. Moments: **RUN** (recording), **PAUSE**
  (`waveInStop`), **STOPOPEN** (`waveInReset`, the handle open),
  **OPENED** (opened, never started), **CYCLE** (opens and closes in a
  loop: during open or close), **IDLE** (nothing open in the agent, a
  10 s wait), **PLAYREC** (the regression: `UACWAV play` and `rec`, 3 s
  each, then an idle removal).
- **Outcomes.** OK; **FAULT** (98 SE's fatal-exception screen; the text
  read from the screenshot); **CLOSE-BLOCKED** (the heartbeat goes on, the
  agent's `waveInReset` after the removal does not return in 60 s,
  Explorer's clock stopped: the stall); **REMOVAL-WEDGE** (the device
  query after an idle removal does not return: the idle-unplug stall);
  **ARRIVAL-WEDGE** (the device query after the plug does not answer,
  the heartbeat alive: in the first six wedges of the record, before the
  runner waited longer, about 85 s after the plug; in the later ones about
  325 s after it, the 40 s arrival wait included). "Did not return" means
  within the window stated, nothing longer was watched. A fault or stall
  ends the boot; the next trial is a new boot on a fresh overlay. Every
  removal count is conditional on the device having arrived; with the
  device plugged at power-on the first hot plug after a cold start is
  avoided on purpose, so none of this is an unconditional plug, use and
  unplug rate.

## Reproduction with `main`'s images

Removals with a handle open (moments RUN, PAUSE, STOPOPEN, OPENED,
CYCLE; a trial whose plug wedged is not a removal and is counted under
the arrival wedge below), and idle removals:

| Stack, device | Image | Removals | OK | Stall (CLOSE-BLOCKED) | Fault (ks) | Idle removals | Idle: OK, stall, fault |
|---|---|---|---|---|---|---|---|
| NUSB EHCI, `x4full` | free | 22 | 5 | 14 | 3 | 10 | 4, 4, 2 |
| NUSB EHCI, `x4full` | trace (main 1; with the markers 4) | 5 | 1 | 2 | 2 | - | - |
| NUSB EHCI, `hs24` | free | 10 | 2 | 4 | 4 | 3 | 0, 0, 3 |
| NUSB EHCI, `uac1` `hs` | free | 10 | 10 | 0 | 0 | 3 | 3, 0, 0 |
| xHCI98, `x4full` | free | 20 | 20 | 0 | 0 | - | - |
| xHCI98, `hs24` | free | 7 | 7 | 0 | 0 | - | - |

By moment (NUSB, `x4full` and `hs24`, free, 32 removals): RUN 8 (OK 3,
stall 5), PAUSE 7 (OK 2, stall 3, fault 2), STOPOPEN 7 (stall 5, fault
2), OPENED 5 (OK 2, stall 2, fault 1), CYCLE 5 (stall 3, fault 2): every moment failed
at least once, and so did idle removals, where the clients still
hold the device's filters. The `uac1` `hs` device, which 98 SE's own
composite parent splits under NUSB, did not fail in 13 removals; under
xHCI98 no `uac2` removal failed in 27.

The fault text was the same every time (14 screens): "A fatal exception
0E has occurred at 0028:FF018C77 in VXD ks(05) + 00004C17. The current
application will be terminated." (as in leg 5 of the stage 2 legs).
After a key the desktop came back with its top rows garbled; the next
plug was not tried in that boot. The stall: the agent's heartbeat and the
driver's own threads went on, its `waveInReset` did not return, and
Explorer's clock stopped; it is the "unplug hang" of the open items (the
same at an idle removal).

## Mechanism (the lifecycle markers)

A diagnostic trace build (`2d3d564`, `de9a7b2`; trace flavour only)
wrote markers to QEMU's debug console: every PnP request at entry, at
the forward to PortCls and at its return; every create, cleanup, close
and KS request through PortCls's dispatch with its thread (stream reads
counted, not written); the stream's references, its destroy (entry and
exit) and the work item that runs it; the transport's run, halt, drain
and close; the Notify count; a `Wave`'s final release (`wavefree`, with
no thread); each pair's shutdown. `rig\rmseq.py` tabulates each remove.
Under NUSB's EHCI with `x4full` recording (`nxd`, `nxe`: 4 removes;
times from the forward):

| Outcome | Observed after the forward |
|---|---|
| OK (`nxe`, boot 1, first) | client's first request +12 ms; pin close returned +17; destroy finished +48; `wavefree` +54; remove returned +62 |
| Fault (`nxd`) | `wavefree` +3; client +7; pin close entered +10; the stream's final release +11; no close or remove return before the fault screen |
| Stall (`nxe`, boot 1, second) | `wavefree` +3; the client's state request +8 returned `STATUS_PENDING`, no `SetState` marker; no remove return in the window watched |
| Fault (`nxe`, boot 2) | `wavefree` +6; client +22; pin close entered +25; final stream release +28; no close or remove return |

- 98 SE sent IRP_MN_REMOVE_DEVICE (minor 2) with no surprise removal. The
  driver's shutdown drained the capture transport (3.5 to 4.1 s: the stack
  gave the cancelled transfers back about 0.5 s apart), then forwarded
  the remove.
- The client's requests came on a thread other than the PnP thread: the
  pin's state from RUN to PAUSE, ACQUIRE and STOP, its cleanup and close,
  then its filter's.
- `wavefree` marks only a final release, with no thread; that it came
  from PortCls's remove on the PnP thread is inferred here, and supported
  by the later variants, whose hold is entered from such a release on the
  PnP thread.
- The driver had no transfer or work item outstanding in any of the four:
  its transport was drained and destroyed before the forward.
- What the traces cannot show: PortCls's or KS's internal locking, the
  object the faulting instruction touched, why the request stayed
  pending. The ordering was traced for these `x4full` removals while
  recording only; for idle removals, the other moments and `hs24` it is
  inferred.

**Variant d2** (`5b44526`): before the forward, the pairs' device
interfaces withdrawn (`IoSetDeviceInterfaceState`, 9 links) and up to
10 s waited for the clients' handles to close: none closed in that time
(3 stayed open), PortCls's remove then returned in 8 ms with no client
request in the trace, and the agent's `waveInReset` did not return in the
window watched (1 trial). Withdrawn.

**Variant d3** (`4fc22fa`, `3b0f01b`): PortCls's remove held at its first
release into a miniport (on the PnP thread, at PASSIVE) until the
device's open handles reached 0, at most 5 s. Two trials completed OK;
the trace also shows a third remove returning, whose trial the runner
did not finish recording. In each traced remove the hold began 3 to 5 ms
after the forward, the client's state walk and closes ran during it (the
close returned), the stream's destroy ran, then the remove went on. One
handle, opened by the client with the recording, was never closed, so
each hold ran its 5 s.

**Variant d4** (`2f28b9a`): the same hold, ending once no stream is left
and no create or close has come for 250 ms (5 s at most). With the
markers (`n4t`): two trials completed OK and a third remove returned;
the three holds were 297, 303 and 287 ms.

A light trace (`cba89f8`, the markers without the transfer ring) was
built to read the free build's timing; its series met only arrival
wedges before it was set aside.

## The mitigation and what was read with it

The mitigation as first committed (`50f0241`, design [uac2.md](../design/uac2.md) section 30): on
a remove with no surprise removal before it, PortCls's remove is held at
its first release into a miniport until no stream of the device is left
and no create or close has come for 250 ms (5 s at most). The same
moments, with the device plugged at power-on for each boot's first trial
(a cold start's first hot plug, the arrival wedge's common case, kept
out of the trial):

| Stack, device | Image | Removals | OK | Stall | Fault | Idle removals (OK) |
|---|---|---|---|---|---|---|
| NUSB EHCI, `x4full` | free | 19 | 19 | 0 | 0 | 10 (10) |
| NUSB EHCI, `x4full` | trace | 10 | 10 | 0 | 0 | 5 (5) |
| NUSB EHCI, `hs24` | free | 10 | 10 | 0 | 0 | 3 (3) |
| NUSB EHCI, `uac1` `hs` | free | 10 | 10 | 0 | 0 | 1 (1) |
| xHCI98, `x4full` | free | 10 | 10 | 0 | 0 | 3 (3) |
| XP SP3, Windows' EHCI, `x4full` | free | 6 | 6 | 0 | 0 | - |

Against `main`'s free images on the same stacks and devices: NUSB
`x4full` 5 of 22 OK and 4 of 10 idle, `hs24` 2 of 10 and 0 of 3, `uac1`
10 of 10 and 3 of 3; xHCI98 `x4full` 20 of 20. Two of the
`uac1` series' idle trials with the mitigation are not counted: their
plug wedged. The regression series below (`n6p`) adds two RUN removals
and two idle removals after play and record, all passed, outside these
totals.

The trace image's line for each hold (`rmhold`: streams left, open
handles, milliseconds held) read, in the 15 holds of its series: 0
streams every time, 0 or 1 handle open (the handle the client opened
with the recording and did not close at the remove), 272 to 298 ms held:
each hold ended about 250 ms after the last counted activity. The hold
trace does not show the client's requests (the final build has no
dispatch markers); that they ran inside the hold rests on the d3 and d4
traces above.

**Regression.** `UACWAV play` and `rec` (3 s, 48 kHz, 16 bits, two
channels), after which the device was removed idle: on XP 2 of 2 (play
and record each PASS); on 98 SE under NUSB 2 of 2 likewise. On XP the remove follows a surprise
removal, so nothing is held there; the counting of creates and closes is
the change that runs, and the six removals while recording, paused,
stopped and cycling passed as on `main` (leg 5 of the stage 2 legs read
XP's removal while recording as passing).

## The arrival wedge

The wedge after a plug (the device query never answered, Explorer's
clock stopped, the processor and the agent's heartbeat going on) came in
these series under both stacks, most often at the first plug after a
cold start, and with `main`'s images and the fix alike (tables above).
The markers of the diagnostic build read two wedges (`nxe`, NUSB's EHCI;
`xxe`, xHCI98) and the healthy arrivals of the same series:

- In each wedge the driver's start had finished (`Start` returned
  success, both pairs' wave miniports initialized), and 262 requests had
  come through PortCls's dispatch and 262 had returned, the last a close.
  No work item, stream or request of the driver's was outstanding.
- In a healthy arrival the same 262 requests come from one thread, then
  about 0.3 s later a second client starts its own enumeration on
  another thread (453 requests before the agent's device query answers).
  In a wedge that second enumeration never starts.

No outstanding driver work was observed in these two wedges; an
upper-layer arrival problem is suspected. That does not establish that
no wedge involves the driver (an earlier answer of the driver could
still matter later), nor which component stops. It agrees with xHCI98's record of the same
wedge with Windows' own `usbaudio.sys` and QEMU's `usb-audio`, on 98 SE's
own UHCI stack as well (xHCI98 roadmap 36.41, its run 36 record: 23 of
39 and 14 of 30 arrivals), and with this repository's control series
(the merged binaries' record, its addition of 2026-11-18): under xHCI98
the first plug after a cold start wedged 4 of 11 and 5 of 12, in-boot
replugs 0 of 26; under NUSB the first plug 3 of 8 and 2 of 7. Here, in
the series whose every trial began with a hot plug (so each boot's first
plug was a cold start's first hot plug; under NUSB these are `main`'s
free and trace series and the diagnostic variants' series together), 8
of 27 hot plugs wedged under NUSB and 2 of 29 under xHCI98; in the series with the device plugged at
power-on (every hot plug a replug in the same boot), 3 of 87 and 2 of 12
(both of those right after a trial of open and close cycles), with
`main`'s images and the fix alike. One more reading: early in this work, with
the agent having made a WinMM call before the plug, four first plugs in
a row wedged under xHCI98, and one plug with no agent running did not;
too few to say whether a WinMM client present at the arrival matters.
No change was made for it.

## The follow-up (`619370d`)

Every device's accounting entry allocated at AddDevice (no fixed table),
creates and closes counted as in flight, a stream counted to the end of
its destroy (design section 30). Images, every build passing its gates:
`uac2wdm.sys` `wdmx86` free `5a27ae5b61583b2f4544e743176dd89849d7f5da015f4d7874cb614fe680fd08`,
trace `2ee1a67429a90616d65b2d6d9df0f0b683f341aa9946066f87236d7df9f1313f`,
`wdm64` free (built, not run) `01eccf78ad5fe880cbee19d2008807dba9e74dce490b48da3dcc1d9eb38876f6`;
`uac1wdm.sys` free `40af42a8262f4aac0cec7e8f998ed2ac7f3fbdf9eb86e76e86ce6a215397d701`.
Device plugged at power-on as above.

| Stack, devices | Image | Removals OK | Idle OK |
|---|---|---|---|
| NUSB EHCI, `x4full` | free | 10 of 10 | 5 of 5 |
| NUSB EHCI, `hs24` | free | 5 of 5 | - |
| xHCI98, `x4full` | free | 5 of 5 | - |
| XP SP3, Windows' EHCI, `x4full` | free | 3 of 3 (play and record PASS) | - |
| NUSB EHCI, `x4full` and `hs24` together, both removed at once while recording | trace | 3 of 3 | - |
| NUSB EHCI, `x4full` (port 1) and `uac1` `hs` (port 2) together, both removed at once with a handle open | free | 0 of 3 (3 stalls; 1 trial lost to an arrival wedge) | - |
| the same pair | trace (`uac2` only) | 0 of 1 (stall) | - |
| the same pair | `main`'s free | 3 of 3 | - |

The two `uac2` devices: each removal logged two holds about 5 s apart,
the first 250 to 258 ms with no stream and no handle, the second 291 to
317 ms with one handle open (trace `rmhold`). **The `uac2` plus `uac1`
pair stalled with the mitigation every time and not with `main`'s images**
(3 of 3 passed there): a regression of the mitigation in this case. The
recording ran on the `uac1` device (the agent takes the first device of
this driver family WinMM lists); `uac1` has no trace build here, so its
hold was not read. The `uac2` device's hold in that trial ended after
252 ms with no stream and no handle. A possible explanation, not read: a
client that the `uac1` device's teardown needs is held up by the other
device's removal, which waits for the PnP thread the hold occupies, so
the `uac1` hold runs to its 5 s cap and its remove then goes on with the
stream still open. Until it is understood the mitigation is not ready to
merge. (Read again below: that explanation did not hold.)

## Two devices removed together, read again with markers in both drivers

The lifecycle markers of the first diagnostic build were applied again
(diagnostic commits after `fac3797`, removed since): every PnP request at
entry, forward and return with its thread; every create, cleanup, close
and KS request through PortCls's dispatch with its thread and return
status; every `Wave` and `Topology` release with its count and thread; the
hold's start and end; the stream's destroy; `PowerChangeState`. `uac1` got
a trace flavour whose lines are told apart from `uac2`'s. The same markers
were applied to `main`'s code. The test agent now records from the device
named (WinMM's names: "UAC2 X4 layout - SIM (wdm-free)", "UAC1 hs -
SIMULATED (wdm-free)"), so each device of the pair could be the one
recording. NUSB's EHCI, `x4full` on port 1 and `uac1` `hs` on port 2, both
plugged at power-on for each boot's first trial, removed together while
recording (`rig\rmtl.py` and `rig\rmsum.py` read the traces):

| Series | Images | Recording | Removals OK | Stall |
|---|---|---|---|---|
| `d1a` | the hold of `619370d`, trace | `uac1` | 0 of 3 | 3 |
| `d1b` | the same | `x4full` | 0 of 3 | 3 |
| `d0a` | `main`'s code, trace | `uac1` | 2 of 3 | 1 |
| `d0b` | the same | `x4full` | 0 of 3 | 3 |
| `e3u1` | the hold of `619370d`, trace; `uac1` alone (port 2) | `uac1` | 3 of 3 | 0 |
| `e2uu` | the same; `x4full` and `hs24` together | `x4full` | 3 of 3 | 0 |
| `t2a` | the first step of the change below, trace | `uac1` | 3 of 3 | 0 |
| `t2b` | the same | `x4full` | 1 of 3 | 2 |

Observed:

- 98 SE removed the two devices one after the other on one thread: the
  `uac2` device's remove first, the `uac1` device's 630 to 930 ms after the
  first had returned (21 pairs read, those of the change below included).
- During the `uac2` device's remove the `uac1` driver logged a
  `PowerChangeState` to D3 for its device, and no D0 came before that
  device's own remove. Where the `uac1` device recorded, its pin was
  stopped on the remove's thread during the `uac2` device's remove (about
  2 s of draining).
- At the `uac1` device's remove a client thread's cleanup and close of one
  of its handles came 6 to 10 ms after the remove's entry (here and below,
  times are from the remove's entry into the driver's PnP dispatch unless
  said to be from the forward, the driver's call into PortCls); the close
  returned `STATUS_PENDING` and the remove did not return within the
  window watched; the agent's `waveInReset` then did not return (the
  stall). With the hold of `619370d` this came at all 6 such removes (the
  close inside the hold; the hold then ran to its 5 s cap while the `uac1`
  stream was still counted, or ended at 250 ms with none); with `main`'s
  code at 1 of 3 (7 ms after the entry, after PortCls's releases), and at the other 2 no
  client request reached the driver during the remove (28 and 52 ms) and it
  returned.
- `d0b`'s three stalls are `main`'s own `x4full` race of the first device
  (its remove did not return; the `uac1` device's remove never came).
- `uac1` alone: no power change before the remove, and the client's closes
  inside the hold returned success. `x4full` and `hs24` together: at the
  end of the first device's remove the `uac2` driver logged D3, then a D0
  about 750 ms later, before the second device's remove (that build's
  line does not name the device); the second device's closes inside its
  hold returned success.

So the earlier guess (one device's hold keeping the other's client
waiting) is not what the traces show: the stall is inside the `uac1`
device's own remove, and it occurs with `main`'s code too, less often.
Explanation proposed, not established: a close that reaches PortCls
during the remove of a device PortCls has set to D3 is not completed and
the remove waits for it; the hold made the client's close certain to come
inside the remove. What PortCls or KS do with the close is not visible to
the driver.

The change came in two steps (design [uac2.md](../design/uac2.md) 30.6).
First (`3684529`): the hold is not taken when the last device power state
PortCls gave the device was not D0, nor when nothing of the device is
left to tear down (no stream, no handle, nothing executing). In `t2a` and
`t2b` the trace showed the hold skipped for the powered-down `uac1` device
and for an `x4full` device with nothing open, and taken for a recording
`x4full` device; `t2b`'s two stalls were `main`'s risk left in place: the
`uac1` device in D3, no hold, the client's close 6 and 8 ms after the
entry, pending.
Then (`c357172`; with `149c49b`'s correction of the stream count, the
tree as it stands): before PortCls is given such a
remove, a device whose last power state from PortCls was not D0 is asked
back to D0 (`PcRequestNewPowerState`, exported by 98 SE's and ME's
PortCls, [w98port.txt](exports/w98port.txt), [wmeport.txt](exports/wmeport.txt));
if it is not brought back the hold is not taken, as in the first step. A
trace build of that step with the markers (the diagnostic commit
`ed2ad56`, the hold of the first step plus the request; the request's
code in `c357172` is the same call made on the same condition):

| Series | Recording | Removals OK | Stall |
|---|---|---|---|
| `v3b` | `x4full` | 3 of 3 | 0 |
| `v3a` | `uac1` | 3 of 3 | 0 |

At each of the 6 `uac1` removes the request returned success, after 2,
37, 1, 3, 1 and 1 ms, and the driver logged D0 before the forward; the
hold was taken. The client's closes came 3 to 12 ms after the forward,
inside the hold, and returned success; each close was preceded by a
cleanup, which returned `STATUS_INVALID_DEVICE_REQUEST` (0xC0000010), as
the cleanups did in every trace of this record, passing or not. In one of
the 6 (the 37 ms request) the client had already begun before the
forward: cleanups, closes and creates of one handle 1 to 11 ms after the
entry, during the request, the closes returning success; its closes after
the forward came inside the hold as in the others. The removes returned
289, 320, 267, 279, 264 and 262 ms after their entry (262 to 320 ms).

## The images of `c357172` against `main`'s, row by row

Free images of `c357172` (below, "Images") and `main`'s (`eb34eff`),
each on its own prepared base with the images installed, one series per
row on the same rig and the same day; every boot's first trial with the
device or devices plugged at power-on, the later trials of a boot replugs.
"Pair": `x4full` on port 1 and `uac1` `hs` on port 2 removed together
while recording from the one named. Removals are counted only for devices
that arrived; a trial whose plug wedged or hung is listed apart.

| Row (stack, devices, moments) | `main`: removals with a handle open | `main`: idle | `c357172`: removals with a handle open | `c357172`: idle |
|---|---|---|---|---|
| NUSB, pair, recording from `uac1` (RUN) | 4 of 10 (6 stalls) | - | 10 of 10 | - |
| NUSB, pair, recording from `x4full` (RUN) | 0 of 10 (6 stalls, 4 faults) | - | 10 of 10 | - |
| NUSB, `x4full` and `hs24` together, recording from `x4full` (RUN) | 0 of 5 (5 stalls) | - | 5 of 5 | - |
| NUSB, `x4full` (RUN, PAUSE, STOPOPEN, OPENED, CYCLE) | 2 of 5 (2 stalls, 1 fault) | 0 of 2 (2 stalls) | 5 of 5 | 2 of 2 |
| NUSB, `hs24` (the same moments) | 1 of 5 (3 stalls, 1 fault) | 0 of 2 (2 faults) | 5 of 5 | 1 of 1 |
| NUSB, `uac1` `hs` (the same moments) | 5 of 5 | 1 of 1 | 5 of 5 | 1 of 1 |
| xHCI98, `x4full` (the same moments) | 5 of 5 | 1 of 1 | 5 of 5 | 2 of 2 |
| XP SP3, Windows' EHCI, `x4full` (RUN, PAUSE, STOPOPEN, CYCLE, OPENED; play and record then an idle removal) | 5 of 5 | 1 of 1 (play and record PASS) | 9 of 9 (two series) | 2 of 2 (play and record PASS both) |

Not counted as removals (the plug did not arrive): an arrival wedge in
`main`'s `uac1` and xHCI98 rows and in `c357172`'s `hs24` and `uac1` rows
(one each; each row's planned idle trials are 2, so those rows show one
idle removal). The faults were the KS screen of the earlier sections; the
stalls as defined there. On every row the `c357172` images did at least as
well as `main`'s; on the pair rows, the two `uac2` devices and the NUSB
`x4full` and `hs24` rows better. These are small samples on simulated
devices in emulation: a row of 5 that passed shows no failure in 5, not a
rate.

The intermediate step (`3684529`, free images, no request for D0) ran
the same rows except the `main` ones: pair recording from `uac1` 6 of 10
(4 stalls), recording from `x4full` 3 of 9 (6 stalls; one more trial's
guest stopped 16 s after a replug, its heartbeat lost, the desktop up: an
arrival hang, not counted), two `uac2` devices 5 of 5, NUSB `x4full`,
`hs24`, `uac1` and xHCI98 `x4full` 5 of 5 each with 1 of 1 idle (and one
arrival wedge each), XP 3 of 3 and 1 of 1 idle with play and record PASS.
On XP that series' third trial ended in a stop screen 35 s after a replug,
before any removal ("STOP: 0x0000007E (0xC0000005, 0x805510BD, ...)", no
driver named; screenshot `g\fxp\shots\fxp02-arr-a.png`). Not read further
(the dump was not read); not seen in the 17 XP trials after it with
`c357172`'s and `main`'s images. The pair rows' stalls with that step were
`main`'s risk left for a device in D3 (above); `c357172` replaced it.

## The images of `149c49b`: a short confirmation

`149c49b` changes one thing against `c357172`: a stream is counted where
`UacStream::Create` publishes it and uncounted at its destroy only when it
was published, so a creation that fails after taking the device reference
(its buffer or its service group not allocated) no longer lowers the count
(with `c357172` one such failure left the count one low, so that a later
live stream could read as none and the hold end with it alive; whether a
creation failed in any trial here is not visible in the free images). `streamtest.cpp` now checks the balance on each injected
failure; with the earlier decrement restored it fails. The free images
(above), on a base made as before, the same rig:

| Row | `149c49b` | the same row in the table above (`c357172`; `main`) |
|---|---|---|
| NUSB, pair, recording from `uac1` (RUN) | 4 of 4, and one arrival hang | 10 of 10; 4 of 10 |
| NUSB, `x4full` (RUN, PAUSE, STOPOPEN, OPENED, CYCLE) | 5 of 5 | 5 of 5; 2 of 5 |
| XP SP3, `x4full`, play and record then an idle removal | play and record PASS, 1 of 1 | PASS, 2 of 2; PASS, 1 of 1 |

The arrival hang: at the pair's third trial, a replug in the same boot
after two removals that passed, the guest's heartbeat stopped about 16 s
after the plug with the desktop up and the pointer busy; not counted as a
removal. The same outcome came once with the intermediate step (`fpb`,
also a pair replug, above) and not with `c357172` (18 pair replugs) nor
with `main`'s pair rows, which replugged only 4 times (their stalls ended
most boots). In-boot pair replugs: 1 hang in 8 with `3684529` (`fpa`
5 and `fpb` 3, the hang in `fpb`), 1 in 3 with `149c49b`, 2 in 11
together; none in 18 with `c357172`, whose difference from `149c49b`
is the stream count alone. Cause not read, not attributed:
open. The confirmation ran only these three rows; every other row of the
table rests on `c357172`'s images.

## Other systems

- **XP SP3** (Windows' EHCI, the fix's image): above; 6 of 6 removals
  and 2 of 2 play and record.
- **Windows 7 x64** and **ME**: not run (below).
- **Shutdowns**: one 98 SE guest under xHCI98 stopped at "Windows is
  shutting down" in a boot in which no audio device had been plugged (the
  driver never loaded); the other shutdowns of the preparation finished.
  Not this driver's by that reading; the open `uac1` shutdown item stays
  as it is.

## What did not run

- Twenty removals per configuration for every device and stack: `x4full`
  under NUSB ran 22 (`main`) and 19 (the fix), `x4full` under xHCI98 20
  and 10; `hs24` and `uac1` under NUSB 10 each, `hs24` under xHCI98 7
  (`main` only); `uac1` under xHCI98 and `hs24` under xHCI98 with the fix
  did not run; `main`'s trace image ran 5 removals in all (with the
  markers); idle removals 10 or fewer per configuration.
- For `c357172`: 5 removals with a handle open (and 2 idle) per single
  device and stack, 10 per pair order, 5 for two `uac2` devices; the pair
  only while recording (no other moment, no idle pair removal); the pair
  only with `x4full` on port 1 and `uac1` on port 2 (the bases have each
  device installed on its port; a swapped-port series met the new-hardware
  wizard and was stopped); two `uac1` devices and more than two devices
  together: not run; play and record on 98 SE: not run with `c357172`
  (XP only).
- ME: not run, though the mitigation's version test covers it, and the
  request for D0 relies on ME's PortCls export (read as an export table
  only).
- Windows 7 x64 removal while recording and its play and record: not
  run (the base needs its driver-signature boot choice and a manual bind;
  no time was left). On 7 the remove follows a surprise removal, so the
  hold and the request for D0 are not reached; only the counting of
  creates and closes and the record of the power state run there, as on
  XP.
- Many devices at once beyond two (the accounting has no fixed table
  since `619370d`).
- The x64 image was built and passed its gate; it ran nowhere.
- On 98 SE under xHCI98 the two play-and-record trials with the fix met
  arrival wedges first; play and record there did not run.
- Real devices and real controllers: none.

## Evidence

On the development host, `E:\usb-wdm-free\uac98rm\`: `STATE.md`,
`bin\` (each image set with its SHA-256), `base\`, `rig\` (the runner,
the agent's source in `agent\`), and per series `g\<name>\`
(`results.jsonl`, `series.log`, `logs\` with the debug console per boot,
the serial log and the simulator logs, `shots\`).
