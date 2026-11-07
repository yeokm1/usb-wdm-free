# Makers' drivers against this repository's simulators, 2026-11-06 and 07

xHCI98 Phase 47 (owner, 2026-11-06). Many drivers here have run only
against simulators built from public sources (Linux and others). Here each
maker's own driver ran, inside a virtual machine, against the same
simulator. The USB traffic was recorded with QEMU's `pcap=` on the
`usb-redir` device and the simulator's own log. Where a maker's driver
needed something the simulator did not do, the simulator was fixed, and
usb-wdm-free's own driver was run again against the changed simulator.
No physical device was used and none is claimed. The maker's binaries
were only run, never read, and none is tracked.

This record covers the network simulators (`ax7emu.py`, `axemu.py`,
`rtlemu.py`, `sremu.py`), the Xbox 360 profile of `xpademu.py`, and the
`penpartner` profile of `absemu.py`. The serial bridge models
(`chipemu.py`) and the models that Windows' own drivers serve are recorded
separately.

## Rig

- Stock QEMU 11.1.0, TCG, headless. q35 guests from the read-only q35
  bases (XP SP3 x86, 7 SP1 x86, 10 22H2 x64), with no xHCI controller:
  `usb-ehci` and `piix3-usb-uhci` on `pcie.0` under Windows' own drivers,
  and a `usb-tablet` for the pointer. Drivers were installed with WDK 7.1's
  `devcon update` (XP, 7) or `pnputil /add-driver /install` (10).
- 98 SE retests: an overlay of `w98-nusb-hub.qcow2` (98 SE, NUSB 3.3),
  `pc` machine, NUSB's EHCI (`ehci.0`, 480 Mb/s). The driver came from a
  virtual floppy through the hardware wizard.
- XP retests of the NT builds: the XP base as above, Windows' EHCI.
- Each simulator ran at High Speed (Full Speed for the pad), from this
  branch's tree. Development host A.

## Captures cited

Each capture is QEMU's pcap of the guest's traffic to the simulated
device. QEMU's pcap keeps 256 bytes of each transfer's data.

| Name | Maker's driver, version | Where it came from | System | Device (simulated) |
|---|---|---|---|---|
| A1 | ASIX AX88179 XP driver 1.4.2.0 (`Ax88179_178a.inf` 05/24/2013) | usb-wdm-nonfree `non-free/ethernet/ax88179/wxpvis/` (ASIX's download, as that repository records) | XP SP3 x86, Windows' EHCI | `axemu.py --speed high` (0B95:1790) |
| A2 | ASIX AX88772B XP driver 5.4.3.0 (`Ax88772b.inf` 05/21/2010) | usb-wdm-nonfree `non-free/ethernet/ax88772b/w2k-xp/` | XP SP3 x86, Windows' EHCI | `ax7emu.py` (0B95:772B, AX88772B) |
| A3 | ASIX AX88772C/B 7 driver 5.12.12.0 (`Ax88772b.inf` 05/14/2015) | usb-wdm-nonfree `non-free/ethernet/ax88772b/w7/` | 7 SP1 x86, Windows' EHCI | `ax7emu.py --chip 772c` |
| R1 | Realtek XP driver 5.23.0123.2018 (`rtuxp.INF`) | usb-wdm-nonfree `non-free/ethernet/rtl815x/wxp/` | XP SP3 x86, Windows' EHCI | `rtlemu.py --chip 8152`, `8153`, `8153b` |
| R2 | Realtek 7 driver 7.61.20.1102 (`rtump62x86sta.INF`) | usb-wdm-nonfree `non-free/ethernet/rtl815x/w7/` | 7 SP1 x86, Windows' EHCI | `rtlemu.py --chip 8156` |
| R3 | Realtek Windows 10 driver 10.68.20.0605 (`rtump64x64sta.INF`, from `Install_USB_Win10_10068_20_07312026.exe`, read as an archive) | Realtek's download, as the owner supplied it | 10 22H2 x64, Windows' EHCI | `rtlemu.py --chip 8153`, `8156`, `8156b` |
| R4 | Realtek USB GbE driver 10.5.920.2015 (`rtux64w10.inf`), as Windows 10 ships it | Windows 10 22H2 x64 | 10 22H2 x64, Windows' EHCI | `rtlemu.py --chip 8153` |
| X1 | x360c 6.1.7600.16385 (`x360c.inf` 05/04/2010), Masahiko Morii's independent driver | usb-wdm-nonfree `non-free/hid/x360c/wxp/` | XP SP3 x86, Windows' UHCI | `xpademu.py --profile x360 --vid 045E --pid 028E` |
| X2 | Windows 10's own Xbox 360 controller driver | Windows 10 22H2 x64 | 10 22H2 x64, Windows' UHCI | as X1 |

## Per simulator

### `axemu.py` (ASIX AX88179)

| Reference driver | Version | OS | Reference result | Gaps found | Model changed | Our driver retested |
|---|---|---|---|---|---|---|
| ASIX (A1) | 1.4.2.0 | XP SP3 x86 | FAIL as found; PASS after the fixes: binds, DHCP, `ping` 32 to 8000 bytes, cable flap ("Hardware error" while out, then replies), removal and replug | six, below | yes | 98 SE, NUSB EHCI: PASS. XP SP3, Windows' EHCI (`AX179F.INF`): PASS |

What A1 showed, and what changed:

- The driver polls PHY register 2 (PHYSID1) about every 60 ms for 2 s
  until it is not 0. The model answered 0. It now answers 0283:BC30
  (INFERRED: any id other than 0; the real id is not known here).
- It reads the eFuse (request 0x05, wIndex 0x20, 64 bytes) and EEPROM
  words 0x3c and 0x05, and writes requests 0x06 and 0x91 with no data.
  All were STALLed. The model now answers a blank eFuse, reads other EEPROM
  words as FFFF, and accepts 0x06 and 0x91 with no effect (all INFERRED).
- It writes RX_CTL as 0x0098 and 0x8800, without IPE (0x0200). The model
  always put two alignment bytes before each received frame, so the driver
  took nothing, and DHCP failed. The two bytes now come only with IPE.
  Linux and `ax179f` set IPE, so they see no change.
- Its transmit form differs from Linux's. Bit 31 is set in the first
  header dword. Each frame is padded to 4 bytes, and several records
  share one bulk-OUT transfer (4140 bytes for the three fragments of a
  4000-byte ping). The model took only Linux's exact form, so it stopped
  on the first frame. It now takes both forms, and Linux's stays checked
  exactly.
- Replies lost: pings whose reply came back in a transfer holding an even
  number of records (3000, 3500, 4000, 6000 and 7000 bytes) got no answer,
  while odd counts worked. The model's transfers then ended 4 bytes short
  of a multiple of 8. The header array is now padded to 8 bytes before the
  trailer, and every size answered. Linux and `ax179f` read the headers'
  offset from the trailer, so the padding does not affect them: `ax179f`
  answered 4000 and 8000 bytes in both retests.
- Host tests: `axsimtest.py` 20 (5 new: the bring-up reads, an exact-fit request, the transmit
  form, IPE, the padded header array), with `ax179f`'s own cores.

### `ax7emu.py` (ASIX AX88772B/C, and SR9800)

| Reference driver | Version | OS | Reference result | Gaps found | Model changed | Our driver retested |
|---|---|---|---|---|---|---|
| ASIX (A2) | 5.4.3.0 | XP SP3 x86 | FAIL: binds, "Media disconnected" | the device is left unconfigured (below); 6 unknown requests | yes (vendor requests in any state) | 98 SE, NUSB EHCI: PASS (after all the changes in this table) |
| ASIX (A3) | 5.12.12.0 | 7 SP1 x86 | FAIL: binds, link up, sends DHCP DISCOVER and receives OFFERs, but never sends REQUEST; Code 43 after about 2 minutes | READ_IPG012 and SW_PHY_STATUS missing; requests Linux does not name; another transmit form | yes | as above |
| (SR9800) | - | - | no maker's driver: CoreChips' download centre lists only the SR9900 series (2026-11-06), so this part stays on Linux facts only | - | - | `test_sr9800` host test still passes |

What A2 and A3 showed, and what changed:

- A2 sends SET_CONFIGURATION 1, then SET_CONFIGURATION 0 within 100 to
  200 ms, on every plug and with KMDF 1.7 or 1.9 installed. After that it
  sends its whole bring-up on endpoint 0, with no interrupt or bulk
  transfer. The model STALLed vendor requests while unconfigured, and now
  answers them in any state (INFERRED: a driver made for real adapters
  sends them that way, but this run still failed, so it is not shown).
  Even so, nothing moves without a configuration. ASIX's
  AX88179 driver (A1), on the same guest and stack, did not deconfigure.
  Why this driver does is not known; the cause may lie in what the model
  presents, but nothing in the traffic shows what.
- A2's bring-up also used requests the model STALLed: c0 21 (one byte),
  40 2E 0307 and 0003, 40 2C 0319, 40 26 003F, and 40 24 with four zero
  bytes.
- A3 reads READ_IPG012 (0x11, three bytes) right after writing the gaps,
  and stopped when it was STALLed. The model now answers it (asix.h names
  it), along with SW_PHY_STATUS (0x21, also in asix.h). It now accepts
  requests 24, 26, 2C and 2E and reads 2D as zeros (INFERRED: no
  specification or open driver gives their meaning).
- A3's transmit header complements only the 11 length bits, sets bits
  11-15 of the length half on some frames (06A9:4156 for a 342-byte DHCP
  request, 07B1:804E), and packs several records back to back into one
  transfer, each starting on an even offset. The model took only Linux's
  form, which complements all 16 bits and sends one record per transfer.
  It now takes both, and Linux's stays exact.
- A3's DHCP request in a flagged record reached the model with a bad UDP
  checksum. For a flagged record, the model now fills in the IPv4 header
  and the UDP or TCP checksum, as checksum offload would (INFERRED: which
  flag asks for which is not known). Four unflagged 54-byte IPv4 frames
  still arrived with a bad header checksum.
- Not explained: A3 ignores the model's DHCP OFFERs. Making the receive
  header complement 11 bits wide, like its transmit form, did not change
  that, so the model keeps Linux's receive header. The cause is not found.
  It may be a receive status the driver expects in the header, or the
  checksum status of receive offload. With no specification, the receive
  side is left as Linux describes it.
- Host tests: `ax7simtest.py` 16 (3 new: vendor requests while
  unconfigured, the requests A3 sends, A3's transmit form), with
  `ax772f`'s own cores. `ax772f` writes the inter-packet gaps as Linux
  does (IPG0 | IPG1 = 0x1D, IPG2 0x12). A3 writes IPG0 0x15, IPG1 0x0C
  and IPG2 0x0E, and A2 0x15, 0x16 and 0x1A.

### `rtlemu.py` (Realtek RTL815x)

| Reference driver | Version | OS | Reference result | Gaps found | Model changed | Our driver retested |
|---|---|---|---|---|---|---|
| Realtek (R1) | 5.23.0123.2018 | XP SP3 x86 | RTL8152: PASS. RTL8153: FAIL as found, the device reset in a loop (24 configurations); PASS after the fix (DHCP, ping). RTL8153B: PASS after the fix | one: PHY writes by byte lane | yes | 98 SE, NUSB EHCI (RTL8153): PASS (DHCP, ping 32 to 4000, cable flap). XP SP3, Windows' EHCI (`RTLXP7.INF`, RTL8156): PASS |
| Realtek (R2) | 7.61.20.1102 | 7 SP1 x86 | RTL8156: PASS (DHCP, ping 32 to 4000) | none | no | - |
| Realtek (R3) | 10.68.20.0605 | 10 22H2 x64 | RTL8153: PASS. RTL8156: binds, DHCP. RTL8156B (id 7410): FAIL, Code 10 | RTL8156B, below | no | - |
| Windows 10 (R4) | 10.5.920.2015 | 10 22H2 x64 | RTL8153: PASS (bound before R3 was added: DHCP) | none | no | - |

- R1 asks for the PHY patch by writing only the low byte of PR_PATCH_CMD
  (40 05 B820, wIndex 0101, data 10). It then polls PR_PATCH_STATE (B800)
  for PATCH_GRANTED for about 1.4 s, and resets the device when the bit
  never comes. The model ran its side effects only on a write of a PHY
  word's high byte. It now makes one PHY write per word, whichever byte
  lanes carry it. Linux and `rtl815x` write whole words, so they see no
  change. The new host test also checks that one word write still counts
  one autonegotiation restart.
- R3 on the RTL8156B reads 13 registers and writes three, the same as on
  the RTL8156 apart from USB B460. It then sets the configuration and
  sends nothing more, and Windows reports Code 10. Every request was
  answered; the traffic does not show which value it refused. Not fixed.
- R1 polls MAC E908 (MR_LINK) every 0.5 s and reads the 64-byte tally
  counters at E890.
- The simulated host does not fragment its answers in `rtlemu.py` (it
  does in `axemu.py`): replies to pings above 1472 bytes come back as
  1472 bytes. This is the test host, not the chip, and is not changed.
- Host tests: `rtlsimtest.py` 31 (1 new), with `rtl815x`'s own cores.

### `xpademu.py` (Xbox 360 profile)

| Reference driver | Version | OS | Reference result | Gaps found | Model changed | Our driver retested |
|---|---|---|---|---|---|---|
| x360c (X1) | 6.1.7600.16385 | XP SP3 x86 | PASS: binds, Game Controllers "OK", axes and buttons follow the pattern | none needed | `--vid`/`--pid` added only (default unchanged) | not needed: earlier legs stand |
| Windows 10 (X2) | inbox | 10 22H2 x64 | PASS: binds, Game Controllers "OK", buttons follow the pattern | two vendor reads STALLed (below) | no | not needed |

- The x360 profile presents 2DC8:3106, an 8BitDo id that neither reference
  driver lists, so `--vid 045E --pid 028E` (the Microsoft pad's id) was
  added for these runs. Without the options the model is as before.
- X1 sends no start request (C1 01 0100), an LED command 0, and a rumble
  report of zeros about every 16 ms. Moving the force-feedback slider in
  Game Controllers sent no non-zero rumble.
- X2 sends the start request, LED command 2, `02 08 03` on the OUT
  endpoint, C1 01 0000 (eight bytes) and C0 01 0000 (four bytes). The model
  STALLs the last two and the pad still works. What a real pad answers is
  not known, so they stay unmodelled.
- The model waits only 5 s for QEMU's usbredir hello. The rig's separate
  `chardev-add` and `device_add` calls missed it once. That is a rig
  matter: one QMP session fixed it.

### `absemu.py` (`penpartner` profile) and `sremu.py`

- PenPartner (056A:0000): Wacom's driver 4.78-6 is no longer on Wacom's
  site (its old URL leads to the driver search). The owner supplied
  PenTablet 5.2.4-5, whose INFs, read as an archive, list Wacom ids from
  056A:0010 on, not 056A:0000. No maker's driver was run; the profile
  stays on QEMU's model and Linux's facts.
- SR9700 (`sremu.py`) and SR9800 (`ax7emu.py --chip sr9800`): no
  maker's driver, as above. These stay on Linux facts only.

## Not run, and what is left

- ASIX AX88772B/C (A2, A3): the remaining gaps above. A real adapter's
  traffic under A3 would settle them. Followed up on 2026-11-07
  (`2026-11-07-ax88772-model.md`): the receive header's extension, one
  record per transfer, BMCR's resolved mode and transmit offload; A2 and
  A3 then pass DHCP, ping and TCP, and A3's Code 43 is its own check
  ("Non-authentic ASIX product."), not imitated.
- Realtek R3 with the RTL8156B: the Code 10 cause.
- Our drivers against the changed models under xHCI98, on ME, and on 2000
  were not rerun. Most of the model changes are paths our drivers do not
  take: the makers' transmit forms and requests our drivers do not send.
  Three touch paths they do take: the AX88179 receive aggregate (the
  padded header array and its size check), and every PHY window write in
  `rtlemu.py`. Those ran in the 98 SE and XP retests above and in the host
  tests, which run our drivers' own cores against the changed models.
- Under bulk TCP load, no aggregation numbers were taken; only ping and
  DHCP ran.

## Evidence

Development host A (not tracked; removed after this record): the pcaps
and simulator logs named in the tables, the guests' traces, and the
screenshots. The makers' packages are in usb-wdm-nonfree (A1-A3, R1,
R2, X1) or were supplied by the owner (R3, PenTablet). None of their
binaries is tracked or was read.
