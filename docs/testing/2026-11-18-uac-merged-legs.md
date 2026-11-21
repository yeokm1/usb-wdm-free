# uac2 and uac1: the merged binaries in guests, 2026-11-17 and 18

The guest legs owed on the binaries of main at `65dd131` (the merge of
`uac2`'s failed state, default off and not complete, with stage 1 of the
capture wiring: [uac2-cap-stage1](2026-11-17-uac2-cap-stage1.md), "After
the merge with main", and
[uac2-failed-state-legs](2026-11-17-uac2-failed-state-legs.md), "After
the merge with main"). Development legs in QEMU (TCG, one processor,
headless) with the simulators (`uacemu.py`, `uac1emu.py` of `65dd131`);
no physical device was used, and nothing here says how a real device
behaves. No README "Tested by the author" cell changes.

## Binaries

Built from clean trees with the packages' own `build.cmd`, the default
level engine (`UAC_LEVEL` unset); every build passed its gates. SHA-256:

| Image | Commit | SHA-256 |
|---|---|---|
| `uac2wdm.sys` `wdmx86` free (B, run on XP, 98 SE and 7) | `65dd131` | `11826C140D703C1CAA2FC927917DD7876DE58D2BABE03A6F92D237F17A784EDF` |
| `uac1wdm.sys` `wdmx86` free (run on 98 SE) | `65dd131` | `2EC2518017AAD0EB384E73991AB29604B6889F00CA397A76454D8AB6EDC03ADB` |
| `uac2wdm.sys` `wdmx86` free (A, run on XP) | `58a8c26` | `BB986CEAEA710F47B595DA69C3AD36088833E4540BBDFCD04A5B7003580EACF5` |
| `uac2wdm.inf` (both commits; the released one) | | `7FE359AC89183D33ED2D6DFD6C64D824B3BF67237B60F48A08B3C2D6E2E66735` |
| `uac1wdm.inf` (both commits) | | `E069FDB4C6231E323CC8AD71E0581312DA3BE5220864C4A2ED0986A270177C41` |

Also built at `65dd131` and not run: `uac2` `wdmx86` checked and trace,
`wdm64` free, checked and trace, `uac1` `wdmx86` checked. `58a8c26` is
main just before the stage-1 merge (`65dd131`'s first parent).

Test programs (scratch, not tracked): `UACWAV.EXE` built by
`src/audio/uac2/wav.cmd` at `65dd131`, and a timed WinMM program (play
with position readings, record with returned-buffer counts, timed opens;
the one the failed-state record used).

## XP: A against B

XP SP3 x86, QEMU 11.1.0 (q35), Windows' own EHCI, a fresh overlay of the
guest the level baselines left (the shipped `uac2` installed, the
`x4full` devnode on EHCI port 1, driver key `0021`), `uacemu.py --profile
x4full` over `usb-redir`. As in the stage-1 record: each image copied
over the installed file with no device present (`fc /b`: no difference),
a restart, then: the MEDIA class keys read, plug, `UACWAV list`, the keys
read, `UACWAV play 2 16 48000 3`, `UACWAV rec 2 16 48000 3`, the keys
read, unplug, the keys read. Pass 1: A (`a1`), then B (`b1`), then B
again after a replug in the same boot (`b2`), while three other guests
of these legs ran. Pass 2: A (`a2`) and B (`b3`) back to back, with no
other guest of these legs running.

- **Every run:** one waveIn and two waveOut devices; playback "PASS: all
  buffers played"; recording "PASS: all buffers recorded, signal present"
  (576000 bytes, peak -6.0 dBFS). At the simulator the play arrived at
  -20.0 dBFS in every one-second window of every run, with no clipping.
- **The device's requests:** 110 control requests in each of the five
  runs, the same in the same order (request, value, index, length, status
  and data), 82 of them class requests, none to Selector Unit 26; the
  same nine SET_INTERFACE changes in the same order.
- **`Uac2VolMapC`** (key `0021`): absent under A in pass 1 (no run before
  it wrote one); under B 412 bytes in every dump after the plug:
  `Channels` 2, `Output` 6, `Gain[8]` 65536, `Sources` 4, `RouteState` 1,
  `GraphFlags` 0, `Selector[0]` Id 26 with `Position` and `Status` 0,
  every other field 0, the record `captest` builds for the recorded X4,
  the same in `b1`, `b2` and `b3`. In pass 2 the value stood in the key
  from pass 1 under A as well, unchanged (A neither writes nor removes
  it).
- **The rest of the MEDIA class key**, after the streams: keys `0000` to
  `0020` the same in every dump; in `0021` the same values (by name, type
  and size) but `Uac2VolMapC`; `Uac2VolMap` the same, except its `Change`
  count in `b2` (8 where the other runs read 6: `b2` was the second plug
  in one boot; its requests were the same); in `Uac2UsbStats` and
  `Uac2UsbStatsIn` the format, route, queue and level words (48 to 55, 57
  to 62, 65 to 97) the same, and per-run counts, bus frames and the
  worker's words different.
- **Run to run:** the render stream's packet errors, data packets the
  host controller did not reach (`0xC0020000`), bad feedback and the
  worker's demotions from real time varied with the run, not with the
  image (the host also ran unrelated guests throughout):

| Run | Image | Other guests of these legs | Packet errors | `0xC0020000` packets | Bad feedback | Worker demotions |
|---|---|---|---|---|---|---|
| `a1` | A | three | 10 | 0 | 10 | 0 |
| `b1` | B | three | 217 | 144 | 73 | 8 |
| `b2` | B | three | 77 | 40 | 37 | 6 |
| `a2` | A | none | 54 | 24 | 30 | 4 |
| `b3` | B | none | 0 | 0 | 0 | 0 |

Verdict: the same requests on the wire under A and B, playback and
recording passing under both, and the capture record present and well
formed under B: PASS.

## 98 SE

98 SE in QEMU 11.1.0 (`pc`, `pentium3,-apic`, 256 MB), each on a fresh
overlay: under NUSB 3.3's EHCI the base `w98-nusb-hub` (98 SE, NUSB 3.3,
xHCI98 3.1.0.0 underneath; the device on `usb-ehci`), under xHCI98 the
base `w98-hcd310` (98 SE with xHCI98 3.1.0.0; the device on
`qemu-xhci`). Install as in
[uac2-98se-inactive](2026-11-17-uac2-98se-inactive.md): the driver folder
on `DevicePath`, a restart, the plug, "Copy files from" answered with the
folder. Devices: `uacemu.py --profile hs24` (`uac2`), `uac1emu.py
--profile hs` (`uac1`); one leg with `--profile x4full`.

Each leg ran, from a batch file: four mixer-API SETs through `UACWAV`
(the speaker line's volume 32768, mute on, mute off, volume 65535), a 10 s
play reading the position each second, an 8 s record counting returned
buffers, three timed open and close pairs, a 5 s play, `UACWAV rec 2 16
48000 3`, then the MEDIA class key exported. The setting
`Uac2FailOnTimeout` was written as four bytes with `REGEDIT /S` and read
back in each export; "absent" ran before it was first written.

"PASS" below: every `UACWAV` SET returned PASS and the simulator logged
the SET_CURs (feature unit 2: volume, mute 1, mute 0, volume), answered;
every open, reset and close returned 0 and both positions advanced;
`UACWAV rec` "PASS: all buffers recorded, signal present" (8 of 8 buffers,
576000 bytes, peak -6.0 dBFS); `Uac2M0Stage` `0x1F`, `Uac2M0Status` 0;
no `Uac2Failed`.

| Stack | Driver | Setting | Seen | Verdict |
|---|---|---|---|---|
| NUSB | `uac2` | absent | install (hot plug, "Insert Disk", `C:\U2`); the leg; simulator 78 control requests, no request unanswered | PASS |
| NUSB | `uac2` | absent | idle unplug: **the guest hung** (the volume icon stayed, the clock stopped, the Start menu no longer opened); QMP quit, cold boot; a plug then started | unplug FAIL |
| NUSB | `uac2` | 0 | written, shutdown with the device attached (clean), cold boot with the device attached: the leg; `Uac2FailOnTimeout` `00000000`; 82 control requests | PASS |
| NUSB | `uac2` | 1 | the same: the leg; `01000000`; 82 control requests, none unanswered; then an idle unplug after the streams: **the guest hung** again | leg PASS; unplug FAIL |
| NUSB | `uac1` | absent | install (98 SE's composite parent from `USB.INF`, then the function's disk, `C:\U1`); the leg (Build 5); 47 control requests; idle unplug: the guest carried on; a replug on another port made a new devnode (MEDIA `0007`), which started | PASS |
| NUSB | `uac1` | 0 | (after a restart and a shutdown with the device attached, both completed) written in a boot without the device, shutdown (clean), cold boot with the device attached: the leg; `00000000`; 45 control requests | PASS |
| NUSB | `uac1` | 1 | written, shutdown with the device attached (clean), cold boot with it: the leg; `01000000`; 45 control requests; idle unplug: the guest carried on; replug: started, the leg again; shutdown with the device attached: clean | PASS |
| xHCI98 | `uac2` | absent | install (`C:\U2`); the leg; idle unplug: the guest carried on; the replug **wedged at arrival** (below); QMP quit, cold boot, plug: the leg again; 76 control requests per leg | legs PASS; one replug wedged |
| xHCI98 | `uac2` | 0 | written, idle unplug, replug: **wedged at arrival**, and the QMP quit lost the write (the cold boot's plug then ran the leg with the value absent: PASS); written again, shutdown with the device attached (clean), cold boot, plug: the leg with `00000000` read back | legs PASS; one replug wedged |
| xHCI98 | `uac2` | 1 | written, shutdown with the device attached (clean), cold boot: the leg with `01000000`; idle unplug, replug, the leg again; idle unplug, a second replug: started; shutdown with the device attached: clean | PASS |
| xHCI98 | `uac1` | absent, 0, 1 | install (`C:\U1`, no composite wizard); the leg with the setting absent; then 0 written, idle unplug, replug, the leg; then 1 written, idle unplug, replug, the leg; 42 control requests per leg, the endpoint's sampling-rate SET_CURs among them; no underrun, overrun or misframed packet at the simulator; shutdown with the device attached: clean | PASS |
| xHCI98 | `uac2`, `x4full` | absent (its own key) | on the `uac2` guest: xHCI98's composite parent split the device; its audio function bound `uac2` with no prompt (MEDIA `0009`); the HID functions took a wizard (one found no driver and was cancelled; 98 SE's `HIDDEV.INF` for the other); one waveIn and two waveOut devices; the leg (the timed program on the wave mapper); the SETs to feature unit 14; 132 control requests, none to Selector Unit 26; idle unplug: clean; then in the same boot two `hs24` replugs, each after an idle unplug, started, and a shutdown with it attached completed | PASS |

Unplugs and starts, by stack:

- **NUSB's EHCI, `uac2`:** both idle unplugs hung the guest (2 of 2), as
  for the binaries before the merge (3 of 3, the released one among them,
  [open-items](open-items.md)). No arrival wedge in this session: four plugs
  started, two hot plugs (the install, and the plug after the first hang's
  cold boot) and two with the device attached at boot.
- **xHCI98, `uac2`:** of six `hs24` replugs, two wedged at arrival (no
  volume icon, the guest clock stopped, the Start menu dead); in both the
  simulator logged the start's 30 control requests and the configuration,
  the same as the starts that came up, then nothing. Each cold boot's plug
  started (5 of 5). The earlier record had 3 of 3 replugs passing under
  xHCI98 with the binary before the merge; no control with the images
  before the merge was run in this session, so whether the merged binary
  wedges more often is not shown here (the control series: the addition below). Idle unplugs under xHCI98: 8 of 8 clean (six on the
  `uac2` guest, the `x4full` one among them, two on the `uac1` guest).
- **Shutdowns:** every shutdown completed (under NUSB five with the
  device attached, three without and one restart; under xHCI98 five with
  the device attached and two without, after the installs' folder copies). No
  `uac1` shutdown hung in this session.
- **Faults at start:** no fatal exception, blue screen or hang at any
  start that came up (four `uac2` and six `uac1` starts under NUSB;
  under xHCI98 nine `uac2` starts with `hs24`, three `uac1` starts and the
  `x4full` start). The two wedges above came after the start's requests.
- **`uac1`'s pace under NUSB:** a 10 s play's position ran ahead of the
  guest's clock again (2572800 bytes at second 9 in one leg), with no
  underrun or overrun at the simulator: an item of [open-items](open-items.md), not judged.

### The capture record

`Uac2VolMapC`, 412 bytes, in every export after a start under both stacks:

- `hs24` (`uac2`) and `hs` (`uac1`): `Channels` 2, `Output` 6, `Gain[8]`
  65536, `Sources` 0, `Confirmed` 0, `RouteState` 0, `GraphFlags` 0, every
  other field 0. Their capture line is input terminal 4, feature unit 5,
  output terminal 6: one source, no chooser, which `UacCapStart` records
  as `Sources` 0 and `RouteState` 0. `captest` has no fixture of these
  two profiles; the record was read against the descriptors and the
  code.
- `x4full` under xHCI98 (MEDIA `0009`): `Channels` 2, `Output` 6,
  `Gain[8]` 65536, `Sources` 4, `RouteState` 1, `GraphFlags` 0,
  `Selector[0]` Id 26 with `Position` and `Status` 0, every other field
  0: `captest`'s X4 record, as on XP.

What the stack legs show: the discovery of these graphs (one source, and
the X4's four sources behind one selector, the deepest of the recorded
devices) at a PnP start on 98 SE under both stacks (the X4 under xHCI98
only), with no fault. They do not show room on the kernel stack for a
graph at the depth bounds of design 28.3: no simulator profile has one.

## Windows 7: the failed state

Windows 7 SP1 x86 (7601), QEMU 11.1.0 (`pc`), Windows' own EHCI, a fresh
overlay of the guest the failed-state legs left (`uac2` installed for the
simulated device's VID/PID, driver key `0009`, `Uac2Failed` Count 7 from
those legs). The merged image copied over the installed file (`fc /b`: no
difference) and `Uac2FailOnTimeout` written as `REG_DWORD` 1 in each
`uac2` driver key, then a restart. Device: `uacemu.py --profile hs24
--vid 3302 --pid 336a` on the EHCI's port 5, with `--stop-after-s` or
`--stop-at-request` as in the earlier legs; the same programs (the timed
WinMM program and `sndvol`'s slider for a volume SET that reaches the
driver). `Uac2Failed`'s words in the earlier record's order; TimeLow,
TimeHigh and the three after LateMs read `0xFFFFFFFF` and Build 29 in every
record.

| Leg | Case | Seen | Against the earlier record |
|---|---|---|---|
| 0 | healthy, setting 1 | Code 0; play 12 s and capture 10 s, both positions advanced; a `sndvol` volume change sent SET_CUR (feature unit 2), answered. `Uac2VolMapC` 412 bytes: `Channels` 2, `Output` 6, `Gain[8]` 65536, the rest 0 | same; PASS |
| 1 | playback; failure by a volume SET (`--stop-after-s 10`) | the SET_CUR the only request after the stop; the OUT stream ended about 7 s after it; `waveOutWrite` 6, positions failed from then on; Code 43; three opens afterwards returned 2 in 0 ms (the immediate refusal); a second press sent nothing (`sndvol` already showed no device). Record: Count 8, Kind 1, Setup0 `0x02000121`, Setup1 `0x00020200`, Status `0xC0000120`, UrbStatus `0xC0010000`, LateMs 2 | same (LateMs 3 before); PASS |
| 1 | unplug of the failed device, replug | the devnode went away, the guest carried on; the replug started healthy | same; PASS |
| 2 | capture; failure by a playback open (`--stop-after-s 8`) | the capture ran (a START after a failure); the open's clock RANGE the only request after the stop; that open returned 1 after 6817 ms, the next 2 in 0 ms; the capture's position failed, its buffers stopped, reset and close at once; Code 43. Record: Count 9, Kind 1, Setup0 `0x010002A1`, Setup1 `0x00021000`, `0xC0000120`, `0xC0010000`, LateMs 4 | same; PASS |
| 3 | duplex; failure by a volume SET (`--stop-after-s 8`) | both positions advanced until the failure; one SET_CUR reached the device (two presses 2 s apart, it arrived 6 s after the first), the only request after the stop; the OUT stream ended about 4.5 s after it; `waveOutWrite` 6, both positions failed, resets and closes at once; Code 43. Record: Count 10, Kind 1, `0x02000121`, `0x00020200`, `0xC0000120`, `0xC0010000`, LateMs 6 | same; PASS |
| 5 | SET_INTERFACE as the expired request (`--stop-after-s 6`) | not arranged, as before: the stream's stop sent SET_INTERFACE(1, 0), unanswered; the close took 5289 ms and no failure was recorded for it (Windows' stack completed it before `uac2`'s deadline); the next open's clock RANGE expired (1 after 5320 ms, then 2 in 16 ms); Code 43. Record: Count 11, Kind 1, `0x010002A1`, `0x00021000`, `0xC0000120`, `0xC0010000`, LateMs 1 | same; PASS |
| 7 | a failed START (`--stop-at-request 15`): the startup latch | request 15, the string descriptor 0 read of `uac2`'s START, the only request after the stop; Code 10, the INF's name; `Uac2M0Stage` 13, `Uac2M0Status` `0xC00000B5`. Record: Count 12, Kind 2, Setup0 `0x03000680`, Setup1 `0x00400000`, `0xC0000120`, `0xC0010000`, LateMs 0 | same; PASS |
| 11 | a volume SET while an open's request hangs (`--stop-after-s 1`) | an open's clock RANGE unanswered; a `sndvol` press 2.7 s into the hang; no request reached the device after the RANGE; the open returned 1 after 5320 ms; Code 43 with the device attached. Record: Count 13, Kind 1, `0x010002A1`, `0x00021000`, `0xC0000120`, `0xC0010000`, LateMs 1 | new; the failure as in leg 2; whether the SET waited in the driver is not observable from the guest |
| 8 | power-down with that failed device attached | QMP `system_powerdown`: QEMU exited about 20 s later; after the reboot the record read Count 13 | same; PASS |
| 9 | after the reboot, healthy | Code 0; an 8 s play reached the device (90680 packets, non-zero data) | same; PASS |
| 12 | leg 11 again, with screenshots | a healthy SET first (answered); then an open's RANGE unanswered and a press 2.3 s into it: the slider moved, no SET_CUR reached the device; the open returned 1 after 5444 ms; Code 43. Record: Count 14, Kind 1, `0x010002A1`, `0x00021000`, `0xC0000120`, `0xC0010000`, LateMs 1; unplug: the guest carried on | as leg 11 |
| 10 | setting 0 with the forced stop (`--stop-after-s 1`) | three opens each returned 1 after about 5.1 to 5.3 s; every open's request went to the stopped device (four unanswered requests); Code 0; `Uac2Failed` unchanged (Count 14) | same; PASS |

In every failing leg the expired request was the last the device saw,
and Status was the lower stack's `STATUS_CANCELLED` with
`USBD_STATUS_CANCELED`, as before. Each healthy start sent the saved
level (a SET_CUR of feature unit 2's volume) before any stream.

## What did not run

- A level SET queued to the level work item, and the teardown that waits
  for it: not arranged. A SET waiting behind an operation whose request
  expires: legs 11 and 12 show only that no SET reached the device.
- Windows 7 leg 6 (an unplug while a request is outstanding), a PnP stop,
  sleep and resume: not run.
- `uac2` and `uac1` under xHCI98 with the forced stop on 98 SE, and on
  98 SE with the trace build: not run.
- The x64 images, the retained build (`UAC_LEVEL=retained`), Windows ME,
  2000, Vista: not run.
- A control with the images before the merge for the xHCI98 replug wedges
  on 98 SE: not run in this session; run afterwards (the addition below).
- A device whose capture routes reach the depth bounds of 28.3: none
  exists among the simulator's profiles.

## Evidence

On the development host, `E:\usb-wdm-free\uaclegs\`: `STATE.md`,
`hashes.txt`, `logs\` (builds); `xp\` (`xa\` serial logs, simulator logs,
screenshots, `cmp.txt`, `keys.txt`, `stats.txt`, `pass2.txt`); `s98\n\` and `s98\x\`
(each `STATE.md`, `ev\` with the program logs and MEDIA exports per leg,
`logs\` simulator logs, `shots\`); `w7\` (`STATE.md`, `stage\` program
logs and key exports, `logs\`, `shots\`).

## Addition, 2026-11-18: the 98 SE wedges and hangs against the image before the merge

A controlled series for the two 98 SE results above that did not pass:
`uac2`'s replugs that wedged at arrival under xHCI98 (2 of 6, where the
earlier record had 3 of 3 passing) and its idle unplugs that hung the
guest under NUSB's EHCI (2 of 2). Image A (`uac2wdm.sys` free at
`58a8c26`, `BB986CEA...`) against image B (`65dd131`, `11826C14...`), the
same procedure for both, one series after the other with no other guest
of these legs running (unrelated guests ran on the host throughout),
except that the two NUSB series ran side by side.

**Rig.** For each stack a prepared overlay: a fresh overlay of the base
above with `uac2` (B) installed once by its first plug (`uac1` on one of
its own for the `uac1` series). Each series on a fresh overlay of that:
its image copied over the installed driver file (`fc /b`: no
difference), a clean shutdown, then the pattern from a cold boot. Device
`uacemu.py --profile hs24` (`uac1emu.py --profile hs` for `uac1`), a new
simulator process per plug. A script drove the guest: 60 s after a plug
it asked the guest, through Start and Run, to run a batch file that
writes a file to the floppy with the driver key's `Uac2M0Stage` and
`Uac2M0Status`. "Up" means that file came back (within 60 s, or within
60 s of a second request 45 s later); "wedged" that it did not, and in
each such case the screenshot showed the taskbar clock stopped. A
replug: the device removed (QMP `device_del`), 5 s, its simulator
stopped, 20 s idle, a new simulator, the plug. An idle unplug was
"alive" or "hung" by the same request 20 s after the removal. After a
wedge or a hang: QMP quit and a cold boot.

**Patterns.** In-boot: a cold boot, its first plug, then replugs in the
same boot (a cold boot again after a wedge). Per-boot: a cold boot, its
first plug, an idle unplug, one replug, a shutdown. NUSB: a cold boot, a
plug, an idle unplug, until five unplugs had run.

| Stack, driver | Pattern | Image | Replugs wedged | First plugs after a cold boot wedged | Idle unplugs hung |
|---|---|---|---|---|---|
| xHCI98, `uac2` | in-boot | A | 0 of 20 | 0 of 1 | 0 of 20 |
| xHCI98, `uac2` | in-boot | B | 0 of 20 | 1 of 2 | 0 of 20 |
| xHCI98, `uac2` | per-boot | A | 0 of 6 | 4 of 10 | 0 of 6 |
| xHCI98, `uac2` | per-boot | B | 0 of 6 | 4 of 10 | 0 of 6 |
| xHCI98, `uac1` | in-boot | B | 0 of 10 | 0 of 1 | 0 of 10 |
| NUSB, `uac2` | plug, idle unplug | A | | 3 of 8 | 4 of 5 |
| NUSB, `uac2` | plug, idle unplug | B | | 2 of 7 | 4 of 5 |

`uac2` under xHCI98 in all: replugs wedged A 0 of 26, B 0 of 26; first
plugs after a cold boot wedged A 4 of 11, B 5 of 12. (A per-boot cycle
whose first plug wedged had no replug, hence 6 replugs in 10 cycles.)

- **On the wire:** the initial `uac2` start sequence at every `uac2` plug
  of these series, wedged or not, was the same 30 control requests in the
  same order (the start's descriptor and string reads, the clock and
  volume RANGE and CUR reads), every one answered, and the configuration
  set; after a wedge, nothing more. (Later requests are not part of it:
  one in-boot plug's log has 36 in all, a stream at its shutdown among
  them. `uac1`'s start at each plug of its series: 24 requests.) Nothing
  the device sees tells a wedged start from a good one, under A as under
  B.
- **The trace flavour** was not run: its port `0xE9` trace has lines only
  for the stream (the `UAC_TRACE_E9` sites in `uacusb.c`: stream start,
  completions, the worker's idle dumps), none for a start, so it cannot
  show where a start-time wedge stops.
- **xHCI98's own diagnostics** (3.1.0.0 keeps its log in the driver and
  gives it through its USB Tree program in the running guest): a wedged
  guest cannot run it, and guest memory was not read. Not taken.
- **The stage word at a wedge** could not be read: the guest did not
  answer, and the QMP quit loses what 98 SE had not written out.

Reading: the replug wedge of the earlier leg (2 of 6, B) did not recur
under either image in 26 replugs each with this procedure; those two
replugs differed from these in what ran before them (the leg's streams
and setting changes) and in timing. What recurred is a wedge at the
first plug after a cold boot, observed 4 of 11 times under A and 5 of 12
under B (every request answered): the 98 SE arrival wedge of the earlier
records (task 36.41), recorded before under NUSB, here under xHCI98 too.
These counts show no difference between the images; samples this small
cannot exclude one, and they do not settle the cause of either wedge.
Under NUSB the idle unplug hung the guest 4 of 5 times under A and 4 of
5 under B; the hang predates the merge (3 of 3 in the earlier record,
the released image among them, and 2 of 2 above), and samples this
small cannot exclude a difference between the images either.

From the rig, not the driver: once the probe's own registry export raced
its search (a sharing-violation prompt; the second request answered),
and that prompt's window later held up a shutdown, ended by QMP quit; the
probe waited for the export from the next series on. A first `uac1`
series attempt was stopped and discarded before its first plug (its
file names collided with the unplug probe's).

Evidence: `E:\usb-wdm-free\uaclegs\s98\`: `rig\series.py` and
`rig\runseries.sh` (the procedure); `p\`, `p1\`, `pn\` (the prepared
overlays); per series `ia\`, `ib\`, `ca\`, `cb\`, `u1b\`, `na\`, `nb\`
(each `series.log`, `stage\` with the probe files and `SWAP.OK`, `logs\`
simulator logs, `shots\`); `u1b-void\` (the discarded attempt).
