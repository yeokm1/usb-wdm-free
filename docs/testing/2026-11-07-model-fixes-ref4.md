# The simulators fixed after the makers' drivers ran against them, 2026-11-07

The makers' own Windows drivers were run against this repository's
simulators in guests, capture only, and every place where a driver stopped
on the simulator's answer was listed (M1 to M18; the findings and their
evidence are outside the repository, in the reference-driver run of the
same day). This record says what was changed in each simulator, what was
left and why, and what the makers' drivers and our own drivers did against
the changed simulators.

The makers' drivers were only installed and run inside guests; their files
were not read, disassembled or debugged. What is recorded is the traffic on
the simulated device and what Windows reported. No physical device was
used, and no README "Tested by the author" cell changes. Facts beyond the
traffic come from Linux 6.12 (Debian's `linux` 6.12.113 source: `cp210x.c`,
`pl2303.c`, `sr9700.c`, `sr9800.h`, `asix.h`, `ax88179_178a.c`,
`wacom_wac.c`, `wacom_sys.c`), facts only; what neither gives is marked
INFERRED in the simulator.

## Changes

| # | Simulator | Before | Change |
|---|---|---|---|
| M3 | `chipemu.py` PL2303 | Prolific 3.8.31.1 (XP, 7) and 2.1.51 (XP) wrote 0x0404 = 1 and read 0x8484 for ever: the simulator read the write back (01) | 0x8484 reads 00 (INFERRED: what makes these drivers go on; Linux ignores the value) |
| M4 | `chipemu.py` PL2303 | reads asked for 2 bytes were answered with 1 | a read answers wLength bytes, the register then 00 (INFERRED) |
| M7 | `chipemu.py` PL2303 | 2.1.51's eight requests before SET_CONFIGURATION (on a replug) stalled | PL2303 requests are answered before SET_CONFIGURATION too (INFERRED) |
| M13 | `chipemu.py` CP210x | Silicon Labs 4.20 (2004) sets every rate with SET_BAUDDIV 0x01, which stalled; the port wedged | SET_BAUDDIV (rate = 3686400 / wValue, 0 refused) and GET_BAUDDIV 0x02 (Linux names both) |
| M14 | `chipemu.py` CP210x | RESET 0x11 at each open stalled | taken, nothing changes (INFERRED) |
| M15 | `chipemu.py` CP210x | SET_LINE_CTL with 1.5 stop bits and 6 to 8 data bits (0x0741) stalled and wedged 4.20 again | taken as 2 stop bits (INFERRED, the 16550 convention) |
| M16 | `chipemu.py` CP2105 interface 1 | 7N1 stalls; Silicon Labs 6.7.0.0 carries on | not changed: the part's own GET_PROPS offers 8 data bits only there, and no fact says the chip takes 7 |
| M5 | `axemu.py` | ASIX 7 1.12.11.0, 3.20.2.0, 4.20.1.0 and Windows 10's own 1.16.27.321 send records of an 8-byte header (bit 31 on ARP and some IPv6 only) and the frame padded to 4, several per transfer: fatal "transmit header length 342, transfer 352" | taken; Linux's exact form is still checked strictly, and a 4-aligned unmarked record that fills a packet may carry flags 0 or Linux's 0x80008000 |
| M6 | `axemu.py` | those drivers write TXCOE (0x35) 0x67 or 0x27 and leave the IPv4 header and UDP/TCP checksums to the chip: every DHCP frame refused | with TXCOE bits set (Linux's `AX_TXCOE_*`: IP 01, TCP 02, UDP 04, TCPv6 20, UDPv6 40) the IPv4 header and TCP/UDP checksums are inserted in an unmarked record or Linux's form (UDP to its own length); INFERRED: bit 31 means none; fragments get the IPv4 header checksum only. `ax179f` writes TXCOE 0, so nothing changes for it |
| M8 | `ax7emu.py --chip sr9800` | Supereal's SR9800 1.1.7.0 starts with SET_FEATURE(DEVICE_REMOTE_WAKEUP); it stalled (Code 10) | SET/CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP) taken; the configuration advertises remote wakeup (0xA0) |
| M9 | `ax7emu.py` | WRITE_MONITOR_MODE 0x1D stalled (Code 10) | 0x1D kept, READ_MONITOR_MODE 0x1C reads it back (`sr9800.h` names both) |
| M10 | `ax7emu.py` | records without flag bits arrived with blank checksums | already met by the merged `ax7emu.py`: records without bit 15 take TXCOE (0x2E), which the driver writes 0307 |
| M11 | `ax7emu.py` | c0 B8 0120 3000 polled every 15 ms, stalled | not changed: no source names it, and the driver works with it refused |
| M1 | `sremu.py` | CoreChips 1.00.0906.0 never resets the PHY and waits for the link: "Media disconnected" | the PHY links at power-on; a 0x1F reset (Linux, `sr9700f`) still takes the link down and up |
| M2 | `sremu.py` | an 8000-byte echo came back as one 6733-byte transfer of five packets, which that driver drops | a receive transfer holds whole packets up to 3072 bytes (INFERRED from `sr9700_bind`'s `rx_urb_size`, which `sr9700f` uses too) |
| M12, M18 | `absemu.py` PenPartner | no Wacom driver sends a mode switch; in its first mode the simulator sent four bytes without the report ID the descriptor declares (no HID parser takes them) and no movement; report 2 was one byte short of its declared length | report 1 as the descriptor declares it: ID, three buttons (the tip the left), relative X, Y, wheel, three constant bytes, moved by the pen (INFERRED: tablet units / 4, steps of at most 127); report 2 sent at its declared length (a 00 after the six bytes Linux reads) |
| M17 | `axemu.py` | ASIX 3.20.2.0 and 4.20.1.0 take no received frame | not changed, cause not found. Their receive set-up (RX_CTL 0x0000, 0x0088, 0x0090, 0x0098; medium 013B; RX_BULKIN_QCTRL 07 99 00 20 1F; RXCOE 67) is byte for byte that of Windows 10's own driver, which takes the same receive transfers. They differ only in requests no public source names: c0 81 0310 (4 bytes, stalled; answering it as written changed nothing), 40 08, 40 91 001C, a 2-byte write of 0x2A, a PHY write of register 0x19. With nothing to model from, it stays open |

Host tests: `chiptest.py`, `axsimtest.py`, `ax7simtest.py`, `sr9simtest.py`
and the new `abssimtest.py`, all passing.

## The makers' drivers again

Rig: stock QEMU 11.1.0, q35 bases (XP SP3 x86 one vCPU, 7 SP1 x86 two),
fresh overlays, Windows' own EHCI/UHCI; each simulator over usbredir;
drivers installed with `devcon update` from the packages of the reference
run.

| Driver | Part | Before | After |
|---|---|---|---|
| Prolific 3.8.31.1, XP | PL2303 HX | start never completed (0x8484 poll) | "Prolific USB-to-Serial Comm Port (COM3)", driver running, the Linux-order start sequence six times; `COMTEST -cts COM3 full` 58/62, the four FAILs the driver's own (9600 7S1.5 refused by it, EV_BREAK, EV_RXCHAR, EV_TXEMPTY masks), as in the reference run |
| Silicon Labs 4.20, XP | CP2102 | port wedged at the first SET_BAUDDIV | "CP2101 USB to UART Bridge Controller (COM4)"; every rate set by SET_BAUDDIV; `COMTEST -cts -ctshw -nobreakin COM4 full` 59/61 (EV_RXCHAR, EV_TXEMPTY), 9600 7S1.5 passes; the process no longer wedges |
| ASIX AX88179 1.12.11.0, 7 x86 | AX88179, High Speed | fatal on the first IPv4 frame | lease 192.168.77.2; `ping` 4/4 at 32, 2/2 at 1400, 4000 and 8000; 125 unmarked padded records, 128 IPv4 header and 133 UDP checksums inserted |
| CoreChips 1.00.0906.0, 7 x86 | SR9700, Full Speed on UHCI | "Media disconnected" | lease; 4/4 at 32, 2/2 at 1400, 4000 and 8000 |
| Supereal 1.1.7.0, 7 x86 | SR9800, High Speed | Code 10 | lease; 4/4 at 32, 2/2 at 1400, 4000 and 8000; only c0 B8 refused (M11) |
| none (Windows' own HID), 7 x86 | PenPartner | the pen never moved the cursor | the pen moves the cursor as a mouse, in the direction it moves (relative: `PTRTEST` saw it reach each screen edge it was moved towards); left, right and middle buttons reach Windows |

Not run again: Windows 10's own AX88179 driver and ASIX 3.20/4.20 (M17
unchanged), Prolific 2.1.51 (the same 0x8484 path), the Wacom drivers
(none sends anything to the tablet; the device stays on Windows' own HID,
whose mouse path is the leg above).

## Our drivers against the changed simulators

| Leg | System | Result |
|---|---|---|
| `usbcom`, PL2303 HX and TA, CP2105 | 98 SE (xHCI98 3.2.0.0), fresh overlay | `COMTEST full` 66/66 (HX, TA), 65/65 (CP2105 interface 0) and `quick` on interface 1, the hold test 6/6 each; the PL2303 start audit in order; no simulator error |
| `sr9700f`, SR9700 on xHCI98 | 98 SE (NUSB 3.3, xHCI98 3.1.0.0), fresh overlay | lease; 4/4 at 1472, 2/2 at 8000 |
| `ax179f`, AX88179 at SuperSpeed | 98 SE, as above | lease; 4/4 at 1472, 2/2 at 8000 |
| `ax772f`, SR9800 on NUSB EHCI | 98 SE, as above | lease; 4/4 at 1472, 2/2 at 8000; the checksums the driver asks of the chip inserted (TXCOE) |
| `absptr`, PenPartner | 98 SE, as above | the wizard offered "Wacom PenPartner tablet (absptr)"; `absptr` switched the tablet to mode 2; the pen at 0.25, 0.25 and 0.75, 0.6 put the cursor at 160, 120 and 480, 288 of 640 x 480 (screenshots) |
| `sr9700f`, SR9700 on UHCI | XP SP3 x86, fresh overlay | lease; 4/4 at 32, 2/2 at 1400, 4000 and 8000 |
| `ax179f`, AX88179 at High Speed | XP, as above | the same |
| `ax772f`, SR9800 at High Speed | XP, as above | the same |
| `absptr`, PenPartner (`absptr2k.inf`) | XP, as above | after the mode switch 6/6 points at the expected pixel, tip left, barrel right; before `absptr` was installed Windows' own HID took it and the pen moved the cursor as a mouse |

Driver images: the w47-rel019 builds of 2026-11-07 (`sr9700f.sys`
`a9ac5b13...`, `ax179f.sys` `f7d47d33...`, `ax772f.sys` `3e0dac47...`,
`absptr.sys` `e2062e90...`; `usbcom` the trace flavour of the serial
reference run). **PASS**: no regression on 98 SE or XP.

One 98 SE restart, the one after the AX88179 install, stayed at "Windows
is shutting down" for more than ten minutes and was ended; the cold start
that followed was clean, and the restarts after the SR9700 and SR9800
installs were not affected. Seen once; not traced to the simulator.

The changes were reviewed and revised until no material finding was left.
