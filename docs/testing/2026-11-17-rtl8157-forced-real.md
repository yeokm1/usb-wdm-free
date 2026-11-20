# The forced speeds with the fixed driver on a real RTL8157, XP SP3 and 98 SE under xHCI98, 2026-11-17

The re-run owed by [netopts-rtl8157](2026-11-17-netopts-rtl8157.md) and
design [ethernet.md](../design/ethernet.md) 4.10.4, "`rtl815x`'s forced
sequence": `*SpeedDuplex` 6, 2500, 5000, 4, 3, 2, 1, then 0, with the
fixed driver, on the same **real** Realtek RTL8157 (USB `0BDA:8157`,
`bcdDevice` 3000, chip version `1030`) passed through to the same two
guests, the adapter not power-cycled between values. Also EEE around a
forced run, Advanced EEE at a forced 10 Mb/s link, a guest restart and a
disable and enable at a forced speed, and traffic at each forced value.
It is not a performance measurement: the rig is TCG emulation with an
emulated xHCI controller over the host's USB stack, and transfer figures
are given only where they show a duplex mismatch or a difference not
otherwise explained.

## Builds and rig

- Branch `netforce` at `4c9ef60` (the fix `e69e740` and its follow-ups
  `68c758e` and others; merged to `main`), built with
  `src\net\rtl815x\build.cmd`: `ndis5 trace` `rtl815x.sys`
  `ed6f4cf789cfee0594feb19572eb1a5801b887484cb5619a156a35cce13eab66`
  (test-only, writes the trace to QEMU's port 0xE9), used on both
  systems; `ndis5 free`
  `26ac364b6a2e12da6c181abadb21fffd70d378dda7f9a0a37a9f68c02a324ddd`
  (not run in a guest). INFs unchanged: `RTLXP7.INF` `5510426e...`,
  `RTL81598.INF` `5cbe8b40...`. Helpers: `NETSET.EXE` `4ea4db4e...`,
  `NETPERF.EXE` `04c289ff...`, Microsoft's `devcon.exe` `800e396b...`
  (throwaway XP guest only).
- The new image has the same file version as the one the guests had
  installed (netopts-rtl8157's), so it was copied over the installed
  `rtl815x.sys` (XP: with the adapter disabled, then enabled; 98 SE:
  then a restart); `fc /b` found no differences on both.
- QEMU 11.1.0 (stock), TCG, headless, `pc` machine. The adapter was
  attached by its vendor and product id only, with QEMU's `usb-host` on
  an emulated `qemu-xhci`, on the command line of each boot, with the
  `pcap=` capture; every string-descriptor reply in the captures was
  zeroed after the run. The register sequences below are from the
  captures, decoded with `src/net/rtl815x/rtlpcap.py regs`; the link
  lines from the trace.
- Guests: fresh overlays of netopts-rtl8157's two guests, XP SP3 under
  xHCI98 3.5.0.0 as released and 98 SE under xHCI98 3.1.0.0 (not the
  current release), each with a static link-local address.
- The link partner: the host port, left on automatic and not changed.
  Its speed and duplex were read from the host side at each value
  (read only). Traffic: pings and `NETPERF` fixed-pattern streams of
  20 s each way, checked byte for byte on the receiving side, every
  connection started from the host.

## The forced sequence on the real PHY

For each forced value, on both systems, the capture showed, in order:

1. the advertisement (MII register 4) written with the one forced
   ability and the pause bits (`*FlowControl` 3): 0x1D01 (100 full),
   0x1C81 (100 half), 0x1C41 (10 full), 0x1C21 (10 half); the
   multi-gigabit control (`0xa5d4`) written 0x0001 (2.5 and 5 Gb/s
   cleared, its other bit kept); the gigabit control (register 9) read
   0x0000, or written from 0x0200 to 0x0000 when an Automatic start came
   before;
2. BMCR **0x8000**, the reset bit alone;
3. BMCR read **0x1040**, the reset bit clear, at the driver's first poll
   30 to 40 ms after the write; no read with the bit still set was seen;
4. BMCR **0x2100 / 0x2000 / 0x0100 / 0x0000**, exactly, for 4 / 3 / 2 / 1.

The driver does not read BMCR while the link is up; the first read at
the next start showed the forced value, with no autonegotiation bit,
every time, and the stop's power-down write kept the forced bits (0x2900,
0x2800, 0x0900, 0x0800). `MR_LINK` (`0xe908`) at each forced link: 0x008B
(100 full), 0x008A (100 half), 0x0087 (10 full), 0x0086 (10 half): no
pause bit. Automatic afterwards wrote the full advertisement back
(0x1DE1, gigabit 0x0200, multi-gigabit 0x0181, then 0x9200) and the link
returned at 5000 full with pause both ways.

## Results

Every value linked at the speed and duplex set, on both systems. XP set
each value with `NETSET` and restarted the adapter (`devcon restart`);
98 SE, which applies a value only at a start, restarted Windows (the
adapter stayed attached and powered). Streams: nominal 20-second tests
(the sending side stops after 20 s by its own clock); each cell gives
the bytes moved, the elapsed time the host measured from the start of
the run to its result, and the rate on that elapsed time. Every stream
had no mismatched byte. The 98 SE host-to-guest runs at the forced
values ended 29 to 31 s after their start, when the guest reported its
result (the guest timed 27 to 29 s of receiving), and one
98 SE guest-to-host run took 52 s (that boot's guest clock ran slow).

| Value | Trace link (both) | The host port | XP: host to guest | XP: guest to host | 98 SE: host to guest | 98 SE: guest to host |
|---|---|---|---|---|---|---|
| 6 | 1000 full | 1 Gb/s, full | - | - | - | - |
| 2500 | 2500 full | 2.5 Gb/s, full | - | - | - | - |
| 5000 | 5000 full | 5 Gb/s, full | - | - | - | - |
| 4 | 100 full | 100 Mb/s, **half** | 21.5 MB, 21.1 s, 8.14 Mb/s | **7.2 MB, 20.8 s, 2.78 Mb/s** | 3.3 MB, 29.5 s, 0.90 Mb/s | 4.4 MB, 20.8 s, 1.69 Mb/s |
| 3 | 100 half | 100 Mb/s, half | 20.9 MB, 21.0 s, 7.95 Mb/s | 34.2 MB, 20.1 s, 13.60 Mb/s | 3.3 MB, 30.5 s, 0.87 Mb/s | 23.0 MB, 52.0 s, 3.53 Mb/s |
| 2 | 10 full | 10 Mb/s, **half** | 23.2 MB, 20.9 s, 8.87 Mb/s | **3.5 MB, 21.9 s, 1.27 Mb/s** | 3.3 MB, 29.6 s, 0.90 Mb/s | 2.9 MB, 20.9 s, 1.10 Mb/s |
| 1 | 10 half | 10 Mb/s, half | 21.8 MB, 20.9 s, 8.33 Mb/s | 15.2 MB, 20.1 s, 6.04 Mb/s | 3.3 MB, 29.4 s, 0.90 Mb/s | 6.4 MB, 20.3 s, 2.52 Mb/s |
| 0 | 5000 full | 5 Gb/s, full | - | - | 12.5 MB, 21.8 s, 4.59 Mb/s | 15.8 MB, 20.3 s, 6.22 Mb/s |

(MB here is 10^6 bytes.) The 98 SE value 1 row is the second run of
that value: the first attempt's setting was not applied (a keystroke
slip left value 2 in place), so it was a repeat of value 2 and is not
in the table.

Ten pings of ten were answered at every forced value on both systems.

**A forced full duplex against an automatic partner.** At 4 and 2 the
adapter has autonegotiation off, and the host port, left on automatic,
detected the speed and came up **half duplex**, which is what IEEE 802.3
parallel detection does against a partner that does not autonegotiate.
The link comes up and pings pass, but the full-duplex side's sending
collapses: on XP, 2.8 Mb/s guest to host against 8.1 the other way at
100 Mb/s, and 1.3 against 8.9 at 10 Mb/s, with 13 and then 36 more TCP
retransmissions in the guest; at 3 and 1 (half duplex) both ends agreed
and XP's guest counted no new retransmission. This is the expected
behaviour of a forced link against an automatic partner, not a fault of
the driver: a forced full-duplex value needs the other end forced the
same way. A matched forced full-duplex link was not run (the partner
was not to be changed). At 10 half the host side counted 40 transmit
errors and 40 transmit discards during the one run, which collisions on
a half-duplex link can produce; not looked at further.

**98 SE's host-to-guest stream at the forced values.** About 3.3 MB in
29 to 31 s, about 0.9 Mb/s, at all four forced values, whatever the
duplex on either side, against 12.5 MB in 21.8 s (4.59 Mb/s) at 5000 in
the same series; XP's ran at 8 to 9 Mb/s at the same values. The cause
was not investigated. The simulator
legs ([rtl815x-sim-legs](2026-11-18-rtl815x-sim-legs.md), "98 SE,
host-to-guest stream at each forced value") did not reproduce it: 23 to
30 Mb/s at every value there, the simulator not modelling the wire. The
trace's `tx .../5` (sends refused before the first
link at a start, as in netopts-rtl8157) appeared at every 98 SE boot.

### The rest of the leg

| Check | XP SP3 | 98 SE |
|---|---|---|
| Advanced EEE 1 at a forced 10 full link (value 2) | EEE+ (`0xe080`) written 0x5FC2, bit 1 set; 0x5FC0 at every 100, 1000 and 5000 link | the same |
| EEE 1 at value 4 | `0xe040` 0xFFC3, EEE advertisement `0xa5d0` 0x0006 and `0xa6d4` 0x0003, then the forced sequence; 100 full | the same |
| EEE 1 at Automatic | the same EEE writes and the full advertisement; 5000 full | the same |
| EEE 0 at Automatic | `0xe040` 0xFFC0, `0xa5d0` and `0xa6d4` 0; 5000 full | the same |
| A guest restart at value 4 | BMCR read 0x2100 at the boot, then 0x8000, 0x1040, 0x2100; 100 full; no hang | every value went through a restart; each forced one came back at that value |
| Disable and enable at value 4 | `devcon disable`, `enable`: halted, then 100 full; no hang | Device Manager, "Disable in this hardware profile" ticked and OK (halted, no restart asked), then unticked and OK: the forced sequence, 100 full; no hang |

## Where the real chip differed from the simulator

1. The real PHY's reset bit was already clear at the driver's first poll
   read. The corrected simulator (`3ad31d9`) reads it set once, then
   clear. The poll handled both.
2. The partner: the simulator links at whatever the adapter forces. A
   real partner left on automatic parallel-detects half duplex against a
   forced full-duplex value, and the full-duplex side's sending
   collapses (above).

Everything else the simulator legs checked (the reset written alone, the
forced bits written alone after it, only the forced ability advertised,
gigabit and multi-gigabit cleared, no pause at a forced link, Automatic
writing every field back, the chip keeping its registers between
starts) was seen the same way on the real chip.

## Not run

- Windows 7 with the real adapter (no installed base with xHCI98); 98 SE
  under the current xHCI98 release.
- A matched forced full-duplex link (a partner forced the same way).
- The cause of 98 SE's host-to-guest rate at the forced values.

## Evidence

`E:\usb-wdm-free\netopts\w7` (not tracked): `logs\e9-*.log` the traces,
`logs\*.pcap` the captures (string descriptors zeroed),
`logs\regs-*.txt` their decode, `logs\x-steps.txt` (XP) and
`logs\w-steps.txt` (98 SE) each step's registers, host-side view and
traffic, `logs\*-recv.txt` and `*-send.txt` the streams, XP's `netstat`
output in `stage\xpx\OUT\`, `shots\`, `STATE.md`, `pids.txt`. The guest
overlays of these legs and of netopts-rtl8157's were deleted after this
run (they held the adapter's instance id).
