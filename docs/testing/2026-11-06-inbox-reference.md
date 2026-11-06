# Simulators against Windows' own class drivers, 2026-11-06

xHCI98 Phase 47 (owner, 2026-11-06): every emulator model in this
repository is checked against a driver that was written without it. For
the device classes Windows serves itself, that driver is Windows' own
class driver. This record runs each class model under the in-box driver
of the systems named below, notes what the in-box driver asked that the
model answered wrongly or not at all, fixes the model where it diverged,
and reruns this repository's driver against each model that changed.
The models of the makers' drivers (ax88772, ax88179, rtl815x, sr9700,
usbcom's `chipemu.py`, xpad98 and absptr's `penpartner` profile) are in a
separate record.

These are development legs in QEMU (TCG), not "Tested".

## Rig

- Emulator: stock QEMU 11.1.0 (`qemu-system-x86_64.exe` SHA-256
  `47d57a6072e0bb3b...`), `usb-redir` to the model on 127.0.0.1, with the
  device's traffic captured by `usb-redir`'s `pcap=` option. 98 SE: the
  usb4-emu fork build `cb36497` (`3e7f6bf5...`), which the 98 SE legs of
  this repository use.
- Guests, overlays of the shared clean q35 bases, Windows' own USB stack
  only (no xHCI98):
  - XP SP3 x86: `usb-ehci` (Windows' `usbehci.sys`) and `piix3-usb-uhci`
    (`uhcd.sys`), 1 vCPU. Full Speed models on UHCI, High Speed on EHCI.
  - 7 SP1 x86: the same two controllers, 1 vCPU.
  - 10 22H2 x64 (build 19045): `qemu-xhci` with Windows' `usbxhci.sys`,
    2 vCPUs.
- Guest tools, all from this repository: `MIDITEST.EXE` (`miditest.c`,
  `ce03c4bc...`), `KEYTEST.EXE` (`keytest.c`, `2f184cdc...`),
  `PTRTEST.EXE` (`ptrtest.c`, `5955e645...`), and a PowerShell helper for
  7 and 10 (play: `Media.SoundPlayer` on three of Windows' own WAV files;
  record: WinMM MCI, 5 s at 48 kHz 16-bit stereo, then the peak and
  non-zero samples of the file; serial: `System.IO.Ports.SerialPort`,
  open, 9600 and 115200 Bd, 1000 bytes written and read back; network:
  `ipconfig`, `ping` to the model's host 192.168.77.1). Each writes its
  lines to the guest's COM1.
- The models are this branch's copies; `absemu.py` (from `w47-wdm2`) and
  `kbdemu.py` (from `w47-wdm5`) were run from the trial merge of the
  Phase 47 branches.
- Evidence: `E:\xhci98-vm\a347\ref2\ev\` (`<leg>.pcap` the USB traffic,
  `<leg>.jsonl` the model's own log, `<leg>-guest.txt` the guest's
  report), `shots\`, `pids.txt`. A host power cut on 2026-11-07 ended the
  first XP and 10 guests; every leg after it ran on fresh overlays, and
  the legs cut short were run again.

## Summary

| Model | Reference driver and OS | Result | Gaps found | Model changed | Our driver retested |
|---|---|---|---|---|---|
| `uacemu.py` (uac2) | `usbaudio2.sys`, Windows 10 22H2 x64 | `hs24`, `clocks`, `adaptive`, `multi` (play), `mic` (record), `x1`, `uac3dual` (configuration 1) PASS; `x4full` and `fs` FAIL (Code 10) before the fix, PASS after (`fs` with `--fs-feedback 4`); `cx31993m` and `alc4080cap` FAIL (Code 10), settled 2026-11-07: the unanswered HID interface's device resets during the audio start (below); PASS with it answered | the X4 layout's Mixer and Selector Units stalled the class requests Windows reads; a Full Speed feedback endpoint of 3 bytes is refused, Windows takes 4 bytes 16.16; `alc4080cap` names string 5 (iFunction, iInterface) and had none; the X4/X1/CX31993/ALC4080 layouts' HID (and the X4/X1's CDC) interfaces answer nothing (not modelled, no capture) | y (`e34bd8b`, `0de729a`, and string 5 of `alc4080cap`) | 98 SE (xHCI98) `x4full` PASS ("Retests") |
| `midiemu.py` (usbmidi) | `usbaudio.sys` MIDI, XP SP3 and 7 SP1 | XP: FAIL before the fix (the model stopped), PASS after: `MIDITEST 0 0` 355/355 twice. 7: 355/355 twice | XP pads each 64-byte bulk OUT packet with zero (reserved CIN 0) events; the model treated them as an error and closed | y (`e34bd8b`) | 98 SE (xHCI98) PASS, XP SP3 PASS ("Retests") |
| `absemu.py` (absptr; not `penpartner`) | XP SP3 `hidusb`/`mouhid`; 7 SP1 HID touch, multi-touch and pen | XP: `tablet`, `pointer`, `touchmouse` 6/6 and all buttons; `kvm16`, `kvm12` 1/6; `touch`, `mtouch`, `pen` nothing (XP has no digitizer support). 7: `tablet`, `touchmouse` 6/6, `pen` 6/6 hovering, `touch` and `mtouch` nothing. 10: `touch`, `mtouch`, `pen` 6/6 | 7 takes no touch from the `touch` and `mtouch` collections, whose X/Y carry no physical units; with Unit and Unit Exponent added (scratch copy) 7 takes them. XP's 1/6 is XP's: it drops absolute values with the field's top bit set | patch for `w47-wdm2` (not committed here) | owed with the patch (98 SE NUSB legs of `touch`, `mtouch`, `touchmouse`) |
| `kbdemu.py` (mediakeys) | XP SP3 `kbdhid` and `hidserv` consumer control | PASS: `twoifid`, `oneif`, `bitmap` - A, every consumer key as WM_APPCOMMAND and its VK, AL Calculator opens Calculator; `boot`: A only | none (the Keyboard page's Mute/Volume 7F-81 do nothing on XP, a Windows trait) | n | earlier legs stand |
| `ncmemu.py` (cdcnet NCM) | `UsbNcm.sys`, Windows 10 22H2 | not run: Code 28 | Windows 10 19041's `usbncm.inf` has its model line commented out (`; %UsbNcm.DeviceDesc%=UsbNcm_Device, USB\Class_02&SubClass_0d&Prot_00`), so no class match; Microsoft lists `UsbNcm.sys` for Windows 11 and Server 2022 only. No target Windows has an in-box ECM driver (Microsoft's class-driver list has no ECM row). The model has no RNDIS function. | n | earlier legs stand |
| `acmemu.py` (usbcom) | `usbser.sys`, Windows 10 22H2 (class match) and 7 SP1 (by `USBCDC.INF`) | 10: `uno` and `pico` PASS (open, 9600 and 115200 Bd, 1000-byte loopback). 7: `uno` PASS, the same | `pico`: the reset interface (vendor class, no endpoints) is Code 28 on 10: the model has no BOS / Microsoft OS 2.0 descriptors, which pico-sdk's reset interface carries for WinUSB (option, not changed) | n | earlier legs stand |
| `ufiemu.py` (usbflop) | `usbstor.sys` UFI, XP SP3 (as `03EE:6901`, which XP's own `usbstor.inf` lists) | PASS: read (3 files, `PATTERN.BIN` copied), full format (160 FORMAT UNIT, no errors), copy back, `fc /b` no differences | none | n | earlier legs stand |
| `uvcsim.py` (uvc98) | `usbvideo.sys`, XP SP3 and 10 22H2 (7 has no capture tool) | XP: `--uvc 1.0` PASS, preview in My Computer's camera view (707 frames); the default 1.1 is Code 10 on XP. 10: 1.1 PASS, Camera app preview (1297 frames) | none in the model: XP's driver takes UVC 1.0 only | n | earlier legs stand |
| `uvcemu.py` (uvc98) | `usbvideo.sys`, 7 SP1 | `usbvideo` binds, no problem code; no stream (7 has no capture tool); XP and 10 not run (bulk UVC 1.1 at Full Speed: XP takes UVC 1.0 only; the model lacks the usbredir capabilities QEMU needs on xHCI) | none found | n | earlier legs stand |

No `usbccid` model exists on any Phase 47 branch (only
`docs/design/ccid.md` on `w47-wdm2`), so nothing was run against
Windows' CCID driver.

## Details

### uac2: `uacemu.py` under `usbaudio2.sys` (Windows 10 22H2 x64)

`qemu-xhci` port 1 (High Speed, or Full Speed for `fs`); play and record
by the helper. Each profile's model log records every class request and
its status; "stall" below is the model's STALL answer.

| Profile | Before the fix | After (`0de729a`) | Capture |
|---|---|---|---|
| `hs24` | PASS: play (alternate 3, 32-bit, 48 kHz, 2346 feedback packets, no zero), record (peak 16385). Requests: clock GET RANGE, SET CUR 48 kHz; Feature Unit 2 and 5 mute/volume GET CUR and RANGE; all answered | - | `w10-uac-hs24` |
| `clocks` | PASS: clock selector 0x12 GET CUR, multiplier 0x13 numerator/denominator GET CUR, SET CUR on sources 0x10 and 0x11; play, record | - | `w10-uac-clocks` |
| `x4full` | FAIL, Code 10 on the audio function: GET CUR of Mixer Unit 7 (`wValue 0100`) and of Selector Unit 26 stalled | PASS: play, record; Mixer Unit 7 crosspoints read 0 dB, Selector Unit 26 read and set (5 SET CURs); one request still stalls, Feature Unit 2's Automatic Gain Control GET CUR, which the descriptor does not declare (correct) | `w10-uac-x4full`, `w10-uac-x4full-fix` |
| `fs` | FAIL, Code 10; System log `usbaudio2` event 37: "The driver could not find a feedback endpoint for an asynchronous data OUT endpoint" | default (3-byte feedback, USB 2.0 5.12.4.2): still Code 10, as Windows requires; `--fs-feedback 4`: PASS, play and record | `w10-uac-fs`, `w10-uac-fs3-fix`, `w10-uac-fs4-fix` |
| `adaptive`, `multi`, `mic`, `x1`, `uac3dual` | PASS (`multi` has no capture, `mic` no playback; `uac3dual`: Windows selected configuration 1, the UAC 2.0 one). `x1`'s HID interface Code 10, its CDC pair bound by `usbser.sys` with its requests stalled, as `x4full`'s | - | `w10-uac-<profile>` |
| `cx31993m` | FAIL, Code 10; System log `usbaudio2` event 16 (a system call failed; status `C0000001`, on a replug `C0000120`). In the first run one volume GET RANGE (channel 2) was answered with a stall in the middle of a device reset Windows started; its HID interface (Code 10) is not modelled. Cause not settled | not changed | `w10-uac-cx31993m`, `w10-uac-cxm-ev` |
| `alc4080cap` | FAIL, Code 10, event 16 (`C0000001`); string 5 (the IAD's iFunction and every interface's iInterface) stalled | string 5 answered: still Code 10 with no class request at all, so the descriptors themselves are refused. Not settled; the capture has a clock source per terminal and no selector, and Microsoft's page says the driver supports one clock source | `w10-uac-alc4080cap`, `w10-uac-alc4080cap-fix` |

Full Speed feedback, read with a +15000 ppm feedback offset (48.72
frames per frame): with a 4-byte endpoint carrying a 3-byte 10.14 value
Windows sent 48 frames in every packet (the value ignored); with 16.16 in
4 bytes it sent 49 frames in 1610 of 2318 packets (the value honoured).
So `usbaudio2.sys` at Full Speed takes only a 4-byte feedback endpoint and
reads it as 16.16 (`w10-uac-fs16ppm` on the scratch variant, and
`w10-uac-fs4-fix` on the committed model).

The X4 layout (`x4full`) also has a HID interface and a CDC ACM pair. The
model answers neither (the capture holds no HID report descriptor):
Windows' HID driver stops that interface with Code 10 (GET_DESCRIPTOR of
the report descriptor stalls) and `usbser.sys` binds the CDC pair but its
line-coding requests stall. Not changed: there is nothing captured to
answer with.

#### `cx31993m` and `alc4080cap`: the HID interface (2026-11-07)

Both layouts have a HID interface beside the audio function, which the
model did not answer. In every log of the two profiles above, Windows'
HID driver sent SET_IDLE and GET_DESCRIPTOR(Report) to that interface,
both stalled, and the HID driver then reset the whole device: a new
SET_CONFIGURATION, a CLEAR_FEATURE(ENDPOINT_HALT) of the HID interrupt
endpoint and the report descriptor again, three times, about 100 ms
apart. Where `usbaudio2.sys` had begun starting the audio function
before those resets (`cx31993m`: one volume GET RANGE answered, the next
stalled because the reset had cleared the configuration; `alc4080cap`:
string 5 read, then nothing), the function failed with Code 10 and event
16; where the resets came first (`x4full`, `x1`), it started. So the
Code 10 is the order of two drivers' starts, not the audio descriptors.

The model now answers a captured layout's HID interface (`x4`, `x4full`,
`x1`, `cx31993m`, `alc4080cap`): SET_IDLE, and GET_DESCRIPTOR(Report)
with a stand-in report descriptor (a vendor-page collection with one
input byte, padded with Usage items to the length the HID descriptor
declares); it sends no reports. The CDC pairs stay unanswered. Read on
the same Windows 10 22H2 x64 base (fresh overlay, stock QEMU 11.1.0,
`qemu-xhci`):

| Profile | Model | Result |
|---|---|---|
| `cx31993m` | `66d99ee` (HID unanswered) | this time PASS: the HID resets came before the audio start; HID interface Code 10 (`w10-cxm-ref2`) |
| `cx31993m` | HID answered | PASS: audio and HID started (HID "vendor-defined device"), one SET_CONFIGURATION, no reset; the three system sounds played (`w10-cxm-hid`) |
| `alc4080cap` | `66d99ee` | FAIL, Code 10 on the audio function and the HID interface, as before (`w10-alc-ref2`) |
| `alc4080cap` | HID answered | PASS: three waveOut and two waveIn devices, the sounds played, the recording carried the model's tone (peak 16385) (`w10-alc-hid`) |

`cx31993m`'s Output Terminal declares its Connector control readable and
Windows reads it (GET CUR, 6 bytes); the model stalls it, and the
function starts all the same. Not changed.

### usbmidi: `midiemu.py` under `usbaudio.sys` (XP SP3, 7 SP1)

`acms` profile, Full Speed on UHCI. XP: before the fix the first loopback
failed (WinMM error 6) and the device vanished: XP's driver sends each
note in a 64-byte bulk OUT packet, the event followed by fifteen zero
events (`09 90 00 01` then 60 zero bytes), and the model stopped on the
first zero event (`xp-midi-acms`). After the fix: 355/355 twice; 2898 of
the 3264 events in 204 packets were zero padding (`xp-midi-acms2`). 7: 355/355
twice (`MIDITEST 0 1`, the GS synth being output 0), no padding
(`w7-midi-acms`).

### absptr: `absemu.py` (XP SP3, 7 SP1, 10 22H2)

`PTRTEST.EXE` reads the cursor; the leg moves the device to six points
(`absleg.py`, as in `2026-11-06-absptr.md` on `w47-wdm2`). A digitizer's
tip is not seen as a held left button by `GetAsyncKeyState` on 7 or 10
(press-and-hold), so the button rows count only for the mouse profiles.

| Profile | XP SP3 | 7 SP1 | 10 22H2 |
|---|---|---|---|
| `tablet` | 6/6, three buttons (`xp-abs-tablet`) | 6/6, three buttons (`w7-abs-tablet`) | - |
| `pointer` | 6/6, buttons (`xp-abs3-pointer`) | - | - |
| `kvm16` (0..65535) | 1/6: points above 32767 land at 0; buttons (`xp-abs3-kvm16`) | - | - |
| `kvm12` (12-bit) | 1/6: points above 2047 land at 0; buttons (`xp-abs3-kvm12`) | - | - |
| `touchmouse` | 6/6 through the mouse collection, buttons (`xp-abs3-touchmouse`) | 6/6, buttons (`w7-abs-touchmouse`) | - |
| `touch` | nothing (`xp-abs3-touch`) | nothing (`w7-abs-touch`); with X/Y units (scratch variant) every point after the first (`w7-absu-touch`) | 6/6 positions (`w10-abs2-touch`) |
| `mtouch` | nothing | nothing (`w7-abs-mtouch`); with X/Y units every point after the first (`w7-absu-mtouch`) | 6/6 positions (`w10-abs2-mtouch`) |
| `pen` | nothing | 6/6 hovering (`w7-abs-pen`) | 6/6 hovering (`w10-abs2-pen`) |

On 7 the touch collection of `touch` and `mtouch` installs as a plain
"HID-compliant device" (as the pen's does; 7's Tablet PC Input Service
reads them directly) and the service was running; no request stalled.
Windows 7 took no touch at all from either profile, and every point (but
the first, which only woke the stack) once the finger collection's X and
Y carried Unit (SI linear, centimetres) and Unit Exponent -2 (the HID
1.11 global items, 6.2.2.7), read with a scratch copy of the model
(`w7-absu-touch`, `w7-absu-mtouch`; `touchmouse` stayed 6/6,
`w7-absu-touchmouse`). The units rest on this reading alone; no published
Windows 7 requirement for them was found. The pen profile has no units
and works on 7. Windows 10 takes both forms. The model change is
offered as a patch for `w47-wdm2` (see "Patches").

The `kvm16` and `kvm12` points are XP's: their descriptors are well
formed (`kvm16` Logical Maximum 65535 in a 4-byte item), and the reports
match them; XP treats a value with the field's top bit set as negative.

### mediakeys: `kbdemu.py` (XP SP3)

`KEYTEST.EXE 80`; the leg sends the letter A, Volume Up, Volume Down,
Mute twice, Play/Pause, Next, Previous, Stop, the Keyboard page's Volume
Up, Volume Down and Mute, then AL Calculator. `twoifid`, `oneif` and
`bitmap`: A as WM_KEYDOWN/UP VK 41; each consumer key a WM_APPCOMMAND
(10, 9, 8, 8, 14, 11, 12, 13) and its VK (AF, AE, AD, AD, B3, B0, B1,
B2); AL Calculator a WM_APPCOMMAND 18 and Calculator in the foreground.
`boot`: A only. Requests: SET_IDLE 0 per interface, SET_REPORT of the LED
output report, all answered (`xp-kbd-*`).

### cdcnet: `ncmemu.py`

Windows 10 22H2 left the device at Code 28 with only its descriptors read
(`w10-ncm`). Its `usbncm.inf` (`DriverVer 10.0.19041.1`) is present but its
only model line is a comment, so Windows 10 has no class match for CDC
NCM; Microsoft's list of in-box class drivers names `UsbNcm.sys` for
Windows 11 and Windows Server 2022. No q35 base of Windows 11 exists, so
the NCM reference is owed. Microsoft's list has no CDC ECM driver for any
Windows; RNDIS (`rndismp.sys`) is listed, but `ncmemu.py` has no RNDIS
function, so no RNDIS reference applies to it.

### usbcom: `acmemu.py` (`usbser.sys`)

| System | Binding | Result | Capture |
|---|---|---|---|
| 10 22H2 | `usbser.inf` class match (`uno` whole device; `pico` function 0 through `usbccgp`) | `uno`: COM4, 9600 and 115200 Bd, 1000/1000 bytes back each; `pico`: COM5, the same | `w10-acm-uno`, `w10-acm-pico` |
| 7 SP1 | `USBCDC.INF` (this repository's INF for Windows' own `usbser.sys`, staged with `pnputil`) | `uno`: "Arduino Uno R3 (USB serial) (COM3)", the same loopbacks PASS | `w7-acm-uno` |

Requests: SET_LINE_CODING, GET_LINE_CODING, SET_CONTROL_LINE_STATE (0, 2,
3), all answered. The `pico` profile's reset interface (vendor class, no
endpoints) is Code 28 on 10: the pico-sdk this shape follows gives that
interface Microsoft OS 2.0 descriptors (a BOS and a WinUSB compatible id)
so that Windows binds WinUSB; the model carries the shape only.

### usbflop: `ufiemu.py` (XP SP3 `usbstor.sys`)

XP's own `usbstor.inf` lists the Mitsumi `03EE:6901` and has no class
match for UFI (the reason `usbflop` exists), so the model ran as
`--vid 03ee --pid 6901` ("Mitsumi USB Floppy", then "SIMULATD UFI FLOPPY
SIM USB Device"). As FFFF:0020 the Found New Hardware wizard found no
driver (`xp-ufi`). Commands seen (`xp-ufi-mit`): INQUIRY, TEST UNIT READY,
READ FORMAT CAPACITIES, READ CAPACITY, MODE SENSE(10) page 3F, READ(10),
WRITE(10), FORMAT UNIT (160, one per track and head), none failed.
`dir`, `type CRC.TXT`, `copy PATTERN.BIN`, `format A: /V:REF` (full),
copy back, `fc /b`: no differences.

### uvc98: `uvcsim.py` and `uvcemu.py` (`usbvideo.sys`)

| System | Model | Result | Capture |
|---|---|---|---|
| XP SP3, EHCI | `uvcsim.py` (UVC 1.1, High Speed) | Code 10, no class request: XP's driver takes UVC 1.0 | `xp-uvcsim` |
| XP SP3, EHCI | `uvcsim.py --uvc 1.0` | "USB Video Device" under Scanners and Cameras; opening it previews the bars (`shots\xp-uvc4.png`); probe, commit, 707 frames | `xp-uvcsim10` |
| 10 22H2 | `uvcsim.py` (UVC 1.1) | Camera app previews the bars (`shots\w10-cam1.png`); 1297 frames | `w10-uvc11` |
| 7 SP1, UHCI | `uvcemu.py` (bulk, UVC 1.1, Full Speed) | `usbvideo` bound, no problem code; no stream (7 has no capture tool) | `w7-uvcemu` |

All class requests answered (processing unit brightness GET CUR/MIN/MAX/
RES/INFO/DEF, probe and commit).

## Retests of this repository's drivers against the changed models

| Model change | System, host controller | Driver | Result | Capture |
|---|---|---|---|---|
| `midiemu.py` (reserved events skipped) | 98 SE, xHCI98 3.4.1.0 (`qemu-xhci`), cold start after the install | `usbmidi.sys` `14437317...` (as `free/`), test INF with the simulator ids (`mkinf.ps1` test folder; the released INF is by hand only), wizard from `D:\MIDIT`, the file copied by hand as in the earlier legs | `MIDITEST 0 0` 355/355 twice; the driver sent no reserved event | `w98-midi2` |
| the same | XP SP3, Windows' UHCI | the same driver and test INF, `MIDITEST install` (unsigned prompt answered) | 355/355 twice; no reserved event | `xp-midi-ours2` |
| `uacemu.py` (Mixer and Selector Units answered) | 98 SE, xHCI98 3.4.1.0 | `uac2wdm.sys` 0.1.8.26 as released (`d6773576...`, installed on the guest) | `x4full`: `UACWAV play 2 16 48000 5` 8/8 buffers, `rec 2 16 48000 3` 8/8 with the 1 kHz tone (999 Hz); the driver read neither unit (no request to entity 7 or 26) | `w98-x4full` |
| `uacemu.py` (`fs`) | - | - | not rerun: the default is the 3-byte form the earlier legs used; the 4-byte form is new and only an option | - |
| `uacemu.py` (`alc4080cap` string 5) | - | - | not rerun: the model now answers one string request it stalled before; `uac2` names a device by its product string (index 1) | - |

The 98 SE guest is an overlay of the `w47-wdm` worker's 98 SE guest (98 SE
with NUSB 3.3's EHCI, `USBHUB.SYS`, xHCI98 3.4.1.0 and `uac2`). The same
retests under NUSB's EHCI were not run (owed): the MIDI function under
NUSB needs its own composite install and restart.

Why the changes cannot touch the earlier results: the MIDI model now
skips only events it used to refuse, and a refusal ended the model, so no
earlier passing leg sent one (confirmed above: zero reserved events from
`usbmidi` on 98 SE and XP). The audio model answers only requests it used
to stall, to two units that `uac2` does not address.

## Patches for other Phase 47 branches

- `absemu.py` on `w47-wdm2` (tip `cc1555e`): X/Y units on the touch
  profiles' finger and contact collections (Unit SI linear centimetres,
  Unit Exponent -2, reset after the Input item), for Windows 7's touch
  input. Patch: `E:\xhci98-scratch\w47\ref2\patch\absemu-units.diff`
  (applies to `cc1555e`). Read on 7 with the same change in a scratch copy
  of the model (`touch` and `mtouch` take every point after the first;
  `w7-absu-*`). Applying it calls for absptr's simulator legs of
  `touch`, `mtouch` and `touchmouse` (98 SE under NUSB's EHCI, design
  record section 12) to be run again.
- `kbdemu.py` on `w47-wdm5`: no change.

## Options (not changed here)

1. `uac2` at Full Speed takes only a 3-byte feedback packet
   (`uacusb.c`, `FeedbackBytes`; `uacrate.c`, `UacRateFeedback`).
   A Full Speed UAC 2.0 device that works with Windows 10's own driver
   has a 4-byte feedback endpoint carrying 16.16; under `uac2` every
   such packet would be refused, and with no usable feedback for 1 s
   after the start the stream fails with STATUS_IO_TIMEOUT
   (`uacusb.c`, `UacFeedbackLate`): read from the code, not run
   (`uacemu.py --profile fs --fs-feedback 4` reproduces the device).
   A driver change would take a 4-byte Full Speed packet as 16.16 (and a
   3-byte one as 10.14, as now).
2. `acmemu.py --profile pico`: add the reset interface's Microsoft OS
   2.0 descriptors (BOS, the descriptor set by vendor request, a WinUSB
   compatible id) so that Windows 10 binds WinUSB to it as it does for a
   pico-sdk device.
3. The X4/X1 layouts' HID and CDC interfaces stay unanswered until a
   capture of the X4's HID report descriptor and CDC behaviour exists.
