# Status sweep legs: libusb0 on 98 SE, cdcnet on 2000 under Windows' EHCI, usbflop (blocked), 2026-10-23

These are development legs in QEMU (TCG, 1 vCPU, headless), run during the status sweep of `libusb0`, `cdcnet`,
`usbflop` and `xpad98`. They are VM passes and do not make any "Tested" cell. Nothing is released.
The builds are this branch at `613207c`, whose sources are the same as `working` at `bf09c16`. They were built
with each package's `build.cmd` and all gates passed.

Kit `E:\usb-wdm-free\sweep\` (scripts under `scripts\`, QMP ports 49030-49039). The 98 SE bases are copies
of the usbcom9x kit's bases:

- `w98-hcd310`: 98 SE with xHCI98 3.1.0.0.
- `w98-nusb`: the same plus NUSB 3.3.

The 2000 base is a copy of `E:\usb-wdm-free\vm\w2k.qcow2` (see below). Every leg runs on a throwaway overlay.
Evidence is under `E:\usb-wdm-free\sweep\evidence\` and `shots\`.

## libusb0: first guest legs (98 SE, three host controller drivers)

This is the first time `LIBUSB0.SYS` and `LIBUSB0.DLL` were loaded anywhere. QEMU has no vendor-class
device model, so the device is QEMU's `usb-storage` (`46F4:0001`, "QEMU USB HARDDRIVE", 8 MiB raw image).
`LIBUSB0.INF` invites a "copy of this INF with their own VID/PID line", and the leg uses exactly that:
`LUTEST.INF` is the built `LIBUSB0.INF` plus one test-only model line,
`%QemuStor%=LibUsb,USB\VID_46F4&PID_0001`. The VID/PID hardware ID outranks NUSB's class match for
mass storage.

| Input (`w98\free`) | SHA-256 |
|---|---|
| `LIBUSB0.SYS` | `39622e36899b2ec10ffed5b86476d53d639a4977d822a4f2bae49d9bf4adcb60` |
| `LIBUSB0.DLL` | `02efd584f15c6f417c02ac833e38bbe465d8a19588d42473aaf2e800074a7078` |
| `TESTLIBU.EXE` | `90d47ff09078994bf21a75c0269a0fc80a8629836a75ef03efe9fa837ecdf92d` |
| `LUTEST.INF` (test copy) | `3d5520e6080b93b954a734cfbeba1a3c285f8fbafc37243724f06591f19c9094` |

Each leg went through the same steps:

1. Plug the device.
2. Install it with the Add New Hardware wizard, searching the floppy. The wizard found
   `A:\LUTEST.INF` ("QEMU usb-storage, libusb0 test binding") and installed it with no prompt for the
   Windows CD. The built INF has LF line endings, and 98 SE parsed it.
3. Copy `TESTLIBU.EXE` alone to `C:\LUT`, so the DLL must come from `SYSTEM`, and run it there,
   plain and with `-v`.
4. Unplug the device (`device_del`) and run `TESTLIBU` again.
5. Replug the device and run `TESTLIBU` again.

| Overlay | Host controller driver | Speed (QEMU `info usb`) | Result |
|---|---|---|---|
| `lu1` | xHCI98 3.1.0.0 (`qemu-xhci`) | SuperSpeed (5000 Mb/s) | PASS: install; the device was listed with its manufacturer and product strings and its descriptors (`-v`: `wTotalLength` 44, the SuperSpeed configuration). After unplug only the bus remained (`Dev #0`); after replug the device was listed again |
| `lun1` | NUSB 3.3 EHCI (`usb-ehci`) | High Speed (480 Mb/s) | PASS: the same steps, `wTotalLength` 32 |
| `luu1` | Windows' UHCI (98 SE `USB.INF`, installed from the CD on that boot) | Full Speed (12 Mb/s) | PASS: the same steps |

These legs show that on 98 SE:

- the INF installs;
- `LIBUSB0.SYS` loads under NTKERN;
- the DLL is found from another folder;
- `\\.\libusb0-NNNN` opens;
- control transfers work: string and configuration descriptors through the driver;
- an idle device can be removed and replugged.

They do **not** cover these checks from design 6.7:

- bulk and interrupt transfers;
- the timeout case;
- `usb_reset`;
- removal while a handle is open;
- avrdude with USBasp or AVRISP mkII.

Those need the real devices. M0 (upstream's binaries) was not run. ME was not run.

## cdcnet: Windows 2000 SP4 under Windows' EHCI, High Speed (ECM)

This is the first `cdcnet` leg under an NT EHCI driver. The record of 2026-10-22 had these as hardware only,
because `usb-net` was Full Speed. The guest QEMU is the net9x kit's patched build
(`qemu-usbnet-hs.patch`, used read-only from `E:\usb-wdm-free\net9x\qemu\build\`), with `usb-net,highspeed=on`
on `usb-ehci`. Its `dgram` netdev runs on 127.0.0.1:49036. A stock QEMU 11.1.0 with `-machine none` bridges it
on 49037 to a user netdev, which serves DHCP and TFTP.

**2000 base.** The pristine `w2k.qcow2` has no USB 2.0 stack files. Its first boot with `usb-ehci` reported
"usbhub20.sys could not be loaded (c0000034)" and the next boot stopped with STOP c000026c. That overlay was
deleted. As the uvc M2 record did, a fresh overlay was booted without EHCI. In it,
`expand -F:usb*.sys "C:\WINNT\Driver Cache\i386\sp4.cab" C:\WINNT\system32\drivers` was run (9 files,
including `usbehci`, `usbhub20`, `usbport`). The guest was shut down cleanly and the overlay kept as
`base\w2k-usb.qcow2`. With it, 2000 installed the EHCI controller silently.

| Input | SHA-256 |
|---|---|
| `cdcnet.sys` (ndis5 trace) | `8ed010e3479055e480ae9aa44c4269a6bcd84fb090e06196fb5681a3696b5a06` |
| `CDCTEST.INF` | `23a94e1789d14b9ba098beeb454d36ec5663d0eea2402d6b8522561b37e84474` |
| `NETTEST.EXE` | `836ae96b26387c3cdbe6ecf9d9b6d263e63f6921b8b7275c77222c27644c06cb` |

Overlay `k2`:

- **Install.** Found New Hardware ("RNDIS/QEMU USB Network Device"), floppy search, `a:\cdctest.inf`,
  unsigned prompt accepted. The trace shows the driver probing both configurations, choosing ECM
  (configuration 1), and reporting "pipes in 512 out 512 bytes per packet" and "high speed, link 1".
  DHCP gave 10.0.2.15.
- **NETTEST runs 1 and 2.** Each: 21/21 ICMP replies, 2 x 1048576 TFTP bytes verified (round 1 10.9 s,
  round 2 11.1 s, 0 retransmits). **PASS.**
- **Idle removal.** `device_del` after run 2. Windows showed its unsafe-removal warning. The driver
  reported sync URB timeouts, a reset, then "device gone: drained, reported", halt, and
  "0 malformed transfers". No fault.
- **Removal under traffic.** After a replug (2000 installs each replug as a new instance, `#2`, `#3`,
  because QEMU puts it on another port), NETTEST was started and the device was removed 8 s into it. The
  driver logged "fault c000009d usbd c0007000", then "device gone: 0 transfers outstanding, drained,
  reported", then halt ("tx 1229/1 rx 1175"). NETTEST showed FAIL, as expected with the device gone.
  No fault in Windows. **PASS.**
- **Replug after that.** NETTEST: 21/21 ICMP, 2 x 1 MiB TFTP verified. **PASS.**
- **Shutdown.** Orderly, to "It is now safe to turn off your computer".

Evidence `E:\usb-wdm-free\sweep\evidence\w2k-ehci-ecm\`:

- `e9-k2.log`: the trace;
- `nettest.txt`: the last run;
- `bridge-k1.pcap`: SHA-256 `ffc0bcd38683a63c48aa420926a1d474a80fedf872b65585350802d498770f47`;
- screenshots.

Not run here:

- RNDIS under 2000 EHCI;
- XP and 7 under EHCI;
- 2000 and XP under xHCI98;
- ME RNDIS.

## usbflop: xHCI98 leg blocked, no device

The plan was to run the first xHCI98 leg on 98 SE: the `w98-nusb` overlay `fln1` (NUSB's `usbstor.sys`
with `USBFLOP.INF`), with the physical Mitsumi `03EE:6901` passed through on `qemu-xhci`. QEMU's
`usb-host` did not open the device. `info usb` showed "USB Host Device", address 0.0, 1.5 Mb/s, which is
QEMU's placeholder for a device it has not opened. So the Mitsumi is not attached to the host now, or not
bound to WinUSB. The device was removed and the guest shut down. No leg ran.

A USB floppy is Full Speed. An EHCI leg needs it behind a real High Speed hub on a real PC (design 11). In
QEMU a Full Speed device on `usb-ehci` is not carried by EHCI.

## usbflop with the Mitsumi attached (2026-10-23, later)

The owner attached the physical Mitsumi `03EE:6901`, and QEMU's `usb-host` opened it on `qemu-xhci`
(`MITSUMI USB FDD`, 12 Mb/s). A disk was in the drive.

**Read-only scope.** The scope is the one the owner set on 2026-10-21: `FREAD.EXE` lists the medium
and reads the first three files with `GENERIC_READ` only, then reports byte counts and CRC32. It never
writes to the medium.

**Binding.** The drive is in the native storage INF's VID/PID list, and a VID/PID match outranks
`USBFLOP.INF`'s class match. Each overlay therefore first renamed the native `USBSTOR.INF` to
`USBSTOR.OLD` and copied our INF into `INF`, as the 2026-10-21 ME leg did. This is a controlled
substitute for an unlisted drive.

| System | Host controller driver | Base | Result |
|---|---|---|---|
| 98 SE | xHCI98 3.1.0.0 (`qemu-xhci`), with NUSB 3.3's `USBSTOR.SYS` | `n-ready` (98 SE + xHCI98 + NUSB) | `USBFLOP.INF` bound the drive ("USB UFI floppy (CBI, usb-wdm-free experiment)"). FREAD on E:: `IO.SYS` 40774 `A7EA3BE4`, `MSDOS.SYS` 38138 `4CC2FC77`, `COMMAND.COM` 54645 `1131ADD3`. **READ PASS**, the same CRCs as 2026-10-21. Idle unplug clean |
| ME | xHCI98 3.1.0.0 | `me-hcd310` | Same binding, from ME's own cached source. **READ PASS** with the same three files and CRCs. This is ME's first leg with a medium. Idle unplug clean |

**98 SE under NUSB EHCI: REAL CHECK NEEDED.** The floppy is Full Speed. A Full Speed device in QEMU
cannot sit on `usb-ehci`; it goes to a companion UHCI, which makes it a UHCI leg. Only a real EHCI
controller with a High Speed hub carries it over EHCI. The same applies to NT under Windows' EHCI.

**NT (2000, XP, 7).** Their earlier legs show `USBFLOP.INF` binding Windows' own `usbstor.sys` under UHCI
(2026-10-21). No NT medium read was run. A read there would test Windows' `usbstor` with the flags our
INF sets, and the drive is natively listed anyway. NT under xHCI98 was not run either.

## usbcom `USBCDC.INF` on 2000: the "regression" reproduced and explained

The matrix row "2000 (regression FAIL)" comes from `2026-10-21-usbcdc-vm.md`: echoes of 1, 63 and 64 bytes
passed and the 65-byte echo failed.

**Rig.** Base `w2k-usb`, Windows' UHCI, stock QEMU. The simulator is the repository's `acmemu.py`
(usbcom's, unchanged), Uno profile, Full Speed, over `usb-redir`. `USBCDC.INF` installs as
"Arduino Uno R3 (USB serial)": service usbser, problem 0, COM3. `CDCPROBE.EXE`
(`eff9890f...47ea`) ran the echo test.

**Reproduced exactly.** 1, 63 and 64 bytes PASS. Then
"SIM read 65 incomplete: got=0 error=0 empty=1 calls=2 elapsed=3004 ms".

The simulator's log shows the cause. After the full 64-byte echo, the model completes the next bulk IN
with a zero-length packet: the separate flush it sends after a full-packet echo. After that, **Windows
2000's `usbser.sys` never submits another bulk IN**. The 64 + 1 bytes of the next echo arrive as OUT, and
no IN ever asks for them.

**Control.** A kit copy of the model with that flush ZLP off (`ACM_NOZLP=1`;
`E:\usb-wdm-free\sweep\acm\acmemu.py`, not a repository change) passes every echo on the same guest:
1, 63, 64, 65, 128 and 1537 bytes, empty reads 0, COM4.

**Verdict.**
- `USBCDC.INF` and the binding are correct on 2000.
- The failure is how 2000's own `usbser.sys` handles a device that ends a full-packet transfer with a
  standalone ZLP: its read loop stops.
- The INF cannot change that.
- Whether the real boards in `USBCDC.INF` send such a ZLP is a property of their firmware, which this
  repository does not have: **REAL CHECK NEEDED** with an Uno R3 or Pico on 2000.
- Under a device without the flush, the leg is **PASS**.

## cdcnet: ME RNDIS under xHCI98, and XP SP3 under Windows' EHCI (2026-10-23, afternoon)

**ME, RNDIS, xHCI98 3.1.0.0, Full Speed.** Base `me-hcd310`, stock QEMU, `usb-net` on `qemu-xhci`,
user netdev (DHCP, TFTP). The binaries were the ndis5 trace `cdcnet.sys` and `CDC98R.INF`, with
NETTEST 12 x 1 MiB.

- On ME the INF's RNDIS preset reached the instance key (98 SE's did not; see above). The first
  initialization was already "protocol 1, full speed", so no `regedit` was needed.
- ME asked for a restart, which ran as a cold start.
- NETTEST: 21/21 ICMP and 12 x 1 MiB verified. **PASS.**
- Removal under traffic: the trace shows "fault c000009d", "device gone: 0 transfers outstanding",
  drained, "gone: 1 sends failed", halt, "0 malformed transfers".
- Replug on the same port: re-initialized as RNDIS. NETTEST PASS. **PASS.**
- One limitation: QEMU once put the replugged device on a different root port (port 3). ME then made a
  new device instance and did not install it silently, so the miniport never started. A replug on the
  same port works.

**XP SP3 x86, ECM, Windows' EHCI, High Speed.** The patched `usb-net,highspeed=on` on `usb-ehci`,
bridged as on 2000 (dgram 49036/49037, bridge capture SHA-256 `09aa7dc7...08c5`). The binaries were
`CDCTEST.INF`, the same trace `cdcnet.sys` and NETTEST.

- Found New Hardware ("No, not this time", install automatically, Continue Anyway). The trace shows
  ECM, "pipes in 512 out 512" and "high speed". DHCP gave 10.0.2.15.
- NETTEST 2 x 1 MiB PASS.
- Removal under traffic: drained and halted ("tx 5005/1").
- Replug: re-initialized at High Speed. NETTEST PASS. **PASS.**

## Guests

One at a time, smp 1. Every QEMU this sweep started (`pids.txt` in the kit) exited, either by an orderly
shutdown or by `quit` after "safe to turn off".
