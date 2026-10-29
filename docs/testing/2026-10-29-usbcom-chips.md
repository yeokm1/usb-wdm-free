# usbcom's chip modules (M5) on Windows 98 SE and ME, 2026-10-29

These are the first guest runs of `usbcom`'s chip modules
([design](../design/serial.md) 6.3, milestone M5). The modules drive the USB
serial bridges that no vendor driver serves on 98 SE and ME:

- FTDI FT-X, FT232H and FT4232H;
- Prolific PL2303 (H, HX, HXD, TA, TB and the G series);
- Silicon Labs CP2105 and CP2108.

Every leg is QEMU (TCG) with one of two devices: this package's SIMULATED
bridges (`chipemu.py`), or QEMU's own `usb-serial` (an FT232BM). Each result
is a VM pass, never a "Tested" cell. No physical bridge was used; real chips
are xHCI98's bench task 45-E.14. Nothing is released and no README "Tested
by the author" cell changes.

QEMU's FT232BM found one driver fault, fixed in `6db425a` (section "QEMU's
FTDI model"). Every simulated part then passes every check on 98 SE under
xHCI98, on 98 SE under NUSB's EHCI and on ME under xHCI98, with one
exception: a 64 KiB echo under NUSB's EHCI failed once each on four parts.
The loss is suspected to be below the driver, but that is not proven
(section "Below the driver").

## Builds

Two commits were built, each from a clean tree with `build.cmd w98 free`
and `build.cmd w98 trace`. Gates and host tests passed for both: 200337
`acmhost.c` checks, 25 INF mutations rejected, `chiptest.py` 7 tests OK
and `acmtest.py` 28 tests OK. `COMTEST.EXE` comes from `comtest.cmd`.

| File | Flavour | Commit | SHA-256 |
|---|---|---|---|
| `USBCOM.SYS` | free | `66fa74a` | `cb47460c1146cf5745eb47fb8e2e1df6293d29351aed842b663c6cd1c290c16c` |
| `USBCOM.SYS` | trace | `66fa74a` | `98b6d6e972c990a3dd65b8e1dcc13080bb9d62f034207bc15566b5eaae2083d4` |
| `USBCOM.SYS` | free | `6db425a` | `1c2b7b73706864b2979a4d0ecb987a6e43811a52a5fc73ad2903fb8f05e5a984` |
| `USBCOM.SYS` | trace | `6db425a` | `a785c888670e7f48ea62563833eb9fe70c115e8633299de4a5831a7c281ed857` |
| `USBCOMV.VXD` | free | both | `93c91d81134c95c125f302fdb9160f8641156fa73c9b46cc046aa6ab97b7371a` |
| `USBCOMV.VXD` | trace | both | `9ea71dd6591aa24ca5b7988abdb7c31200a5bccd5fb6f1f063fef3d64d5d8dfd` |
| `USBCOM.INF` | - | both | `d05c3dcd6b29bed541c3ec79c4a0ecce150b5ad02465838b9c4d6e623e72ca86` |
| `USBCOMP.INF` | - | both | `8c7f54dd298f7aa332f9e773abeddae4664c5dbff4016781e5ea37b51910f62b` |
| `COMTEST.EXE` | - | both | `b707ef53222c306fa38cd6282be2433a2240f5522b5a53ddedb18b765d2a4aba` |

`6db425a` changes only `ucio.c` and `usbcom.h`, so the VxD is the same file
in both commits. The three guest tables in "Legs" and the free-build legs
on `n` ran the `66fa74a` files. The free-build legs on `x` and `m`, the QEMU
FTDI leg and the FTDI and CDC-ACM regression legs ran the `6db425a` files.

## Setup

The kit is `E:\xhci98-vm\p45\w45-serial\`: scripts under `scripts\` and
ports 49230-49239. Each guest is headless with `-machine pc -accel tcg -cpu
pentium3,-apic -smp 1 -m 256` and `-action reboot=shutdown`, no NIC, on
QEMU 11.1.0. Each runs on a fresh overlay of a prepared, read-only base:

| Guest | Base | Device bus |
|---|---|---|
| `x` | 98 SE with xHCI98 3.1.0.0 (the net9x kit's `w98-hcd310`), updated in the kit to xHCI98 3.2.0.0 (`XHCIINST /dir release-x86 /yes`, exit 3, one cold boot) | `qemu-xhci` |
| `n` | 98 SE with NUSB 3.3 and Windows' `USBHUB.SYS` (the net9x kit's `w98-nusb-hub`), with NUSB's EHCI driver installed from `C:\WINDOWS\INF\USB2.INF` at its first boot | `usb-ehci` |
| `m` | ME with xHCI98 3.1.0.0 (`me-hcd310`), updated to xHCI98 3.2.0.0 as `x` was | `qemu-xhci` |

**Why xHCI98 3.2.0.0, not 3.1.0.0.** A development run under 3.1.0.0
configured only two of the FT4232H's four functions. The `SELECT
CONFIGURATION` of the third and fourth failed with `0xC000009A`. 3.1.0.0
allows four transfer rings per device, and each FT4232H function has two
endpoints. xHCI98 task 43.67 raised the limit to 16 in 3.2.0.0.

**Why not the q35 bases.** The xHCI98 q35 bases were not used. Their
emulator has no `usb-redir`, and QEMU 11.1.0, which has it, stopped at the
98 SE logo on the q35 base twice.

**Installing.** `usbcom` was installed once per guest with the first part
plugged in (the FT-X), through the Found New Hardware wizard from `A:`. The
`USB\VID_...` wizard comes first, then the `USBCOM\COMPORT` child's wizard.
Each later part's models were then in the installed INF.

**Shutting down.** A 98 SE or ME guest is stopped only by
`rundll32.exe user.exe,ExitWindows` or Start > Shut Down. Stopping the QEMU
process instead lost an install once: the registry had not been flushed.

### The simulated parts

`chipemu.py` runs one part per process on 127.0.0.1:49234 or 49235, and
QEMU attaches it with `usb-redir`. Each process:

- answers the part's vendor requests as design 6.3 lists them;
- decodes the rate and framing back to numbers and logs them as JSONL;
- echoes what it receives, with the part's receive framing (the FTDI puts
  two status bytes in each packet);
- loops DTR back to DSR, RTS to DCD and CTS, and a sent break back as a
  received one (not on CP210x).

It also enforces the part's rules:

- **FTDI:** the channel in `wIndex`.
- **PL2303:**
  - class requests with `wIndex` 0;
  - its start sequence before any line coding, audited at the end of the
    leg (`startup ok`);
  - its list of direct rates: an unlisted direct rate runs at 9600;
  - no divisor on the G series.
- **CP210x:**
  - `IFC_ENABLE` before anything else: data sent before it is dropped;
  - the request directions;
  - the framing of the CP2105's second interface;
  - RTS writes are ignored while the chip's own handshake drives RTS.

**Full Speed parts on EHCI.** QEMU's `usb-ehci` has no companion
controller or transaction translator. On guest `n` the Full Speed parts
(FT-X, PL2303, CP2105, CP2108) are therefore presented at High Speed
(`--speed high`, 512-byte bulk packets). That is a simulation artifact.

### The test program

`COMTEST.EXE` (`comtest.c`) is started through Win+R. It logs to the
console, to `A:\COMTEST.TXT` and to port 0xE9, where its lines interleave
with the trace flavour's `uc:` and `uv:` lines. `scripts\leg.ps1` runs each
part:

1. `COMTEST -cts -baud LO HI COMn full` on the first port. LO and HI are
   the driver's own limits, read from the trace's `rates LO to HI` line.
   CP210x parts also get `-ctshw -nobreakin`.
2. `COMTEST COMn quick` on each other port.
3. `COMTEST COMn hold 60 120` on the first port. The part is pulled after
   15 s and plugged back in 15 s later.
4. The model's log is checked with `scripts\evcheck.py`.

`COMTEST full` is the CDC-ACM battery
([usbcom98 legs](2026-10-22-usbcom98-legs.md)) with these additions for the
bridges:

- **Rates.** Every rate in the list that is inside the port's range is
  echoed; those outside it are refused and the state is kept. The port's
  lowest and highest rates are set and echoed, and the rates one below and
  one above are refused.
- **Framings.** `COMTEST` has a table of eight framings (7E1, 8O1, 8N2,
  7M1, 7S1.5, 6N1, 5N1.5 and 8N1), all tried at 9600. Each one the port's
  `COMMPROP` offers is echoed; the others are refused and the state is
  kept. Other combinations are not tried.
- **CTS.** CTS follows RTS (`-cts`), both in `GetCommModemStatus` and as
  `EV_CTS`.
- **`fOutxCtsFlow` with CTS off.** On FTDI and PL2303 the VxD holds the
  write (`fCtsHold`): 16 bytes stay queued and nothing comes back. On CP210x
  (`-ctshw`) the chip holds it: the write drains, the model logs the 16
  bytes arriving while CTS was low, and nothing comes back. With CTS back,
  all 16 bytes come back.

## Legs

A cell gives the ports with the driver's rate range, then the results:
`full` on the first port as checks passed / run, `quick` on the others, and
the pull while open (`hold`, 6 checks). "Model" is PASS when, in the
model's logs of the leg:

- `evcheck.py` finds no error event (wrong index or channel, a request
  before `IFC_ENABLE`, an unlisted direct rate, data dropped);
- on the PL2303, `evcheck.py`'s audit finds the start sequence before any
  line coding;
- on the CP210x, `evcheck.py` finds the 16 bytes arrived with CTS low
  (`ctshw`);
- the decoded rates that `evcheck.py` lists are the rates `COMTEST` set,
  read by hand. The FTDI parts and the CP2105's interface 0 give the
  nearest rate they make ("What the rates show"). The decoded framings
  were not compared.

### 98 SE under xHCI98 3.2.0.0 (guest `x`)

| Part | Ports (rates) | full | quick | hold | Model |
|---|---|---|---|---|---|
| FT-X | COM3 (184-3000000) | 66/66 | - | PASS | PASS |
| FT232H | COM4 (184-12000000) | 66/66 | - | PASS | PASS |
| FT4232H | COM4-COM7 (184-12000000 each) | 66/66 | 3 x PASS | PASS | PASS |
| PL2303 H | COM8 (75-1228800) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HX | COM8 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HXD | COM8 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TA | COM8 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TB | COM9 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 G | COM10 (75-12000000) | 66/66 | - | PASS | PASS |
| CP2105 | COM11 (2400-921600, interface 1), COM12 (300-2000000, interface 0) | 65/65 on COM11 | PASS | PASS | PASS, `ctshw` |
| CP2108 | COM13-COM16 (300-2000000 each) | 65/65 | 3 x PASS | PASS | PASS, `ctshw` |

CP210x runs have one check fewer: the looped-back break is not expected
(`-nobreakin`).

The FT-X and FT232H ran on a first overlay that a hard stop later spoiled
(see "Setup"); the other parts ran on a fresh overlay with `usbcom`
installed again. COM numbers are reused across overlays.

### 98 SE under NUSB 3.3's EHCI (guest `n`)

| Part | Ports (rates) | full | quick | hold | Model |
|---|---|---|---|---|---|
| FT-X | COM3 (184-3000000) | 65/66: 64 KiB echo | - | PASS | PASS |
| FT232H | COM4 (184-12000000) | 65/66: 64 KiB echo | - | PASS | PASS |
| FT4232H | COM5-COM8 | 65/66: 64 KiB echo | 3 x PASS | PASS | PASS |
| PL2303 H | COM9 (75-1228800) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HX | COM4 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HXD | COM4 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TA | COM4 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TB | COM5 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 G | COM6 (75-12000000) | 66/66 | - | PASS | PASS |
| CP2105 | COM7 (300-2000000, interface 0), COM8 (2400-921600) | 64/65: 64 KiB echo | PASS | PASS | PASS, `ctshw` |
| CP2108 | COM9-COM12 | 65/65 | 3 x PASS | PASS | PASS, `ctshw` |

### ME under xHCI98 3.2.0.0 (guest `m`)

| Part | Ports (rates) | full | quick | hold | Model |
|---|---|---|---|---|---|
| FT-X | COM3 (184-3000000) | 66/66 | - | PASS | PASS |
| FT232H | COM4 (184-12000000) | 66/66 | - | PASS | PASS |
| FT4232H | COM5-COM8 | 66/66 | 3 x PASS | PASS | PASS |
| PL2303 H | COM9 (75-1228800) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HX | COM9 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 HXD | COM9 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TA | COM9 (75-6000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 TB | COM10 (75-12000000) | 66/66 | - | PASS | PASS, startup audited |
| PL2303 G | COM11 (75-12000000) | 66/66 | - | PASS | PASS |
| CP2105 | COM12 (300-2000000, interface 0), COM13 (2400-921600, interface 1) | 65/65 on COM12 | PASS | PASS | PASS, `ctshw` |
| CP2108 | COM14-COM17 (300-2000000 each) | 65/65 | 3 x PASS | PASS | PASS, `ctshw` |

NUSB's EHCI is 98 SE only, and this kit has no USB 2.0 stack for ME, so ME
has no EHCI leg (design 12, M5).

### Free build

The free build was run on a subset of the parts.

| Guest | Part | full | quick | hold |
|---|---|---|---|---|
| `n` (`66fa74a`) | FT-X | 65/66: 64 KiB echo | - | PASS |
| `n` (`66fa74a`) | PL2303 G | 66/66 | - | PASS |
| `n` (`66fa74a`) | CP2108 | 65/65 | 3 x PASS | PASS |
| `x` (`6db425a`) | FT-X | 66/66 | - | PASS |
| `x` (`6db425a`) | PL2303 G | 66/66 | - | PASS |
| `x` (`6db425a`) | CP2108 | 65/65 | 3 x PASS | PASS |
| `m` (`6db425a`) | FT-X | 66/66 | - | PASS |
| `m` (`6db425a`) | PL2303 G | 66/66 | - | PASS |
| `m` (`6db425a`) | CP2108 | 65/65 | 2 x PASS | PASS |

The free build writes no trace, so `leg.ps1 -Free` takes the ports from
`COMTEST list`, which tries COM1 to COM16 only. On `m` the CP2108's fourth
port, COM17, was therefore not tested with the free build. In the free
legs no `uc:` line reached port 0xE9, which confirms that the free files
were the ones loaded.

### What the rates show

The model decodes each rate from the part's own encoding and logs it:

- **PL2303 and CP210x:** every listed rate comes back exact. The PL2303 G
  takes the rate directly; the older parts take it directly or through the
  divisor, as design 6.3 says.
- **FTDI:** the rates are the nearest divisors. On FT-X, 115200 is 115385
  and 57600 is 57554. On FT232H and FT4232H (the 120 MHz clock), 115200 is
  115246 and 57600 is 57588. Within 0.2 %, a real UART at the other end
  takes these.
- **CP2105, first interface (interface 0):** 115200 is logged as 115384 and
  57600 as 57553. This interface takes the actual rate the part makes
  (design 6.3, `cp210x_get_actual_rate`), not the AN205 table, and the
  model logs that rate. The second interface's rates come back exact.

## QEMU's FTDI model

QEMU's `usb-serial` is an FT232BM (`0403:6001`, `bcdDevice` 0x0400) written
independently of this package. `USBCOM.INF` does not list `0403:6001`, so
the leg reaches it through the manual model:

1. Found New Hardware: "Display a list of all the drivers in a specific
   location", "Universal Serial Bus controllers", Have Disk `A:\`.
2. Choose "USB serial port (CDC-ACM), manual selection" and confirm the
   warning.
3. The child's wizard installs `USBCOMP.INF`.

The trace showed `family 1, bcdDevice 0400`, `part 2` (FT232BM), `rates 184
to 3000000`, `port 0 is COM17`.

**The fault (`66fa74a`).** With a null backend, `COMTEST COM17 quick`
opened the port and set 115200 8N1, but every write timed out (error 1121,
nothing written). After an open, `USBCOM.SYS` sends no bulk OUT on an FTDI
until the chip's first status report arrives (design 6.3). A real chip
sends one within its latency timer. QEMU's model answers bulk IN with NAK
until it has data, so the report never came and the port could never send.

**The fix (`6db425a`).** While the FTDI waits for its first report, the
status poll timer runs. Once two whole periods (`UC_POLL_MS`, 100 ms) have
passed after the opens with no report, bulk OUT goes ahead on the CTS last
known. The trace logs `COM17 FTDI: no status report after the open,
transmitting` at each open.

**With the fix.** The leg ran on guest `x` with the trace build of
`6db425a`. QEMU's chardev was a socket to `scripts\echosrv.py`, which
echoes every byte: a loopback with no control lines.

- **`COMTEST -baud 184 3000000 COM17 full`: 52 of 62 checks pass.** Echo
  (1 byte to 64 KiB), every rate in the list, both ends of the range and the
  refusals beyond them, the framings of COMTEST's table that the port
  offers and the refusal of the others, the timeouts, `GetCommProperties`, purges, `TransmitCommChar`,
  `SetCommBreak`, a 30000-byte write, `EV_RXCHAR`, `EV_TXEMPTY`, the second
  open's refusal and five opens.
- **The 10 failures are the backend, not the driver.** They are the DTR and
  RTS loopback checks, their `EV_DSR` and `EV_RLSD` events, and the
  looped-back break. The socket backend reports CTS, DSR and DCD always on
  (modem status `b0`) and carries no break.
- **The divisors QEMU accepted** (the `SET_BAUDRATE` `wValue`, BM
  encoding): 300 `2710`, 9600 `4138`, 115200 `001a`, 3000000 `0000`. Each is
  the BM divisor for its rate, and QEMU stalled no request.
- **`COMTEST COM17 hold 60 120`** (pulled after 15 s, back 15 s later):
  PASS, 6 of 6.

The simulated FTDI parts send a report in every packet, so they did not
show the fault. After the fix they were run again on `x` with the
`6db425a` trace build:

| Part | Ports | full | quick | hold |
|---|---|---|---|---|
| FT-X | COM3 | 66/66 | - | PASS |
| FT4232H | COM4-COM7 | 66/66 | 3 x PASS | PASS |
| FT232H | COM18 | 66/66 | - | PASS |

## CDC-ACM regression

The chip modules changed the shared core (`ucio.c`, `usbcom.c`,
`usbcomv.c`, `acmcore.c` and `ucif.h` version 2), so the CDC-ACM legs were
run again with `acmemu.py --mirror` on guest `x`, with the trace build of
`6db425a`:

| Profile | Port | full | hold |
|---|---|---|---|
| Uno (`2341:0043`, Full Speed) | COM19 | 58/58 | PASS |
| Pico (`2E8A:000A`, IAD composite, Full Speed) | COM20 | 58/58 | PASS |

The Pico model's first plug on this overlay needed two extra steps:

- The first devnode's start failed (`FDO start: c0000001, 0 ports`)
  before the `device` trace line, so one of the descriptor requests failed
  below `usbcom`.
- The vendor interface got the Found New Hardware wizard, which found no
  driver, as expected. It was finished without one.

`&MI_00` then bound as COM20, and the leg above is the next plug.

## Below the driver: NUSB's EHCI under QEMU

The 64 KiB echo failed under NUSB, once in each `full` run, on the FT-X,
FT232H, FT4232H and CP2105 (and on the FT-X again in the free-build run).
It passed on every PL2303 and on the CP2108. In each failure the model's
log shows all 65536 bytes received and sent back, and `USBCOM.SYS` logged
no transfer error, reset or resubmission. The bytes missing in the program
were:

- a 16-byte tail at offset 4080 (FTDI): the last packet of a model
  response that crossed a guest URB boundary;
- a 4 KiB block at offset 40960 (CP2105).

Under xHCI98 every part passes it, the High Speed FT232H and FT4232H
included, and six `COMTEST COMn echo 65536` runs on their own under NUSB
passed.

Where the bytes went is not proven. The model delivered them, and the
program did not get them, but nothing recorded here shows whether they were
lost in QEMU's EHCI model, in NUSB's driver or in `USBCOM.SYS`'s receive
path. The loss is therefore only suspected to be below the driver. It is
not the same as the CDC-ACM legs' NUSB fault
([usbcom-nusb-echo](2026-10-23-usbcom-nusb-echo.md)): that was an OUT block
replayed by QEMU's EHCI model, which `USBCOM.SYS` now avoids by keeping one
bulk OUT in flight. These are IN losses with one OUT in flight. A closer
reading (QEMU's EHCI trace events with the model's packet ids), and a real
controller with NUSB, are owed.

## Evidence

All of it is in `E:\xhci98-vm\p45\w45-serial\`:

- `evidence\<guest>-<part>-summary.txt`: each leg's summary, with every
  COMTEST result line and failed check. `<guest>` is `x`, `n` or `m`; `nf`
  and `xf`/`mf` are the free-build runs; `x2` is the FTDI rerun after
  `6db425a`; `x-qemuftdi` is QEMU's FT232BM; `x-uno` and `x-pico` are the
  CDC-ACM regression.
- `evidence\<guest>-<part>.jsonl` and `-replug.jsonl`: the model's log for
  each plug.
- `g\<guest>\e9-<tag>.log`: the 0xE9 logs (trace lines and COMTEST).
- `pids.txt`: each QEMU and model process with its start and stop.

## Owed

- Real chips: xHCI98's bench task 45-E.14 (FT-X, FT232H, FT4232H, PL2303
  old and G, CP2105, CP2108), under xHCI98 and under NUSB's EHCI behind a
  High Speed hub.
- ME under an EHCI stack (none in this kit).
- Standby and resume with a bridge port open (the restore path, design
  7.10, runs only in the host tests).
- Two bridges at once.
- Where the 64 KiB IN loss under NUSB/QEMU happens, and the same echo on
  a real EHCI controller with NUSB.
