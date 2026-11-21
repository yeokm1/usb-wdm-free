# uac2 and uac1 capture discovery, stage 1, 2026-11-17

Design record section 28.18 (`docs/design/uac2.md`): stage 1 of the
recording-input and capture-level wiring builds each capture endpoint's
graph at start and writes its route diagnostics (`Uac2VolMapC`), and is
meant to change nothing else. This record holds what shows that, on the
host and in one development leg in QEMU (TCG) with the simulator. It is
not "Tested"; no real device was used.

## Host

- `uac2`'s `test.cmd` and `uac1`'s `test.cmd` at the stage's commits: every
  gate passes. Their output is line for line the output before the stage
  except `captest`'s count (`uac2` 431 to 574, the UAC 1.0 build 296 to
  454): the new checks compare `UacCapStart`'s whole 412-byte record byte
  for byte (the X4, Play! 2, Logi headset and Brio 100 fixtures; a
  chooser, none, a selector behind a join, a loop, a truncated unit, no
  storage) and the value names. Seven mutations of `UacCapStart` and
  `UacCapDiagName` (a field dropped or wrong, the record part-cleared, the
  no-storage `RouteState` missing, a name digit wrong) each fail
  `captest` in both families.
- Builds: the eight default builds (`uac2` x86 and x64 free, checked and
  trace; `uac1` x86 free and checked) and the three retained builds
  (`UAC_LEVEL=retained`: `uac2` x86 and x64, `uac1` x86) pass their gates
  before and after; every image's import list is the same before and
  after.
- Objects, before and after (the COFF time stamp aside): in the free,
  trace and retained builds every object but the driver's own
  (`uac2wdm.obj`, `uac1wdm.obj`) is byte for byte the same, and
  `uaccap.obj` is new. In the driver's object, function by function (code
  bytes and relocations): x86 free and retained, `uac2` and `uac1`, and
  x64 free, trace and retained: `Wave::Discover` is new; `Wave::Prepare`
  differs by its call to it (and the jumps over it), the constructor and
  destructor by the new members' setting and freeing, and `Start` only in
  the size of the pair object it allocates (x86 `0x29F0` to `0x2B90`);
  every other function is identical (x86 `uac2` free: 129 of 133).
  The checked builds' objects differ throughout by their debugging
  information, as two checked builds of one tree do.

## Guest

One leg, 2026-11-17: XP SP3 x86 in QEMU 11.1.0 (TCG, q35, one vCPU),
Windows' own EHCI (`usb-ehci`, no xHCI), a fresh overlay of the guest
the level baselines left (the shipped `uac2` installed, the `x4full`
devnode on EHCI port 1, driver key `0021`), `uacemu.py --profile x4full`
over `usb-redir`, one simulator process per plug.

- Image A: the free x86 `uac2wdm.sys` built just before the stage
  (`f51e8331...`); image B: the stage's (`a35a7f24...`). Each copied over
  the installed file with no device present (`fc /b` found no
  difference), then a restart, then the same steps: the MEDIA class keys
  read, plug, `UACWAV list`, the keys read, `UACWAV play 2 16 48000 3`,
  `UACWAV rec 2 16 48000 3`, the keys read, unplug, the keys read.
- Both: the device listed with one waveIn and two waveOut devices (its two
  pairs); playback "PASS: all buffers played" (8 of 8); recording "PASS:
  all buffers recorded, signal present" (8 of 8, 576000 bytes, peak
  -6.0 dBFS), the same figures under both images. No dialog on screen
  after either plug.
- The device's requests: the simulator logged 110 control requests under
  each image, the same in the same order (request, value, index, length,
  status and data); 82 of them class requests, none to Selector Unit 26.
- `Uac2VolMapC` (key `0021`) under B: 412 bytes; `Channels` 2, `Output`
  6, `Gain[8]` 65536, `Sources` 4, `RouteState` 1 (a chooser, its
  position not read), `GraphFlags` 0, `Selector[0]` Id 26 with
  `Position` and `Status` 0, every other field 0: the record `captest`
  builds for the recorded X4. No `Uac2VolMapC01` (pair 1 has no capture
  line).
- The rest of the MEDIA class key, A's dumps against B's: keys `0000` to
  `0020` the same in every dump. In `0021`, after the streams ran, the
  same values (by name, type and size) but `Uac2VolMapC`; `Uac2VolMap`
  the same; in `Uac2UsbStats` and `Uac2UsbStatsIn` the format, route,
  queue and level words (48 to 55, 57 to 64, 65 to 97) the same, and only
  per-run counts and bus frame numbers different (bytes and packets
  completed, captured bytes, feedback counts, start frames). Before a
  plug, each dump holds what the run before it left (B's starts after
  A's recording, so it already has `Uac2UsbStatsIn`).
- Not shown by this leg: whether those per-run words vary as much between
  two runs of one image (no repeat run was made), a graph that is
  incomplete or not allocated, and any other system.

## What only other legs can show

- 98 SE (`uac1` and `uac2`, under NUSB's EHCI and xHCI98): not run. The
  stage-1 images there, the value written by 98 SE's registry, and room
  on 98 SE's kernel stack at a PnP start for the walk's worst case (design
  28.18, "Stack") are not shown.
- The x64 images in a guest, and the retained images in a guest: not run.
- A device whose graph is incomplete, a chooser shared by two pairs, and
  an allocation failure, in a guest: not run (host only).

## After the merge with main (2026-11-17)

The branch was merged with main at `58a8c26` (`uac2`'s failed state,
default off and not complete, and the Ethernet forced-speed fix); the
merged result is `937db77`, and `a6a4e87` after it changes text and a
comment only (design section 29's references, the level engine's
contract). The merged result has been built and host-tested only, at
`a6a4e87`:

- Built: `uac2` `wdmx86` and `wdm64`, each free, checked and trace, and
  `uac1` `wdmx86` free and checked, each with the default level engine
  and with `UAC_LEVEL=retained`: 16 of 16 builds pass, every one its
  import gate; `testgate.ps1` passes its 21 cases on each engine's
  images.
- Host-tested: `uac2`'s `test.cmd` (`captest` 574, `leveltest` 1220,
  `routetest` 174, `alttest` 246, the failed state's decisions 60,
  `infchk`), `uac1`'s `test.cmd` (`captest` 454, `leveltest` 35),
  `streamtest.cmd`, `uacsimtest.py` (34) and `uac1simtest.py` (51), every
  one passing.

No merged binary has run in a guest or on any system. The XP comparison
under "Guest" above was made before the merge, with images A and B both
built before it; it does not carry over to the merged binaries. To be
run on the merged binaries, each recorded with its result, a failure or
a leg not run named as such:

- the XP comparison above, with main's images at the merge as A and the
  merged result's as B;
- 98 SE, `uac2` and `uac1`, under NUSB's EHCI and under xHCI98 (the
  room on the kernel stack for the walk at a PnP start is an item of its
  own in [open-items](open-items.md), "kernel stack room for the capture
  discovery on 98 SE and ME");
- the failed state's legs in
  [uac2-failed-state-legs](2026-11-17-uac2-failed-state-legs.md), "After
  the merge with main".

Run on main's binaries at `65dd131` on 2026-11-17 and 18, with what did
not run or did not pass:
[uac-merged-legs](2026-11-18-uac-merged-legs.md).
