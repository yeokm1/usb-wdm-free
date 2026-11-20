# Pause choices and receive checksums on the guests, with simulated RTL8157 and RTL8159, 2026-11-17

Guest legs for the two options of design [ethernet.md](../design/ethernet.md)
section 4.10.9: `*FlowControl` 0, 1, 3 (`49dff55`, `0c5cacc`) and receive
checksums on the RTL8157 and RTL8159 (`e19ad59`, `c393997`). Every leg ran
against the simulator (`rtlemu.py`); no physical adapter was used and
none is claimed. What the simulator reports about pause is its model of
IEEE 802.3 Table 28B-3, and its receive checksum verdicts are the model's
(4.10.9); the driver's behaviour on top of them is what was tested.

## Builds and rig

- Branch `netopts` at `2952348` (the code) and `6755a41` (the simulator's
  link record gains `pause_rx`, `pause_tx` and `partner_pause`, read by
  these legs). `rtl815x.sys` trace build (`ndis5 trace`, test-only,
  never released) `dd29bad6...`; INFs `RTL81598.INF` `5cbe8b40...`,
  `RTLXP7.INF` `5510426e...`. The previous INF and driver: `main`
  (`456b56b`), trace build `60bf525f...`, `RTLXP7.INF` `22b816b9...`,
  `RTL81598.INF` `40b1b23a...`.
- For the checksum cases, a scratch copy of the same tree whose trace
  build also writes one line per received IPv4 frame with identification
  0xC5xx (the chip's verdict, TCP/IP's enabled receive tasks, the claim
  passed to NDIS), `75756863...`; nothing else differs.
- On 98 SE the legs after the update ran the step 0 test build of the
  same tree (record `2026-11-17-trim-step0.md`): Windows copied
  `rtl815x.sys` from the floppy's root, where that build lay. Its pause
  and checksum code is the tree's; it adds only the registry probe.
- QEMU 11.1.0 (stock), TCG, headless, throwaway overlays of the installed
  bases. XP SP3: Windows' own EHCI; 98 SE: NUSB 3.3's EHCI; ME: ME's own
  UHCI at Full Speed. The simulator over `usb-redir` at High Speed (Full
  Speed on ME), link partner 10 Gb/s, so the link is 1 Gb/s on High
  Speed. Values set with `NETSET.EXE` or on the Advanced tab; on XP the
  adapter restarted with `devcon restart` and the driver updated with
  `devcon update` (the Microsoft tool, throwaway guests only).
- Checksum traffic: the simulator's `--udp` bridge to a scratch harness
  (`cspeer.py`) that forwards frames to and from a netdev-only QEMU
  user-mode network (DHCP gave the guest 192.168.77.2) and injects crafted
  IPv4 frames toward the guest, reading the guest's answers: a UDP
  datagram to a closed port that TCP/IP accepts draws ICMP port
  unreachable, a TCP SYN to a closed port draws RST, a dropped one draws
  nothing. XP's firewall was turned off for it.

## Pause (`*FlowControl`)

**XP SP3 (Windows' EHCI, RTL8157):**

| Step | Result |
|---|---|
| Fresh install of the tree's NT INF content (the step 0 INF, which adds only probe entries) | `setting *FlowControl = 3` (the default) |
| Install from the previous INF, `*FlowControl` set to 0, update to the new INF | trace `setting *FlowControl = 0`; link: advertisement 0x11E1 (no PAUSE, no ASM_DIR), no pause. The stored value kept. PASS |
| The Advanced tab, Flow control | three choices: "Enabled (negotiated)", "Off", "Transmit only"; current value Off (stored 0) |
| "Transmit only" chosen, OK | the adapter restarted at once; `setting *FlowControl = 1`; advertisement 0x19E1 (ASM_DIR alone); against a partner with both bits: transmit only. PASS |
| Reboot | `setting *FlowControl = 1`, the same advertisement and directions. PASS |
| Stored 2 (`NETSET`), adapter restart | `setting *FlowControl = 2: not offered, default 3`; advertisement 0x1DE1 (both bits); both directions. PASS |
| Forced 100 Mb/s full (`*SpeedDuplex` 4), `*FlowControl` 3, partner both bits | BMCR 0x2100 (autonegotiation off); 100 full; no pause either way. PASS |
| Forced 100 Mb/s half (3) | BMCR 0x2000; 100 half; no pause. PASS |
| Back to automatic (0) | 1000 full, both directions |

**Note added 2026-11-17, after the real RTL8157 legs
([netopts-rtl8157](2026-11-17-netopts-rtl8157.md)).** The forced-speed
rows above were observed against a simulator whose PHY reset kept the
bits written with it (`rtlemu.py` before `3ad31d9`), and the driver of
these legs wrote the forced speed and duplex in the same write as the
reset. A real RTL8157 drops such bits and autonegotiates, so these PASS
results do not show the driver forcing a speed on a real chip. They stand
as observed; the forced speeds are to be re-run with the fixed driver
(`e69e740`, `68c758e`) against the corrected simulator and on the real
adapter ([open-items](open-items.md)). The Automatic results (pause,
checksums) are not affected: their only reset write is the
autonegotiation one, whose other bits are the defaults a reset restores.

**Note added 2026-11-18:** the forced values 1 to 4 (and 6 and back to
Automatic) were run again with the fixed driver against the corrected
simulator on XP SP3 under Windows' EHCI and 98 SE under NUSB's EHCI, the
RTL8157 and the RTL8156B ([rtl815x-sim-legs](2026-11-18-rtl815x-sim-legs.md);
simulator evidence). Values 1 to 4 each linked at the speed and duplex
set with autonegotiation off (BMCR 0x0000, 0x0100, 0x2000, 0x2100), only
the forced ability and the pause bits advertised, register 9 cleared,
and no pause. Value 6 autonegotiated at 1000 full, the advertisement
holding no 10/100 ability (RTL8157 0x1C01, RTL8156B 0x0C01), register 9
0x0200, pause both ways. Automatic, before and after, autonegotiated at
1000 full with the full advertisement (0x1DE1, 0x0DE1), register 9
0x0200, pause both ways. The real-adapter re-run is not part of that
record.

**Note added 2026-11-18:** the real-adapter re-run is done
([rtl8157-forced-real](2026-11-17-rtl8157-forced-real.md)): every forced
value linked at the speed and duplex set on the real RTL8157, XP SP3 and
98 SE under xHCI98.

The twelve partner combinations (each a replug of the simulator with
the partner's bits; the directions from the simulator's link record):

| Value | Partner none | PAUSE only | ASM_DIR only | PAUSE + ASM_DIR |
|---|---|---|---|---|
| 0 (advertisement 0x11E1) | none | none | none | none |
| 1 (0x19E1, ASM_DIR) | none | none | none | transmit only |
| 3 (0x1DE1, both) | none | both | receive only | both |

All twelve as Table 28B-3 gives them (design 4.10.9). PASS.

**98 SE (NUSB 3.3's EHCI, RTL8157):**

| Step | Result |
|---|---|
| Fresh install of the tree's 98 INF content (step 0 INF) | `setting *FlowControl = 3` |
| Install from the previous INF (Device Manager, Update Driver, Have Disk), `*FlowControl` set to 0 (`NETSET`), update to the new INF the same way | root `*FlowControl` read "3" after the update: **the stored 0 was not kept**. Observed in this update path; the step 0 record saw the same reset in the updates it tested to a different INF (no-clobber notwithstanding), while the same INF installed again kept the value. Its cause is not established beyond those tested paths. |
| Restart (cold), the new driver | `setting *FlowControl = 3`, offload 77 with the receive bits left out (as below) |
| `*FlowControl` 0, replug, partner none | `setting *FlowControl = 0`; advertisement 0x11E1; no pause. PASS |
| Further replugs | not enumerated: an "Unknown Device" hardware wizard and a setup program started by stray keystrokes held the guest; the other eleven combinations, the forced speeds and the reboot were not run on 98 SE |
| The Advanced tab's three choices | not shown on 98 SE: that overlay's Network dialog asked "Your network is not complete" at each tab change. The tab of the earlier overlay listed "Flow control" (its choices not opened). |

## Receive checksums (RTL8157 and RTL8159, XP SP3)

The tab, `TCP checksum offload (IPv4)` on the RTL8157: "Off", "On,
transmit" (the current value, the default), "Receive enabled",
"Transmit and receive enabled". The same four strings are the INF's for
the IPv4 and UDP entries.

What XP SP3's TCP/IP enabled (`OID_TCP_TASK_OFFLOAD` set, trace `task
offload: tx T rx R`; bits 1 IPv4, 2 TCP, 4 UDP), all three keywords at
the value, RTL8157 and RTL8159 alike:

| Value | Keywords leave | TCP/IP enabled |
|---|---|---|
| 0 | nothing | no set (the query is not answered) |
| 1 (default) | transmit | transmit IPv4 and TCP; receive none |
| 2 | receive | transmit none; receive IPv4 and TCP |
| 3 | both | transmit IPv4 and TCP; receive IPv4 and TCP |

XP SP3's TCP/IP never enabled UDP, either way, though it was offered;
so these legs did not exercise UDP receive checks, and what 2000 SP4 or
a later system enables is not inferred from XP.

Crafted frames from the simulated peer, at value 3 and at value 2, on
the RTL8157 and the RTL8159 (56 cases, 14 per run), run on the tree at
`2952348`: the guest's answer and the claim the driver passed to NDIS
(1 IPv4 header, 2 TCP). "Options" here are **IPv4** options (a 24-byte
IPv4 header); every crafted TCP segment had a plain 20-byte TCP header
and every UDP datagram a correct length, so these cases do not exercise
TCP options or malformed transport lengths (the host tests do, since
`9e029e9`):

| Frame | Expected | Guest | Chip verdict | Claim |
|---|---|---|---|---|
| UDP, good checksum | accepted | port unreachable | IPv4, UDP good | IPv4 only (UDP not enabled) |
| UDP, bad checksum | dropped | nothing | IPv4 good, UDP failed | IPv4 only |
| TCP SYN, good | accepted | RST | IPv4, TCP good | IPv4 + TCP |
| TCP SYN, bad checksum | dropped | nothing | IPv4 good, TCP failed | IPv4 only (not claimed good) |
| UDP, 24-byte IPv4 header (options), good / bad | accepted / dropped | as expected | as without options | IPv4 only |
| TCP SYN, IPv4 options, good / bad | accepted / dropped | as expected | as without options | IPv4 + TCP / IPv4 only |
| UDP in two fragments, good / bad | accepted / dropped | as expected | no transport check | IPv4 only, each fragment |
| UDP, checksum 0 | accepted | port unreachable | no UDP check | IPv4 only |
| UDP, bad IPv4 header checksum | dropped | nothing | no good bit | nothing |
| IPv4 total length past the frame | dropped | nothing | IPv4 good | nothing ("not checked") |
| IPv4 total length shorter than the header | dropped | nothing | IPv4 good | nothing |

Every case PASS in all four runs. In these cases a bad TCP checksum was
never claimed good and the stack dropped it; fragments, IPv4 options, UDP
checksum 0 and the malformed IPv4 lengths were reported as the design
says. The verdicts are the simulator's model, not a chip's.

Throughput, guest receiving (`NETPERF.EXE` in the guest, `netperf.py`
through the user-mode network's forwarded port, 20 s each, interleaved,
RTL8157): value 3, 39.85 Mb/s (guest CPU 96.9%); value 1, 32.35 Mb/s
(81.0%); value 3 again, 36.19 Mb/s (97.8%); no mismatched byte, no stall,
and with value 3 the driver's counter showed every frame but one of
65536 indicated as checked. This rig is the emulated machine and the
simulator, not a measure of the chip.

## 98 SE and ME: no checksum settings

98 SE (NUSB's EHCI) and ME (its own UHCI), the tree's 98 INF content:
the trace reads no checksum keyword, `offload 77, keywords leave 7` (the
chip's bits less the receive ones; a new trace line), and no task-offload
query is answered (no `task offload` line), as before; the 98 SE
Advanced tab listed no checksum entry. Nothing else changed in what the
driver does there.

## Not run

2000 SP4 (its base has no USB stack files; the rig was not set up), XP
x64, Vista, 7; xHCI98 as the host stack on any system; 98 SE's remaining
pause cases above; ME's pause cases; a reboot with the checksum values
set; the RTL8159's throughput.

Evidence: `E:\usb-wdm-free\netopts\w3` (`logs\e9-xp-b3.log` and
`logs\e9-w98-s0b*.log` the traces, `logs\sim-*.jsonl` the simulator's
link records, `logs\pausemx-xp.txt`, `logs\sdmx-xp.txt`,
`logs\peer-x3.jsonl` the crafted frames, `shots\`, `STATE.md`).

## Second round (tree at `d89f912`)

Built from `d89f912` with `src\net\rtl815x\build.cmd` (both PASS):
`ndis5 free` `rtl815x.sys`
`4ca05b519653b29e84a60ca855bffc27edc1725fd0ab5b79ebcfa7d7321f913d` and
`ndis5 trace` (the same free build writing the trace; test-only)
`626e61c71c26135496f7db589fda1bfa197007b2d5adc815df7d3d6fd2b0bef6`;
the INFs as above (`RTL81598.INF` `5cbe8b40...`, `RTLXP7.INF`
`5510426e...`). The free build writes no trace, so the 98 SE legs read
the simulator's link record and the registry; the XP leg, which needs
the driver's own lines, ran the trace build. The previous released
package, for the update control: `free\net\rtl815x\w98se-me` (release
0.1.10, `DriverVer` 0.2.3.0), `RTL81598.INF` `40b1b23a...`,
`RTL815X.SYS` `f7e7c9d0...`. Every guest a fresh throwaway overlay of
the installed base, QEMU 11.1.0, TCG, headless, the simulated RTL8157 at
High Speed with a partner advertising PAUSE and ASM_DIR unless noted;
each key typed into a guest only after a screenshot showed the expected
window active.

### XP SP3, Windows' EHCI: checksum values across a reboot (trace build)

Installed by the Found New Hardware wizard from the floppy (default 1:
`task offload: tx 3 rx 0`). Each value then written to all three
keywords with `NETSET` and Windows restarted; after the boot (a new
`DriverEntry` line):

| Value | Trace after the reboot | What TCP/IP enabled |
|---|---|---|
| 0 | `setting *...ChecksumOffloadIPv4 = 0` (each), `keywords leave 0` | no `task offload` line (no set) |
| 1 | `= 1`, `keywords leave 7` | `tx 3 rx 0` |
| 2 | `= 2`, `keywords leave 70` | `tx 0 rx 3` |
| 3 | `= 3` | `tx 3 rx 3` |

Each value was kept across the reboot, and what XP SP3's TCP/IP enabled
after it is what it enabled after an adapter restart in the first round
(IPv4 and TCP, never UDP). PASS.

### 98 SE, NUSB 3.3's EHCI: pause (free build)

| Step | Result |
|---|---|
| Fresh install (Add New Hardware wizard, floppy, restart) | stored `*FlowControl` "3"; advertisement 0x1DE1, both directions. PASS |
| The Advanced tab (Network, the adapter's Properties) | "Flow control" with three choices: "Enabled (negotiated)", "Off", "Transmit only"; no checksum entry |
| "Transmit only" chosen, OK, Network OK, restart declined, the adapter restarted by a replug | stored "1"; 0x19E1 (ASM_DIR alone); transmit only. PASS |
| `NETSET` 0, replug | 0x11E1; no pause. PASS |
| `NETSET` 3, replug | 0x1DE1; both. PASS |
| `NETSET` 2, replug | stored "2"; 0x1DE1; both (read as 3). PASS |
| `NETSET` 1, replug; then a reboot | 0x19E1, transmit only, before and after the reboot. PASS |
| Value 3, forced 100 full (`*SpeedDuplex` 4) | BMCR 0x2100, 100 full, no pause. PASS |
| Forced 100 half (3) | BMCR 0x2000, 100 half, no pause. PASS |
| Back to automatic (0) | 1000 full, both |

At a forced speed the driver leaves the advertisement register alone
(`Negotiate` returns before it), so the record shows the simulator's
power-on 0x1DE1 there; with autonegotiation off it carries no meaning.
The other partner combinations were not repeated on 98 SE (XP ran all
twelve).

**Note added 2026-11-17:** the two forced-speed rows of this table are
qualified as those of the XP table above (see the note there): observed
against a simulator whose PHY reset kept the forced bits, to be re-run.
Re-run on 2026-11-18 against the corrected simulator with the fixed
driver, every forced value 1 to 4 on the RTL8157 and the RTL8156B under
NUSB's EHCI, and the RTL8156B under xHCI98 3.5.0.0: each linked as set
([rtl815x-sim-legs](2026-11-18-rtl815x-sim-legs.md); simulator evidence).
The advertisement paragraph describes the driver and simulator of these
legs only: since `e69e740` the driver writes only the forced ability into
the advertisement at a forced speed, and since `4b2992d` the simulator's
RTL8157 starts with 0x11E1, as the real chip read.

### 98 SE: the sheet-open update control (released package)

Fresh overlay (NUSB 3.3's EHCI); the released package installed by the
wizard from the floppy, Windows restarted, the driver loaded (link up).
Then two updates to the same released package, each by Device Manager,
the adapter, Driver, Update Driver, "Display a list", Have Disk `A:\`,
the one model, Next:

| | Sheets closed | Network's adapter sheet open on Advanced, Flow control changed On to Off and left pending |
|---|---|---|
| The update | completed ("finished installing"); restart declined | completed; restart declined; on closing Device Manager a second "You must restart ... new settings" prompt (Network's) stood behind the open sheet; declined; the sheet still showed the pending Off |
| Driver key (`Enum\USB\VID_0BDA&PID_8157\...` `Driver`) before / after | `Net\0001` / `Net\0001` | `Net\0001` / `Net\0001` (also after the sheet's OK and Network's OK) |
| Root `*FlowControl` | "3" / "3" | "3" throughout: the pending Off was not written, neither by the sheet's OK nor by Network's OK |
| Network's OK | (not used) | asked for the "RTL815X installation disk" (`rtl815x.sys` looked for in `D:\win98`; `A:\` typed), copied; an "Unknown Device" hardware wizard (a device of the base) came up and was cancelled; restart declined |
| After a restart | the driver loaded (link up) | three "Unknown Device" wizards held the boot; the device enumerated but no driver started until the first was cancelled; then the driver loaded (link up); key `Net\0001`, root "3" |

The guest keeps no device-install log: no `SETUPLOG.TXT`;
`C:\NETLOG.TXT` dates from the base install, `C:\WINDOWS\NDISLOG.TXT`
is empty. After each update 98 SE rewrote the devnode's `Enum` entry
(its `Capabilities`, `Serial` and `SymbolicName` dropped until the next
boot, `HardwareID` and `CompatibleIDs` reduced to the bare id) and moved
the TCP/IP binding to a new `MSTCP` instance.

**Independent control:** a fresh overlay with QEMU's PCnet card, for
which 98 SE installed its own "AMD PCNET Family Ethernet Adapter
(PCI-ISA)" (`NETAMD.INF`, `pcntn4m.sys`) at the first boot. The same
pair, updating to the same model from the list (no Have Disk), with
"Full Duplex" changed from Default to OFF and left pending for the open
case: both updates completed; driver key `Net\0001` before and after;
the device "working properly" after each restart; the pending OFF ("1")
not written (root `FDUP` stayed "0"); the same Network-side restart
prompt behind the open sheet, the same "Unknown Device" wizards at
Network's OK and at the next boot, and the same `Enum` rewrite. Twice
the control panel host faulted (`RUNDLL32 caused a general protection
fault in module SETUPX.DLL at 0006:00000049`): on closing the device's
properties after the closed-sheet update, and after declining the
restart that followed Network's OK in the open-sheet case.

**Outcome:** the earlier failure ("Windows was unable to install", the
driver key gone, no driver after the restart) reproduced **with
neither**: not with the released package, not with Windows' own PCnet
driver. Both showed the same Windows behaviour around an update with
the sheet open (a pending edit lost, an extra restart prompt, the boot
held by hardware wizards), and only the Windows driver's runs showed a
`SETUPX.DLL` fault. These runs used Have Disk for our package; the
failing run had taken the installed model without it. What caused the
earlier failure stays unattributed; it was not seen again with an
unmodified package.

### ME, ME's own UHCI at Full Speed: pause, no checksum settings (trace build)

ME's automatic search installed the tree's `RTL81598.INF` from the
floppy and the driver started before the restart it asked for: `setting
*FlowControl = 3`, advertisement 0x1DE1, both directions; `offload 77,
keywords leave 7`; no checksum keyword read and no `task offload` line
in the whole run (four starts). The Advanced tab (Network, the
adapter's Properties) lists "Flow control" with "Enabled (negotiated)",
"Off", "Transmit only", and no checksum entry. "Transmit only" chosen
there (OK, Network OK, restart declined, then a replug): `setting
*FlowControl = 1`, 0x19E1, transmit only. `NETSET` 0, replug: `= 0`,
0x11E1, none. `NETSET` 2, replug: `= 2: not offered, default 3`,
0x1DE1, both. PASS. ME's EHCI has no driver in this base (its wizard was
cancelled), so ME ran at Full Speed as in the first round.

### 98 SE under xHCI98: pause (free build)

No 98 SE base has xHCI98, so a throwaway overlay got it from the
xHCI98 3.5.0.0 release (`releases\3.5.0.0` of the xHCI98 repository;
`98-me\xhci98.sys` `b3ea6650...`, `SETUP.EXE` `7ee4842b...`) with its
own Setup in the guest, QEMU's `qemu-xhci` added. Getting the package
into the guest: a host-folder hard disk stopped 98 SE's boot at
"Booting from Hard Disk" (with and without the xHCI controller); the
package's 98 files (`SETUP.EXE`, `INSTALL.BAT`, `LICENSE`, `98-me\`,
`tools\xhciinst\XHCIWIZ.EXE` and `XHCIDI.EXE`) on a 2.88 MB host-folder
floppy as B: booted, but **attempt 1**, `B:\SETUP.EXE`, ended at once
with "<unknown> caused an exception c0000006H ... at 0000:bff85ae6" (an
in-page error reading that medium). **Attempt 2**: the same files
copied to `C:\XH` by the guest's `xcopy` from three 1.44 MB floppies,
then `C:\XH\SETUP.EXE`: Welcome (the controller listed for install),
Components left as offered, Install, "Done. Please restart Windows
now", restart. The xHCI controller needed no wizard afterwards (the
PIIX UHCI and the EHCI, which this leg does not use, were cancelled at
each boot). The simulated RTL8157 on the xHCI bus enumerated, the
wizard installed the tree's `RTL81598.INF` from the floppy (copying
networking files from the Windows CD), restart:

| Step | Result |
|---|---|
| Fresh install, default | 0x1DE1, both. PASS |
| `NETSET` 0, replug | 0x11E1, none. PASS |
| `NETSET` 3, replug | 0x1DE1, both. PASS |
| `NETSET` 1, replug; then a reboot | 0x19E1, transmit only, before and after the reboot. PASS |
| `NETSET` 2, replug | 0x1DE1, both (read as 3). PASS |

The tab's choices were not reopened under xHCI98 (the INF and the page
are the same as under NUSB above).

### Not run in the second round, and why

- **2000** (the receive-option task request and indication behaviour):
  not started. Making a USB 2.0 base from the guest's own `sp4.cab` and
  then the checksum rig would not have fitted in what remained of this
  session; nothing about whether it can be done was learnt.
- **XP under xHCI98** (one checksum run): not started, for the same
  reason. The 98 SE route above (the package copied in over several
  floppies, its Setup run from the hard disk) would apply; the XP
  package is larger than one floppy.
- *Added 2026-11-18:* both run since with the simulator, on an existing
  2000 SP4 base with 2000's own EHCI and on an XP SP3 base that already
  had xHCI98 3.5.0.0 ([rtl815x-sim-legs](2026-11-18-rtl815x-sim-legs.md)).
- On 98 SE nine of the twelve partner combinations (the values 0, 1 and
  3 ran only against the partner advertising both pause bits), and under
  xHCI98 the forced speeds and the tab; ME's forced speeds; XP x64,
  Vista, 7.

Evidence of the second round: `E:\usb-wdm-free\netopts\w4`
(`logs\e9-xp-l1.log` and `logs\e9-me-l4.log` the traces,
`logs\sim-*.jsonl` the link records, `stage\*\OUT\` the registry
exports and `NETSET` output, `shots\`, `STATE.md`).
