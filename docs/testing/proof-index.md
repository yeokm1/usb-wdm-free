# Device screenshot evidence index

Inventory date: 2026-10-21. These are existing development-run screenshots,
not new tests, release certification or updates to the author-tested table.
**Windows 98 SE functional read proof is now recorded for the physical
Mitsumi floppy only**, after the user inserted media. The earlier 98 floppy
binding and CamCheck no-device UI images alone do not satisfy that
requirement. A successful load, open or descriptor query is described
as such; it is not promoted to a data-transfer pass.

Copies linked below are ignored local artifacts in `out/proof/`. They are
unchanged images, with original absolute paths, original filesystem UTC
timestamps, run-record dates and SHA256 values in
[manifest.json](../../out/proof/manifest.json). All 19 copies were checked
byte-for-byte by SHA256 against their originals. The run records establish
the OS, device and driver route; a screenshot alone may not show all three.
Guest taskbar clocks are not evidence of the run date. Ignored artifacts
will not accompany a Git clone unless separately preserved.

## Physical devices

| Package / device | Exact OS and USB path | Project/native route | Existing screenshot and demonstrated scope | Windows 98 SE functional proof / remaining limit |
|---|---|---|---|---|
| `uac2` / Creative X4, 041E:3278 | Windows 7 x86; physical X4 through QEMU EHCI, host libusbK | Project UAC2 v3 | [Playback](../../out/proof/x4/play.png): 8/8 buffers, 96000 samples, close 0 after restart/reopen. [Run](2026-10-21-uac2-vm.md). | **Missing.** Later raw diagnostics exposed folded errors hidden by the old library: one later WinMM pass still had24 raw OUT errors and24 empty-ring losses. Larger-ring repeats removed queue overflow but still failed timing/raw-error checks. Audible fidelity, capture and full UAC2 features remain unverified. Screenshot establishes buffer completion, not sound quality. Each later run retains its own artifact identity. |
| `usbmidi` / Roland UM-ONE mk2 TAB, 0582:0168 | Windows 7 x86; physical device through QEMU UHCI | Project USB MIDI v4, installed own SYS hash verified | [Startup/open](../../out/proof/umone2/open.png): problem 0 and two WinMM input/output open-close passes. [Run](2026-10-21-umone2.md). | **Missing.** No DIN cable loopback or physical MIDI byte transfer; vendor-mode 0582:012A is a separate untested startup case. This is open-only evidence. |
| `usbmidi` / Roland UM-ONE mk2 TAB, 0582:0168 — native 98 negative | Windows 98 SE; physical device through QEMU | Native USB audio installation from the Windows 98 CD; **no project MIDI driver installed** | [Fatal exception](../../out/proof/umone2/98fatal.png): 0E at 0028:C0014FD0 during native installation. [Run](2026-10-21-umone2.md). | **Functional proof missing.** Negative reference result; cause undetermined and not attributed to project code. Device detached and owned guest closed. |
| `usbflop` / Mitsumi, 03EE:6901 | Windows 98 SE + NUSB 3.3; physical drive through QEMU UHCI | Project CBI INF binding Windows storage drivers | **[98 physical read PASS](../../out/proof/mitsumi/98read.png)**: three complete files, 133557 bytes, zero failures, CRC32 matches earlier reads. [Actual parent](../../out/proof/mitsumi/98bind.png), [provider](../../out/proof/mitsumi/98driver.png). [Media run](2026-10-21-usbflop-media.md). Earlier [parent](../../out/proof/mitsumi/98load.png)/[child](../../out/proof/mitsumi/98child.png) are load-only. | **Present for these three file reads only.** No disk-wide integrity, write or format claim. A prior longer read was cancelled by operator USB detach; its partial error is preserved separately. Native versus project controls are distinguished in the media run. Other OS [98/XP/7](2026-10-21-usbflop-nt-followup.md), [ME](2026-10-21-usbflop-more.md), [2000](2026-10-21-usbflop-vm.md) runs predate inserted media. |
| `usbcdc` / X4 MI01 serial function | Windows XP x86; physical X4 through QEMU | Project INF binding native `usbser` | [Open report](../../out/proof/x4cdc/xpopen.png): COM3 open/GetCommState/close pass, problem 0. [Run](2026-10-21-uac2-vm.md). | **Missing.** No serial payload sent; no UART loopback or physical Arduino/Pico claim. |
| `uvc98` / Logitech Brio100, 046D:094C | Windows 7 x86; physical camera through QEMU EHCI, host WinUSB | Native `usbvideo`; project CAMCHECK is the user-mode viewer | [Frames](../../out/proof/brio100/7frames.png): 332 callbacks, 38400 bytes, 160x120 YUY2. [Reopen](../../out/proof/brio100/7reopen.png): 131 callbacks after stop/reopen. [Run](2026-10-21-brio100-native.md). | **Missing.** Native reference only, not project USB capture. Gray/noisy image remains unexplained; no clear-scene/image-quality claim. |
| `ax88179` / ASIX, 0B95:1790 | Windows host, actual SuperSpeed device; WinUSB/libusb, not a guest network interface | Original host diagnostic and actual portable C framing; no project NDIS driver | **No functional screenshot captured.** [Physical run](2026-10-21-ax88179-physical.md); ignored `out/net/ax88179/physical/ax-router-net1.jsonl` and `ax-router-net2.jsonl` record two DHCP ACK/router ARP/exact ICMP reply passes. | **Missing, including the requested screenshot.** Host logs are useful I/O evidence but not a 98/guest NDIS or USB2 Ethernet pass. [2000 descriptor diagnostic](2026-10-21-ax88179-2k-descriptors.md) is enumeration only; QEMU's EHCI speed/SS-descriptor mismatch remains a blocker. |

## Simulated devices and utility/load evidence

| Package / target | Exact OS and model | Project/native route | Existing screenshot and demonstrated scope | Windows 98 SE functional proof / remaining limit |
|---|---|---|---|---|
| `usbmidi` / FFFF:0001 | Windows 2000 SP4; original one-cable MIDI model, QEMU UHCI | Earlier project USB MIDI build | [Loopback](../../out/proof/midisim/2kloop.png): 355 bytes matched, outputDone 1, errors 0, cleanup complete. [Run](2026-10-21-usbmidi-vm.md). | **Missing.** Not physical Roland I/O or current v4 validation; later 2000 build/load attempts have separate unresolved results. |
| `usbcdc` / simulated Uno-ID 2341:0043 | Windows 7 x64; original CDC-ACM echo model, QEMU UHCI | Project INF binding native `usbser` | [Final echoes](../../out/proof/acmsim/7echo.png): exact 1/63/64/65/128/1537-byte echoes and port close with final full-packet ZLP model. [Run](2026-10-21-usbcdc-vm.md#windows-2000-regression-and-bounded-empty-read-handling). | **Missing.** No physical Uno/Pico test and no project 98 serial binary. [2000 negative](../../out/proof/acmsim/2kfail.png): 65-byte read remains incomplete despite bounded empty-read retry. Earlier 2000 pass predates the model correction. |
| `cdcnet` / 0525:A4A2 ECM model | Windows 2000 SP4 and Windows XP x86; QEMU UHCI, original ECM model/network harness | Project CDCNET driver | [2000](../../out/proof/cdcnet/2knet.png), [XP](../../out/proof/cdcnet/xpnet.png): three ICMP replies and 32768 deterministic TCP bytes verified; DHCP and replug details in [run](2026-10-21-cdcnet.md). | **Missing.** No physical NIC claim; simulated ECM results do not establish ASIX, RTL815x or another transport. |
| `uvc98` / FFFF:0002 UVC model | Windows 7 x86; original full-speed bulk YUY2 model, QEMU UHCI | Native `usbvideo` and project CAMCHECK viewer | [Preview](../../out/proof/uvcsim/7moving.png): bars, 143 callbacks, 38400-byte buffers. [Run](2026-10-21-camcheck.md). | **Missing.** Not physical camera or project USB driver; reconnect error 418 remains scoped in the run. |
| `uvc98` / synthetic ROOT M0 | Windows 2000 SP4; no USB device | Project STREAM.SYS miniport | [Load/status](../../out/proof/uvcm0/2kload.png): service `uvcmini`, problem 0. [Run](2026-10-21-uvc-miniport.md). | **Missing. Load only:** RUN unsupported, no preview frames. Portable worker/ownership tests are not kernel capture evidence. |
| `uvc98` / synthetic ROOT M1 | Windows 2000; no USB device | Separate project STREAM.SYS worker prototype | [Initial pending install](../../out/video/uvc98/m1-native/w2kuvc1-installed.png), revised [Code10 status](../../out/video/uvc98/m1-native/w2kuvch-installed.png), [run](2026-10-21-uvc-worker.md). LOW-to-HIGH revision completes NewDev with reboot requested; restart did not return to desktop within the bounded wait. Later budget-fix SYS C0799AEA: start CM problem 0 and idle removal pass; KsCreatePin 87 traced to KSPRIORITY subclass 0 in the checker, then [three changing synthetic frames, RUN/STOP, close, removal](2026-10-21-uvc-worker.md) (2026-10-22; screenshot `E:\usb-wdm-free\evidence\w2k-m1cap\w2kuvck-cap3.png`). | **Missing for a camera:** synthetic worker frames through the project STREAM.SYS only; no USB, UVC payload, physical camera or 98 SE run. |
| `uvc98` / Logitech Brio100, 046D:094C, M2 | Windows 2000 SP4 guest (TCG); physical camera through QEMU EHCI passthrough | Project STREAM.SYS M2 (`uvcm2.sys` 018DE1C5, checked VM-trace) | [Run](2026-10-22-uvc-m2.md): M1CAP PASS, three complete changing 153600-byte YUY2 frames, saved and converted (`E:\usb-wdm-free\evidence\w2k-m2\run6\frame1-3.png`, checker screenshot `w2km2h-cap.png`); removal and safe-off shutdown. | **Partial:** real camera frames through the project driver on 2000 in a VM. The image is dark grey noise, so no scene claim. No 98 SE, physical-host or release claim. Uses a per-camera bandwidth quirk; TCG drops most frames. |
| `uvc98` / CAMCHECK no-device case | Windows 98 SE; no camera attached | Project user-mode viewer only | [UI](../../out/proof/camcheck/98ui.png): 640x480 no-device UI fits and reports no capture device. [Run](2026-10-21-camcheck.md). | **Missing. Utility UI only:** no live camera feed or project capture driver. |
| `rtl815x` | No functional device run indexed | Planned project package | No functional screenshot. | **Missing.** No inferred network pass from another chipset. |
| `xpad98` | No functional device run indexed | Planned project package | No functional screenshot. | **Missing.** |
| `usbprn` | No functional device run indexed | Planned project package | No functional screenshot. | **Missing.** |
| `libusb0` | No functional device run indexed | Planned project package | No functional screenshot. | **Missing.** |

The initial inventory launched no VM or hardware test. A subsequent
authorized floppy run supplied the new physical 98 read evidence above;
the copies themselves remain unmodified.
The next proof capture should show an actual completed device operation on
the tested OS, alongside a run record naming the exact installed build and
host stack. Preserve negative results and native references separately.
