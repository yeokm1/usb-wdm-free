# Device development checkpoint, 2026-10-21

Development evidence only. This does not update the release's author-tested
table, and no package has been released. A simulated device is never a
substitute for the corresponding physical-hardware claim.

| Device/package | Recorded result | Remaining limit |
|---|---|---|
| UAC2/X4 | Latest physical Windows7 x86 repeat passes unchanged WinMM verifier: 8/8 buffers, 96000 samples, close0. Independent raw backend logging exposes errors formerly classified as success. Linux-derived mixer and control host suites pass. | Latest pass still has 24 failed raw OUT packets and 24 QEMU empty-ring events. Guest timer drift and incomplete guest error propagation remain unresolved. Reliable playback, audible fidelity, capture, full UAC2 features and 98/ME streaming are unverified. Host scheduling and guest buffering audits are active. |
| USB floppy | Project CBI binding on Windows98SE/NUSB3.3 reads three complete physical media files: 133557 bytes, zero failures, matching CRC32 across completed reads. Actual screenshots and run record preserved. | Bounded read-only result, not disk-wide integrity or writes/formatting. Separate native-control reboot stalled; no cause assigned. Drive released and QEMU closed. |
| USB serial | NT INF binds native usbser; final Windows 7 x64 simulated echoes pass at 1/63/64/65/128/1537 bytes, with bounded probe deadlines and clean close. | Windows 2000 ZLP regression remains unresolved at 65 bytes despite bounded empty-read retry; rejected model experiments are recorded. Earlier 2000 pass used the older model. No project 98/ME serial binary. |
| USB MIDI | Earlier project driver passes 355-byte simulated Windows2000/UHCI loopback. Version4 starts on physical UM-ONE mk2 TAB0582:0168 in Windows7/UHCI: project service/problem0, WinMM input/output open-close twice pass; installed own SYS hash verified. Idle removal gives Add1/Remove1/Destroy1. | No DIN loopback, so no physical byte roundtrip. 98SE and same-port reconnect remain unverified. Earlier2000 overlay has loader-integrity failure; fresh version3 install exited without a verdict. Vendor012A startup remains untested; TAB0168 uses standard descriptors. |
| UVC/CamCheck | Native Windows 7 usbvideo delivered physical Brio100 160x120 YUY2 frames with callback counts increasing 94 to 332; stop/reopen delivered another 131. Original synthetic STREAM.SYS M0 loads and removes on Windows 2000. Typed UVC1.5 probe-tail accessors pass host tests. | Physical image was gray/noisy; quality remains unexplained. These native-driver frames do not establish project USB capture. Project miniport RUN and full UVC features remain unfinished. |
| CDC network | Project ECM driver passes DHCP, ICMP and deterministic 32KiB TCP transfers on the recorded 2000/XP simulation paths. | No physical network-device claim; other protocols/platforms remain scoped in the run logs. |
| AX88179 | Real SuperSpeed device passes two host USB Ethernet runs: volatile init, 1000Mb/s full duplex, DHCP ACK, router ARP and exact ICMP echo reply. Second run uses actual C aggregate framing plus verified/stripped Ethernet FCS. MAC preserved, captured MAC-register state restored and interface released. | Host WinUSB/libusb test only; no guest NDIS driver or USB2 Ethernet claim. QEMU EHCI SS/HS descriptor mismatch and general throughput/reliability/other protocols remain unresolved or untested. |

Detailed records:
[audio](2026-10-21-uac2-vm.md),
[host audio blocker](2026-10-21-uac2-winusb-boundary.md),
[floppy](2026-10-21-usbflop-more.md),
[physical floppy media](2026-10-21-usbflop-media.md),
[serial final regression](2026-10-21-usbcdc-vm.md#windows-2000-regression-and-bounded-empty-read-handling),
[MIDI](2026-10-21-usbmidi-vm.md),
[physical Roland startup](2026-10-21-umone2.md),
[CamCheck](2026-10-21-camcheck.md),
[synthetic camera miniport](2026-10-21-uvc-miniport.md),
[network](2026-10-21-cdcnet.md),
[ASIX guest descriptors](2026-10-21-ax88179-2k-descriptors.md),
[ASIX host controls](2026-10-21-ax88179-host-reads.md).

Specifications, datasheets and pinned reference sources remain ignored in
`docs/references/`, with acquisition records and licence distinctions in
the corresponding audit documents. VM overlays, exchanges and scratch
files are on E:. Each run record states its own process cleanup; do not
assume a whole-machine process inventory while another agent is testing.
