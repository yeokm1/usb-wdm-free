# Legs by host controller driver

Owner rule (2026-10-22, `AGENTS.md`): every driver's legs run under a USB
2.0 (EHCI) host controller driver (Windows' own, or NUSB's on Windows 98
SE) **and** under xHCI98. A driver is not ready until both pass on its
systems. This table shows which legs have run under which host
controller driver, so the gaps are visible. It counts only legs where
this repository's driver ran, in a guest or on a PC. A UHCI leg is
neither EHCI nor xHCI98. Update it with every recorded run.

Owner rule (2026-10-22, `AGENTS.md`): every driver works on Windows 98
SE at minimum, `uvc98` and `uac2` above all. Its 98 SE legs must pass
under NUSB's EHCI and under xHCI98 before it is ready; the "98 SE" column
below shows that gap.

Compiled 2026-10-22 from the records in this folder. "-" means no leg.

## Summary per driver

| Driver | 98 SE: NUSB EHCI / xHCI98 | EHCI legs | xHCI98 legs | UHCI-only legs (count as neither) |
|---|---|---|---|---|
| uac2 | build 16, QEMU simulator: NUSB six of seven profiles PASS (8-channel playback FAIL, Full Speed no leg) / xHCI98 all eight profiles PASS | XP x86 (real X4 build 16: playback PASS, 30 s PASS; simulator build 16: playback PASS, capture FAIL); 7 x86 (real X4 build 16: 30 s soak PASS 2 of 2, e2 of build 6 FAIL; simulator build 16: playback and capture PASS); 98 SE NUSB (simulator profiles, build 14 to 16) | 98 SE (simulator profiles, build 16 PASS) | - |
| uvc98 (M2) | FAIL in QEMU (Brio via VM and simulator) / 160x120, 320x240 and (after the stride fix) 640x480 PASS with the simulator, QEMU-paced (Brio via VM: 320x240 FAIL) | 2000 SP4: PASS for 160x120, 176x144, 320x240; FAIL for 640x480 (high bandwidth); 98 SE NUSB: 160x120 FAIL (QEMU EHCI passthrough) | 98 SE (160x120 PASS, 320x240 FAIL); 2000 SP4 (160x120 FAIL, empty stream) | - |
| usbmidi | PASS except replug / PASS except replug (QEMU simulator; on 98 SE a replugged device works again only after a restart) | 98 SE (NUSB 3.3, High Speed; replug FAIL); 2000 SP4 and XP SP3 (Windows' usbehci, High Speed: PASS, replug PASS; XP numbers the output ports in reverse) | 2000 SP4 (PASS, Full and High Speed); 98 SE (PASS; input fails after a replug) | 2000 SP4 (M1 PASS); 98 SE (M1 PASS, 2026-10-22, see below); ME (PASS after the SELECT_INTERFACE fix, replug PASS); 7 x86 (UM-ONE, open/close only) |
| cdcnet | PASS / PASS (QEMU, ECM and RNDIS, High Speed usb-net; 2026-10-22; NCM with the SIMULATED function `ncmemu.py`, 2026-10-23, `2026-10-23-cdcnet-ncm-sim.md`) | 98 SE (NUSB 3.3); 2000 SP4, XP SP3 (Windows' usbehci, ECM, High Speed) | 98 SE (FS and HS), ME (HS ECM, FS RNDIS) | 2000, 98 SE, XP, 7 x64 (PASS) |
| usbcom (`USBCDC.INF` binding Windows' usbser) | n/a (2000 and later) | XP x86 (open/close only, X4 CDC function) | - | 2000 (PASS without a flush ZLP; 2000's usbser stops reading after one: REAL CHECK NEEDED), 7 x64 (PASS) |
| usbcom 98/ME driver | PASS / PASS (QEMU, CDC-ACM simulator; under NUSB a 64 KiB echo fails about 1 run in 6, below the driver) | 98 SE (NUSB 3.3, High Speed) | 98 SE (Full and High Speed), ME (High Speed) | 98 SE (PASS, Full Speed) |
| usbflop | REAL CHECK NEEDED / PASS (real Mitsumi via VM; a Full Speed drive is not carried by QEMU's EHCI) | - | 98 SE, ME (read PASS) | 98 SE (read PASS), ME, 2000, XP, 7 x64 (Mitsumi: binding only; simulated unlisted CBI and CB drive: read PASS, `2026-10-24-usbflop-nt-xpad-x64.md`) |
| ax88179 | PASS / PASS (real AX88179 via VM, 2026-10-22) | 98 SE (NUSB 3.3, High Speed behind a USB 2.0 hub) | 98 SE, ME (SuperSpeed) | - |
| rtl815x | PASS / PASS with the SIMULATED RTL8153 (`rtlemu.py`; install, link flap, DHCP, ping 32-1472, removal; `2026-10-23-rtl815x-sim.md`); no real device; the rewritten core (2026-10-25) again with the RTL8153 under both, and the RTL8153B under xHCI98 only, `2026-10-25-rtl815x-rewrite.md` | 98 SE NUSB (simulator) | 98 SE (simulator) | - |
| libusb0 | PASS / PASS (install, load, listing only; QEMU usb-storage via a test INF) | 98 SE (NUSB 3.3, High Speed) | 98 SE (SuperSpeed) | 98 SE (PASS) |
| xpad98 | PASS / PASS (simulated pads, after the 2026-10-23 fixes; the receiver unsupported under xHCI98 3.1.0.0, PASS with 43.67) | 98 SE (NUSB 3.3: 360, Xbox One, receiver); 2000 SP4, XP SP3, Vista SP2 x86 (Windows' usbehci) | 98 SE (360, Xbox One; receiver with 43.67), ME (360, Xbox One) | 98 SE, ME (PASS) |

Only cdcnet has legs under xHCI98 and under NUSB's EHCI stack on 98 SE
(QEMU only, `2026-10-22-cdcnet-9x.md`).
usbmidi, uvc98 and uac2 have their first xHCI98 legs, on 98 SE and (usbmidi,
uvc98) 2000 SP4 (`2026-10-22-98se-legs.md`, `2026-10-22-uac2-98se.md`).
All three have NUSB EHCI legs on 98 SE (`2026-10-22-usbmidi-m1.md`,
`2026-10-22-98se-nusb-legs.md`); uvc98's and uac2's fail there in QEMU,
below the driver as far as read.

## Legs in detail

| Driver | System | Host controller and its driver | Device kind | Device | Result | Record |
|---|---|---|---|---|---|---|
| uac2 | XP SP3 x86 | QEMU usb-ehci, Windows' EHCI | real device via VM | Creative X4, passthrough | started, no WinMM device; one STOP 0x7E | `2026-10-21-uac2-vm.md` |
| uac2 | 7 x86 | QEMU usb-ehci, Windows' usbehci | emulated | X4 model over usb-redir | PASS (repeat) | `2026-10-21-uac2-vm.md` |
| uac2 | 7 x86 | QEMU usb-ehci, Windows' usbehci | real device via VM | X4, passthrough | VM gate r1, r2, e1 PASS; e2 soak FAIL | `2026-10-21-uac2-vm.md`, `handoff.md` |
| uac2 | XP SP3 x86 | QEMU usb-ehci, Windows' EHCI | emulated | X4 model over usb-redir, build 6 | INF NTx86 rerun: `MI_03` binds, parent stays `usbccgp`; playback PASS | `2026-10-22-98se-nusb-legs.md` |
| uac2 | 7 x86 | QEMU usb-ehci, Windows' usbehci, TCG | emulated | X4 model over usb-redir, build 6 | INF NTx86 rerun: `MI_03` binds, parent stays `usbccgp`; 8/8 buffers, tick-count check fails under TCG | `2026-10-22-98se-nusb-legs.md` |
| uac2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | X4 model (`uacemu.py`) over usb-redir | starts (all PortCls stages), unplug clean; no WinMM wave device, playback not reached: FAIL | `2026-10-22-98se-legs.md` |
| uac2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | X4 model over usb-redir, build 4 (SUM node) | `waveOut=1`; playback PASS; unplug, replug, unplug during playback PASS | `2026-10-22-uac2-98se.md` |
| uac2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | X4 model over usb-redir, build 6 | 9 of 10 plays PASS; unplug, replug, unplug during playback PASS | `2026-10-22-98se-nusb-legs.md` |
| uac2 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | X4 model over usb-redir, build 5 (whole device) | binds and starts, `waveOut=1`; playback 5 of 7 PASS (default `maxframes`), 0 of 3 (`maxframes=512`): FAIL; feedback data lost in QEMU's EHCI and `usb-redir` (build 6 diagnostics); unplug, replug, unplug during playback PASS | `2026-10-22-98se-nusb-legs.md` |
| uac2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | `uacemu.py` profiles (eight UAC 2.0 layouts), build 16 | every profile PASS (playback with signal, capture with tone); builds 14, 15: capture-only arrival faulted WDMAud | `2026-10-24-uac2-profiles.md` |
| uac2 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | `uacemu.py` profiles except `fs`, cold boot with the device present, builds 14 to 16 | six PASS; `multi` (8 channels) FAIL, the stream stalls | `2026-10-24-uac2-profiles.md` |
| uac2 | XP SP3 x86 | QEMU usb-ehci, Windows' EHCI | emulated | `uacemu.py` `x4full`, build 16 | playback PASS; capture FAIL (no tone) | `2026-10-24-uac2-profiles.md` |
| uac2 | 7 x86 | QEMU usb-ehci, Windows' usbehci, TCG | emulated | `uacemu.py` `x4full`, build 16 | playback and capture PASS | `2026-10-24-uac2-profiles.md` |
| uac2 | XP SP3 x86 | QEMU usb-ehci, Windows' EHCI, TCG | real device via VM | X4, passthrough, build 16 | no STOP 0x7E; playback 2 s and 30 s PASS; unplug during playback survives; replug PASS | `2026-10-24-uac2-profiles.md` |
| uac2 | 7 x86 | QEMU usb-ehci, Windows' usbehci, TCG | real device via VM | X4, passthrough, build 16 | 30 s soak PASS 2 of 2 | `2026-10-24-uac2-profiles.md` |
| uvc98 M2 | 2000 SP4 | QEMU usb-ehci, Windows' usbehci (SP4) | real device via VM | Brio 100, passthrough | runs 5-7, 10, 12, 14 PASS; 640x480 FAIL | `2026-10-22-uvc-m2.md` |
| uvc98 M2 | 2000 SP4 | QEMU usb-ehci, Windows' usbehci (SP4) | emulated | `uvcsim.py` over usb-redir (control for NUSB) | `a8bf2bc`: 160x120, 320x240, 640x480 at 30 fps (high bandwidth) PASS; failed packets 7-23 against NUSB's ~1000 per run | `2026-10-23-uvcsim-98se.md` |
| uvc98 M2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | real device via VM | Brio 100, passthrough | 160x120 PASS (3 runs, one after a replug); 320x240 FAIL (3 runs, every payload error-flagged); unplug clean | `2026-10-22-98se-legs.md` |
| uvc98 M2 | 2000 SP4 | QEMU qemu-xhci, xHCI98 3.1.0.0 | real device via VM | Brio 100, passthrough | 160x120 FAIL (2 runs, stream nearly all empty packets); unplug and safe-off clean | `2026-10-22-98se-legs.md` |
| uvc98 M2 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | real device via VM | Brio 100, passthrough (whole device) | 160x120 FAIL (4 runs, default and `maxframes=512`: failed packets in every frame; QEMU restarted the iso stream 275 times in one capture); 640x480 hung the guest | `2026-10-22-98se-nusb-legs.md` |
| uvc98 M2 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | `uvcsim.py` over usb-redir (whole device) | 160x120, 320x240, 640x480 at 5 fps FAIL in QEMU (a third of packets lost, QEMU underruns, failed packets; no frame); 640x480 at 30 fps (3 x 800) hung the guest | `2026-10-23-uvcsim-98se.md` |
| uvc98 M2 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | `uvcsim.py` over usb-redir | 160x120 PASS (byte-exact frames), 320x240 PASS; 640x480 at 30 fps FAIL (uvc98's 4096-byte stride refused, USBD 80000300), PASS after the fix `a8bf2bc` (3 frames, about 18 % of microframes through); 640x480 at 5 fps FAIL (qemu-xhci pacing, 21 % of packets); unplug and replug PASS | `2026-10-23-uvcsim-98se.md` |
| uvcdec | 98 SE, 2000 SP4 | none (codec only, no USB) | emulated | test vectors and MJPEG AVIs made from them; no USB device | JDTEST PASS (98 SE pentium3 and core2duo, 2000 pentium3); INF and VfW / DirectShow playback PASS; counts as neither EHCI nor xHCI98 | `2026-10-22-uvcdec-guest.md` |
| usbmidi M1 | 2000 SP4 | QEMU UHCI, Windows' uhcd | emulated | simulator (multi) over usb-redir | PASS | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | 98 SE | QEMU UHCI, Windows' uhcd (USB.INF) | emulated | simulator (multi, lone) over usb-redir | PASS after the thread-wait fix | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | simulator (multi, High Speed) over usb-redir | loopbacks and open PASS; unplug clean; same-port replug FAIL (MMSYSERR_ALLOCATED on input and output; 98 SE limitation) | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | simulator (multi, High Speed) over usb-redir, `w98-nusb` image with `USBHUB.SYS` supplied | loopbacks, isolation, unplug PASS; stress lost 6-168 bytes in 3 of 4 runs below the driver; same-port replug FAIL (98 SE limitation) | `2026-10-22-98se-nusb-legs.md` |
| usbmidi M1 | 98 SE | QEMU UHCI, Windows' uhcd (USB.INF) | emulated | simulator (multi, lone) over usb-redir, evening replug runs | any replug, either port: every MIDI open fails MMSYSERR_ALLOCATED until a restart; other devices unaffected; 98 SE limitation | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | ME | QEMU UHCI, Windows' uhcd | emulated | simulator (multi) over usb-redir | start FAIL C0000182 (empty pipe list from ME's composite parent); after the SELECT_INTERFACE fallback loopbacks, open, unplug and same-port replug PASS | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | 2000 SP4 | QEMU UHCI, Windows' uhcd | emulated | simulator (multi) over usb-redir, stop-path rerun | loopbacks, open, unplug, same-port replug PASS | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | 2000 SP4 | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | simulator (multi; Full and High Speed) over usb-redir | PASS: loopbacks, isolation, stress, unplug under traffic, replug, new port after restart | `2026-10-22-98se-legs.md` |
| usbmidi M1 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | simulator (multi) over usb-redir | loopbacks, stress, unplug PASS; same-port replug FAIL (MMSYSERR_ALLOCATED) | `2026-10-22-98se-legs.md` |
| usbmidi M1 | 2000 SP4 | QEMU usb-ehci, Windows' usbehci (SP4) | emulated | simulator (multi, High Speed) over usb-redir | loopbacks and open PASS, unplug clean, same-port replug PASS | `2026-10-22-usbmidi-m1.md` |
| usbmidi M1 | XP SP3 | QEMU usb-ehci, Windows' usbehci | emulated | simulator (multi, High Speed) over usb-redir, test INF installed over Windows' usbaudio | by-name loopbacks (`0 1`, `1 0`) and open PASS; replug PASS; WinMM output order reversed against input | `2026-10-22-usbmidi-m1.md` |
| usbmidi | 7 x86 | QEMU UHCI | real device via VM | Roland UM-ONE mk2, passthrough | open/close only | `2026-10-21-umone2.md` |
| cdcnet 0.1.0.0 | 2000, 98 SE, XP, 7 x64 | QEMU UHCI, Windows' own | emulated | QEMU usb-net | PASS (ECM; RNDIS on 2000, 98 SE) | `2026-10-22-cdcnet.md` |
| cdcnet 0.1.0.0 | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | QEMU usb-net, High Speed (test patch) | ECM and RNDIS: NETTEST and removal under traffic PASS after `8438c07` | `2026-10-22-cdcnet-9x.md` |
| cdcnet 0.1.0.0 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | QEMU usb-net, Full and High Speed | ECM (FS, HS) and RNDIS (HS): NETTEST and removal under traffic PASS after `2e6b56a` | `2026-10-22-cdcnet-9x.md` |
| cdcnet 0.1.0.0 | ME | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | QEMU usb-net, High Speed (test patch) | ECM: install, NETTEST and removal under traffic PASS | `2026-10-22-cdcnet-9x.md` |
| cdcnet 0.1.0.0 | 2000 SP4 | QEMU usb-ehci, Windows' usbehci (SP4) | emulated | QEMU usb-net, High Speed (test patch) | ECM: NETTEST x2, idle removal, removal under traffic, replug and NETTEST PASS | `2026-10-23-sweep-legs.md` |
| cdcnet 0.1.0.0 | XP SP3 x86 | QEMU usb-ehci, Windows' usbehci | emulated | QEMU usb-net, High Speed (test patch) | ECM: NETTEST, removal under traffic, replug and NETTEST PASS | `2026-10-23-sweep-legs.md` |
| cdcnet 0.1.0.0 | ME | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | QEMU usb-net, Full Speed | RNDIS: NETTEST 12 x 1 MiB, removal under traffic, same-port replug PASS | `2026-10-23-sweep-legs.md` |
| xpad98 | ME | QEMU qemu-xhci, xHCI98 3.1.0.0; UHCI, Windows' uhcd | emulated | pad simulator (x360, xone; x360 and receiver on UHCI) over usb-redir | JOYTEST PASS | `2026-10-23-xpad98-legs.md` |
| xpad98 | 98 SE | QEMU qemu-xhci, xHCI98 43.67 qemu test build | emulated | pad simulator (x360, receiver) over usb-redir | full select accepted (7, 16 endpoints); JOYTEST PASS | `2026-10-23-xpad98-legs.md` |
| usbflop | 2000 SP4, XP SP3, 7 SP1 x64 | QEMU UHCI, Windows' UHCI driver | emulated | UFI floppy simulator (unlisted FFFF:0020 CBI, FFFF:0021 CB; FS) over usb-redir | class match binds; FREAD three files, CRC-32s match (READ PASS); Windows writes the root directory (access dates) during reads | `2026-10-24-usbflop-nt-xpad-x64.md` |
| xpad98 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | pad simulator (x360, xone, receiver; FS) over usb-redir | first run Code 10 (7 endpoints refused); after the fixes 360 and Xbox One JOYTEST PASS, receiver FAIL (8 endpoints refused) | `2026-10-23-xpad98-legs.md` |
| xpad98 | 98 SE | QEMU UHCI, Windows' uhcd; usb-ehci, NUSB 3.3 EHCI | emulated | pad simulator (x360; xone and receiver on NUSB; FS, HS) over usb-redir | first run "not connected" (Rx/Ry, 0..65535 range); after the fixes JOYTEST PASS (receiver: slot 0) | `2026-10-23-xpad98-legs.md` |
| xpad98 | 2000 SP4, XP SP3, Vista SP2 x86 | QEMU usb-ehci, Windows' usbehci | emulated | pad simulator (x360 on 2000; xone on all; HS) over usb-redir | JOYTEST PASS (all buttons, axes, POV); GIP handshake as designed | `2026-10-23-xpad98-legs.md` |
| xpad98 (x64 build) | XP x64, Vista SP2 x64 (throwaway in-guest test certificate, testsigning) | QEMU usb-ehci, Windows' usbehci | emulated | pad simulator (xone, HS) over usb-redir | JOYTEST PASS; GIP handshake as designed; unplug clean | `2026-10-24-usbflop-nt-xpad-x64.md` |
| libusb0 | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0; usb-ehci, NUSB 3.3 EHCI; UHCI, Windows' uhcd | emulated | QEMU usb-storage (46F4:0001) bound by a test copy of LIBUSB0.INF; SuperSpeed, High Speed, Full Speed | install, load, TESTLIBU listing, idle unplug and replug PASS on each | `2026-10-23-sweep-legs.md` |
| ax88179 (AX179F) | 98 SE, ME | QEMU qemu-xhci, xHCI98 3.1.0.0 | real device via VM | the owner's AX88179, passthrough, SuperSpeed | install, DHCP, ping 0-4000 bytes, removal under traffic PASS after `d6f514e`; a running ping did not recover across the replug | `2026-10-22-ax88179-9x.md` |
| ax88179 (AX179F) | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | real device via VM | the owner's AX88179 behind a USB 2.0 hub, passthrough, High Speed | install, DHCP, ping 0-4000 bytes, removal under traffic PASS; FCS safety net fired in the first session | `2026-10-22-ax88179-9x.md` |
| usbcom (USBCDC.INF) | 2000, 7 x64 | QEMU UHCI | emulated | CDC-ACM simulator | 7 x64 PASS; 2000 regression FAIL | `2026-10-21-usbcdc-vm.md` |
| usbcom 98/ME driver | 98 SE | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | CDC-ACM simulator (Uno, Pico; FS and HS) over usb-redir | COMTEST full 58/58, pull while open and replug, then 97 (free) and 161 (trace) opens with 64 KiB echoes PASS | `2026-10-22-usbcom98-legs.md` |
| usbcom 98/ME driver | ME | QEMU qemu-xhci, xHCI98 3.1.0.0 | emulated | CDC-ACM simulator (Uno and Pico, HS) over usb-redir | COMTEST full 58/58, pull while open and replug, 40 opens, 16 × 64 KiB echo PASS (free and trace) | `2026-10-22-usbcom98-legs.md` |
| usbcom 98/ME driver | 98 SE | QEMU UHCI, Windows' uhcd | emulated | CDC-ACM simulator (Uno, FS) over usb-redir | COMTEST full 58/58, pull while open and replug, 40 opens, 16 × 64 KiB echo PASS (free and trace) | `2026-10-22-usbcom98-legs.md` |
| usbcom 98/ME driver | 98 SE | QEMU usb-ehci, NUSB 3.3 EHCI | emulated | CDC-ACM simulator (Uno, Pico; HS) over usb-redir | COMTEST full 58/58, pull while open and replug PASS; 64 KiB echo fails about 1 in 6 below the driver | `2026-10-22-usbcom98-legs.md` |
| usbcom (USBCDC.INF) | XP x86 | QEMU usb-ehci, Windows' EHCI | real device via VM | X4 CDC function | open/close only | `2026-10-21-uac2-vm.md` |
| usbflop | 98 SE, ME, 2000, XP, 7 x64 | QEMU UHCI (98 SE: NUSB 3.3 installed, its EHCI not used) | real device via VM | Mitsumi floppy, passthrough | 98 SE read PASS; others binding | `2026-10-21-usbflop-*.md` |
| usbflop | 98 SE, ME | QEMU qemu-xhci, xHCI98 3.1.0.0 | real device via VM | Mitsumi floppy, passthrough, native USBSTOR.INF set aside | read PASS (3 files, CRCs as 2026-10-21) | `2026-10-23-sweep-legs.md` |

"Device kind" is one of three labels: **emulated** (a QEMU device model,
or this repository's simulator or synthetic source), **real device via VM**
(a physical device passed through from the author's host with QEMU
`usb-host`), **real PC** (a physical machine running the system). No leg
has run on a real PC yet (the owner's bench session); `README.md`,
"Legs run so far, by kind of device", has the same legs per driver.
The synthetic camera legs of `uvc98` (root-enumerated, no USB, on 2000)
run under no host controller driver and are listed there only.

Legs of Windows' own class drivers (usbvideo, usbaudio) and root-enumerated
smoke tests (wdmsmoke) are not in the table: they run no USB driver from
this repository.

## What closes the gaps

- xHCI98: every driver, on its systems (98 SE, ME, 2000 and later as the
  driver targets), with xHCI98 on QEMU `qemu-xhci` and on a real xHCI
  controller.
- EHCI: usbmidi on Vista and later (its 98 SE, 2000 and XP EHCI legs
  ran), cdcnet on XP and 7 and on hardware (its 98 SE and 2000 QEMU legs ran), ax88179 on ME (later, SweetLow's stack), usbflop, usbcom's 98/ME driver; uvc98 on 98 SE once its
  98 path exists; uac2 on 2000; uvc98 and uac2 on 98 SE with NUSB on a
  real EHCI controller (their QEMU legs fail below the driver).
- High-bandwidth isochronous (uvc98 640x480): the QEMU passthrough on this
  host fails those packets on the host side, so it needs a real PC.
