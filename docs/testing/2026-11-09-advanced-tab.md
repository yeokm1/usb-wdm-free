# The Advanced tab's keywords with simulated chips, 2026-11-09

Open items, "Ethernet drivers: EEE and the Advanced tab, as the makers'
INFs offer them" (owner, 2026-11-09); design [ethernet.md](../design/ethernet.md)
section 4.10. Every leg here ran against the simulators (`rtlemu.py`,
`axemu.py`, `ax7emu.py`, `ncmemu.py`); no physical adapter was used and
none is claimed. A misreading the drivers and the simulators share passes
here: the real chips stay owed.

## The builds and their host tests

- Commits `6adff1a` (design) to `4adb03c`-and-later fixes on branch
  `w48-wdmb` (the code at `13d83ee`, the review fixes after it). Trace
  builds under test (`ndis5 trace`, test-only): `rtl815x.sys`
  `1f8f439a...`, `ax179f.sys` `5d42b292...`, `ax772f.sys` `851b3f9e...`,
  `cdcnet.sys` `b827a3c8...`; the test aid `NETSET.EXE` `b7bc0ab6...`
  (`src/net/cdcnet/netset.c`). Every package's `ndis5` free and trace and
  `ndis51` builds and their import gates PASS.
- Host tests PASS: `rtltest` 1254223 checks (the `Settings` cases: the
  table, the defaults with EEE, green, EEE+ and U2P3 off, forced 10/100,
  1000 and 2.5G alone, the RTL8152's EEE registers, `EEEMaxSupportSpeed`,
  the keywords a chip has no function for reported in
  `NET_PARAMS.Ignored`); AX179F `chiptest` 19971 (EEE on as ASIX's 7
  default, forced speed with no pause, 1000 alone, receive-only and
  transmit-only pause by IEEE 802.3 Table 28B-3); ax772f `chiptest` 58883
  (forced 100 half, pause off, receive only, transmit only, the SR9800's
  forced speed and its flow-control value reported); the shared core's
  `coretest` (the keywords read as text or a DWORD, malformed or not
  offered values taken as the default, a power cycle keeping the values
  read, the transmit checksum keywords on NT); `rtlsimtest.py` and
  `axsimtest.py` (the keywords through the simulators' registers);
  `ncmsimtest.py` and `sr9simtest.py` as regression only (they do not
  exercise a keyword).
- Not part of this change, seen while running the host tests: `cdctest`
  (cdcnet's pure-code fuzz) crashed once in four runs (an access
  violation, random inputs); `test.cmd`'s `if errorlevel 1` does not see
  the negative exit code, so the run printed no PASS line and still
  wrote `PASS.TXT`. Open, not analysed here.

## Rig

The 0.1.9 9x matrix's harness (`docs/testing/2026-11-07-matrix-9x.md`),
copied to `E:\xhci98-vm\p48\wdmb\kit`: QEMU (usb4-emu fork `cb36497`),
TCG, one vCPU; read-only bases `w98x` (98 SE, NUSB 3.3, xHCI98 3.4.2.0
`3990e17d`), `w98n2` (98 SE, NUSB 3.3), `men`, `mex` (ME); the drivers
staged by `DevicePath`; a new simulator process per plug. A leg: install,
the keywords read back with `NETSET.EXE`, then for each case the values
written with `NETSET.EXE` as the Advanced tab writes them (`REG_SZ`), the
adapter restarted by an unplug and a replug (`MiniportInitialize` reads
the keywords), the simulator's link-up event checked for the registers the
values set, and a ping (4 x 1000 bytes) to the simulated host.

## `rtl815x`, 98 SE, xHCI98 (RTL8156B at SuperSpeed, partner 2.5G)

Leg `advrtl-w98-xh-a`: 17 of 17 PASS.

| Case | Registers at link-up (simulator) | Result |
|---|---|---|
| after install | `NETSET`: `*SpeedDuplex` "0", `*FlowControl` "3", `*EEE` "0", `EEEMaxSupportSpeed` "1000", `EnableGreenEthernet` "0", `AdvancedEEE` "0", `EnableExtraPowerSaving` "0" | PASS |
| defaults | 2500 full; BMCR 1000 (autonegotiation); advertisement 0DE1 (10/100, pause); 1000 full; 2.5G; EEE MAC 0, advertisement 0, 2.5G 0; U2P3 0; green 0 | PASS, ping |
| `*EEE` 1, `EEEMaxSupportSpeed` 2500, `EnableGreenEthernet` 1, `EnableExtraPowerSaving` 1 | EEE MAC 3, advertisement 6 (100, 1000), 2.5G 1; U2P3 1; green 8000 | PASS, ping |
| `EEEMaxSupportSpeed` 1000 | EEE advertisement 6, 2.5G 0 | PASS, ping |
| `*SpeedDuplex` 4, `*FlowControl` 0 | 100 full, BMCR 2100 (autonegotiation off) | PASS, ping |
| `*SpeedDuplex` 6, `*EEE` 0 | 1000 full; advertisement 0001 (no 10/100, no pause); 2.5G off; EEE off | PASS, ping |
| `*SpeedDuplex` 2500, `*FlowControl` 3 | 2500 full; advertisement 0C01 (pause only); 1000 off; 2.5G on | PASS, ping |
| `*SpeedDuplex` 7 (not offered) | the default: autonegotiation of everything, 2500 | PASS, ping |

`AdvancedEEE` acts only at a 10 Mb/s link (EEE+, section 4.10.4) after the
link-up the simulator logs; it is covered by the host tests, not here.

### The Advanced tab itself on 98 SE

On a new overlay of that leg's disk (`gui-rtl`, screenshots
`kit\shots\gui-rtl-*.png`): Control Panel, Network (`gui-rtl-05`: the
adapter "Realtek RTL8156 USB 2.5G Ethernet (RTL815X)" listed), its
Properties, the Advanced tab (`gui-rtl-16`): `netdi.dll` lists all eight
keywords by their descriptions, the starred names included. "Energy-
Efficient Ethernet (EEE)" changed from Off to On in the dialog
(`gui-rtl-23`, `-26`), OK, the Network dialog's OK, Windows' restart
question answered No (`gui-rtl-32`); `NETSET RTL815X *EEE ?` then read
`*EEE = "1"`, a `REG_SZ` as `NETSET.EXE` writes it in the legs. PASS.

## `rtl815x`, 98 SE, NUSB's EHCI (RTL8156B at High Speed, partner 1000)

Leg `advrtl-w98-nusb-n2`, on a fresh overlay of `w98n2`: every step PASS.
At High Speed the 8156B links at 1000; the leg ran the two cases below
(the others, `EEEMaxSupportSpeed` 1000 and `*SpeedDuplex` 6 among them,
ran under xHCI98 and, the latter, on XP at High Speed):

| Case | Registers at link-up (simulator) | Result |
|---|---|---|
| after install | `NETSET`: the seven keywords at their defaults, as under xHCI98 | PASS, ping |
| `*EEE` 1, `EEEMaxSupportSpeed` 2500, `EnableGreenEthernet` 1, `EnableExtraPowerSaving` 1 | 1000 full; EEE MAC 3, advertisement 6, 2.5G 1; U2P3 1; green 8000 | PASS, ping |
| `*SpeedDuplex` 4, `*FlowControl` 0 | 100 full, BMCR 2100 (autonegotiation off) | PASS, ping |

(Leg `n1` was stopped before its cases to free the slot for the real
AX88179; void.)

## `ax88179`, 98 SE, xHCI98 (AX88179 at SuperSpeed)

Leg `advax179-w98-xh-a`: every step PASS (pings after each case PASS).

| Case | Registers at link-up (simulator) | Result |
|---|---|---|
| after install | `NETSET`: `*SpeedDuplex` "0", `*FlowControl` "3", `*EEE` "1" (ASIX's 7 default; owner question, ethernet.md 12.3) | PASS |
| defaults | 1000 full; BMCR 1140; advertisement 05E1 (the PHY's own); 1000 control 0300; EEE: page 3 register 0x19 3247 (`ax88179_enable_eee`), advertisement 6 | PASS |
| `*EEE` 0, `*SpeedDuplex` 3 | 100 half, BMCR 2000 (autonegotiation off); EEE 3246, advertisement 0 | PASS |
| `*SpeedDuplex` 6, `*FlowControl` 2 | 1000 full; advertisement 0C01 (no 10/100; PAUSE and ASM_DIR for receive only); 1000 control 0200 | PASS |

## `ax88772`, 98 SE, xHCI98 (AX88772B at High Speed)

Leg `advax772-w98-xh-a`: every step PASS. The simulator's partner stays
100 full; the medium the driver wrote is the evidence.

| Case | Medium, BMCR, advertisement after the driver's link-up | Result |
|---|---|---|
| after install | `NETSET`: `*SpeedDuplex` "0", `*FlowControl` "3" | PASS |
| defaults | medium 0336 (full, 100, both pause enables); BMCR 1000; advertisement 0DE1 (both pause bits) | PASS |
| `*SpeedDuplex` 1 | medium 0104 (10 half, no pause); BMCR 0000 | PASS |
| `*SpeedDuplex` 0, `*FlowControl` 0 | medium 0306 (100 full, no pause enables); advertisement 01E1 (no pause bits) | PASS |

## `cdcnet`, 98 SE, xHCI98 (`ncmemu.py`, a CDC NCM function as the Linux gadget's `0525:A4A2`)

Leg `advncm-w98-xh-d`: every step PASS. `CDC98.INF` offers two keywords,
`Protocol` (as before) and the new "Network Address" (`NetworkAddress`,
optional, none written at install); this leg checks `NetworkAddress`:
the driver reads it with `NdisReadNetworkAddress` and, on a function
whose `bmNetworkCapabilities` has D1, sends `SetNetAddress` before it
opens the data interface (section 4.10.4).

| Case | The simulator at link-up | Result |
|---|---|---|
| after install | `NETSET`: no `NetworkAddress` value (the driver key as the INF wrote it) | PASS |
| defaults | no `SetNetAddress` request (`net_address` none) | PASS, ping |
| `NetworkAddress` "02AABBCCDDEE" | `SetNetAddress` 02:AA:BB:CC:DD:EE before the data interface's alternate 1; the guest's DHCP requests from 02:AA:BB:CC:DD:EE | PASS, ping |

Legs `a` to `c` of the same test were void: the harness's `NETSET.EXE`
match text did not match the adapter's description (and in `c` held a
space, which `NETSET.EXE` takes as the end of an argument, so it wrote a
stray value `Ethernet` = "(" that was never the driver's); `d` matches
"gadget".

## `rtl815x`, XP (the NT form, `RTLXP7.INF`), Windows' own EHCI (RTL8156B at High Speed, partner 1000)

The NT matrix's harness (`docs/testing/2026-11-07-matrix-nt.md`), copied
to `E:\xhci98-vm\p48\wdmb\nt`: stock QEMU 11.1.0, q35, the `xp-win` base;
`RTLXP7.INF` staged (`SetupCopyOEMInf`); set `advrtl-xp-win-11091119`.
After install the driver key held every keyword at its default
(`InfSection` `RTL_G25`, the 2.5 Gb/s class). Each case (written with
`NETSET.EXE`, the adapter restarted by an unplug and a replug), first
run and again after the harness's replug: PASS, ping PASS each time.

| Case | Registers at link-up | Result |
|---|---|---|
| `*EEE` 1, `EEEMaxSupportSpeed` 2500, `EnableGreenEthernet` 1, `EnableExtraPowerSaving` 1 | EEE MAC 3, advertisement 6, green on, U2P3 1 (1000 full) | PASS |
| `*SpeedDuplex` 4, `*FlowControl` 0 | 100 full, BMCR 2100 | PASS |
| `*SpeedDuplex` 6, `*EEE` 0, green 0, idle 0 | 1000 full, advertisement 0001, EEE and U2P3 off | PASS |

The harness's replug and restart runs of the same test then reported
FAIL only at their first check, the defaults, which the first run had
changed on purpose; their cases passed.
