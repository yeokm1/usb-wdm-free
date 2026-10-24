# Simulator coverage: what a physical device or a simulator exercises, per driver

Owner, 2026-10-23: add simulator profiles for every driver whose physical
device is missing, or whose physical device does not show the driver's full
characteristics. This page is the audit that decides which profiles come
first. It was compiled on 2026-10-23 from the design documents, the driver
sources (what the code implements, not only what the design plans), the
testing records in this folder and the simulators' own sources. Update it
when a profile or a physical leg is added.

Terms:

- **Claimed**: the design says so AND the driver's code implements it.
  Features the design plans but the code does not yet have are listed only
  where a simulator profile would be premature because of them.
- **Physical**: a recorded run of this repository's driver with a real
  device (any OS, any host controller). Host libusb reads of a chip without
  our driver count as "chip only".
- **Simulator**: a usb-redir model in this repository (`uvcsim.py`,
  `uacemu.py`, `uac1emu.py`, `midiemu.py`, `acmemu.py`, `xpademu.py`) or a
  QEMU device model (`usb-net`, `usb-storage`), with the guest leg that ran it.
- **Gap**: claimed, and exercised by neither.

Owner's devices (as recorded): Logitech Brio 100 (UVC 1.0 camera with a
UAC1 microphone), Creative Sound Blaster X4 (UAC2), Mitsumi USB floppy
`03EE:6901` (UFI CBI, Full Speed), an ASIX AX88179 dongle, a Roland UM-ONE
mk2 (`2026-10-21-umone2.md`). No Arduino Uno or Pico board run is recorded;
no RTL815x, CDC-NCM, RNDIS phone or Xbox pad is on the bench.

## Ranking (release relevance)

The release gate is 98 SE under NUSB's EHCI and under xHCI98
(`AGENTS.md`). Rank 1 is the most relevant.

| Rank | Driver | Gap | Profile | State |
|---|---|---|---|---|
| 1 | `uvc98` | UVC 1.5: `bcdUVC 0x0150`, the 48-byte probe/commit, 1.5 VC descriptors (3-byte PU controls, encoding unit), formats the parser must skip (frame-based, H.264), NV12 | `uvcsim.py --uvc 1.5` (UVC 1.0 and 1.1 profiles exist) | this task |
| 2 | `rtl815x` | everything: no device at all | new `rtlemu.py`, RTL8152/8153 bring-up, MAC, link, bulk RX/TX | this task |
| 3 | `cdcnet` | CDC-NCM has never run on a guest; link-state and speed notifications never seen (QEMU `usb-net` sends only "connected") | new NCM model (NTB16, notifications) | this task, after 1-2 |
| 4 | `xpad98` | Xbox 360 wireless receiver with more than one pad: `xpademu.py receiver` populates slot 0 only, no connect/disconnect on a live receiver | `xpademu.py receiver --pads N` | this task, after 3 |
| 5 | `usbcom` | more than one CDC-ACM function per device (the driver reports one COM port per function, up to `AC_MAX_FUNCS` 4); `acmemu.py` has one function | `acmemu.py --profile dual` | after 4 |
| `ufiemu.py` (new: UFI CBI and `--cb`, unlisted FFFF:0020/0021, FAT12 medium with CRC list; gate item: usbflop NT medium reads) | fc5cd5e | `ufisimtest.py`: 17, incl. the CBI stall and QEMU's interrupt-stop race | not run (the gate item is NT) | not run; 2000, XP and 7 x64 READ PASS for CBI and CB under UHCI (`2026-10-24-usbflop-nt-xpad-x64.md`) |
| `xpademu.py xone` on the x64 build (gate item: xpad98 x64) | - | as above | - | XP x64 and Vista x64 (throwaway in-guest test certificate, testsigning) JOYTEST PASS under usb-ehci (`2026-10-24-usbflop-nt-xpad-x64.md`) |
| `midiemu.py umtab/quad/wide/intr` (`--pid`) | 087cc87, 91fb2d5 | `midisimtest.py`: 15, through `msdesc.c` | umtab PASS on a fresh overlay | umtab, quad (4 cables), wide (2 pipes, 6 ports), intr (interrupt EPs): PASS (`2026-10-23-receiver-dualacm-sim.md`) |
| 6 | `uac2` | non-nominal / drifting / lost feedback (the cause of the NUSB EHCI FAIL is feedback lost below the driver); `uacemu.py` sends exactly 6<<16 every 1 ms | `uacemu.py --fb-ppm/--fb-drop` | owed |
| 7 | `usbmidi` | the UM-ONE mk2's own identity and descriptors (`0582:0168`) never moved bytes on 98 SE; interrupt endpoints; more than 2 cables | `midiemu.py` profiles `umtab`, `intr`, `multi4` | owed |
| 8 | `uac1` | no guest leg at all, but `uac1emu.py` already has headset, speaker and mic profiles | run them first (branch `uac1` owns `uac1emu.py`) | uac1 agent |
| 9 | `usbflop` | the EHCI leg: a Full Speed drive needs a real High Speed hub's TT, which QEMU does not model (REAL CHECK NEEDED in the sweep). A High Speed UFI CBI model would be a test shape only (no real floppy is High Speed) but would put Windows' (NUSB's) CBI path behind NUSB's EHCI | new UFI CBI model | owed, low |
| 10 | `libusb0` | transfers (control 0-4 KB, bulk to 1 MB, interrupt): only install/listing ran, on QEMU `usb-storage`. Needs a loopback vendor device AND a guest transfer tool (none here yet) | loopback model + tool | owed, low |
| 11 | `ax88179` | link down/up, multi-record RX aggregates, the TX padding flag, multicast, 100/10 Mb/s rows: all closable on the real dongle | bench, not a model | bench |

Not ranked: `usbprn` (no source). The `usb-wdm-nonfree` set is vendor
binaries, not class drivers written here (ASIX AX88772/AX88179 for XP-7,
Realtek RTL815x for XP-7, x360c/x360wc, CH341, FTDI/FT2232D, NUSB's mass
storage): their characteristics are not designed here, so no profile is
owed for them. Existing models already reach some (QEMU `usb-storage` for
NUSB's `USBSTOR.SYS`; `xpademu.py x360` for `x360c` within its ID list;
`rtlemu.py` could serve Realtek's XP driver).

## Per driver

### `uvc98` (webcam)

Claimed (`uvcdesc.c`, `uvcctrl.c`, `uvcneg.c`, `uvcfmt.c`, `uvcusb.c`):

- Descriptors: configuration walk, IAD scope, VC header for `bcdUVC`
  0x0100, 0x0110 and 0x0150 only (`UvcCtrlSize`), VS input header,
  uncompressed YUY2/NV12/I420/M420 and MJPEG formats and frames, discrete
  and continuous interval tables, isochronous (1-3 transactions) and bulk
  streaming alternates, Full and High Speed. Other formats (DV, MPEG-2 TS,
  frame-based, stream-based) are skipped by subtype.
- Probe/commit at 26, 34 or 48 bytes; the 1.5 tail (bUsage ...
  bmLayoutPerStream) is kept verbatim (`UvcCtrl15Get/Set`), no encoder
  policy.
- Runtime: the KS pin offers YUY2 only, stream 0, frames up to 640x480
  (`UvfBuild`); the Brio bandwidth quirk lowers the committed payload.
- Not claimed: encoding units, H.264/VP8, still capture, extension units,
  controls beyond what the miniport asks.

| Feature | Physical (Brio 100) | Simulator (`uvcsim.py`) |
|---|---|---|
| UVC 1.0, 26-byte probe | yes (Brio is 1.0) | `--uvc 1.0` |
| UVC 1.1, 34-byte probe | no | default |
| UVC 1.5, 48-byte probe, 1.5 descriptors | no | **gap -> `--uvc 1.5`** |
| YUY2 160/320/640, HS isochronous incl. high bandwidth | yes (2000 usbehci; 98 SE xHCI98 160) | yes, 98 SE xHCI98 PASS all three |
| MJPEG | descriptors only (runtime is YUY2-only) | streams, host tests |
| NV12 / I420 / M420 | no | **gap** (1.5 profile adds NV12) |
| Frame-based / H.264 format present beside YUY2 | no | **gap** (1.5 profile) |
| Continuous interval triple | no | **gap** (1.5 profile) |
| Bulk streaming alternate | no | gap (not taken: the runtime path is isochronous) |
| Full Speed | no | `--speed full` |
| Unplug in capture, replug | yes | yes |

### `rtl815x` (Realtek RTL8152/8153 Ethernet)

Claimed (`rtlchip.c`, adapted from Linux `r8152.c`; `netusb.c`,
`netcore.c`): vendor register access (request 0x05, PLA/USB MCU types,
byte enables), chip identification from the `PLA_TCR0`/`TCR1` version bits (`RtlVersionFromTcr0`), RTL8152,
8153, 8153B and 8153C bring-up sequences, MAC from `PLA_IDR` (or
`PLA_BACKUP`), PHY access through OCP, link from `PLA_PHYSTATUS`, interrupt
endpoint link status, RX descriptors (24-byte `rx_desc`, several frames per
transfer), TX descriptors (8-byte `tx_desc`), multicast hash in `PLA_MAR`,
promiscuous/all-multicast in `PLA_RCR`.

Physical: none (`2026-10-22-ax88179-rtl815x.md`: "no device available").
Simulator: none. Host unit tests (`rtltest.c`, `coretest.c`) run the cores
against a mock register file. **Everything is a gap.** QEMU has no Realtek
USB model.

### `cdcnet` (CDC-ECM, CDC-NCM, RNDIS)

Claimed: protocol probe NCM > ECM > RNDIS, functional descriptors (header,
union, Ethernet, NCM), MAC from `iMACAddress`, NCM setup (0x80, 0x84 when
NTB32 is offered, 0x86, 0x8A, 0x87/0x88), NTB16 receive (several NDPs and
datagrams, bounds checks) and transmit (divisor, remainder, alignment, up
to 32 datagrams), ZLP rules per protocol, packet filter 0x43 and multicast
0x40, NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE notifications, RNDIS
INIT/QUERY/SET/HALT/RESET/KEEPALIVE/INDICATE and several PACKET_MSGs per
transfer.

Physical: none. Simulator: QEMU `usb-net` (ECM and RNDIS; Full Speed stock,
High Speed with `qemu-usbnet-hs.patch`): 98 SE NUSB EHCI and xHCI98 PASS,
ME xHCI98 ECM, 2000/XP/7. QEMU sends only "connected", has
`wNumberMCFilters` 0, one RNDIS packet per transfer.

Gaps: **all of NCM**; link down/up and speed notifications (all
protocols); 0x40 perfect multicast; RNDIS multi-packet transfers,
keepalive, indicate, reset; High Speed ZLP boundaries (NETTEST sizes miss
512-byte multiples); SuperSpeed endpoints.

### `ax88179` (ASIX AX88179)

Claimed: vendor bring-up (adapted from Linux), MAC from NODE_ID, PHY link
decode 1000/100/10, bulk-in queue row by link and USB speed, interrupt
link bit plus PHY poll, multicast hash, RX aggregate trailer with an FCS
safety net, TX 8-byte header and padding flag, checksum offload off.

Physical: the owner's dongle, passed through: 98 SE and ME under xHCI98
(SuperSpeed and High Speed), 98 SE NUSB EHCI (High Speed behind a hub):
install, MAC, 1000 FD, DHCP, ping 0-4000, removal and replug.
Simulator: none (`axnet.py` drives the real chip from the host).
Gaps (rank 11): link down/up, multi-record aggregates, the TX padding
flag, multicast, the 100/10 rows. All closable on the bench with the real
dongle; a model is lower value than NCM or RTL815x.

### `usbcom` (CDC-ACM)

Claimed: one COM port per CDC-ACM function (up to 4), line coding, control
line state, SERIAL_STATE (DCD, DSR, break, ring, errors), send break, IAD
composites, ZLP handling, Full and High Speed, the board ID list
(`boardids.txt`).

Physical: none recorded (Uno, Pico owed). Simulator: `acmemu.py` profiles
`uno` (single function) and `pico` (IAD composite, one function),
`--mirror` loops DTR/RTS/break back in SERIAL_STATE, `--speed`: 98 SE PASS
under xHCI98, NUSB EHCI, UHCI; ME xHCI98.
Gap: **more than one ACM function** (two COM ports from one device), rank 5.

### `xpad98` (Xbox pads)

Claimed: Xbox 360 wired protocol (start, LED, rumble, XUSB input), Xbox
One/Series GIP (announce, power-on, input, guide key ack), the 360 wireless
receiver (four slots, presence, connect/disconnect, LED, rumble,
power-off), 9x axis mapping.

Physical: none. Simulator: `xpademu.py` profiles `x360`, `xone`, `series`,
`receiver` (pad on slot 0 only, empty slots answer 08 00): 98 SE PASS
under NUSB EHCI (all three), xHCI98 (360, One; receiver needed 8
endpoints, cap since raised in xHCI98 43.67), UHCI (360); 2000, XP, Vista.
Gap: **receiver with pads on slots 1-3, a pad connecting and leaving on a
live receiver**, rank 4.

### `uac2` (UAC 2.0 audio)

Claimed at runtime: the X4's exact layout only (`uacusb.c` ~626-639:
interface 4 alt 1, stereo 16-bit 48 kHz, async OUT with explicit 16.16
feedback, High Speed, clock 33). The parser and host tests cover far more
(clock graphs, FS 10.14 feedback, other subslot sizes), but the runtime
refuses any other shape.

Physical: X4 on 7 x86 (QEMU EHCI passthrough, PASS with packet errors; soak
FAIL); XP start only. No physical 98 SE run.
Simulator: `uacemu.py` (the X4's captured configuration): 98 SE xHCI98
PASS, NUSB EHCI FAIL (feedback lost below the driver).
Gap now: feedback drift/loss (rank 6). Other rates, multi-clock and Full
Speed profiles are premature until the runtime accepts other layouts.

### `usbmidi` (USB MIDI 1.0)

Claimed: AC+MS and MS-only functions, embedded/external jacks, bulk and
interrupt endpoints, 1-16 cables per endpoint, up to 4 endpoint pairs,
running status, SysEx of every length, realtime inside messages, port
naming rules, Full and High Speed.

Physical: UM-ONE mk2 on 7 x86 (start, WinMM open/close; no bytes moved).
Simulator: `midiemu.py` profiles `acms`, `multi` (2 cables), `lone`:
98 SE UHCI and NUSB EHCI PASS, ME, 2000, XP.
Gaps (rank 7): UM-ONE identity on 98 SE, interrupt endpoints, 3+ cables,
two endpoint pairs, the product-prefix naming rule.

### `uac1` (UAC 1.0 audio, branch `uac1`)

Claimed: Type I PCM/PCM8, 1-4 byte subframes, 1-8 channels, discrete and
continuous rates, async/adaptive/sync endpoints, 10.14 and 16.16
feedback, capture and render, MaxPacketsOnly, endpoint rate SET/GET,
unmute at start.

Physical: none (the Brio's microphone has not run under uac1).
Simulator: `uac1emu.py` profiles `headset`, `speaker`, `mic` with
`--fb-format`, `--fb-ppm`, `--max-packets-only`: host tests only, no guest
leg yet. The uac1 agent owns `uac1emu.py`; rank 8 means "run the existing
profiles", not a new one.

### `usbflop` (UFI floppies, an INF over Windows' storage driver)

Claimed: class match `08/04/00` (CBI) and `08/04/01` (CB) on 98 SE/ME
(NUSB's `USBSTOR.SYS` on 98 SE) and NT; media read.

Physical: the Mitsumi, passed through: 98 SE and ME xHCI98 read PASS, UHCI
read PASS. NUSB EHCI: REAL CHECK NEEDED (a Full Speed drive behind a High
Speed hub's TT; QEMU hands it to the companion).
Simulator: none (QEMU's `usb-storage` is Bulk-Only SCSI, not UFI).
Gap: the EHCI leg (rank 9); writes and format; 720K media.

### `libusb0` (libusb-win32 0.1.12.2)

Claimed: control, bulk, interrupt (isochronous built, not tested by owner
decision), claim/release, set configuration/alternate, reset.

Physical: none. Simulator: QEMU `usb-storage` bound by `LUTEST.INF`:
install, load, `TESTLIBU` listing PASS under xHCI98, NUSB EHCI, UHCI.
Gap: transfers (rank 10). A loopback model is easy; the missing piece is a
98 SE program that runs the transfers.

## Profiles added (this audit's follow-up)

Filled in as each lands; each has host tests in its simulator's own test
file and one 98 SE guest leg under xHCI98 and NUSB EHCI.

| Profile | Commit | Host tests | 98 SE xHCI98 | 98 SE NUSB EHCI |
|---|---|---|---|---|
| `uvcsim.py --uvc 1.5` (`--h264 frame/native/both/none`) | fabbc81 | `uvcsim.py`: 24, incl. the driver's own `UvcNegotiateLimited`; found the H.264/VP8 format defect, fixed on `uvc15-fix` 576fc46 | PASS 160/320/640, and 160 with `VS_FORMAT_H264` present (`2026-10-23-uvc15-legs.md`) | binds, 48-byte commit; streaming FAIL in QEMU (NUSB vehicle, unchanged) |
| `uacemu.py --profile x4full/hs24/multi/mic/fs/clocks/adaptive`, `--fb-ppm/--fb-zero-every/--fb-stop-after` (owner 2026-10-23: uac2 beyond the X4 layout; the uac2 agent does the driver side) | see git log | `uacsimtest.py`: 16, incl. every profile through the driver's own `uacdesc.c` (`UacFunction`, `UacParse`, `UacClock`) | owed: the driver accepts only the X4 layout and IDs until branch `uac2-rel` generalises it | same |
| `rtlemu.py --chip 8152/8153/8153b`, `--flap-after` | 620d7c9 | `rtlsimtest.py`: 8, the driver's own `NetPackageChip` over the model's usbredir paths | PASS: install, link, DHCP, ping 32-1472, removal (`2026-10-23-rtl815x-sim.md`) | PASS, plus a cable flap |
| `ncmemu.py` (`--caps`, `--ntb32`, `--out-divisor/remainder/alignment`, `--in-layout front/end/split`, `--flap-after`) | a74727c, 61ebf86 | `ncmsimtest.py`: 6, cdcnet's own `cdcdesc.c`/`cdcncm.c` | PASS: NCM setup, NTB16 both ways, DHCP, ping to 1472 (`2026-10-23-cdcnet-ncm-sim.md`) | PASS, with NTB32 offered, div 512 rem 14 align 16, a cable flap |
| `xpademu.py receiver --pads 1-4 --join/--leave` | 124e415 | `xpsimtest.py`: 15, through `XpX360wParse` | owed (8 endpoints; needs 43.67's xHCI98) | PASS: 4 pads, JOYTEST 4/4; join and leave as scheduled (`2026-10-23-receiver-dualacm-sim.md`) |
| `acmemu.py --profile dual` | f35d2a1 | `acmtest.py`: 25, `AcFindFunctions`/`AcParseNotify` of `acmcore.c` | COM port for function 0 only: the `&MI_02` child has no INF line (INF gap) | PASS: COM3 and COM4, COMTEST full 58/58 each, concurrent hold |
