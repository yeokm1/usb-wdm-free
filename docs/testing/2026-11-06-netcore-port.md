# The shared Ethernet core in every package: ax88179, rtl815x, ax88772, sr9700, 2026-11-06

Owner, 2026-11-06: the fixes made in `ax88772`'s copy of the shared NDIS
core (ethernet.md section 6.8, items 1 to 6) go into every package's
copy, `ax88179` and `rtl815x` included, with a short leg on each changed
driver. Simulator legs only: no adapter was used and none is claimed.

## What changed

- `netcore.c`, `netusb.c`, `netchip.c`, `netchip.h`, `netusb.h`,
  identical in `src/net/ax88179`, `rtl815x`, `ax88772` and `sr9700`
  (`3278ee5`, `fa9791d`, `5a10485`). The chip cores, INFs and simulators
  of `ax88179` and `rtl815x` are unchanged.
- Item 4 adds three tries of the chip's Resume before an adapter is
  reported gone. Item 6, 98 SE and ME only: the worker's exit event is
  set from no instruction of the image (`ExitByKernel`).
- `DriverVer` and file versions: `ax88179` 0.1.3.0 (`AX179F.INF`,
  `AX179F98.INF`, `ax179f.rc`), `rtl815x` 0.2.1.0 (`RTL81598.INF`,
  `RTL815X.INF`, `RTLXP7.INF`, `rtl815x.rc`).
- Host tests at `5a10485`, `test.cmd` PASS in each package: `ax88179`
  18746 chip-core, about 34570 core (the count varies by a few with the
  fake threads' timing), 62861 framing and 66542 descriptor checks;
  `rtl815x` 1219967 chip-core and 34568 core checks; `ax88772` 58863
  chip and 34568 core checks; `sr9700` 50 chip and 34568 core checks.
  Every `ndis5` and `ndis51` build and import gate PASS. In the `ndis5`
  build the tail of `ExitByKernel` reads `KeSetEvent` and
  `PsTerminateSystemThread` from the import table and ends in
  `jmp eax` (objdump of `ax772f.sys`). The next commit to the core
  changes a comment only.

## Rig

- NT: stock QEMU 11.1.0, q35, TCG, one vCPU; fresh overlays of the 2000
  and XP x86 q35 bases with xHCI98 3.4.1.0 (`nt-x86`); the simulator over
  usbredir on `qemu-xhci` port 1.
- 98 SE: the overlay of [ax88772-sim](2026-11-06-ax88772-sim.md) (NUSB
  3.3 EHCI, xHCI98 3.4.1.0) with all four drivers installed; the new
  images copied over `C:\WINDOWS\SYSTEM` from a virtual floppy; a new
  simulator process per plug, on `qemu-xhci` port 1.

## Results

| Leg | Build | Result |
|---|---|---|
| `ax88179`, 2000, `axemu.py` (SuperSpeed) | `fa9791d` trace, `ax179f.sys` `e5df17ce...` | link 1000 Mb/s; DHCP; `ping -n 4` at 32, 1472 and 4000 bytes, 4/4 each; removal under `ping -t`: "device gone", "0 malformed transfers" |
| `rtl815x`, XP, `rtlemu.py --chip 8157`, `RTLXP7.INF` | `fa9791d` trace, `rtl815x.sys` `12defdf2...` | link 5000 Mb/s; DHCP; 4/4 at each size; removal clean |
| `ax88179`, 98 SE, xHCI98 (SuperSpeed) | `5a10485` trace, `ad00fc8a...` | link 1000 Mb/s; 4/4 at 1472; removal under `ping -t`: "device gone", "halted"; replug: "DriverEntry" again (the image had been unloaded), the running `ping -t` answered again; removal clean |
| `rtl815x`, 98 SE, `--chip 8153` | `5a10485` trace, `278bfbe1...` | link 1000 Mb/s; the `ping -t` answered; 4/4 at 1472; removal under `ping -t` clean |
| `ax88772`, 98 SE, `--chip 772c` | `5a10485` trace, `45610224...` | link 100 Mb/s; 4/4 at 1472; removal under `ping -t` clean |
| `ax88772`, 98 SE, `--chip sr9800`, three cycles | the same | each plug "DriverEntry", bring-up and link; each removal "device gone", "halted"; the guest stayed up |
| `sr9700`, 98 SE, `sremu.py` (Full Speed) | `5a10485` trace, `d218a815...` | link 10 Mb/s half duplex; 4/4 at 1472; removal under `ping -t` clean |

Every halt logged "0 malformed transfers, 0 dropped frames, 0 with an
FCS". On 98 SE each plug loaded the image anew and each removal of the
last adapter unloaded it, so every removal above ran the new exit with
the image going after it. **PASS** with the simulated chips.

## Not run

- ME with the `5a10485` builds; the earlier ME legs of `ax88772` and
  `sr9700` ran the 100 ms wait it replaces.
- `ax88179` and `rtl815x` on XP x64, Vista and 7 with the new core;
  `rtl815x`'s `ndis51` (x64) build in a guest.
- What no simulator can show: the 98 SE and ME exit's assumptions about
  NTKERN (ethernet.md section 6.8, item 6) are read from the trace's
  absence of a fault, not proven.
- A real adapter of any of the four packages with the new core.

## Evidence

Development host A, `E:\xhci98-vm\a347\wdm4\` (not tracked): `pids.txt`;
`ev\n2k-ax.*`, `ev\nxp-rtl.*`, `shots\k2k-*`, `kxp-*` (NT);
`ev\h98-debug.txt`, `ev\h98*-*.jsonl`, `ev\h-sha256.txt`,
`shots\h98*` (98 SE).
