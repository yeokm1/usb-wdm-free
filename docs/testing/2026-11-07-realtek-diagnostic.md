# Realtek's diagnostic, a real RTL8157 and rtlemu.py, 2026-11-07

xHCI98 Phase 47 (owner, 2026-11-07). Realtek's Ethernet Diagnostic Utility
2.0.8.3 and Realtek's Windows 10 driver 10.68.20 were run inside throwaway
guests, against a real RTL8157 passed through to the guest and against
`rtlemu.py`. The USB traffic was recorded with QEMU's `pcap=` option on the
`usb-host` and `usb-redir` devices. `rtlemu.py` was changed where the
traffic showed the model answering differently from the device or from what
Realtek's driver waited for. usb-wdm-free's `rtl815x` was then run against
the changed model on 98 SE and XP. Nothing of Realtek's is tracked; its
binaries were only run inside the guests, never read. This continues
`2026-11-06-reference-drivers.md`.

## Rig

- Stock QEMU 11.1.0, TCG, headless, on development host A.
- Windows 10 22H2 x64: an overlay of the read-only q35 base, Windows' own
  xHCI driver on `qemu-xhci`. Realtek's driver was put in the driver store
  with `pnputil /add-driver /install`. The diagnostic was installed with its
  own silent setup from a copy in the guest.
- XP SP3: the q35 base on `qemu-xhci` under xHCI98 3.4.1.0 as released;
  `rtl815x` 0.2.1.0 as released (`RTLXP7.INF`) by WDK 7.1's `devcon update`.
- 98 SE: xHCI98 3.2.0.0 on `qemu-xhci` (the `w98-x32` base of an earlier
  run), `pc` machine; the driver from a virtual floppy through the hardware
  wizard.
- The real RTL8157 (chip id `1030`, `0BDA:8157` rev `3000`) was bound to
  WinUSB on the host and given to one guest at a time by `usb-host`, as in
  `2026-11-07-rtl8157-hardware.md`. Its cable went to a 10GbE port of the
  host. No host setting was changed.
- No real RTL8156, RTL8156B or other Realtek adapter was available; every
  other chip here is simulated only.

## What the diagnostic offers

With Realtek's driver bound, the diagnostic lists the adapter. Its pages:

| Page | What it showed on the real RTL8157 | Traffic it caused |
|---|---|---|
| General | address, IP, link 5000 Mb/s full duplex, VID/PID, bcdDevice `3000`, port | none of its own (the driver's cached values) |
| Driver, Advanced Settings | driver 10.68.20.605; the INF's advanced properties | none |
| Diagnostics | Register, MAC Loopback, PHY Loopback, FIFO tests (Start Test) | not run on the real device (below) |
| Send and Receive | a broadcast ping-pong between two computers running it | not run (it needs a second computer) |
| Statistics | packets sent and received, errors | none |
| Cable | per-pair length (5 m each, "Normal") while linked | one PHY reset and renegotiation (BMCR `0x9240`) |
| Wake On LAN | sends a magic packet | not used |
| EEE Status | local and link-partner EEE abilities (partner 1000BASE-T and 100BASE-TX) | none |

The Diagnostics tests were not run on the real RTL8157: the session's
permission check refused that step, as a write test on real hardware.
They were run against the simulated RTL8156 instead: Register, MAC
Loopback, PHY Loopback and FIFO each passed once, and the utility
reported no hardware error. The model's Cable page reads 0 m per pair and its EEE page no
partner ability, where the device read 5 m and 1000/100. Those values
come from PHY registers the model does not fill in. They are left as they
are.

## The real RTL8157 and the model

Captures of the device: Windows 10 with no Realtek driver (Windows has
none for it: Code 28), Realtek 10.68.20 with the diagnostic's pages, an
unplug and replug, and `rtl815x` 0.2.1.0 on XP. From them `rtlemu.py`'s
`--chip 8157` profile now answers as the device did (`REAL8157`):

- Descriptors at SuperSpeed: configuration attributes `0x80` (bus powered,
  no remote wakeup), bMaxPower `0x44`, bulk bMaxBurst 7, an interrupt
  endpoint of 16-byte packets with bInterval 11, the BOS values, and three
  configurations. The model describes configuration 2 (CDC NCM) and 3 (CDC
  ECM), as xHCI98 read them from the device, but runs only configuration
  1. The serial number, when `--serial` gives one, has string index 7 as on
  the device, and `--serial mac` gives the device's form (`0003` and the
  address). The address in every table is the model's own.
- MR_TX_CONFIG's high word reads `0x1330`, not `0x1030`: under Linux's mask
  `0x7cf0` both give the id `1030`.
- The PHY OCP window at USB `D630`/`D634`. Realtek's driver reaches the
  RTL8157's PHY only through it: the address in D630's low word, the data
  in its high word, a command at D634 (1 read, 3 write), done at once. The
  model had no such window, so the driver waited for each command and reset
  the device in a loop (Code 31).
- First reads: the first value the drivers read from each register byte
  before writing it, for the MAC, USB, PHY, PHY SRAM and ADV spaces. Only
  bytes read with their byte lane enabled count (the device returns stale
  data in the others), and only where two Realtek driver runs read the
  same. They are the state left by earlier drivers, not a cold reset,
  which could not be done. Three are overridden on purpose so that a
  driver must still clear them, as on the other newer profiles: the
  transmit signature check (USB `0xd4ae` bit 1), the U2P3 bit of IP
  `0xc2c0` (bit 29) and the receive zero-length bit of BMU `0x2300`
  (bit 3). The device read them clear because earlier drivers had cleared
  them; Realtek's driver and `rtl815x` both clear them.
- Bits that clear themselves after a write sets them: MAC `0xdc6a` bit 4,
  MAC `0xe800` bit 0, USB `0xd4e8` bit 2, `0xe434` bit 4, `0xd437` bits 0-1,
  the ADV window's whole command word, and BMU `0x0050`, `0x2350` and
  `0x2450` after a write with bit 0.
- MR_LINK keeps bit 7 set, reads `0x10e3` at 5 Gb/s with `0x0009` above
  it, and on a link down only the link bit goes.
- The interrupt endpoint answers every poll with two bytes (`ec 12` linked,
  `ea 12` not). The model sends one report per bInterval, each showing the
  link as the report before did: the device's report changed a poll after
  MR_LINK. `--irq-on-change` gives one report per change, as before.
- For every chip, the usbredir endpoint information now carries the
  interrupt endpoint's real size. With a 2-byte size QEMU joined eight
  reports into each 16-byte transfer.

The device's PHY state register `0xa420` reads `0x0023`, so for this
profile the model keeps bit 5 set above its own state bits.

### Side by side

The control sequences were aligned request by request (wValue, wIndex,
length, written data), and the answers compared over the enabled byte
lanes only.

| Driver, system | Real RTL8157 | `--chip 8157` model | Same requests |
|---|---|---|---|
| Windows 10 inbox | no driver, Code 28 | not run (the same id) | - |
| Realtek 10.68.20, Windows 10 | Started, link 5000, DHCP | before: Code 31 (OCP window). After: Started, link 5000 read, but "Media disconnected": the driver never takes the link up (open, below) | bring-up 0.96 of the controls the same; differences in register values left by earlier drivers |
| Realtek diagnostic, Windows 10 | pages as above | not run against the 8157 model (it shows no link); run on the 8156 model | - |
| `rtl815x` 0.2.1.0, XP (xHCI98) | binds, link 5000 | binds, link 5000, DHCP, ping 1400 x4 | 0.92 of the controls the same |
| `rtl815x` (w47-tcp98 `b06feb2`), 98 SE (xHCI98) | 300 s TCP both ways, unplug and replug: below | binds, link 5000, DHCP, ping 1400 x4 | - |

## The RTL8156B Code 10

Cause: the bcdDevice. Realtek's INFs choose a chip's install section by
the high byte of the device revision. The model gave every chip `0x3000`,
so 10.68.20 ran its RTL8156 section on an RTL8156B (chip id `7410`), read
13 registers and failed with Code 10. With `0x3100` the RTL8156B section
loads. It then set MAC `0xdc6a` bit 4 and waited for it to clear, and reset
the device in a loop; then it polled USB `0xd4e8` bit 2 the same way. With
both bits clearing themselves, as on the real RTL8157, the device is
"Started" with no problem code.

Still open: under 10.68.20 the RTL8156B, RTL8153B (`0x3100`), RTL8153C
(`0x3200`) and RTL8157 models report "Media disconnected". The driver
reads MR_LINK as up after each interrupt report, but never starts its
link-up configuration. The RTL8153 and RTL8156 (both `0x3000`) link and
get DHCP under the same driver, and Windows 10's own driver links the
RTL8153 and RTL8153B. On the real RTL8157 the same reads were followed at
once by the configuration. Nothing in the traffic shows what decides it; a
real RTL8156B or RTL8153B under 10.68.20 would.

## Model changes

Commits on branch `w47-rtldiag` (from `w47-rel019` `2759e91`, which has
`w47-ref` and `w47-r57` merged in): `dadc3fb`, `2efb2c6`, `00dd6d3`
(reviewed and revised).

- bcdDevice per chip, from Realtek's INFs' revision map: `0x2000` RTL8152,
  `0x3000` RTL8153, RTL8156, RTL8157, RTL8159, `0x3100` RTL8153B and
  RTL8156B, `0x3200` RTL8153C (inferred from the order), `0x3300` RTL8153D.
  Only the RTL8157's is read from a device.
- `--vid`, `--pid`, `--bcd-device` put a profile behind another id (an
  OEM's or a dock's).
- New profiles, so each per-version path in `rtlchip.c` has one:
  `8152-4c00`, `8152-4800`, `8153-5c00`, `8153-5c20`, `8153-5c30`,
  `8153b-6000`, `8153c` (id `6400`), and `8153d-8153` (the RTL8153D under
  `0BDA:8153`). `rtl815x`'s own core brings each of the family-A ones up
  in the host tests.
- The RTL8157 profile as above.
- For the newer chips (RTL8153D and later): MAC `0xdc6a` bit 4 and USB
  `0xd4e8` bit 2 clear themselves (inferred from the RTL8157).
- For every chip: the BMCR autonegotiation restart bit clears itself (IEEE
  802.3 22.2.4.1; the RTL8157 read it back clear); GET_STATUS reports the U1
  and U2 enables a host set, and a USB reset clears them.
- Host tests: `rtlsimtest.py` 36 (4 new), all passing with `rtl815x`'s
  cores.

## Our drivers against the changed model

| System | Profile | Result |
|---|---|---|
| XP SP3, xHCI98 3.4.1.0, `rtl815x` 0.2.1.0 | 8157 (3 configurations) | PASS: configuration 1, link 5000, DHCP, ping 1400 x4 |
| same | 8156b (`0x3100`) | PASS: link 2500, DHCP, ping |
| same | 8156 | PASS: link 2500, DHCP, ping |
| 98 SE, xHCI98 3.2.0.0, `rtl815x` `b06feb2` | 8157 | PASS: link 5000, DHCP, ping 1400 x4 |
| same | 8153 at High Speed | PASS: link 1000, DHCP, ping |
| same | 8156b | PASS: link 2500, DHCP, ping |
| Windows 10, Realtek 10.68.20 | 8153, 8156 | PASS: link, DHCP |
| same | 8153b, 8153c, 8156b, 8157 | binds, "Media disconnected" (open, above) |
| Windows 10, Windows' own 10.5.920 | 8153, 8153b | PASS: link 1000, DHCP |

XP lists the RTL8153 only in `RTL815X.INF`, so the RTL8153 profiles ran
on 98 SE only. No `rtl815x` code change was needed, and no `rtl815x` bug
was seen.

## 98 SE with the fixed rtl815x on the real RTL8157

Owner's leg (2026-11-07): `rtl815x` from branch w47-tcp98 `b06feb2`
(ndis5 free), 98 SE, the real RTL8157, `NETPERF.EXE` in the guest and
`netperf.py` from the host over link-local addresses. The stock
passthrough path caps the rate (about 6 Mb/s out, 1 Mb/s in here).

| Step | Result |
|---|---|
| 300 s both ways | out 223.5 MB at 5.91 Mb/s, in 40.9 MB at 1.08 Mb/s, 0 mismatched bytes; no stall, no fault |
| unplug during both-ways traffic | the sessions ended (connection reset); the guest kept running |
| replug | the driver reloaded; a session 45 s after it timed out, one 1.5 minutes after it ran 60 s both ways: out 44.6 MB, in 8.6 MB, 0 mismatched |

## Evidence

Development host A (not tracked; removed after this record): the pcaps,
the model's logs and the screenshots. Realtek's packages came from the
owner. None of their binaries is tracked or was read.
