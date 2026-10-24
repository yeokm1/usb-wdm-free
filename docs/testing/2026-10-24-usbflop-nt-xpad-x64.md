# usbflop NT medium reads and the xpad98 x64 legs, with simulators, 2026-10-23/24

These are emulated legs. A simulator stands in for each device and is exported to QEMU over localhost
usbredir. No physical device was used, and no real drive or pad is claimed. Every guest ran headless
under TCG.

Kit: `E:\usb-wdm-free-vm\simcov`:

- `launchnt.ps1` launches the NT guests. Each guest is an overlay of the shared base
  `E:\usb-wdm-free\vm\<base>.qcow2`, which is only read. The machine has PIIX3 UHCI, usb-ehci and a
  usb-tablet.
- `attachufi.sh`/`detachufi.sh` plug and pull `ufiemu.py`.
- `attachxp.sh`/`detachxp.sh` do the same for `xpademu.py`.
- `runnt.sh` types into Start > Run (Dvorak).
- `click.sh` clicks a point with the PS/2 pointer: HMP `mouse_move` is relative, so it moves in
  10-unit steps.

The simulator logs and screenshots are in the kit's `ev\`.

## usbflop: the medium-read legs (2000, XP, 7 x64)

### The simulator

`src/storage/usbflop/ufiemu.py` (commit `fc5cd5e`) is a UFI floppy drive. Its sources are the UFI
Command Specification 1.0, CBI Transport 1.1, USB 2.0 chapter 9 and the usbredir protocol.

- **Identity.** It presents as **unlisted**: FFFF:0020, class 08/04/00 (CBI). With `--cb` it is class
  08/04/01 (CB), and these legs used PID 0021 for that. No Microsoft INF names it, so only
  `USBFLOP.INF`'s class match can bind it. The 2026-10-21 Mitsumi legs could not test this, because
  Microsoft lists the Mitsumi drive.
- **Medium.** A FAT12 1.44 MB image built in memory. It holds `README.TXT` (119 bytes),
  `PATTERN.BIN` (204800 bytes of a known pattern) and `CRC.TXT`, which lists the CRC-32 of each file.
- **Host tests.** `ufisimtest.py` has 17. They cover descriptors, INQUIRY, the capacities, MODE SENSE
  page 5, a whole-disk read, writes, write protect, unit attention, the CB shape, the two transport
  rules below, and a FAT12 reader that checks `CRC.TXT` against the files.

**Two simulator defects found in the guest legs, both fixed in `fc5cd5e`.** Neither is a usbflop or
Windows defect.

1. **A status block lost in QEMU.** When QEMU's UHCI cancels an idle interrupt TD, usb-redir sends
   "interrupt receiving stop", and QEMU frees the interrupt data it had buffered. A UFI status block
   sent just before that stop was lost. The guest's next interrupt TD then waited about 20 s and reset
   the device, and FREAD saw error 1117 (`ERROR_IO_DEVICE`).
   - XP trace: TEST UNIT READY's status was sent at 202.287 s, the stop arrived at 202.303 s, and the
     reset came at 222.8 s.
   - A real device keeps its status until an IN token takes it. The model now offers the block again
     on the next start, and it drops the block at the next ADSC.
   - This explains the one 2000 failure (the first read after installation) and the first XP run. The
     same race was then caught and recovered 4 to 8 times per run (`status_requeued` in the logs).
2. **No stall after a failed command.** Windows 7 sends MODE SENSE(6) (0x1A), which UFI does not
   define. The model failed the command (ASC 20h) but left the host's bulk IN pending forever, so 7
   reset the drive every 20 s.
   - Under CBI 1.1, a failed command stalls the bulk pipe. The model now does this; CLEAR_FEATURE or
     the next ADSC clears the stall.
   - 7 then reads the sense data and carries on. It sent 34 MODE SENSE(6) and 35 REQUEST SENSE in the
     passing run.

### Inputs

| Input | SHA-256 |
|---|---|
| `nt/USBFLOP.INF` (CRLF, staged) | `8d4b63240b6cc04ff54bdc982a45300936f06e9ff61928da700fb49e97ec9d42` (the same bytes as the 2026-10-21 legs) |
| `FREAD.EXE` (`fread.cmd`, this branch) | `5ea3c64d3668439d2192f2c9f0e98aa445fed82c61db4fe5a32b38dd97b62812` |
| medium image (`ufiemu.py` default) | `3813b4b5c2cd4fc91d74db6fd0a64e3d799a6ebc7579f62bf911a693b938ad14` |

Expected CRC-32s (from `CRC.TXT`): `README.TXT` A4C945E0, `PATTERN.BIN` 9512AC89, `CRC.TXT` 02BD3B5B.

### Results

Every drive was on QEMU's UHCI at Full Speed, so these are not EHCI legs. Each READ PASS below means
FREAD read all three files with byte counts equal to the directory sizes, zero failures and the
expected CRC-32 for each file.

| System | How it bound | CBI (FFFF:0020) | CB (FFFF:0021) |
|---|---|---|---|
| 2000 SP4 (`w2k`, overlays `n2k`, `n2kb`) | Found New Hardware wizard. "Search for a suitable driver", with floppy drives ticked, found `a:\usbflop.inf` by itself; no model choice, no prompt. "USB UFI floppy (CBI, usb-wdm-free experiment)" with the `sfloppy` child. The CB drive later matched the stored `oem0.inf` | **READ PASS**: on `n2k` 2 of 3 runs (the first, just after installation, got 1117, the race above); on a fresh overlay `n2kb`, install then read straight away, PASS | **READ PASS** |
| XP SP3 x86 (`wxp`, overlay `nxp`) | Found New Hardware wizard: "No, not this time", then "Install automatically" found the INF on A:. Unsigned prompt, "Continue Anyway" | first run 1117 (the race, traced); rerun with the fixed model **READ PASS** | **READ PASS** |
| 7 SP1 x64 (`w764`, overlay `n7`) | `pnputil -a a:\usbflop.inf`, elevated. One unsigned-publisher prompt, "Install this driver software anyway". Published as `oem2.inf`. The plug then installed it **automatically**, with no prompt: SetupAPI shows `USB\VID_FFFF&PID_0020\SIM0020` with `UFI_CBI.NT` from the driver store, `USBSTOR_CBI.NT` included | first run hung (MODE SENSE(6), above); rerun with the fixed model **READ PASS** | **READ PASS** (automatic) |

**Windows writes the medium during a read.** FREAD only opens and reads files, but 2000, XP and 7
each wrote the root directory sector (LBA 19, 4096 bytes) three times per FREAD run, once per file
opened. This is the FAT last-access date. The writes reached only the simulator's in-memory image.
They matter for a real floppy: a "read-only" probe on these systems is not write-free unless the disk
is write-protected.

What this settles:

- `USBFLOP.INF`'s class match binds an unlisted UFI drive and reads its medium on 2000, XP and 7 x64,
  for both CBI and CB.
- 2000 and XP find the INF on a floppy through the ordinary wizard. 7 takes it from the driver store
  without a prompt at plug time.

What it does not settle:

- **REAL CHECK NEEDED:** a real unlisted drive, and a real CB drive. The simulator is built from the
  specification, and a real drive's quirks and timing are not modelled.
- Writes and formatting are not claimed.
- The EHCI leg is unchanged (`hcd-matrix.md`).

## xpad98: the x64 build on XP x64 and Vista x64

`xpad98.sys` was built with `build.cmd wdm64 free` (WDK 7.1, WNET amd64) at this branch's head, which
contains `5758369`. The pad was the simulator's `xone` profile (045E:02EA, GIP) at High Speed on
usb-ehci, under Windows' own `usbehci`, as in the x86 XP and Vista legs.

| Input | SHA-256 |
|---|---|
| `xpad98.sys` (wdm64 free, unsigned) | `26105cf495d7d5c10853a837692aa3b1b4843e9c4c57fa3dd4e82574557b1ca8` |
| `xpxpv.inf` | `3615a9649b2a514f717d36f50bcbe6f277730b0eecf47f322d5b3f492ca4b3fc` |
| `JOYTEST.EXE` | `b5e51856f02cb9ac649f4b2849fe0eef4024e1c0424188356eb9cb798281efa4` |

### Signing on Vista x64

The Vista x64 leg used a **throwaway test certificate made inside the guest**. Nothing was signed on
the host, and the host's certificate stores were not touched. The owner's certificate and key were
not used or read.

The guest's elevated prompt ran `SIGN.CMD` from the floppy:

- WDK 7.1's `MakeCert` made a self-signed code-signing certificate, "CN=simcov throwaway test
  2026-10-24", SHA-1 `2B5051BD75B9A60008266E40F6DB690970389968`. The key stays in the guest's user
  store `SimcovThrowaway`.
- `CertMgr` added the certificate to LocalMachine Root and TrustedPublisher.
- `SignTool` embedded a signature in a copy of the `.sys` at `C:\xp64`; `verify /pa` succeeded.
- `bcdedit /set testsigning on`, then a restart ("Start Windows Normally"; Disable Driver Signature
  Enforcement was not selected).

There is no catalog, so `pnputil -a` raised the publisher prompt for the INF, which was accepted. The
certificate, key and test-signing setting exist only in the overlay `nv64`; delete it to discard
them.

### Results

| System | Install | JOYTEST |
|---|---|---|
| XP x64 (`wxp64`, overlay `nx64`) | Found New Hardware wizard, "Install automatically" from A:, unsigned prompt "Continue Anyway". The unsigned x64 binary loads; XP x64 does not enforce kernel signing | **PASS**: id 0, 5 axes, 11 buttons, every button pressed, X/Y/Z/R/U at both ends, POV 0/9000/18000/27000/centre. The right stick is R/U, as on XP x86. GIP handshake in order (announce, power-on, the One S packet, LED, auth done); every Guide report acknowledged. Unplug clean |
| Vista SP2 x64 (`wv64`, overlay `nv64`), testsigning on | `pnputil -a`, then the plug installed "Xbox One/Series compatible pad (xpad98)". `signtool verify /pa` on `System32\drivers\xpad98.sys` succeeded, and the `xpad98` service is RUNNING | **PASS**, the same readings. 24 Guide acknowledgements. Unplug clean |

What this settles:

- The x64 build loads and works under Windows' x64 HID and WinMM paths, on XP x64 unsigned and on
  Vista x64 with an embedded signature and test signing. Gamepad design section 11 had left exactly
  this "built, not tested".

What it does not settle:

- Vista x64 without test signing. That needs a certificate that chains to a trusted root, which is
  the owner's signing decision (design section 4.5).
- **REAL CHECK NEEDED:** a real pad.

## Guests

Every guest exited by an orderly shutdown (`pids.txt` and `pids-closed.txt` in the kit):

| PID | Overlay | Guest |
|---|---|---|
| 47932 | `n2k` | 2000 |
| 84640 | `n2kb` | 2000 |
| 98036 | `nxp` | XP |
| 32220 | `n7` | 7 x64 |
| 80084 | `nx64` | XP x64 |
| 59448 | `nv64` | Vista x64 |
