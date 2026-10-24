# USB floppy guest experiment, 2026-10-21

Development INF only; no release and no author hardware-test claim.
The first run uses the owner's Mitsumi `03EE:6901` through QEMU's USB
passthrough. Its descriptor capture and hashes are in
`src/storage/usbflop/testdata/mitsumi/README.md`.

## Windows 2000 SP4

- Fresh overlay `E:\usb-wdm-free\wave1\w2kflop.qcow2`, backed by the
  repository's `vm/w2k.qcow2`. Initially booted with a window, then cleanly
  shut down and restarted headless (`-display none`).
- QEMU 11.1.0, `-machine pc,acpi=off -cpu pentium3,-apic -m 256
  -vga cirrus -rtc base=localtime -usb`. Guest stack: Microsoft's UHCI,
  USBSTOR and SFLOPPY. This is not an EHCI test.
- The NT experimental INF was copied to
  `E:\usb-wdm-free\wave1\w2kflop-a\USBFLOP.INF`, mounted as virtual
  floppy `A:`. Source SHA-256:
  `b4eb10ec2c9006351805ae2502dc6b7042041252ed2bfd2621127307e65c6077`;
  staged ASCII/CRLF SHA-256:
  `8d4b63240b6cc04ff54bdc982a45300936f06e9ff61928da700fb49e97ec9d42`.
- Actual USB floppy enumerates separately as `B:`. The source virtual
  floppy is not used as evidence for the USB drive.

### Baseline and explicit class-entry installation

The listed Mitsumi first bound automatically to Microsoft's
`USBSTOR_CBI.NT` entry. Device Manager showed it working properly, provider
Microsoft, driver date 14-Nov-1999, version 5.0.2183.1.

Update Driver, select from list, Have Disk `A:\`, choose
"USB UFI floppy (CBI, usb-wdm-free experiment)". Windows completed the
installation. Parent status: working properly, provider `usb-wdm-free`,
version `0.0.0.1`, not digitally signed. No source-file prompt was observed.
The previously installed system storage files were already present, so
this does not prove source lookup on a system lacking those files.

The child is "High-Capacity Floppy Disk Drive", working properly, on the
new parent. Driver File Details names `C:\WINNT\system32\DRIVERS\sfloppy.sys`,
Microsoft version `5.00.2195.6655`.

The guest's SetupAPI log confirms:

- The class ID `USB\Class_08&SubClass_04&Prot_00` selected the experimental
  `UFI_CBI.NT` section, installed as `oem0.inf`, and completed successfully.
- Unsigned package diagnostics `0xe000022f` were logged; those messages
  did not prevent this installation.
- The child matched `GenSFloppy` in `flpydisk.inf`, using
  `sfloppy_install.NT`.
- Windows 2000 explicitly ignores the driver date because the INF is
  unsigned. This explains the Device Manager "Not available" date even
  with a `$Windows NT$` signature; it is not a signature-string problem.

### No-medium and idle removal

Opening `B:\` produced "Please insert a disk into drive B:". No medium
was present; no sector/file reads, media geometry/format support, or
format/write operations were tested.

After cancelling that dialog, HMP `device_del` detached the idle USB drive.
Windows showed its normal "Unsafe Removal of Device" warning, naming both
the experimental parent and the high-capacity floppy child. `B:` vanished;
the guest remained responsive. Reattaching the drive with `device_add`
restored `B:` without a crash.

The reattachment used another USB port instance. SetupAPI considered both
the cached experimental class match and Microsoft's Mitsumi VID/PID match,
and selected Microsoft's `USBSTOR_CBI.NT`. This is positive ranking
evidence for a listed drive keeping Microsoft's match on a new instance;
it is not an unlisted-drive test or evidence that our manual binding
survives a same-instance replug.

### Evidence and automation

Input and screenshots came from QEMU's local HMP monitor `127.0.0.1:44332`,
using `src/generic/usbprobe/monitor.py`. No host cursor is needed. The guest
uses Dvorak; `--layout dvorak --text ...` types paths correctly. Screenshots
are in ignored `out/storage/usbflop/`: `w2k-status.png`, `w2k-child.png`,
`w2k-child-driver.png`, `w2k-media.png`, `w2k-unplug.png`, `w2k-replug.png`.

The guest's `C:\WINNT\setupapi.log` was opened in Notepad and saved as a
copy to `A:\W2KSETUP.TXT`, without changing the original. Host copy:
`E:\usb-wdm-free\wave1\w2kflop-a\w2ksetup.txt`, 88,315 bytes, SHA-256
`a2bc6ffe4ba0d751bb82a8c645001cd0e5761d9fde52d726f40021edb64a1c50`.
It records the manual install at 01:04 and new-instance replug at 01:09
on 2026-10-21, Singapore local time.

## Still required

98 SE, ME, XP and 7 x64 installations; an unlisted drive; CB transport;
actual media reads; package trust on x64; physical EHCI legs; and fresh
system source-file resolution. The README table remains Planned because
this passthrough plumbing experiment does not meet the release test plan.
