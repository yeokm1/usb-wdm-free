# USB floppy: further guest experiments, 2026-10-21

These are development experiments, not release or physical EHCI claims.
The Windows 2000 result is recorded separately in
[usbflop-vm](2026-10-21-usbflop-vm.md).
The [follow-up run](2026-10-21-usbflop-nt-followup.md) resolves the
98 SE immediate error after a clean boot and adds XP/Windows 7 evidence.

## Pristine 9x file inventories

Only filesystem directory entries and the two named in-box INF text files
were inspected. No Microsoft driver binary contents or disassembly were
read. The repository's clean `vm/wme.qcow2` and `vm/w98se.qcow2` were
converted with QEMU 11.1.0 `qemu-img convert -O raw` to ignored scratch
files on `E:\usb-wdm-free\wave1\`; 7-Zip 26.00 listed their FAT volumes.

The pristine ME image has `WINDOWS\INF\USBSTOR.INF`,
`WINDOWS\INF\USBNTMAP.INF` and `WINDOWS\SYSTEM\IOSUBSYS\DISKTSD.VXD`.
It has none of `USBSTOR.SYS`, `USBNTMAP.SYS` or `USBMPHLP.PDR` as installed
files. Its cached install source includes
`WINDOWS\OPTIONS\INSTALL\BASE2.CAB`. Thus the generic 98 SE no-copy INF
is insufficient for a fresh, unlisted ME floppy; a driver file may need
to be copied even though ME supplies it.

The pristine 98 SE image with NUSB 3.3 already has:

| Path | Bytes |
|---|---:|
| `WINDOWS\SYSTEM32\DRIVERS\USBSTOR.SYS` | 21,040 |
| `WINDOWS\SYSTEM32\DRIVERS\USBNTMAP.SYS` | 7,136 |
| `WINDOWS\SYSTEM\IOSUBSYS\USBMPHLP.PDR` | 4,861 |
| `WINDOWS\SYSTEM\IOSUBSYS\DISKTSD.VXD` | 18,663 |

The no-copy 98 SE precondition is satisfied in that image. Directory
presence alone says nothing about whether a binary loads or works.

## Separate ME source INF

`src/storage/usbflop/me/USBFLOP.INF` is an original INF which adds the
system `LayoutFile` references and copies only `USBSTOR.SYS` and
`USBNTMAP.SYS` to `system32\drivers`. ME's own `USBNTMAP.INF` installs the
floppy child and copies `USBMPHLP.PDR` to IOSUBSYS. The package contains
none of those Microsoft files. The `9x/` variant stays copy-free for
98 SE with an already installed storage stack.

The facts above were checked in the pristine ME image's own INF text:
`USBSTOR.INF` SHA-256
`9917c2476296cde348554ea5e7d8a4be469cc8eb23162d2cff8493987f51b818`;
`USBNTMAP.INF` SHA-256
`1f3c8bf9887db9d69a31ccecf3fdfcef5bb8082935ec251c69c5ea27d4956326`.
No Microsoft INF text is copied into the original package INF; only the
required filenames, destination and registry facts are used.

The package build now stages `nt/`, `9x/` and `me/` variants.
`test.ps1` passes the three originals and case/spacing variants, rejecting
45 unsafe mutations, including missing ME layout/copy references,
wrong destination and an added foreign file. `build.cmd inf checked`
passes its static gate. This is not guest source-copy evidence.

## ME initial baseline

An overlay `E:\usb-wdm-free\wave1\wmeflop.qcow2` used
`-machine pc -cpu pentium3 -m 256 -vga cirrus -rtc base=localtime -usb`,
`-display none`, and the physical Mitsumi via `usb-host`. No project INF
had been installed. The guest automatically installed its own listed
Mitsumi entry and requested a restart after hardware detection. After
accepting that restart, the guest remained at the ME splash screen with
no disk activity for over two minutes. Detaching the USB floppy and
pressing Esc did not recover it. The stalled QEMU process was quit, with
the overlay retained for diagnosis. This failure precedes project INF
installation and cannot be attributed to it.

The controller-only overlay also stalled after its initial restart with
Cirrus. Relaunching that same overlay with `-vga std` and default ACPI
recovered the desktop. The same change recovered 98 SE after its initial
controller restart. Use standard VGA for these bases. An ME experiment
with ACPI removed reached the shell but lacked its PCI/USB devices and
was rejected as an invalid USB baseline.

All subsequent launches set process `TEMP` and `TMP` to
`E:\usb-wdm-free\wave1\tmp` so virtual-FAT temporary write backends stay
on the requested scratch drive as well.

## 98 SE: immediate live rebind failed

The `w98flop.qcow2` overlay used 256 MB, Pentium III, standard VGA,
default ACPI, no network, and QEMU's UHCI controller. Windows 98 SE
installation media supplied the initial controller setup. With NUSB 3.3,
the physical Mitsumi `03EE:6901` installed automatically under its native
Mitsumi entry and its parent reported **This device is working properly**.
The native Driver tab reported Microsoft, 11-16-2007. This is a useful
working baseline, not evidence for this project's INF.

Update Driver / display list / Have Disk accepted `A:\USBFLOP.INF` and
the sole compatible **USB UFI floppy (CBI, usb-wdm-free experiment)**
model. Windows warned that its current driver might be a closer match;
the class model was explicitly selected for this experiment. Installation
completed without a file-copy request or reboot request. The Driver tab
then showed usb-wdm-free, 10-21-2026. The General tab instead reported:

> This device has a problem, but Windows cannot determine what the problem is.

This is a blocking negative result for immediate live rebinding. No reboot
or detach/re-enumeration with this INF was tested, so persistent failure
after reboot is not established. No speculative source change follows
from this status alone. Registry export and setup-log inspection remain
future diagnostic work. No medium was present and no media writes ran.

Exact staged CRLF INF SHA-256:
`388AF7C47C38AEE82AAD7EB416CC2A94EAFD80658CFD278FFCB71AD48AFFF44D`.
Source LF INF SHA-256:
`A8699BEBB53ABEA28A448C3B110E02B5771CF6A46FE6F4161D8B7695B7E5A884`.
Evidence in ignored `out/storage/usbflop/`: `98-parent.png` and
`98-driver.png` (native baseline), `98-after.png` (our Driver tab),
`98-ours-status.png` (our negative status). The overlay was shut down
cleanly and its QEMU process exited.

## ME: controlled first-copy and binding succeeded

The `wmebare.qcow2` overlay used the same QEMU hardware settings with
standard VGA and default ACPI. It had never seen the physical floppy.
The new ME INF was copied to `C:\WINDOWS\INF\USBFLOP.INF` and the guest's
native `USBSTOR.INF` was temporarily renamed `USBSTOR.OLD` before first
attachment. `USBNTMAP.INF` stayed intact. This removes the existing
VID/PID candidate and prevents its copy list from masking the experiment.
It is a modified-guest control, not proof of natural ranking or of an
actually unlisted hardware device. The original INF remains in that
throwaway overlay under its `.OLD` name.

Attaching `03EE:6901` selected the project's CBI model automatically.
No source prompt appeared; ME had its own cached installation source.
The parent reported **This device is working properly**, provider
usb-wdm-free, date 10/21/2026, unsigned. The Storage device child named
**USB Floppy** independently reported **This device is working properly**.
Screenshots: `me-parent.png`, `me-driver.png`, `me-child-status.png` in
ignored `out/storage/usbflop/`.

After clean shutdown, a raw conversion and directory-only inventory
showed these files installed where the pristine image had none:

| Path | Bytes |
|---|---:|
| `WINDOWS\SYSTEM32\DRIVERS\USBSTOR.SYS` | 21,040 |
| `WINDOWS\SYSTEM32\DRIVERS\USBNTMAP.SYS` | 7,136 |
| `WINDOWS\SYSTEM\IOSUBSYS\USBMPHLP.PDR` | 4,861 |

This establishes system source-copy resolution and successful parent and
child binding for the controlled ME case. No Microsoft driver contents
were read. Exact staged CRLF ME INF SHA-256:
`A8E9B082DE3036900F5D7E424A754226EFF854A76E4C1B9FF74C60F43010CBE5`.
No medium was inserted; read/write, formatting, reboot persistence and
physical-controller operation remain untested.

Both final guests were cleanly shut down and their QEMU processes exited.
The final host inventory contained no `qemu-system` processes. All guests
in this leg were headless. XP and Windows 7 were not started in this leg.
Scratch overlays and raw inventory copies remain on E: for diagnosis;
no release files or author-tested table cells were changed.
