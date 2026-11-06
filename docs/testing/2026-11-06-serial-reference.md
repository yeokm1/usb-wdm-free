# usbcom's chip simulator against the makers' own drivers, 2026-11-06

`chipemu.py` is the simulated FTDI, PL2303 and CP210x bridge that the
`usbcom` chip-module legs run against
([chip legs](2026-10-29-usbcom-chips.md)). Until now it had only been
checked against the requests `usbcom` itself sends. Here each maker's own
Windows driver was run against it in a guest, the USB traffic was recorded,
and the simulator was changed where it did not answer what the maker's
driver asks. `usbcom` was then run again against the changed simulator on
the systems of its earlier chip legs.

The makers' drivers were only installed and run inside guests. Their files
were not read, disassembled or debugged; what is recorded below is the
traffic on the simulated USB device and what Windows reported. No physical
bridge was used, and no README "Tested by the author" cell changes.

## Results

| Chip family | Maker driver and version | OS | Result | Gaps | Model changed | usbcom retested |
|---|---|---|---|---|---|---|
| FTDI FT-X, FT232H, FT4232H, FT232BM, FT2232C | CDM 2.12.36.20 (WHQL, `ftdibus.inf` and `ftdiport.inf`) | Windows 10 22H2 x64 | After the fixes every part gets its COM port(s). `COMTEST full` 61 of 62 on each first port, `quick` passes on the others, and every rate from 50 to 12000000 is set and echoed. | EEPROM read stalled; wIndex 0 on one-port parts; `SET_EVENT_CHAR` | y | PASS: FT-X, FT232H, FT4232H on 98 SE (xHCI98, NUSB EHCI) and ME |
| FTDI FT232BM | FTDI 98 SE / ME driver (`FTDIBUS.INF` 1.00.2162, `FTDIPORT.INF` 1.09.06) | 98 SE, xHCI98 3.2.0.0 | COM2. `COMTEST full` 56 of 62. | EEPROM read before `SET_CONFIGURATION`; no status packets while idle (98 SE hung once); 1.5 stop bits | y | PASS: FT-X, FT232H, FT4232H on 98 SE (xHCI98, NUSB EHCI) and ME |
| FTDI FT2232C | FTDI FT2232 98 SE / ME driver 1.00.00 (`FTCUSB.INF`, `FTCSER.INF`) | 98 SE, xHCI98 3.2.0.0 | COM2 and COM3. `COMTEST full` 56 of 62 on COM2, `quick` passes on COM3. | as the FT232BM | y | PASS: FT-X, FT232H, FT4232H on 98 SE (xHCI98, NUSB EHCI) and ME |
| FTDI FT-X | CDM 2.12.36.20 | XP SP3 x86 | Installed from the INF's undecorated section (with "Continue Anyway" at the logo warning), then Code 39: the bus driver does not load, and the part gets no request past enumeration. | none shown | n | - |
| Silicon Labs CP2102, CP2105, CP2108 | CP210x Universal Windows Driver 11.6.0.420 (`silabser.inf`) | Windows 10 22H2 x64 | After the fixes every port starts. `COMTEST full` 59 of 62 on the CP2102, on both CP2105 interfaces and on the CP2108's first port; `quick` passes on the CP2108's others. | `GET_PROPS`; settings before `IFC_ENABLE`; `GET_COMM_STATUS` to the device; interface strings; `IMM_CHAR`; 1.5 stop bits; the framings `GET_PROPS` offers | y | PASS: CP2105, CP2108 on 98 SE (xHCI98, NUSB EHCI) and ME |
| Prolific PL2303 HX, HXD, TA and G | PL23XX installer 4.3.0.0 (PL2303 driver 3.8.43.0, PL2303G driver 5.1.7.0) | Windows 10 22H2 x64 | No variant starts (Code 10). | Identification reads whose answers no public source gives (not modelled) | y (a register read-back) | PASS: H, HX, HXD, TA, TB, G on 98 SE (xHCI98, NUSB EHCI) and ME |
| WCH CH9102 | CH343SER | - | Not run. | `chipemu.py` has no CH9102: `usbcom` serves it as a CDC-ACM device, and `acmemu.py` has no CH9102 profile. | n | - |

"Model changed" names the simulator, not `usbcom`: `usbcom` itself did not
change.

## Setup

- **Simulator.** `chipemu.py` over QEMU `usb-redir` on 127.0.0.1, each plug a
  fresh process with its own JSONL log. The first runs used `chipemu.py` as
  on `main` (3942e20, SHA-256 `b62dbd79...ddce0a`); each gap was fixed and
  the part plugged again. The fixes are commits `dae6af1`, `6ae42d8` and
  `8eff292`.
- **Windows 10.** The q35 base `w10x64` (22H2, build 19045) on a fresh
  overlay, QEMU 11.1.0 TCG, 2 vCPUs, the simulator on `qemu-xhci` (Windows'
  own xHCI driver), with a `pcap=` capture of each plug. FTDI's and Silicon
  Labs' INFs were added with `pnputil /add-driver ... /install`; Prolific's
  installer was run and its licence accepted inside the guest. The guest
  ran `COMTEST.EXE -noe9 -cts COMn full` (the `usbcom` test program), a
  PowerShell `SerialPort` script that sets 50 rates from 50 to 12000000 and
  echoes 64 bytes at each, and sends the results to COM1.
- **98 SE.** The chip-leg base with xHCI98 3.2.0.0 (`w98-x32`, guest `x` of
  the [chip legs](2026-10-29-usbcom-chips.md)), fresh overlays, the maker's
  files on `A:`, installed through the Found New Hardware wizard.
  `COMTEST -cts COMn full` with the 0xE9 log. The simulator's own log is the
  capture for these runs (no `pcap=`).
- **XP.** The q35 base `xp` (SP3 x86) on a fresh overlay, 1 vCPU, the
  simulator on `piix3-usb-uhci`; FTDI's INF applied with the WDK 7.1's
  `devcon update`.
- **Not run.** FTDI CDM 2.06.00 (2000), the FT232AM, the PL2303 TB, and
  the CP2101, CP2103 and CP2104 with Silicon Labs' driver. The FT232AM, FT232BM, FT2232C and
  CP2101 to CP2104 parts were added to the simulator for these runs; `usbcom`
  lists none of their ids in `USBCOM.INF` (manual model only), so they have
  no `usbcom` leg.

## What the makers' drivers asked, and what changed

### FTDI

- **`READ_EEPROM` (0xC0/0x90, wIndex the word address).** CDM 2.12.36.20
  reads 64 words (128 on the FT2232C, 256 on the FT4232H) after the
  configuration; the 98 SE drivers read them before `SET_CONFIGURATION`.
  The simulator stalled them. On Windows 10 the bus driver then started but
  made no COM port, and that device instance kept no COM port when plugged
  again with the fix (a new port number, a new instance, was needed). Now
  every word reads FFFF, a blank EEPROM, configured or not. An FT-X image
  laid out as FTDI's AN_201 describes was tried and was not needed: the
  blank EEPROM gives the FT-X its COM port too.
- **wIndex 0.** CDM 2.12.36.20 sends wIndex 0 to the FT232H and FT-X;
  `usbcom` sends 1. The simulator took only 1 to n. Now 0 is the
  port of a one-port part. The FT4232H gets 1 to 4 from both drivers, and 0
  still stalls there.
- **`SET_EVENT_CHAR` and `SET_ERROR_CHAR` (6, 7).** Sent at each open; they
  stalled. Now taken and logged.
- **The latency timer.** CDM 2.12.36.20 and the 98 SE FT232BM driver set it
  (9, 16 ms). The simulator answered a waiting bulk IN only when it had
  data or a changed status. With FTDI's 98 SE driver on the FT232BM, 98 SE
  stopped responding after the install (desktop frozen, the CPU busy in the
  kernel, one bulk IN waiting). The simulator now sends the two status
  bytes when the latency timer runs out, as the [chip legs](2026-10-29-usbcom-chips.md)
  record of a real part, keeps `SET_LATENCY_TIMER` and reads it back; the
  FT232AM, which has none, stalls both (Linux tells the AM from the BM so).
  With it the same install on a fresh overlay did not hang. One hang and one
  clean run do not prove the cause.
- **1.5 stop bits.** FTDI's 98 SE drivers send `SET_DATA` with stop bits 1
  (1.5), which `ftdi_sio.h` defines; it stalled. Now taken.

What stays different, and why it is not a simulator gap:

- `COMTEST`'s `fOutxCtsFlow` check with CTS off fails on every FTDI part
  with every FTDI driver: FTDI's drivers switch on the part's RTS/CTS
  handshake before they lower RTS, and the simulator ignores RTS writes in
  that mode (design 6.3 takes the part to drive RTS itself then), so the
  test plug keeps CTS up and the bytes come back. `usbcom` holds the write itself; `COMTEST`'s expectation is
  `usbcom`'s.
- FTDI's 98 SE drivers (FT232BM and FT2232C alike): a 65536-byte write
  times out with nothing written, a 30000-byte write times out after 8192
  bytes, `EV_RXCHAR` and `EV_TXEMPTY` are not signalled, a looped-back
  break raises `EV_BREAK` but sets no `CE_BREAK`, and 6N1 is passed to the
  part (which takes 7 or 8 data bits; the simulator stalls it). The
  same simulator passes the large writes with CDM 2.12.36.20 on Windows 10,
  so the write stop is not shown to be the simulator's; its cause was not
  found.
- FTDI's FT2232 98 SE driver sends `SET_FLOW_CTRL` and `SET_ERROR_CHAR` with
  wIndex 3 on the FT2232C (a two-port part) as port B closes; the simulator
  stalls them and the driver clears the endpoint halt and carries on.

### Silicon Labs

- **`GET_PROPS` (0xC1/0x0F).** Asked first, before `IFC_ENABLE`, with
  wLength 164, then 66 and 64; it stalled and the port never started. Now
  a 66-byte answer laid out as a published CP2102N dump (linux-usb, 2021;
  the fields AN571 names in its Table 7), with each part's rate and
  framings: 8 data bits, 1 stop bit and no, odd or even parity on the
  CP2101 and the CP2105's second interface. The driver then offers only those framings, as `COMTEST`
  shows on the CP2105.
- **Settings before `IFC_ENABLE`.** The driver sets the line, flow,
  characters (`SET_CHARS`, 0x19) and rate before it enables the interface.
  The simulator stalled every request before `IFC_ENABLE` and the port
  failed to start ("A device attached to the system is not functioning").
  Now the settings are taken and logged (`before_enable`); data sent before
  `IFC_ENABLE` is still dropped. `GET_CHARS` reads them back.
- **`GET_COMM_STATUS` (0x10) to the device.** Sent with bmRequestType 0xC0,
  not 0xC1. Now the 0x40/0xC0 forms are taken as the interface requests and
  the 19-byte status of `cp210x.c` is answered.
- **Interface strings.** On the CP2108 the driver reads string 3 + n for
  interface n and failed each port's start (Code 10) when it stalled. The
  CP2105 and CP2108 now name each interface in its own string.
- **`IMM_CHAR` (6).** `TransmitCommChar` sends it; it stalled. Now taken (a
  byte only) and looped back.
- **1.5 stop bits.** The driver sends 5N1.5; it stalled. Now taken with 5
  data bits only.

What stays different: `EV_BREAK` and the looped-back break (the simulator
loops no break back on CP210x, as in the chip legs), `SetCommBreak` on the
CP2105's second interface (refused, as before), 7S1.5 (refused by the
driver itself), rates above 2000000 (refused by the driver), and the
`fOutxCtsFlow` check, which expects `usbcom`'s hold where the part holds
(`COMTEST -ctshw`).

### Prolific

Every older variant (HX, HXD, TA) gets the same requests: reads of 0x8C to
0x8F and 0x9494 (wLength 2), a write of 0xAA to 0x0606, a read of 0x8686, a
write of 0 to 0x0606, and nothing more; Windows reports Code 10. The G
(23A3) gets a read 0x01/0x0081, which stalls (as Linux expects of a G), then
request 0x83 with values 0x1B and 0xE7, which stall; Code 10.

- The simulator stored a write of 0x0606 under 0x0606 and read 0x8686 from
  register 6, so a written register did not read back. It now keys the
  older parts' registers by the low seven bits, as the reads already did
  (`pl2303.c` pairs 0x0404 with 0x8484). This did not make any variant
  start.
- The driver gives up after the reads of 0x0C to 0x0F and 0x14 (the older
  variants) or request 0x83 (the G). Neither Linux nor a public Prolific
  document says what a real chip answers to them, so the simulator does
  not model them (it answers 0, or stalls 0x83) and no variant can start
  under Prolific's driver. The HX's refusal by design cannot be
  told apart from this gap here.
- The simulator presents: HX (bcdUSB 1.10, bcdDevice 3.00), HXD (1.10,
  4.00), TA (2.00, 3.00) and G (067B:23A3, 2.00, 1.00).

## usbcom again

`usbcom` itself did not change. It was built from `w47-ref3` (`build.cmd w98
trace`: `USBCOM.SYS` `5327a293...`, `USBCOMV.VXD` `9ea71dd6...`, the same VxD
as the chip legs; the build's INF mutation gate "bad date" fails on `main`,
as fixed on another branch, and the binaries are the ones it built).
`COMTEST.EXE` from `comtest.cmd` (`f88172d0...`). The legs are those of the
[chip legs](2026-10-29-usbcom-chips.md) (`leg.ps1`: `full` on the first
port with the driver's own rate range, `quick` on the others, the pull while
open) on fresh overlays of the same bases.

Every leg below ran the simulator at `8eff292` (SHA-256 `1c74efee...59a8fb`,
a copy kept so that the legs could not see a later edit), with the trace
build. A cell is `full` (checks passed / run), the other ports' `quick`,
and the pull while open (`hold`); "Model" is the chip legs' `evcheck.py`
reading of the simulator's log (no error event; the PL2303 start sequence
audited; on CP210x the 16 bytes reached the part while CTS was low).

| Part | 98 SE, xHCI98 3.2.0.0 (`x`) | 98 SE, NUSB 3.3 EHCI (`n`) | ME, xHCI98 3.2.0.0 (`m`) |
|---|---|---|---|
| FT-X | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| FT232H | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| FT4232H | 66/66, 3 x quick, hold PASS | 66/66, 3 x quick, hold PASS | 66/66, 3 x quick, hold PASS |
| PL2303 H | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| PL2303 HX | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| PL2303 HXD | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| PL2303 TA | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| PL2303 TB | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| PL2303 G | 66/66, hold PASS | 66/66, hold PASS | 66/66, hold PASS |
| CP2105 | 65/65, quick, hold PASS | 65/65, quick, hold PASS | 65/65, quick, hold PASS |
| CP2108 | 65/65, 3 x quick, hold PASS | 65/65, 3 x quick, hold PASS | 65/65, 3 x quick, hold PASS |
| Model | PASS on every part | PASS on every part | PASS on every part |

Every check passed on every system, so the earlier chip legs' results
stand with the changed simulator, and the 64 KiB echo that failed under
NUSB's EHCI in the chip legs passed this time.

Two runs before these are not the record. On guest `x` every part also
passed with the simulator part-way through the changes. On guest `n` the
FT-X, FT232H, FT4232H and PL2303 H first ran with an earlier commit of the
changes: the FT-X lost 4080 bytes of a 30000-byte echo and the FT232H 16
bytes at offset 4080 of a 16 KiB echo, the signature of the loss under
NUSB's EHCI that the chip legs suspect below the driver
([chip legs](2026-10-29-usbcom-chips.md), "Below the driver"). The four
were run again at `8eff292` and passed; the table gives those runs.

## Evidence

All of it is in `E:\xhci98-scratch\w47\ref3\evidence\`:

- `w10\`: each Windows 10 plug's simulator log (`w10-<part>.jsonl`) and
  capture (`w10-<part>.pcap`), the guest's COM1 log (`serial-a.log`) with
  every `COMTEST` and `sertest` line, and screenshots. The host restarted
  during the work; `before-power-cut\` holds the first FTDI and CP2102
  runs' logs and captures (their guest's COM1 log was not kept).
- `xp\`: the XP run's simulator log, capture and COM1 log.
- `98\maker\`: the 98 SE runs of FTDI's drivers (`x-bm*`, `x-2232c*`) and
  their 0xE9 logs (`e9-f*.log`); `98\shots\` the screenshots.
- `98\x\`, `98\n\`, `98\m\`: the `usbcom` legs,
  `<guest>-<part>-summary.txt` (`x2-` and `n2-` on `x` and `n`), the
  simulator logs and the 0xE9 logs; `before-freeze\` the earlier runs
  described above.
- `models\`: the simulator as on `main` and as the `usbcom` legs ran it.
