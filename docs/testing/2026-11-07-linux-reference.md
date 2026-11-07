# The simulators against Linux's own drivers, 2026-11-07

Several of this repository's simulated devices stand in for chips that no
maker's or Windows driver can be run against here (the CoreChips SR9700
and SR9800 have none at all). Linux has a driver for each, so a Linux
guest was booted and Linux's own drivers were run against the simulators
over usbredir, the USB traffic was recorded, and a simulator was changed
wherever Linux's traffic showed it answering wrongly or not at all. Our
own drivers were then run again against the changed simulators on 98 SE
and XP.

Linux's source was read for facts only (request numbers, structure
sizes, what the driver does with an answer); no code was taken. No
physical device was used and none is claimed: every result here is a
simulated chip, and no README "Tested by the author" cell changes.

## Vehicle

- Guest: Debian 13 "generic" cloud image (`debian-13-generic-amd64.qcow2`
  from cloud.debian.org, SHA-512 `1207e6e3efaff8ee...`, as its published
  `SHA512SUMS`), Linux `6.12.115+deb13.1-amd64`, booted from a fresh
  overlay with cloud-init (NoCloud). The facts were read from the same
  version's source (`linux-source-6.12`) and from v7.3.
- Stock QEMU 11.1.0, q35 machine, two vCPUs, guest networking user-mode
  only. `qemu-xhci`, `usb-ehci` and `piix3-usb-uhci` side by side, so that
  a simulator could be put on xHCI (whole transfers), EHCI or UHCI (Full
  Speed, where each transfer descriptor is its own usbredir packet).
- Each simulator ran as a new process per plug, attached by `usb-redir`
  with QEMU's `pcap=` capture; `usbmon` (text, all buses) and `dmesg` were
  captured in the guest for each leg.
- Network legs: `systemd-networkd` DHCP on the adapter (a lease of
  192.168.77.2 from the simulated host), then ICMP echo with a checked
  pattern at every payload size from 0 to 1472 (step 1, 3, 5 or 7 by leg)
  and at 2000 to 8000 bytes (fragmented), a 20-second flood ping at 1400
  bytes, `ethtool`, a cable flap where the simulator has one, removal under
  a running `ping`, replug, and runtime autosuspend (`power/control` auto,
  interface down, `runtime_status` suspended, interface up, traffic) where
  the driver supports it. Linux's `ping` compares each answer with what it
  sent ("wrong data byte", "truncated").
- Serial legs: a loop-back test (Python `termios` in the guest) per port:
  rates 300 to 921600 (by part), 8N1 everywhere and 7E1, 7O2, 8N2, 6N1, 5N1
  at 9600 and 115200, a patterned block per setting compared byte by byte,
  the rate read back, DTR to DSR and RTS to DCD (and CTS) through the
  simulated plug, and a break sent and looked for (`PARMRK`/`INPCK`, and
  `TIOCGICOUNT` where the driver has it).
- Pointer legs: `evtest` on each input node while a fixed script drove
  `absemu.py`'s control port (three positions, the tip or buttons, the pen
  switches and pressure).

## Results

"Before" is the simulator as merged in `w47-rel019` (9bd7eab); "after" is
this branch.

| Simulator | Linux driver | Before | Mismatch found | After |
|---|---|---|---|---|
| `sremu.py` SR9700 (Full Speed) | `sr9700` | xHCI: binds, works. UHCI: the simulator died at the first DHCP frame ("transmit header 333, transfer 64") | Linux on UHCI sends a transfer one 64-byte packet at a time; the simulator took the first packet as the whole frame | UHCI and xHCI: lease; sweep 0..1472 (all sizes) and 2000..8000, no loss, no wrong data; flood 1 loss in 4047 (the last in flight); flap; removal under ping and replug, ping resumes |
| `ax7emu.py --chip sr9800` | `sr9800` | High Speed (EHCI, xHCI): all pass, autosuspend and resume pass. Full Speed on UHCI: dies as the SR9700 did | the same packet-at-a-time gap | not changed here (`ax7emu.py` is another task's); reported |
| `axemu.py` AX88179 | `ax88179_178a` | SuperSpeed works but: a 1024-byte boundary frame (`ping -s 974`) killed the simulator ("header length 1016, transfer 1025"); request 0x05 wIndex 0x40 stalled ("Failed to read reg index 0x0040: -32"); `ethtool` read 10 Mb/s half duplex; Full Speed on UHCI died at the first frame | usbnet's one pad byte on a packet boundary; `ax88179_check_efuse`'s eFuse read; ADVERTISE, LPA and 1000BASE-T status read 0; packet-at-a-time | SuperSpeed and Full Speed (UHCI): sweep 0..1472 (all sizes) and fragmented, no loss; flood 0 loss in 28778; `ethtool` 1000 Mb/s full duplex, partner 10/100 with pause; autosuspend and resume; removal and replug. High Speed on EHCI: see below |
| QEMU `usb-net` (ECM) | `cdc_ether` | binds ECM configuration 1; sweep and flood clean | none (QEMU's model, not this repository's) | - |
| `ncmemu.py` NCM | `cdc_ncm` | binds (with a neutral id, see below); every NTB logged `ntb_out_bad` "datagram at 186 not 0 mod 4"; UHCI: every NTB cut into 64-byte pieces; a 4000-byte echo answered with its first 1472 bytes ("truncated") | the remainder read for the payload, not the datagram; packet-at-a-time; the simulated host (`rtlemu.py`'s) neither reassembles nor fragments | both placements accepted and counted; NTBs joined; the host is `axemu.py`'s: sweep and fragmented sizes clean on EHCI and UHCI, flap, autosuspend, removal and replug |
| `chipemu.py` PL2303 H, HX, HXD, TA, TB, G | `pl2303` | the simulator died at the first open (GET_LINE_CODING before any SET) | no power-up line coding | type detected as Linux names it (H, HX, HXD, TA, TB, G); every rate and framing, modem lines: pass. Break: not seen (below) |
| `chipemu.py` FT232AM, FT232BM, FT2232C, FT-X, FT232H, FT4232H | `ftdi_sio` | all pass, break included | none | - |
| `chipemu.py` CP2101, CP2102, CP2104, CP2105, CP2108 | `cp210x` | data and modem lines pass; no break ever came back; CP2104/2105: "GPIO initialisation failed: -32" | the two-byte part-number probe answered with one byte (Linux then takes the part for one without event insertion); GPIO configuration reads stalled; no `EMBED_EVENTS` | every port passes, the break included (an event) |
| `acmemu.py` uno, pico, dual, and uno with id 1A86:55D4 | `cdc_acm` | with `--mirror`: all pass, the break included | none | - |
| `absemu.py` tablet, kvm16, kvm12, pointer, touch, touchmouse | `hid-generic` | positions exact (e.g. 0.25 of 32767 = 8192), buttons, tip | none | - |
| `absemu.py` mtouch | `hid-multitouch` | slots, tracking ids, positions, `BTN_TOUCH` | none | - |
| `absemu.py` pen | `hid-generic` | `BTN_TOOL_PEN`/`RUBBER`, `BTN_STYLUS`, pressure 512 of 1023 at 0.5 | none | - |
| `absemu.py` penpartner (056A:0000) | `wacom` | the feature-report mode switch, then report 2: X 0..5040, Y 0..3780, pressure, `BTN_TOUCH`, `BTN_STYLUS` | none | - |

### What was changed

- `sremu.py`, `axemu.py`, `ncmemu.py`: a bulk-OUT transfer ends with a
  packet shorter than the endpoint's maximum (a zero-length one included),
  or for NCM at `dwNtbOutMaxSize`, so a transfer that comes a packet at a
  time is joined up before it is taken. AX88179's Linux form (its header
  gives the length) is taken as soon as it is whole, and a lone pad byte or
  zero-length packet after a boundary frame ends it; the project's driver
  sends a boundary frame with no pad, which is taken as before. An accepted
  `SET_INTERFACE`, a reset or a data-interface restart drops a transfer cut
  off before its end; a joined transfer is bounded.
- `axemu.py`: usbnet's one zero pad byte after a boundary frame (flags
  0x80008000) is accepted; the eFuse read is answered at wIndex 0x40 (Linux,
  its length, as for `ACCESS_MAC`) as well as 0x20 (ASIX's XP driver); the
  PHY's power-up ADVERTISE 05E1 and 1000BASE-T control 0300, and while
  linked a partner LPA 45E1 (4461 at 10 Mb/s) and 1000BASE-T status 3C00 at
  1000 Mb/s (IEEE 802.3 clauses 22 and 28; INFERRED values), so that the
  MII view agrees with PHYSR. The SuperSpeed interrupt endpoint's companion
  has wBytesPerInterval 0 in the physical capture; Linux corrects it to 8
  with a warning, and it was left as captured.
- `ncmemu.py`: Linux's `cdc_ncm` places each datagram so that the payload
  after the 14-byte Ethernet header falls at `wNdpOutPayloadRemainder`
  modulo `wNdpOutDivisor` ("adjust TX-remainder according to NCM
  specification"); `cdcnet` places the datagram itself there. Both are
  accepted and counted (`tx_aligned_payload`, `tx_aligned_datagram`); which
  reading the specification intends was not settled here. The simulated
  host is now `axemu.py`'s, which reassembles a fragmented request and
  fragments a long answer.
- `chipemu.py`: the PL2303 answers `GET_LINE_CODING` before any
  `SET_LINE_CODING` (INFERRED 9600 8N1). The CP210x refuses `GET_PARTNUM`
  unless wLength is 1 (Linux's `cp2102_determine_quirks` takes a one-byte
  answer to its two-byte probe for a part without event insertion; `usbcom`
  and the makers' drivers ask with wLength 1); answers `GET_PORTCONFIG`
  (0x370C: 13, 15 or 0x49 bytes on the CP2104, CP2105, CP2108) with zeros
  and the CP2105's `GET_DEVICEMODE` (0x3711) with modem mode (INFERRED);
  and takes `EMBED_EVENTS` (0x15): each escape character in echoed data
  is sent as escape, 00, and a looped break comes back as escape, 01, the break status
  (0x10) and its NUL. Disabling the interface, a reset or a configuration
  ends event insertion.
- Host tests for each change (`sr9simtest.py`, `axsimtest.py`,
  `ncmsimtest.py`, `chiptest.py`); all pass.

### Left as found

- SR9700 PHY: Linux 6.12 still reads PHY registers through EPCR/EPAR/EPDR,
  which v7.3 no longer does (it reports 10 Mb/s half duplex fixed). The
  simulator has no PHY there, so a read returns the last word written to
  EPDR and Linux 6.12 prints "100Mbps, full-duplex, lpa 0x05E1". No fact
  gives the chip's answer.
- PL2303 break: Linux flags a break on the data of the next read, so it
  sees one only if the chip sends a character with it. Neither Linux nor a
  public document says what the chip sends, so the simulator still sends
  the line status alone and Linux shows no break.
- AX88179 at High Speed on QEMU's EHCI: Linux's 24 KiB receive URB spans
  two qTDs; when an answer is split over two bulk-IN replies (an echo
  answered in fragments), QEMU's pipelined second packet belongs to a qTD
  that the first, short, completion has already ended, and its data is
  lost: every fragmented echo (2000 to 8000 bytes) and about 0.15 % of a
  flood. On xHCI and on the real bus the host issues no IN for that qTD.
  A host-side artefact; the project's driver (smaller transfers) did not
  meet it on XP.
- `ncmemu.py`'s default id, 0525:A4A2 (QEMU `usb-net`'s, which the
  `cdcnet` test INFs bind), is in Linux's `cdc_subset` table, which claims
  the data interface first ("failed to claim data intf"); the Linux legs
  used `--vid 1209 --pid 0001`.
- CP2101: Linux refuses 7-bit and 2-stop framings on it (the part's own
  limits); CP2105 interface 1: Linux's minimum rate is 2400 and it refuses a
  break there.
- `cdc_acm` reports CTS always on (ACM has no CTS bit).
- CH9102 (1A86:55D4): no entry in Linux 6.12's `cdc-acm.c` (1A86:55D3 has
  one, `MISSING_CAP_BRK`); `cdc_acm` binds it by class, as the leg with
  that id shows. Linux's `ch341` serves 1A86:5523, 7522 and 7523 only.

### Compared with Prolific's own driver

Prolific's 4.3.0.0 driver gave every variant Code 10 after reads of 0x8C to
0x8F and 0x9494 (older parts) or request 0x83 (the G)
([serial reference](2026-11-06-serial-reference.md)). Linux needs none of
those: it tells the types apart by bDeviceClass, bMaxPacketSize0, bcdUSB
and bcdDevice, and by whether a read of 0x8080 succeeds (TA and TB answer,
HX and HXD stall), and every variant started.

### The SR9800 and AX88772B/C (`ax7emu.py`)

Run again against `w47-ax7` a5872fd's `ax7emu.py`: `asix` (772B, 772C) and
`sr9800` at High Speed on EHCI pass (bind, link 100 Mb/s full duplex,
lease, sweep, flood). At Full Speed on UHCI it still dies at the first
frame ("transmit transfer 64 bytes, expected 337"). Linux names the 772C
"ASIX AX88772B USB 2.0 Ethernet" (one id for both).

## Our drivers against the changed simulators

| Leg | System | Result |
|---|---|---|
| `sr9700`, SR9700 on xHCI98 (Full Speed) | 98 SE (net9x base: NUSB 3.3, xHCI98 3.1.0.0), fresh overlay; `sr9700f.sys` `673ce035...` (w47-rel019 9bd7eab, ndis5 free) | installed from the floppy, restart; lease; `ping` 4/4 at 1472, 4/4 at 4000, 3/3 at 20 (a boundary frame, `tx_padded` 3); no simulator error |
| `ax88179`, AX88179 at SuperSpeed on xHCI98 | 98 SE, as above; `ax179f.sys` `ec7cc867...` | installed, restart; lease; 4/4 at 1472, 4/4 at 4000, 3/3 at 974 (1024-byte boundary, the driver's own unpadded form) |
| `cdcnet`, NCM on NUSB EHCI | 98 SE, as above; `cdcnet.sys` `ac5b164f...` (`CDCT98.INF`) | installed, restart; lease; 4/4 at 1472; at 4000 "bytes=1472 (sent 4000)" with the old host, 4/4 "bytes=4000" after the host change; no `ntb_out_bad` |
| `usbcom`, PL2303 HX, H, G, TA on xHCI98 | 98 SE (w98-x32 base, xHCI98 3.2.0.0), fresh overlay; `USBCOM.SYS` `5327a293...`, `USBCOMV.VXD` `9ea71dd6...` (trace flavour) | `COMTEST full` 66/66 and the pull-while-open hold 6/6 on each; the simulator's start audit in order |
| `usbcom`, CP210x | 98 SE, as above | not run: the fresh overlay's wizard did not install `USBCOM.INF` for 10C4:EA60 ("Unknown Device"); `usbcom`'s CP210x requests in the earlier 98 SE and ME legs are none of the changed ones (`GET_PARTNUM` only with wLength 1, no 0x15, no 0x370C/0x3711) |
| `sr9700`, SR9700 on UHCI (Full Speed) | XP SP3 x86 (q35 base, Windows' own EHCI/UHCI), fresh overlay; `sr9700f.sys` `a9ac5b13...` (w47-rel019 9ec0a00, ndis5 free) | the wizard took `SR9700F.INF` ("Continue Anyway"); lease; 4/4 at 1472, 4/4 at 4000, 3/3 at 20 and at 84; `tx_padded` 6: the packet-at-a-time path, which the old simulator could not take |
| `ax88179`, AX88179 at High Speed on EHCI | XP, as above; `AX179F.SYS` `3643d0df...` | lease; 4/4 at 1472 and 4000, 3/3 at 462 and 974 (512 and 1024-byte boundaries) |
| `cdcnet`, NCM on EHCI | XP, as above; `cdcnet.sys` `ac5b164f...` (`CDCTEST.INF`) | lease; 4/4 at 1472, 4/4 at 4000 ("bytes=4000"), 3/3 at 20 |

`usbcom`'s chip modules have no XP build, so there is no XP serial leg.
**PASS**: no regression on 98 SE or XP.

The changes were reviewed and revised until no material finding was left.
