# A UAC 2.0 adapter that stops answering, rough playback, and silence: what uac2 sends (2026-11-17)

What was read and measured for three field symptoms of one CX31993
USB-C headphone adapter driven by `uac2` 0.1.11.0, and what `uac2`
could do differently. No driver code was changed. No physical device was
used: every guest result here is the `cx31993` simulated device, and shows
`uac2`'s own request pattern, not the real device's failure. No README
"Tested by the author" cell changes.

## The field reports

A field report, Windows 7 x64 (no USB Audio 2.0 driver of its own, so
`uac2` 0.1.11.0 drives the function), xHCI98 3.5.0.0 as host controller
driver. The device: 3302:336A, product string "CX31993+MAX97220 PRO",
UAC 2.0 at High Speed, one isochronous OUT endpoint `0x01` (synchronous,
`bmAttributes` `0x0D`, `bInterval` 1) at alternates 1 to 3 (16, 24 and
32-bit stereo, 192, 288 and 384 bytes), clock source 9 (frequency
read/write, validity readable), Feature Unit 2 (master mute, volume per
channel), no AudioControl interrupt endpoint, and a HID interface
(interrupt IN `0x83`). Its descriptors were read from an earlier save of
the same day in which it worked.

Three symptoms, all on the same system and evening:

1. **The device stops answering control transfers** (three times, on an
   Intel 7-series root port and on a Fresco Logic FL1100 card with the
   device behind a hub): a SET_INTERFACE to alternate 0 got no completion
   of any kind; after the host driver's recovery the device's Address
   Device never completed again; the host driver latched the controller
   failed. Unplugging the adapter restored it (it was moved from the
   Intel port to the FL1100 card and worked there before failing again).
2. **Playback that sounds wrong with the pitch unchanged**, once, when a
   program opened the device; reopening the program two or three times
   afterwards was fine.
3. **"Audio just gone"** a few minutes later, without the controller
   stopping; unplugging and replugging the adapter brought audio back.

The tree tool's saves read (as text only): three saves at the first
failure, three at the second, two at the third (FL1100), one at the
rough playback and one at the silence, and the earlier working save.

## How the saves were read

The host driver writes, per isochronous pipe, `iso.page` when the
pipe is made (a SELECT_INTERFACE to an alternate with the endpoint), and
at the pipe's end `iso.page.free`, `iso.recs` (most records out, count),
`iso.gap` (longest gap between publications, with its end time),
`iso.lag`, `iso.dry` (Ring Underrun/Overrun events) and `iso.close`
(publications, one per URB) with the close's time (xHCI98
`src/hcd_cfg.c`, `hcdCfgIsoCountReturn`; `src/xhci_xstat.c`). The close
notes are written once the switch back to alternate 0 has ended, so the
close time is the end of the SET_INTERFACE(0) (in the failures, 5 s after
it was sent). `cfg.select.end` is written only for a switch that was not
a plain success (`hcdCfgSwitchNote`); `01010407` and `03010407` read slot
1 or 3, interface 1, answer 4 (not established), event 7
(SET_INTERFACE not established). No note records the alternate number or
the bytes per packet; the open time of a session is not stamped either.

`uac2` queues 8 URBs of 4 ms (32 packets at `bInterval` 1) at the start
(`Uac2QueueMs` 32), so a session of P publications streamed for about
(P - 8) x 4 ms before its stop (9 publications: one refill, a few ms; 16
to 17: about 35 to 40 ms). A scratch parser (not tracked) turned each
note ring into one line per session.

## 1. The timeline before each failure (established from the notes)

**First failure (Intel, the saves of 20:55-20:58 local).** The ring
covers about 70 minutes: 50 sessions, 30 of 30 publications or fewer and
20 longer (up to 649130 publications, about 43 minutes of continuous
streaming). Short sessions came in clusters (4 within 6 s at 04:00:45
UTC, 9 within 20 s at 04:05:44), close-to-close 0.58 to 2.2 s inside a
cluster, minutes apart between clusters. The last cluster: closes at
04:54:22.967 (19 publications), 24.217 (17), 25.917 (15), 30.317 (16),
41.027 (19); the fatal session opened after 41.027, its longest gap ended
41.705, it had 16 publications, and its SET_INTERFACE(0) was answered by
nothing: the close notes came 5.012 s after the gap's end, with
`ctrl.failed.here` written before them. Interval from the previous close
to the fatal session's last publication: 0.68 s.

**Second failure (Intel, after the controller was restarted).** 37
sessions (33 short). The fatal session (17 publications) came after
**329 s with no stream** (previous close 05:08:15.212, a 275-publication
session; fatal close notes 05:13:44.133, 5.047 s after its gap's end). The
shortest close-to-close of a short session in this ring was 0.239 s.

**Third failure (FL1100, behind a hub, right after a fresh plug).** 7
sessions: 341 publications (about 1.4 s) after the enumeration notes
(not time-stamped), 126 (0.5 s) 26.8 s later, then **five sessions of 9
or 10 publications whose closes were 40, 37, 36 and 36 ms apart**
(05:28:35.553, .590, .626, .662), and the next session (9 publications)
began within a few ms of the last close (its longest gap ended 3 ms after
that close) and its SET_INTERFACE(0) was the unanswered one (close notes
05:28:40.698). This pattern, about 27 alternate changes per second, is in
no other save.

**What was in flight.** By `uac2`'s code (section 2): every control
request and every SELECT_INTERFACE is sent under the device lock, one at
a time, and the stop selects alternate 0 only after the stream thread has seen
every URB of the stream come back (cancelled): no isochronous transfer of
`uac2`'s and no other `uac2` request was outstanding when the fatal
SET_INTERFACE(0) was sent. The host driver, for its part, aborts and
detaches the interface's pipes and drops the endpoint with Configure
Endpoint before it sends SET_INTERFACE (`hcdCfgSelectInterface`, steps 1
to 3). `dry` was 0 in every session of every save: no ring under- or
overrun. The `xfer.unmatched` notes in the first save are slot 4 DCI 3,
another device.

**What distinguishes the fatal SET_INTERFACE(0).** Nothing in the notes
is unique to it. All three ended a short session (9 to 17 publications:
alternate 0 sent some 5 to 40 ms of streaming after the alternate was
selected), but so did 79 short sessions that closed normally in the
failure and later saves, and dozens in the earlier working save; no long
session ever failed.
The spacing differs between the three: 0.68 s, 329 s, 36 ms. The
alternate selected is not recorded. **Hypothesis, low to medium
confidence:** the device's firmware can wedge when alternate 0 arrives
while it is still bringing the stream up after an alternate change (codec
and amplifier start, rate lock), so every short session is a small risk,
and a burst of them (the FL1100 case) a larger one. Of the 128
sessions in those saves 82 were short, so three failures would all fall
among short sessions by chance about one time in four: the reports
cannot prove it.

**Were the sessions playback?** The user reports no sound playing at the
failures. The guest (section 4) shows a Windows 7 device arrival and an
audio initialisation with no sound start no stream at all; every stream
session is a RUN of the pin, which on 7 comes from a program or a system
sound starting a stream. Sessions of 126 to 143 publications (0.5 s)
recur through the saves, the length of a system sound; the short ones
(about 40 ms of streaming) are a stream started and stopped almost at
once, silence included, from whatever program opened the device (a game
starting or exiting, by the user's account). That they carried no sound
is consistent with the notes; the notes cannot show content.

**The hub and the controller.** `uac2` sends the same sequence whatever
the host stack; the request timing on the bus is set by `uac2`, the audio
stack and the host driver's switch (Stop Endpoint, Configure Endpoint,
SET_INTERFACE). A High Speed hub forwards a High Speed device's
transfers without split transactions. Nothing in the saves shows the hub
or the FL1100 changing the order; the 36 ms cycles of the third failure
are the stream being opened and closed that fast, not a retry.

## 2. What `uac2` sends, and how fast (established from the code)

Code read: `src/audio/common/uacusb.c`, `uacwdm.cpp`, `uacstream.h`
(unchanged in the stream path between 0.1.11.0 and this tree).

- **RUN** (`UacStream::SetState`, `Wave::Run`): creates the transport at
  the first RUN of a pin, then `UacUsbStart`: `SetClock` (GET_CUR of the
  clock's frequency; SET_CUR and a second GET_CUR **only if it differs**;
  GET_CUR of clock validity), `ReadFeature` (GET_CUR of the master mute
  on this device), `SelectAlt` (SELECT_INTERFACE to the alternate:
  SET_INTERFACE on the bus), GET_CURRENT_FRAME_NUMBER (no bus traffic),
  then 8 URBs. A level write is added only when a level is stale.
- **PAUSE or STOP from RUN** (`Halt`, `UacUsbStop`): the stream thread cancels
  every URB with `IoCancelIrp`, exits only when all are back, then
  SELECT_INTERFACE to alternate 0, then the clock claim is released.
  There is no wait, delay or minimum hold between selecting the
  alternate and selecting 0, or between 0 and the next start.
- **Close** (`Close`): stops if running and destroys the transport; the
  next pin waits for the closing one's destroy (section 27).
- Every request: synchronous (`UsbRequest`, `CallAndDrain`), under the
  device mutex, one at a time; a request not completed in **5 s** is
  cancelled, its completion awaited without bound, and the start fails
  with `STATUS_IO_TIMEOUT`. There is no retry and no remembered "device
  not answering" state: the next RUN or pin open sends again.
- The rate is not re-sent when unchanged; it is read (GET_CUR) at every
  start.

So the cycle rate is set by the client: `uac2` adds about five control
requests and two SELECT_INTERFACEs per RUN/PAUSE pair and can follow a
client as fast as the requests complete.

## 3. Linux's USB audio driver (facts, read 2026-11-17)

Read on the public mainline tree (`sound/usb/`, Makefile 7.4.0-rc3) and
checked against v6.12 where noted. Facts only; no code taken.

- **Delay flags.** `QUIRK_FLAG_CTL_MSG_DELAY` (20 ms),
  `QUIRK_FLAG_CTL_MSG_DELAY_1M` (1-2 ms) and `QUIRK_FLAG_CTL_MSG_DELAY_5M`
  (5-6 ms) are applied after each class request in
  `snd_usb_ctl_msg_quirk` (called from `snd_usb_ctl_msg`, `helper.c`).
  `QUIRK_FLAG_IFACE_DELAY` adds 50 ms after each successful
  `usb_set_interface` in `endpoint_set_interface` (`endpoint.c`).
  `QUIRK_FLAG_FORCE_IFACE_RESET` makes `snd_usb_endpoint_stop` mark the
  interface for a full alternate-0, rate, alternate-N setup at the next
  prepare. `QUIRK_FLAG_IFACE_SKIP_CLOSE` leaves the interface at its
  alternate at close. `QUIRK_FLAG_ALWAYS_SET_RATE` sends SET_CUR even when
  GET_CUR already reads the rate; `QUIRK_FLAG_SET_IFACE_FIRST` sets the
  alternate before the rate.
- **None of them applies to every device**; the delays come only from
  `quirk_flags_table` (`quirks.c`).
- **This vendor in the table:** 3302:17c2 ("TTGK Technology USB-C
  Audio", the adapter's maker's other product) carries
  `QUIRK_FLAG_FORCE_IFACE_RESET | QUIRK_FLAG_IFACE_DELAY`; 3302:39c3
  carries `QUIRK_FLAG_DSD_RAW`. 3302:336A and 3302:12C3 are absent; no
  entry names the CX31993. The same pair of flags is on a dozen other
  USB-C headset adapters, all added in 2026; v6.12 has no 0x3302 entry.
  The reason given for the 3302:17c2 entry is interface setup failing
  across suspend and resume.
- **When Linux switches alternates:** at prepare (alternate 0, rate,
  alternate N, only when setup is needed) and at the last close of the
  interface (`snd_usb_endpoint_close`, alternate 0). Trigger start and
  stop only submit or kill URBs, and a pause does not stop the endpoints:
  **a stream stopped and started without a close does not switch
  alternates on Linux.** `endpoint_set_interface` skips a set to the
  alternate already current; mainline (not v6.12) retries one that fails
  with `-EPROTO` with a growing back-off.
- **Rate:** `set_sample_rate_v2v3` (`clock.c`) reads the rate, skips
  SET_CUR when equal (unless `ALWAYS_SET_RATE`), reads it back, checks
  validity; for UAC 2.0 all at alternate 0 before alternate N, as `uac2`.

## 4. The guest runs

Vehicle: Windows 7 SP1 x86, a fresh overlay of the repository's base
image, QEMU (TCG, one vCPU), `usb-ehci` with Windows' own EHCI driver,
`uac2` 0.1.11.0 as released (the `wv-8-x86` files of release 0.1.10) and
`UACCI.INF`, the device simulated over `usb-redir` by `uacemu.py
--profile cx31993 --vid 3302 --pid 336a` (the published CX31993 layout:
clock 9, master mute, synchronous 192/288/384-byte alternates, the same
shape as the field device's). A scratch test program (`waveOut` open,
5 ms write, reset, close; DirectSound create, primary buffer, release;
DirectSound 5 ms secondary-buffer play) was run in the guest. TCG is far
slower than the real system, so the times below are this guest's; the
order and counts are what `uac2` does.

| Run | Streams (alternate 1 selected, then 0) | Requests per stream |
|---|---|---|
| Device arrival, no program, 4 minutes | none | at start only: clock RANGE x6, volume RANGE x2, GET_CUR mute and volume x2 |
| DirectSound create + primary buffer + release, 5 times | none | none |
| DirectSound 5 ms play, 5 times 1 s apart | 5, alternate held 256-555 ms | 2 clock RANGE (pin creation), GET_CUR rate, GET_CUR validity, GET_CUR mute, SET_INTERFACE 1; SET_CUR rate only the first time; SET_INTERFACE 0 at the stop |
| `waveOut` 48 kHz, 5 ms, 20 times back to back | 20, alternate held 174-210 ms, next selected 105-140 ms after alternate 0 (about 3 cycles a second) | the same |
| A system sound (a dialog's) | 1, 2.8 s | the same |

Every stream ran at **44.1 kHz, 16-bit, alternate 1**, the shared-mode
engine's format, whatever rate the program asked: on 7 a shared-mode
program never changes the device's rate. Packets were 20 and 24 bytes
(5 and 6 frames) in the 44.1 kHz ratio (0.512-0.514 of them 24 bytes) in
every stream, the first included.

**The stop model** (a scratch copy of `uacemu.py`, not committed: after
10 alternate changes within 2 s the device answers no more control
requests and no SET_INTERFACE). The same `waveOut` loop: the 10th change
was the 14th stream's SET_INTERFACE(1); its start failed after `uac2`'s
5 s timeout; that `waveOutOpen` and each later one returned
`MMSYSERR_ERROR` after 10 to 15 s; and every new open sent another
request to the silent device (a clock RANGE every 7.3 s, 13 in 110 s).
`uac2` went on sending to a device that had stopped answering, and each
attempt blocked its caller 5 s or more. This is `uac2`'s behaviour against
a modelled stop, not the real device's.

Not run: Windows 7 exclusive mode (a program choosing the device's rate),
XP, and the reproduction of the real device's failure, which only the
device can show.

## 5. Hypotheses, ranked

1. **The device's firmware wedges its control endpoint on an alternate
   change soon after another** (alternate 0 a few to some tens of ms
   after alternate N), more likely the faster the changes come.
   Supported by: all three fatal requests were SET_INTERFACE(0) closing
   a short session; the FL1100 failure followed five 36 ms cycles;
   Linux puts a 50 ms delay after every SET_INTERFACE, and a full reset
   after every stop, on another product of the same maker. Against: the
   second failure followed 329 s of quiet; dozens of identical short
   sessions passed. Confidence: medium that alternate changes are the
   trigger, low on the exact rule.
2. **Control traffic right after SET_INTERFACE** (`uac2`'s three GET_CURs
   come within a few ms of an alternate 0). Linux's `CTL_MSG_DELAY`
   flags exist for such devices. Confidence: low; no save separates it
   from 1.
3. **A device fault independent of the host's timing** (heat, power,
   firmware age), with the short sessions only coincident. Confidence:
   low; it cannot be ruled out.

`uac2` does not overlap requests, does not leave isochronous transfers
out at the switch, and does not re-send an unchanged rate (established);
none of those is the cause.

## 6. The rough playback (22:08) and the silence (22:16)

**The rough playback.** The save has 26 sessions; the affected one is
not marked. Every session near the save (the 10 to 47 s ones from
06:05:56 to 06:08:19 UTC and the short ones after) has the same pipe
(`iso.page` `01020070`, `iso.recs` `01020808`), `dry` 0, and a longest
gap of about 4.0-4.1 ms (one URB), the same as every other session: the
host side published URBs on time and the controller never ran dry
(established). The total publications times 32 agree with the
controller's isochronous packet count, so every session ran 32-packet
URBs (established). Bytes per packet, the alternate and the rate are not
in the notes and cannot be recovered.

Ways the device's clock and the stream can disagree in `uac2`, by
reading: (a) the device reads back the requested rate but applies
another, or resets its clock at SET_INTERFACE (the rate is set before the
alternate, as Linux does for UAC 2.0; Linux has `SET_IFACE_FIRST` and
`ALWAYS_SET_RATE` for devices that need otherwise); `uac2` trusts a
GET_CUR that reads the target and skips SET_CUR; (b) a SET_CUR that fails
fails the start (not silent); (c) a rate change only happens between
exclusive-mode opens or between a shared and an exclusive program; in
shared mode the device stays at the engine's rate (measured); (d) the
packet cadence is computed fresh at every start from the stream's own
rate (`UacRateInitialize`), measured correct from the first packet.
**Pitch unchanged but rough** fits a device whose clock and stream differ
while the device keeps its buffer level by dropping or repeating frames
(content time-compressed or stretched to the device's clock: distortion
without a pitch change), or damaged content; a plain clock mismatch on a
device that plays what it gets changes pitch. **Hypothesis, low
confidence:** a start race on the device side (its clock or the
alternate not settled when the stream began, as in 5.1), cleared by the
next open; nothing in the save or the guest shows it, and no start race
inside `uac2` was found (each start is serialised under the stream's
operation mutex and the device lock after the previous stop has
finished).

**The silence.** In the 22:16 save the controller is not stopped
(recovery window 0 of 3, no `ctrl.failed`, no command time-out). The last
closed session (17 publications) ended at 06:14:27.872 UTC; after it a
session **opened and was still running at the save**: 17530 publications
(about 70 s), its longest gap 4.1 ms ending 15.6 s before the snapshot,
`dry` 0. So its SET_INTERFACE to the alternate was answered, and `uac2`
was still streaming. After it, `door.ep0.withdrawn` (slot 1, the
adapter) and `door.desc.fail` `03000300`, `03000301`, `03000302`: the
tree tool's reads of string descriptors 0, 1 and 2 (`hcd_door.c`: outcome
3, `HCD_CTL_NOT_SENT`, not answered within the bound) and the request
withdrawn from EP0 (`hcd_enum.c`). The adapter is absent from the
report's device list. So **the device stopped answering control requests
while its isochronous stream went on being sent** (established). An
isochronous OUT endpoint has no handshake: neither the controller nor
`uac2` can tell whether the device takes the data, and this device has no
feedback endpoint. "Audio gone" is therefore the device playing nothing
while the host streams; `uac2` reported nothing because nothing failed on
its side. A later start would have failed after 5 s (section 4).

**One progressive fault or several.** Same device, same boot, ten
minutes apart, and each symptom fits the device's firmware degrading
(rough output, then a dead control endpoint while streaming, and on other
occasions a dead control endpoint at an alternate change). That is
consistent with one device-side fault, but the saves cannot show the
device's internal state; separate causes are not ruled out. The rough
playback and the stop could share the trigger of 5.1 (a start right after
other alternate changes) only as a hypothesis.

## 7. Candidate changes in `uac2`

| Change | Cost and risk | Owner decision |
|---|---|---|
| **A. Linger:** after a stop or close, keep the alternate (and the clock claim) selected for `L` ms with no URBs, and drop to alternate 0 only when it ends, at a D-state change, a removal or another format; a RUN of the same format within `L` sends neither SET_INTERFACE nor clock requests | A deferred alternate-0 (timer or work item, never blocking the audio stack); the device sits at its alternate with no packets for up to `L` (a synchronous sink plays nothing; some amplifiers stay powered, a few mA on a bus-powered dongle; no selective suspend during it, which `uac2` does not use); Windows expects nothing of the device after a pin close, so keeping the alternate is allowed; it must end before power-down and removal | yes: the default `L` and whether it is on for every device |
| **B. Minimum hold:** never select alternate 0 sooner than `H` ms (e.g. 200) after alternate N, the stop deferring it | Small; covers the "alternate 0 soon after N" case of all three failures even after long quiet; adds `H` to the next format change | yes, with A or alone |
| **C. Delay after SET_INTERFACE**, Linux's `IFACE_DELAY` (50 ms) | Start latency +50 ms; for every device or a listed few (3302 products); proven only as Linux's choice for a sibling product | yes: all devices or a list |
| **D. Fewer requests at a start:** the clock RANGE and mute read once per device, not per pin; a short pause before the first request after a SET_INTERFACE (Linux's 1-5 ms flags) | Small; reduces EP0 traffic near alternate changes | no for D's caching, yes for a delay |
| **E. Not re-sending an unchanged rate** | Already done (measured) | - |
| **F. Serialising requests** | Already done (device mutex) | - |
| **G. "Not answering" state:** after a request times out, stop sending; fail opens at once; ask PnP to report the device failed (`IoInvalidateDeviceState`, `PNP_DEVICE_FAILED` at the next query) so Device Manager shows a problem; record it in the driver key (written and never read, as the other diagnostics) and tell the user to unplug and replug | Removes the 5-15 s blocks and the hammering seen in section 4; the user sees one clear state instead of silence | yes: the user-facing message and whether to fail the devnode |
| **H. Per-device setting** for A to C, written by the INF at the default and documented, as `Uac2QueueMs` (the project's rule: every value an INF writes is visible and documented) | INF and design record | yes |

A port reset or cycle requested by `uac2` (`IOCTL_INTERNAL_USB_RESET_PORT`,
`IOCTL_INTERNAL_USB_CYCLE_PORT`) is not recommended as automatic
recovery: both drive a USB reset with bus power kept, and the first save
already shows the device after such resets (the host driver's recovery
reset the port, which came back enabled at High Speed, and Address
Device still never completed); only an unplug (bus power removed)
restored it. The simulator cannot show firmware reset behaviour: a
simulated device re-enumerates after any reset. Which of the two
requests each system and xHCI98 honour from a function driver was not
checked in this run.

**Recommendation:** A with B (one mechanism: a deferred alternate 0, at
least `H` after the alternate was selected and `L` after the stop), on
for every device with conservative defaults (`H` 200 ms, `L` 2 s), each a
documented INF value; and G. Against the three failures: the FL1100
burst (36 ms cycles) would have made no alternate change at all; the
first failure (0.68 s spacing) and the second (after 329 s) would have
had their alternate 0 sent no sooner than the hold after the alternate.
C only for listed devices if A and B do not cure it. The rough playback
gives no direct case for a change.

**Tests.** Simulator: a model that stops answering on an alternate 0
within `X` ms of alternate N, and one after `N` changes in `T` ms; the
`waveOut` loop and the DirectSound play loop with the change: no
SET_INTERFACE between streams inside `L`, none sooner than `H`, the
alternate 0 at `L`, at removal and at a D-state change; G: the first
timeout fails later opens at once and sends nothing more. Only the real
device can show whether the stops end.

## 8. What to ask the user

- Whether unplugging and replugging alone restores it each time (the
  silence: yes, by the user; the controller stops: the device worked
  again after a replug on another controller).
- Whether it happens with another player or only with the game, and
  whether the game or player uses exclusive mode.
- The device's default format in the Sound control panel (Advanced), and
  the sample rate and format the program plays.
- At the next occurrence, before any unplug or restart: an export of
  the device's driver key (`Uac2UsbStats`, `Uac2Streams`, `Uac2Waits`,
  `Uac2VolMap`) taken while it is wrong, and a tree tool report saved
  with `XhciLogVerbosity` raised to 3.
- For the rough playback: whether closing and reopening the program
  always cures it.
