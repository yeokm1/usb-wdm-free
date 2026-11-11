# Open items moved from xHCI98's roadmap

The legs and bench items still open for the work that xHCI98's roadmap
tracked until 2026-11-08 and then moved here (owner, 2026-11-08). The
text below is the moved record, verbatim. Close an item here when a
record of this repository shows it run.

xHCI98's roadmap-usb4.md ticked 47.18, 47.19 and 47.27 (and the benches 45-E.13 and 45-E.14) with "the rest is tracked in usb-wdm-free's testing records" (owner's rule of 2026-11-07: usb-wdm-free's tasks are not xHCI98's roadmap's). What remains, copied as it stood in xHCI98 3.4.2.0 at ef2b58f5:

## 47.18 (was 45.25) Realtek ALC4080 through `uac2`

Done: usb-wdm-free `d14dab1` (branch `w47-wdm`): Linux master read again, one new quirk for `0DB0:A073` that skips a probe-time setup `uac2` never does, so nothing beyond the class is needed; a capture replayed on XP x64, three outputs and three inputs played and recorded (xHCI98 `runs/run-47.md`, "47.18").

Left: 98 SE under NUSB and xHCI98, 7 x64, two streams at once and the topology names on Vista and 7, the real board 45-E.13.

Run 2026-11-09 ([alc4080-left](2026-11-09-alc4080-left.md)): with the replay and 0.1.9, every output and input pair played and recorded, and two streams at once passed, on 98 SE under NUSB's EHCI and xHCI98 (under xHCI98 one of the two output-and-input cases did not overlap), and on 7 x64 and Vista x64 under xHCI98 and Windows' EHCI; Vista and 7 name the outputs Speakers, Headphones and Speakers (S/PDIF, by design) and the inputs Microphone and Line, and keep the analog connector input out of WinMM (an "Analog Connector" endpoint in state `0x10000001`, as Windows 10's own driver lists the same two inputs). Left: the real board 45-E.13.

Bench 45-E.13: the Realtek ALC4080 under `uac2` on a real board that carries one (45.25): playback and recording on each output and input, 98 SE, XP and 7 x64, from the asset of the release that carries 45.25.

## 47.19 (was 45.26) USB serial bridges in `usbcom`, 98 SE and ME

Scope: FTDI FT-X, FT232H and FT4232H, Prolific PL2303 (the old parts and the G series), Silicon Labs CP2105 and CP2108; legs under xHCI98 and NUSB's EHCI, each chip simulated or passed through. CH340 and CH341 dropped (owner, 2026-11-06: WCH's driver, in usb-wdm-nonfree, covers 98 SE to XP).

Done: usb-wdm-free `eba39b5` (branch `w47-wdm`): `usbcom`'s INF gate fixed (`build.cmd w98` had failed at `main`), no driver file changed; the makers' coverage per chip in xHCI98 `runs/run-47.md`, "47.19".

Done (2026-11-09, [usbcom-rest](2026-11-09-usbcom-rest.md)), on simulators: the chip legs on 98 SE under xHCI98 3.4.2.0, every part's `full` run passing every check (the 64 KiB echo included), the other ports' `quick` runs and the FT-X's pull, replug and restart passing; the 64 KiB IN loss under NUSB read again (FT-X 20 of 20, FT232H 19 of 20): at the one loss every byte was sent by the simulator, no answered packet was cancelled, and QEMU's trace has the leg's only "guest updated active qTD", which fits QEMU's EHCI model dropping the completion of bulk-IN packets already answered when its descriptor check fails (a supported explanation, not a proof); no fault found in `usbcom`, no driver change.

Left: the CP2108's fourth port (the harness lists COM1 to COM16 only) and the CP2105's repeated echoes (that leg was void); a packet-level reading in the emulator (QEMU's packet states and write-backs), owed to usb4-emu's QEMU; the same echo on a real EHCI controller with NUSB; ME under an EHCI stack (NUSB does not install on ME: a bench item); the real chips 45-E.14.

Bench 45-E.14: the USB serial bridges of 45.26 on real chips under `usbcom`, 98 SE and ME, from the asset of the release that carries 45.26.

## 47.27 CoreChips SR9700 and SR9800 USB 2.0 Ethernet

Done: usb-wdm-free `e496866` to `b141b47` (branch `w47-wdm4`): no maker's driver found; the SR9800 a second chip in `ax88772`, the new package `sr9700`, both built x86 and x64; read against simulators on 98 SE and ME, and on 2000, XP and 7 x86; the shared core's fixes (the 98 SE and ME unload race among them) in all four network packages; the owner-signed x64 images read on XP x64, and on 7 x64 once Microsoft's SHA-2 code-signing update (KB4490628, then KB4474419) is installed, refused without it (xHCI98 `runs/run-47.md`, "47.27").

Done (2026-11-09, [sr9700-sr9800-rest](2026-11-09-sr9700-sr9800-rest.md)), on simulators: the `0FE6:9702` composite (`sremu.py --composite`, `7c1142b`, `8fd3e92`) on XP, its Ethernet function bound by `sr9700f` under Windows' composite driver, DHCP, pings and TCP on the first plug, a replug and a restart; Vista x86 with the SR9700 and the SR9800 (also at 10 half), every step PASS; Vista x64 in the 0.1.9 NT matrix (both chips, both stacks, the images signed in the guest for that test; and with the owner-signed images of `3df15d0` under xHCI98); ME with the new exit: `sr9700f` in the 0.1.9 9x matrix (ME under xHCI98 and ME's own UHCI) and the SR9800 on ME under xHCI98 3.4.2.0 here; the SR9700 under 98 SE's own UHCI (no NUSB) at Full Speed, ping, TCP, unplug under traffic, replug, restart and a link drop.

Left: Vista x64 with the owner-signed images under Windows' own stack, the real SR9700 and SR9800 chips on the bench.

# Open after 0.1.9 (2026-11-08)

Items from the 0.1.9 matrices ([9x](2026-11-07-matrix-9x.md), [NT](2026-11-07-matrix-nt.md)) and the release's own records, added after the release (owner, 2026-11-08).

## 47.11 `uac2`: Windows' preferred device on a real device swap

Done: `SetupPreferredAudioDevices` 2 in both INF copies (design [uac2.md](../design/uac2.md) section 22); read on XP x64 with the real X4 and X1 and simulators, and on 98 SE under xHCI98 3.4.1.0 and NUSB's EHCI with simulators only ([uac2-swap](2026-11-06-uac2-swap.md)).

Left: the rest of the legs on a real device swap: 98 SE with real devices (one `uac2` device replacing another as the preferred one, and the update of the preferred device's driver), ME and 2000 (not read); on 98 SE under xHCI98 the recording choice that moved after the restart, cause not settled.

Read 2026-11-09 ([uac2-pref-98se](2026-11-09-uac2-pref-98se.md), simulators): 98 SE keeps the choice in force (`Playback`, `Record`) and the user's choice (`UserPlayback`, `UserRecord`); while the chosen device is absent at a start it puts another present device in force and keeps the user's choice, which comes back when the device is there. That is the likely cause of the recording choice read as moved after the restart, and it is 98 SE's own. On ME the same fallback and return were read, but the values written to that key did not hold ME's choice, and ME's panel was not used. Left: 98 SE and ME with real devices, the choice made in ME's Multimedia panel, 2000.

## `uvc98` on 98 SE: the matrix's 1784 FAILs, and Full Speed under 98 SE's own UHCI

Done: the matrix's `M1CAP` error 1784 on every 98 SE `uvc98` cell was the matrix's stale test tool, `M1CAP` `7ef711ff`, which predates the driver's RGB24 ranges and its KS_FRAME_INFO extended stream header: it connected to range 0 (now RGB24) and read with a bare stream header, and the read was refused before the camera started. With the current `M1CAP` `06e30623` the xHCI98 leg passed 8 of 8; `uvcm2.sys` is the 0.1.8 binary. The matrix's 98 SE cells rerun with `06e30623` (2026-11-08): under xHCI98 every `M1CAP` run passed, 12 of 12; under NUSB every High Speed cell of the matrix's table passed ([9x matrix](2026-11-07-matrix-9x.md), "`uvc98` on 98 SE with the current `M1CAP`").

Cause and fix 2026-11-09 ([uvc98-fs-uhci](2026-11-09-uvc98-fs-uhci.md), design [uvc.md](../design/uvc.md) 5.4a, `f9f6aed`): after a guest stall the UHCI stack refused every resubmitted transfer for its start frame (`USBD_STATUS_BAD_START_FRAME`) and the driver ended the stream after 8; now such a refusal resyncs the stream at explicit start frames. With the fix: 98 SE's own UHCI 5 of 5, NUSB's UHCI companion 5 of 5, ME's own UHCI 4 of 4. Left: the High Speed profiles under NUSB's EHCI and `uvc11-fs` under xHCI98 with the fix on a host that is not loaded (both failed on 2026-11-09, the 0.1.9 controls alike and the trace showing the new code idle; the loaded host is suspected), 2000 under its own UHCI.

Was left: under 98 SE's own UHCI at Full Speed (`uvc11-fs`), with `06e30623`, the third frame times out in 4 of 4 runs: READ 1 and 2 are good, READ 3 completes empty, and QEMU stops the isochronous stream 0.255 s after every start. The driver is unchanged since 0.1.8, which was never run on that stack. The rerun of 2026-11-08 met the same under NUSB, whose Full Speed camera sits on the UHCI companion: 5 of 5 runs failed, the stream stopped by QEMU after 4 to 9 frames, READ 3 empty (READ 2 in one run). Separately, once in 2 at 640x480 and 5 frames a second under NUSB's EHCI, READ 3 came back empty while the simulator kept streaming. Under xHCI98 the same Full Speed profile passed. Not settled whether the driver, the UHCI driver or QEMU's UHCI model is the cause.

## `usbmidi` on 98 SE under xHCI98, `multi`: one fatal exception (watch)

One VMM fatal exception 0E in 9 legs of the `multi` profile, during the first `MIDITEST` of matrix leg b; caller unknown. The matrix legs c, r4 and r5 and 5 reruns in a separate kit had none (1 in 7 before r4 and r5). `usbmidi.sys` is unchanged since 0.1.8 (only its INF changed in 0.1.9). Watch item: record any repeat with its screen and the leg.

2026-11-09 ([usbmidi-multi-watch](2026-11-09-usbmidi-multi-watch.md)): 6 more legs of the matrix's leg on a loaded host, 16 `MIDITEST` loopbacks, no fault: 1 in 15 legs. Still a watch item.

## `uac1`: the real Sound Blaster Play! 2 on 98 SE

Was left (until 2026-11-09): the real Creative Sound Blaster Play! 2 (`041E:323D`) not yet run with `uac1` on 98 SE; a simulator replaying its captured descriptors stands in ([uac1-98se](2026-11-07-uac1-98se.md), the 9x matrix).

Run 2026-11-09 ([uac1-play2-real](2026-11-09-uac1-play2-real.md)), the real device through QEMU `usb-host`: under xHCI98, with `isobsize=1` (QEMU's default 32 is refused by the host's WinUSB for isochronous OUT, so nothing had reached the device on 2026-11-07), install, binding, mixer, volume and mute, playback at 48 and 44.1 kHz and recording all passed with no transfer refused by the host. Under NUSB's UHCI companion no setting carried both directions through passthrough (QEMU's UHCI model with `usb-host` the likely limit). The owner's bench, 2026-11-09 (98 SE, xHCI98 3.4.2.0, a ThinkPad P14s Gen 1): playback clean (tones, left and right, sweeps, 48 and 44.1 kHz), volume and mute followed, recording from a microphone clean at 44,100 Hz 16-bit stereo (static at Sound Recorder's 22,050 Hz 8-bit default, taken by the owner for the format's). Unplugged and plugged in again with nothing playing (the same port and another): still the preferred device, playback clean. Left: the device under NUSB on a 98 SE machine with UHCI ports.

## `rtl815x` in the 9x matrix: the harness, not the driver

The 9x matrix's `rtl815x` FAILs (no frame sent) were the test setup. On 98 SE, Windows binds TCP/IP to the first five network adapters a guest makes (about nine on ME), and it makes an adapter per device id and root port, so the sixth new adapter has no protocol and sends nothing; the chip list was only the order the shapes ran. On a fresh guest the "failing" shapes pass with 0.1.9 (8157 at High Speed and 8159 at SuperSpeed under xHCI98, 8157 under NUSB's EHCI). Done (2026-11-09, [rtl815x-9x-harness](2026-11-09-rtl815x-9x-harness.md)): `rtlemu.py` joins a bulk-OUT transfer handed over in pieces (`ec8703a`); the matrix harness makes at most five new adapters per fresh overlay (id, controller and speed counted) and splits a driver's shapes into such jobs; rerun on fresh overlays, ME's own UHCI (the four chips and four rebrand ids at Full Speed) and 98 SE's NUSB Full Speed shapes (four chips) all passed ping, TCP, unplug under traffic, replug and restart, 12 of 12 shapes. Closed.

## Ethernet drivers: EEE and the Advanced tab, as the makers' INFs offer them

Owner, 2026-11-09. Each Ethernet driver's settings on its adapter's Advanced tab, matched chip by chip to what the chip maker's own INF offers: Realtek's for `rtl815x` (its `*EEE`, `EnableGreenEthernet`, `AdvancedEEE` and `EnableExtraPowerSaving`, each 0 by default, and its other keywords, such as speed and duplex and the network address), ASIX's for `ax88179` and `ax88772`; for `sr9700` and `cdcnet`, which no maker's INF covers, only what the chip or the class supports (for example `NetworkAddress`, design [ethernet.md](../design/ethernet.md) section 4.8). Today `rtl815x` keeps EEE on as Linux sets it, where Realtek's driver leaves it off (design [ethernet.md](../design/ethernet.md), "Kept as Linux sets them"), and no INF of these drivers has an Advanced-tab keyword.

Done (2026-11-09): the makers' INFs read per chip and system, and each package's keyword set with its defaults, in design [ethernet.md](../design/ethernet.md) section 4.10 (`6adff1a`); the keywords in every network INF (the 9x form and the NT form), read by the shared core at start and applied by each chip's code, `rtl815x`'s EEE, green Ethernet, Advanced EEE and idle power saving off by default as Realtek's 7 INF leaves them, `ax88179`'s EEE on as ASIX's 7 INF sets it (owner, 2026-11-09, design 12.3), `cdcnet`'s `NetworkAddress` sent to an NCM function as `SetNetAddress` (`13d83ee` to `9ca35a1`). Read with simulated chips ([advanced-tab](2026-11-09-advanced-tab.md)): `rtl815x` on 98 SE under xHCI98 (every keyword and value class, 17 of 17) and NUSB's EHCI, and its NT form on XP; `ax88179`, `ax88772` and `cdcnet` on 98 SE under xHCI98; the Advanced tab itself on 98 SE (a value changed in the dialog reached the driver key as text). A real AX88179 with EEE on and off, through QEMU on 98 SE under xHCI98 ([ax88179-eee-real](2026-11-09-ax88179-eee-real.md)): link, pings and TCP.

Left: `rtl815x` on 7 x64; the Advanced tab itself on an NT system, and the dialog reopened to show a changed value; `ax88179`, `ax88772` and `cdcnet` under NUSB's EHCI, on ME, 2000, XP and 7; the real chips with each setting (Realtek's above all), and whether EEE is negotiated (no driver reads the partner's EEE ability); with the real AX88179, the guest that stopped answering once after a hot replug (EEE on), cause not known.

## `uac2`, `uac1`: the names without "(experiment)" (owner, 2026-11-09)

Done 2026-11-09 (`8d7411c`, [uac-names](2026-11-09-uac-names.md)): `uac2`'s devices are "USB Audio 2.0 (uac2)" (" 2" to " 4" for later pairs) and its disk "usb-wdm-free uac2 driver"; `uac1`'s "USB Audio 1.0 (uac1)" and "usb-wdm-free uac1 driver"; the driver's fallback name the same, `infchk` checking both. Read on 98 SE (`uac2`, `uac1`), XP and 7 x64, with a device without a product string for the fallback. Left, at the release that carries it: the owner's signature on the rebuilt x64 `uac2wdm.sys` (`wxp-x64`, `wv-8-x64`), the version, and `free/` from the release process.

## `uac1` 0.1.1.0: the layouts 98 SE's own driver runs (xHCI98 48.1)

Done: design [uac2.md](../design/uac2.md) section 26.12, `uac1` 0.1.1.0 (usb-wdm-free `77da30c` to `294c891`): PCM8, IEEE float, A-law and mu-law converted to and from a 16-bit PCM pin, MaxPacketsOnly both ways, Ff fed to an adaptive source's synch endpoint. On 98 SE under xHCI98 with the simulator ([uac1-formats](2026-11-09-uac1-formats.md)) every profile was bound with a wave device and played or recorded the tone at the right frequency and level; the adaptive source's synch values matched the rate at Full and High Speed; `uac2`'s `hs24` played and recorded on the same base.

Known limit (owner, 2026-11-09): MaxPacketsOnly capture from an asynchronous source, and MaxPacketsOnly capture in PCM8, A-law or mu-law, are not carried; such a device loads under `uac1` with no sound device (no hang).

Under 98 SE's own UHCI (run as time allowed) the formats played and recorded too, but some streams stopped completing buffers and two devices were not listed by WinMM after a slow first plug, not tied to a format. Left: the legs under NUSB's EHCI (UHCI for Full Speed); the own-UHCI stalls and unlisted devices (cause: the driver, QEMU's UHCI model or the loaded host); failure injection on the synch pipe; unplug while streaming for the new profiles; the first A-law session that failed every step (not reproduced since; watch item); real devices of these layouts.

## `uac2`, `uac1` on 98 SE: the level offset and the stalls under 98 SE's own UHCI (xHCI98 48.1, owner 2026-11-09)

The level: every stream on 98 SE reached the device 5.5 dB below what `UACWAV` plays. Read 2026-11-09 ([uac-98se-level](2026-11-09-uac-98se-level.md)): 98 SE's own Wave line, which it adds to the mixer of the device under `uac2` and Windows' `usbaudio.sys` alike, stood at 32845 of 65535; with it at the top the device received -20.0 dBFS, at 48 and 44.1 kHz and in mono. Not the driver's: no change. A level criterion on 98 SE reads against the PCM control (owner) or sets the Wave line to the top first (`UACWAV srcvol wave 65535`).

The stalls: the driver's. A failed stream's counters under 98 SE's own UHCI read 250 data URBs in a row refused for their start frame (25.8's run limit); a trace read the stack's ASAP chain lagging the bus after a stall until it refuses. Fixed, design [uac2.md](../design/uac2.md) 26.13 (`744bc33` to `98270a5`): a resync at explicit start frames after a refusal or a URB placed behind the bus, and `uac1`'s synch endpoint passing over lost intervals ([uac-uhci-stalls](2026-11-09-uac-uhci-stalls.md)). Read on 98 SE's own UHCI in QEMU: `uac2` `fs` 43 of 48 streams before (every failure the driver ending the stream), 46 of 48 after (none ended by the driver); `uac1` `adsrc` recordings 1 of 8 (0.1.1.0) before, 8 of 8 after, `alaw` 16 of 16 after. Revised 2026-11-10 (26.13 items 4, 5 and 8, merged at `a27d075`: only a data URB that carried audio clears the resync limit's run and the lag test, so a recording's URB placed behind the bus and given back successful and empty starts a resync; frame requests the worker does not wait on) and read again on the same stack: `uac2` `fs` 48 of 48 streams as found and 48 of 48 with host stalls added, `uac1` `adsrc` 8 of 8 and 16 of 16, `alaw` 16 of 16 and 16 of 16, no stream ended at the driver; with stalls, 31 of 48 recordings resynced at a URB behind the bus with no data URB failed, the empty URB's rule at work.

Read 2026-11-10 under ME's and 2000's own UHCI and NUSB's UHCI on 98 SE (the same build, simulators and injector, [uac-uhci-stalls](2026-11-09-uac-uhci-stalls.md), "ME's, 2000's and NUSB's UHCI"): `uac2` `fs` on ME 69 of 72 streams (24 as found, 48 with stalls), on 2000 48 of 48, under NUSB 72 of 72; `uac1` under NUSB `adsrc` 16 of 16 as found and 8 of 8 with stalls, `alaw` 16 of 16 and 16 of 16; no stream ended at the driver, and with stalls the resyncs ran as on 98 SE (recordings at a URB behind the bus, mostly with no data URB failed). The 3 ME failures were 44.1 kHz recordings ME could not open (`open=8`), refused at the capture direction's one-stream gate (`Uac2M0Stage` 30, `STATUS_DEVICE_BUSY`) while ME's audio stack created and closed capture pins in quick succession; nothing reached the device. `uac1` is 98 SE only, so it has no ME or 2000 leg.

Left: the two failed recordings after the fix (one ran at the driver while the client got no buffer, one got almost no data from the emulated device; neither came back with the revision, as found or with stalls); ME's 44.1 kHz recording refused at the gate (cause by reading: the gate is freed only when a just-closed stream's destroy, queued to a work item, has run; fixed 2026-11-11 in design [uac2.md](../design/uac2.md) section 27, `uac2` build 29 and `uac1` build 5, host-tested; read 2026-11-11 under ME's own UHCI in QEMU with the simulator: before the fix 6 of 20 recordings at 44.1 kHz refused (53 of 87 pin opens refused), after it 20 of 20 (40 of 40 opens, 15 of them waiting for a closing stream, the last 14 ms), 6 of 6 at 48 kHz and 6 of 6 plays; 98 SE's round owed); NUSB on Windows 95 (no such image here); real USB 1.1 ports; `uac1`'s other profiles under this stack with the fix; one guest that stopped answering after a `uac2` play under the release on this stack (not seen again).
