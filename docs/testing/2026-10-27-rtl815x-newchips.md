# rtl815x with the newer Realtek chips on 98 SE, ME and 2000, 2026-10-27

Task 44.70 part B (owner, 2026-10-27, xHCI98 3.3.0.0): `rtl815x` serves
the RTL8153D, RTL8156, RTL8156B, RTL8157 and RTL8159 (`ethernet.md`
sections 6.4.0 and 6.4.1). No such adapter is on the bench. The owner
accepted the simulator only. Nothing here is "Tested".

This record covers the chip core's checks and the 98 SE, ME and 2000
legs. The 8157/8159 core has its own record
(`2026-10-27-rtl8157-8159.md`), and the simulator has its own
(`2026-10-27-rtl815x-sim-newchips.md`). The XP to 7 legs are in the
packaging record.

## Sources

- Linux `drivers/net/usb/r8152.c` at tag v7.3 (SHA-256 `9d766f20...`).
  It is the same file as at `f47d2236`, which `ethernet.md` pins. The
  file is GPL-2.0-only, so it was read for register facts only. Each
  sequence is restated as this core's own step tables, under this
  project's names.
- No firmware blob is shipped, embedded or read. Linux runs every one of
  these chips without its patch. The RTL8159 then links at 5 Gb/s at
  most (Linux commit `a51e171e`, as the simulator's record reports).

## Chip core checks

- `test.cmd` at `07f87c4`: 1,219,967 chip-core checks and 34,568 core
  checks PASS (MSVC 6.0, `/W4 /WX`).
  - Per chip: bring-up at Full, High and SuperSpeed; FIFO and size
    settings; EEE and multi-gigabit advertisement (2.5G only at
    SuperSpeed, none on the 8153D); PHY values that land in the SRAM
    windows; the link-up receive settings at 2.5G and 1000.
  - The RTL8156's interleaved receiver and transmitter reset, checked
    write by write, and the RTL8156B's single MAC reset.
  - A PHY loading from flash; a refused patch window at each table, with
    no PHY reset after it; the RTL8156's reversed-pair case.
  - `coretest.c` (shared with `ax88179`): `OID_GEN_LINK_SPEED` at 10, 100,
    1000, 2500, 5000 and 10000 Mb/s (100,000,000 in 100 b/s units).
- The old chips (`4C00` to `6400`) are unchanged. A trace harness (scratch,
  not kept) logged every control request, sleep and result of whole lives
  of each old chip id at Full, High and SuperSpeed, in five model
  variants. The logs against `062989b` are byte-identical.
- `rtlsimtest.py`: 29 tests OK. The driver's core drives the independently
  written simulator for every new chip.
- Codex review: four rounds on the core and its tests. Round 1 found the
  RTL8152 EEE guard count, the PHY reset after a refused patch and a name
  equal to Linux's. Round 2 found no P1, and its test gaps were filled.
  Round 4: converged.

## Legs

Rig, kits `E:\usb-wdm-free-vm\rtl-new` (monitor 49250, simulator 49251)
and `rtl-new-me` (49252, 49253):

- QEMU 11.1, TCG, 1 vCPU, headless.
- `qemu-xhci` with the simulator on its port 1 (SuperSpeed, or High Speed
  with `--speed high`).
- `rtl815x.sys` of `build.cmd ndis5 trace` at `1aff65c` (SHA-256
  `c2abb84d...`), with its INFs on a floppy.
- `rtlemu.py` of `rtl-new` (simulator `83a18bf`).
- Each guest on a fresh overlay of a clean base. Every QEMU and simulator
  start and stop is in the kits' `pids.txt`. Guests were stopped by
  monitor `quit`, or exited at a guest restart (`-action reboot=shutdown`).

Each leg:

1. Plug the simulator.
2. Bring-up, then link at the speed given.
3. The cable pulled 40 s after link for 8 s, and back.
4. `ping -n 8 -l 1472 192.168.77.1`, then `ping -n 2 -l 8000` (sent as six
   fragments; the simulated host answers the first, 1472 bytes).
5. Unplug.

DHCP is counted from the simulator's log (DISCOVER/OFFER and
REQUEST/ACK pairs). PASS means all of these:

- the trace shows chip start 0, then link up, down and up at the speed;
- a lease;
- 8/8 and 2/2 replies;
- removal "halted" with 0 malformed transfers.

### Windows 98 SE (base `w98-nusb-hub`, xHCI98 3.1.0.0)

Overlays `s98b` (installed through the 8153D, then the others found the
INF) and `s98c` (RTL8159 only):

| Chip (id) | Link | Result |
|---|---|---|
| RTL8153D (7420) | 1000; partner 100: 100 | PASS |
| RTL8156 (7030) | 2500; High Speed: 1000 | PASS |
| RTL8156 (7020) | 2500 | PASS |
| RTL8156B (7410) | 2500; partner 1000: 1000 | PASS |
| RTL8156B (7400) | 2500 | PASS |
| RTL8157 (1030) | 5000; High Speed: 1000 | PASS |
| RTL8159 (2020) | 5000; `--phy-patched`: 10000; High Speed: 1000 | PASS (`s98c`) |

Notes on 98 SE:

- On `s98b` the RTL8159 came up and linked, but Windows sent no frame:
  "Destination host unreachable". The Network panel showed TCP/IP bound
  to six adapters (Dial-Up, two RTL8156 instances, two RTL8157 instances,
  the 8153D) and none to the RTL8159. This is the six-binding limit the
  0.1.2 record met. On a fresh overlay with the RTL8159 alone, every
  RTL8159 leg passed.
- A first run on overlay `s98` with an earlier build (`ad4ed332`, before
  the RTL8157/8159 merge) passed for 7030, 7020, 7410 and 7400, and
  includes an unplug and replug.

### Windows ME (base `me-hcd310`, xHCI98 3.1.0.0)

Overlay `me1`. ME asks for the path to `rtl815x.sys` for each new device
instance; each speed is a new instance. A:\ was typed each time.

| Chip | Link | Result |
|---|---|---|
| RTL8156 (7030) | 2500; High Speed: 1000; partner 1000: 1000 | PASS |
| RTL8156B (7410) | 2500 | PASS |
| RTL8153D (7420) | 1000 | PASS |
| RTL8157 (1030) | 5000; High Speed: 1000 | PASS |
| RTL8159 (2020) | 5000; partner 2500: 2500 | PASS |

Typing over the monitor is slow on ME, so some 8000-byte pings were cut
short. Every ME leg had its 8/8 at 1472 bytes. With the floppy holding
both INFs, ME offered `RTL815X.INF` (the 2000 INF) beside
`RTL81598.INF`; the release keeps them in separate folders.

### Windows 2000 (base `w2k-xhci`, xHCI98 3.1.0.0, no EHCI driver)

Overlay `k2k`. The base's keyboard layout is Dvorak; text was typed with
it. The floppy held only `RTL815X.INF` and the driver (2000 picks the
`$Chicago$` INF if both are there, and refuses it for Net). The driver
installed with "Digital Signature Not Found": Yes.

| Chip | Link | Result |
|---|---|---|
| RTL8156 (7030) | 2500 | PASS |
| RTL8156B (7410) | 2500; partner 1000: 1000 | PASS |
| RTL8153D (7420) | 1000 | PASS |
| RTL8157 (1030) | 5000 | PASS |
| RTL8159 (2020) | 5000; `--phy-patched`: 10000 | PASS |

Notes on 2000:

- At High Speed the simulated RTL8156 was never configured: six control
  requests, then nothing, so no driver was reached. 98 SE and ME
  configured the same device at High Speed on the same QEMU. This is
  for xHCI98's 2000 path, not for this driver.
- Each unplug showed 2000's "Unsafe Removal of Device" box.
- At each unplug the trace shows "filter 0: -4": NDIS clears the packet
  filter after the device has gone.

## Open

- REAL CHECK NEEDED: no adapter with any of these chips has run.
  - Which USB id an RTL8153D uses is not in Linux's table; the simulator
    uses 0BDA:8053.
  - Whether an RTL8159 without its patch links at 5 Gb/s or not at all
    against a 10 Gb/s partner.
  - Link quality without the PHY patches, on every chip.
- 98 SE's TCP/IP binding limit: a user who plugs many adapters (or one
  adapter at several speeds and ports) may see a new one get no TCP/IP.
