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
