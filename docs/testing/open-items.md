# Open items moved from xHCI98's roadmap

The legs and bench items still open for the work that xHCI98's roadmap
tracked until 2026-11-08 and then moved here (owner, 2026-11-08). The
text below is the moved record, verbatim. Close an item here when a
record of this repository shows it run.

xHCI98's roadmap-usb4.md ticked 47.18, 47.19 and 47.27 (and the benches 45-E.13 and 45-E.14) with "the rest is tracked in usb-wdm-free's testing records" (owner's rule of 2026-11-07: usb-wdm-free's tasks are not xHCI98's roadmap's). What remains, copied as it stood in xHCI98 3.4.2.0 at ef2b58f5:

## 47.18 (was 45.25) Realtek ALC4080 through `uac2`

Done: usb-wdm-free `d14dab1` (branch `w47-wdm`): Linux master read again, one new quirk for `0DB0:A073` that skips a probe-time setup `uac2` never does, so nothing beyond the class is needed; a capture replayed on XP x64, three outputs and three inputs played and recorded (xHCI98 `runs/run-47.md`, "47.18").

Left: 98 SE under NUSB and xHCI98, 7 x64, two streams at once and the topology names on Vista and 7, the real board 45-E.13.

Bench 45-E.13: the Realtek ALC4080 under `uac2` on a real board that carries one (45.25): playback and recording on each output and input, 98 SE, XP and 7 x64, from the asset of the release that carries 45.25.

## 47.19 (was 45.26) USB serial bridges in `usbcom`, 98 SE and ME

Scope: FTDI FT-X, FT232H and FT4232H, Prolific PL2303 (the old parts and the G series), Silicon Labs CP2105 and CP2108; legs under xHCI98 and NUSB's EHCI, each chip simulated or passed through. CH340 and CH341 dropped (owner, 2026-11-06: WCH's driver, in usb-wdm-nonfree, covers 98 SE to XP).

Done: usb-wdm-free `eba39b5` (branch `w47-wdm`): `usbcom`'s INF gate fixed (`build.cmd w98` had failed at `main`), no driver file changed; the makers' coverage per chip in xHCI98 `runs/run-47.md`, "47.19".

Left: the chip legs on xHCI98 `3.4.1.0` (the 2026-10-29 legs stand), ME under an EHCI stack, the 64 KiB IN loss under NUSB, the real chips 45-E.14.

Bench 45-E.14: the USB serial bridges of 45.26 on real chips under `usbcom`, 98 SE and ME, from the asset of the release that carries 45.26.

## 47.27 CoreChips SR9700 and SR9800 USB 2.0 Ethernet

Done: usb-wdm-free `e496866` to `b141b47` (branch `w47-wdm4`): no maker's driver found; the SR9800 a second chip in `ax88772`, the new package `sr9700`, both built x86 and x64; read against simulators on 98 SE and ME, and on 2000, XP and 7 x86; the shared core's fixes (the 98 SE and ME unload race among them) in all four network packages; the owner-signed x64 images read on XP x64, and on 7 x64 once Microsoft's SHA-2 code-signing update (KB4490628, then KB4474419) is installed, refused without it (xHCI98 `runs/run-47.md`, "47.27").

Left: Vista x64 (no SHA-2 update listed for it), ME with the new exit, Vista, the `0FE6:9702` composite, the SR9700 under 98 SE's own USB 1.1 stack, the real SR9700 and SR9800 chips on the bench.

# Open after 0.1.9 (2026-11-08)

Items from the 0.1.9 matrices ([9x](2026-11-07-matrix-9x.md), [NT](2026-11-07-matrix-nt.md)) and the release's own records, added after the release (owner, 2026-11-08).

## 47.11 `uac2`: Windows' preferred device on a real device swap

Done: `SetupPreferredAudioDevices` 2 in both INF copies (design [uac2.md](../design/uac2.md) section 22); read on XP x64 with the real X4 and X1 and simulators, and on 98 SE under xHCI98 3.4.1.0 and NUSB's EHCI with simulators only ([uac2-swap](2026-11-06-uac2-swap.md)).

Left: the rest of the legs on a real device swap: 98 SE with real devices (one `uac2` device replacing another as the preferred one, and the update of the preferred device's driver), ME and 2000 (not read); on 98 SE under xHCI98 the recording choice that moved after the restart, cause not settled.

## `uvc98` on 98 SE: the matrix's 1784 FAILs, and Full Speed under 98 SE's own UHCI

Done: the matrix's `M1CAP` error 1784 on every 98 SE `uvc98` cell was the matrix's stale test tool, `M1CAP` `7ef711ff`, which predates the driver's RGB24 ranges and its KS_FRAME_INFO extended stream header: it connected to range 0 (now RGB24) and read with a bare stream header, and the read was refused before the camera started. With the current `M1CAP` `06e30623` the xHCI98 leg passed 8 of 8; `uvcm2.sys` is the 0.1.8 binary. The matrix's 98 SE cells rerun with `06e30623` (2026-11-08): under xHCI98 every `M1CAP` run passed, 12 of 12; under NUSB every High Speed cell of the matrix's table passed ([9x matrix](2026-11-07-matrix-9x.md), "`uvc98` on 98 SE with the current `M1CAP`").

Left: under 98 SE's own UHCI at Full Speed (`uvc11-fs`), with `06e30623`, the third frame times out in 4 of 4 runs: READ 1 and 2 are good, READ 3 completes empty, and QEMU stops the isochronous stream 0.255 s after every start. The driver is unchanged since 0.1.8, which was never run on that stack. The rerun of 2026-11-08 met the same under NUSB, whose Full Speed camera sits on the UHCI companion: 5 of 5 runs failed, the stream stopped by QEMU after 4 to 9 frames, READ 3 empty (READ 2 in one run). Separately, once in 2 at 640x480 and 5 frames a second under NUSB's EHCI, READ 3 came back empty while the simulator kept streaming. Under xHCI98 the same Full Speed profile passed. Not settled whether the driver, the UHCI driver or QEMU's UHCI model is the cause.

## `usbmidi` on 98 SE under xHCI98, `multi`: one fatal exception (watch)

One VMM fatal exception 0E in 9 legs of the `multi` profile, during the first `MIDITEST` of matrix leg b; caller unknown. The matrix legs c, r4 and r5 and 5 reruns in a separate kit had none (1 in 7 before r4 and r5). `usbmidi.sys` is unchanged since 0.1.8 (only its INF changed in 0.1.9). Watch item: record any repeat with its screen and the leg.

## `uac1`: the real Sound Blaster Play! 2 on 98 SE

Left: the real Creative Sound Blaster Play! 2 (`041E:323D`) not yet run with `uac1` on 98 SE; a simulator replaying its captured descriptors stands in ([uac1-98se](2026-11-07-uac1-98se.md), the 9x matrix).
