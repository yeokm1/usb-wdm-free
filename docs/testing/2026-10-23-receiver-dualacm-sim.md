# xpad98 receiver with four pads, usbcom with two ACM functions: 98 SE, 2026-10-23

Development legs, not a release and not "Tested". Two gaps of the
simulator-coverage audit (`simulator-coverage.md`, ranks 4 and 5), run
with new simulator profiles on 98 SE.

Rig: kit `E:\usb-wdm-free-vm\simcov` (monitor 49200, simulator 49201;
`attachxp.sh`, `attachacm.sh`, `runst.sh`), overlay `s1` of
`net9x\base\w98-nusb-hub.qcow2` (98 SE, NUSB 3.3, xHCI98 3.1.0.0), headless
QEMU, TCG, 1 vCPU.

## xpad98: the 360 wireless receiver with four pads

Simulator `xpademu.py --profile receiver --speed high` (branch `simcov`
124e415): `--pads 4` puts pads on all four slots; `--join SLOT:SECONDS` /
`--leave SLOT:SECONDS` switch a pad on or off later with an unsolicited
08 80 / 08 00 status change. Driver: `build.cmd wdmx86 trace` of `simcov`
(source as `working`), `xpad98.sys` `4cbafdb0...`, `xp98me.inf`
`b3e34e23...`; `JOYTEST.EXE` `68b44483...`.

### NUSB 3.3 EHCI: PASS

- Install: Add New Hardware found "Xbox 360 wireless receiver -
  SIMULATED", chose `A:\XP98ME.INF` ("Xbox 360 Wireless Receiver
  (xpad98)"); the driver started with four top-level collections.
- `--pads 4`: presence asked on all four slots, all four answered present;
  the driver lit each slot's quadrant (LED commands 2, 3, 4, 5 for players
  1-4). `JOYTEST 50`: "PASS: 4 joystick id(s) answered, 4 passed".
- `--pads 2 --join 2:40 --leave 1:60`: the pad joining slot 2 after 40 s
  was lit at once (LED command 4); after slot 1's pad left, `JOYTEST 40`
  read ids 0 and 2 walking the whole pattern (PASS, 142 changes each) and
  ids 1 and 3 still (1 change: the pad that left, centred, and the empty
  slot), "4 joystick id(s), 2 passed" as intended.

### xHCI98 (xHCI98 3.2.0.0 batch 8, gated x86 release build): PASS

The receiver needs 8 endpoints, refused by xHCI98 3.1.0.0's ring pool;
43.67 raises the cap. Batch 8's gated build (`E:\xhci98-wt\integ8`, built
at 546331f2; `xhci98.sys` `0416c8ba...a398`, `xhcihlpr.sys`
`cff7b706...839a`, the hashes `gated-x86-release.txt` lists) was copied
read-only and over the guest's 3.1.0.0 files, then a restart.

- `--pads 4`: the device configured, presence asked on all four slots,
  LEDs 2-5; `JOYTEST 50`: PASS, 4 ids, 4 passed (163 changes each).
- `--pads 2 --join 2:40 --leave 1:60`: slot 2 lit on joining (LED 4);
  `JOYTEST 40`: ids 0 and 2 PASS (129 changes), ids 1 and 3 still.

## usbcom: two CDC-ACM functions in one device

Simulator `acmemu.py --profile dual --speed high --mirror` (`simcov`
f35d2a1): the Pico IDs 2E8A:000A with two CDC-ACM functions behind their
own IADs (interfaces 0-1 and 2-3) and the reset interface 4. Driver:
`build98.ps1 -Profile w98 -Flavour trace` of `simcov`: `USBCOM.SYS`
`bfac1d2b...`, `USBCOMV.VXD` `4ae48e4e...`, `USBCOM.INF` `ee33ed00...`,
`USBCOMP.INF` `85a3ddb1...`; `COMTEST.EXE` `417dec9c...`.

### NUSB 3.3 EHCI: PASS, two ports

NUSB's hub does not split an IAD composite, so the whole device bound
`USBCOM.INF`'s "Raspberry Pi Pico SDK serial (whole device)" line:
"configuration 224 bytes", "port 0 is COM3", "port 1 is COM4", "FDO
start: 2 ports"; two "USB Serial Port" children installed from
`USBCOMP.INF`.

- `COMTEST COM3 full` and `COMTEST COM4 full`: PASS 58 checks each.
- The simulator log shows each port on its own function: line coding,
  line state, SERIAL_STATE (35/36/68 and 35/36/67 events), break and echo
  (94 bulk OUT transfers each) on function 0 and function 1 separately.
- `COMTEST COM3 hold 30` and `COMTEST COM4 hold 30` run together (about
  15 s overlap): PASS 6 checks each.

### xHCI98, after the `&MI_02` line (simcov 37b0b69): PASS, two ports

(Trial only: the line was reverted in cc0965f, Owner, 2026-10-23, because on stock single-CDC firmware `&MI_02` is the reset interface and would show Code 10; serial.md 13.)

Owner, 2026-10-23: `boards.ps1` adds `USB\VID_2E8A&PID_000A&MI_02`;
`USBCOM.INF` regenerated (`44167e56...5977` as staged by the trace build).
Under xHCI98 3.2.0.0 batch 8 (above), the device on a new root port: the
wizard found "Raspberry Pi Pico SDK serial, second port" on `A:\` for the
`&MI_02` child ("1 CDC-ACM functions", "comm if 2 data if 3", COM6), the
`&MI_00` child bound as before (COM7), and the reset interface's child
found no driver (as before). `COMTEST COM6 full` and `COMTEST COM7 full`:
PASS 58 each; both `COMTEST ... hold 30` started together from one batch
file (4 s apart): PASS 6 each.

### xHCI98 3.1.0.0, before the `&MI_02` line: one port of two

xHCI98 splits the composite: the `&MI_00` child (the first function's
IAD view, 75 bytes) bound `USBCOM.INF`'s Pico `&MI_00` line and became
COM5 ("1 CDC-ACM functions"); `COMTEST COM5 full` PASS 58. The second
function's child and the reset interface's child found no driver (the
wizard twice: "Windows was unable to locate a driver"), so the second port
does not exist under xHCI98. `USBCOM.INF` lists only `&MI_00` for
composites (`geninf.ps1`, `boardids.txt`); a dual-CDC board under a
splitting bus needs a `&MI_02` line too (USBCOM.SYS on that child would
see one function). Reported to the coordinator for the INF owner.

## usbmidi with the UM-ONE identity (`midiemu.py --profile umtab`)

`midiemu.py` (simcov 087cc87, 91fb2d5) gained the `umtab` profile
(0582:0168, the UM-ONE mk2's captured TAB descriptors), `quad`, `wide`
and `intr`, and the 64-bit ids qemu-xhci needs. Driver: `build.cmd wdmx86
checked` with `UMTRACE=1` (`usbmidi.sys` `c41b100f...`), the test INF
staged with `MIDITEST stage` as `oem0.inf`, `MIDITEST.EXE` `b1ea37bc...`.

- **NUSB 3.3 EHCI, High Speed: PASS.** "USB Composite Device" from
  `USB.INF`, then the MI_00 child matched "Roland UM-ONE mk2 (TAB)" (the
  KS file prompts skipped as in the earlier legs); WinMM lists IN 0 and
  OUT 0 "UM-ONE layout - SIMULATED"; `MIDITEST 0 0`: received 355 of 355,
  0 errors, cleanup complete. First time this identity moved MIDI bytes
  through the driver on 98 SE.
- **xHCI98 (3.1.0.0, and batch 8): not enumerated.** The device is read
  (device, configuration 9 and 101 bytes, strings 0 and 2, twice) and then
  nothing: no SET_CONFIGURATION, no child, no wizard. The `acms` profile
  (FFFF:0001, the shape that passed under xHCI98 3.1.0.0 on 2026-10-22,
  `2026-10-22-98se-legs.md`) does the same in this guest. With 98 SE's own
  `USBAUDIO.SYS` present the guest also froze in VMM code (the freeze
  that record describes for the Brio's audio function); with it renamed
  `.BAK` the guest stays responsive but the device still never gets a
  configuration. The IAD composites (`acmemu --profile dual`) and
  single-function devices in the same guest and controller
  (`qemu-xhci,p2=4,p3=4`) enumerate. Not settled: whether xHCI98's
  composite path for a class-0 device without an IAD, this guest's
  device-node history, or the usb-redir peer is at fault; reported to the
  coordinator for xHCI98.

### The xHCI98 non-configuration, rechecked on a fresh overlay: closed

Coordinator, 2026-10-23: chase it on a fresh overlay. Overlay `s2` of the
same base, NUSB's EHCI installed at first boot, batch 8's gated **qemu**
flavour copied read-only from integ8 (`objchk_qemu`: `xhci98.sys`
`64c008d1...`, `xhcihlpr.sys` `94dffd67...`, the hashes
`gated-x86-qemu.txt` lists; it traces to port 0xE9), `USBAUDIO.SYS`
renamed `.BAK`, the usbmidi test INF staged, restart.

- `umtab` on `xhci.0`, High Speed: xHCI98's trace shows the whole path:
  enable slot, address device, device descriptor 0582:0168, configuration
  101 bytes, "split device configured", "composite parent PDO,
  functions=1", "function PDO, MI=0, interface mask=3, class 01/01/00",
  "device enumerated". The New Hardware wizard then asked for the usbmidi
  disk (`A:\`, KS files skipped), usbmidi started, WinMM listed IN 0 / OUT
  0 "UM-ONE layout - SIMULATED", and `MIDITEST 0 0` passed: 355 of 355, 0
  errors, cleanup complete. **PASS under xHCI98.**
- The same ten control requests the simulator logged on `s1` appear here
  too: xHCI98 does configure the device (its configuration reaches the
  simulator as a usbredir SET_CONFIG message, which `midiemu.py` did not
  log until now; it does from this commit). So "no SET_CONFIGURATION" on
  `s1` was a misreading of the simulator log, not an xHCI98 refusal.
- What differed on `s1`: the guest froze in VMM code (EIP C000A881) during
  PnP processing, with 98 SE's `USBAUDIO.SYS` present and again with it
  renamed; on `s2`, with batch 8's free build and the `acms` profile,
  usbmidi started and the guest then froze the same way. This is the
  intermittent 98 SE freeze already recorded for MIDI and audio devices
  (`2026-10-22-usbmidi-m1.md`, "Intermittent guest hang", seen there
  without usbmidi too, and `2026-10-22-98se-legs.md`), not a configuration
  failure. Closed as an xHCI98 configuration bug; the freeze stays open
  as before.

Coordinator note: the C000A881 freeze right after usbmidi started fits
the first-WDM-audio-arrival-after-boot pattern that the uac2-rel and
brio-null work measured on 2026-10-23 (usb-wdm-free README known issue).
Here: on `s2` the `acms` freeze was that boot's first WDM audio arrival;
on `s1` the freeze came with the first MIDI device after the restart (the
xpad receiver legs before it are HID, not audio).

## usbmidi: four cables, two endpoint pairs, interrupt endpoints (NUSB EHCI)

Overlay `s2` (above), NUSB 3.3 EHCI, High Speed, usbmidi checked UMTRACE
build as above, one profile per boot (the 98 SE MIDI replug limitation).
`--pid` presents the profiles under the test INF's FFFF:0001/0002.

- **`quad` (one bulk pair, four named cables), `--pid 0002`: PASS.** WinMM
  lists IN/OUT "MIDI 4x4 SIM Port 1" .. "Port 4" (the external jacks'
  names); `MIDITEST n n` for n = 0..3: 355/355 each, cleanup complete;
  `MIDITEST 0 1`: nothing received (cable isolation).
- **`wide` (two bulk pairs 01/81 and 02/82, three cables each), `--pid
  0001`: PASS.** The first arrival after a boot froze the guest in VMM code
  right after usbmidi's start (the known pattern); after relaunching QEMU,
  six ports "MIDI 6x6 SIM MIDI 1" .. "MIDI 6"; `MIDITEST n n` for n =
  0..5: 355/355 each; `0 1` nothing; the simulator saw both pipes carry
  traffic (259 transfers on 01/81, 212 on 02/82).
- **`intr` (one cable on interrupt endpoints), `--pid 0002`: PASS.** The
  driver opened both pipes as interrupt (`cp 01 03 ...`, `cp 81 03 ...`);
  the first two boots froze at the arrival (the known pattern), the third
  did not: `MIDITEST 0 0` 355/355, 81 interrupt OUT and 81 interrupt IN
  packets in the simulator log.

Freezes in these legs: 4 of 7 first arrivals after a boot (wide 1 of 2,
intr 2 of 3, acms 1 of 1), none on a later arrival.
