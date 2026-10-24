# Physical floppy read-only media test, 2026-10-21

Development run, not a release. The user inserted an existing readable disk
and authorized read-only media detection, file listing and file reads.
No formatting, repair, file changes or raw-sector writes were requested or
performed by the diagnostic. File contents are not displayed or saved;
only names, byte counts and CRC32 checksums are reported.

## Setup and diagnostic

Physical Mitsumi `03EE:6901`, host service verified `WinUSB`, forwarded by
QEMU `usb-host` to emulated PIIX UHCI at 12Mb/s. Guest Windows 98 Second
Edition 4.10.2222A with NUSB3.3; PentiumIII, 256MiB, stdVGA, default ACPI,
TCG, no NIC, `-display none`. New disposable
`E:\usb-wdm-free\wave2\w98read.qcow2` backs onto the closed known
`E:\usb-wdm-free\wave1\w98flop.qcow2`. QEMU PID33108, localhost monitor
44338. All scratch, virtual floppy, process TEMP/TMP and logs are on E:.

The old overlay unexpectedly retained the project's **USB UFI floppy
(CBI, usb-wdm-free experiment)** parent on this arrival; the run therefore
started with the project binding, rather than the intended native-first
order. `w98read-usbtree2.png` records that actual parent. No inference of a
native control is made from it. Exact source9x INF SHA256:
`A8699BEBB53ABEA28A448C3B110E02B5771CF6A46FE6F4161D8B7695B7E5A884`.
This package is an INF binding the installed storage stack, not a project
USB storage binary.

Original `src/storage/usbflop/fread.c`, built by `fread.cmd` with MSVC6
`/W4 /WX`, opens source files with `GENERIC_READ`/`OPEN_EXISTING` only.
It refuses a source that is not removable and never accepts A: as the
source argument. A: is exclusively the staged virtual report disk;
`A:\FREAD.TXT` is the sole report write destination. There are no format,
repair, delete, physical write or raw-drive APIs. B: is the default, but an
explicit single removable-drive letter can be supplied.

Inventory showed A: virtual removable, C: system hard disk, D: CD-ROM and
**E: physical USB floppy**. Initial B: attempts correctly refused before
media-file access. Those are drive-letter guard results, not evidence of
a storage-driver failure. Logs/screenshots remain under ignored
`E:\usb-wdm-free\wave2\w98read-*` and `w98read-a\`.

The final bounded diagnostic reports progress before I/O, suppresses modal
critical-error prompts and checks the first three directory-order files.
It validates exact read byte counts and computes standard reflected CRC32;
this is a data checksum, not a cryptographic authenticity claim. Recursion,
path lengths, directory/file counts, per-file size and aggregate reads are
bounded. Synchronous OS disk reads have no diagnostic-enforced timeout;
the surrounding VM test is manually bounded. A pass covers only the named
files, not every sector or disk integrity.

## Earlier cancelled read — preserve the limitation

The first E: read used an earlier helper which painted its window only
after completing all reads. Without visible progress, the operator detached
USB after roughly two minutes. It had actually completed **13 file reads**
with matching directory byte counts and checksums. The next file was
interrupted at40960 of66294 bytes; error31 and an enumeration error followed
the detach. Windows then displayed its removable-disk read-error prompt.
The prompt was cancelled. This is an operator-cancelled run, **not a
diagnosed media or driver fault** and not an all-files pass.

Preserved report `w98read-detach-partial.txt`; screenshot
`w98read-cancelled.png`. The bounded repeat below is separate evidence.

## Bounded repeat

After reconnecting and allowing enumeration to finish, `A:\FREAD3.EXE E`
completed the bounded read with the project CBI binding still installed:

| File | Complete bytes | CRC32 | Result |
|---|---:|---|---|
| `IO.SYS` | 40774 | `A7EA3BE4` | PASS |
| `MSDOS.SYS` | 38138 | `4CC2FC77` | PASS |
| `COMMAND.COM` | 54645 | `1131ADD3` | PASS |

Summary: **3 files, 133557 bytes, failures0, READ PASS**. All three byte
counts and CRC32 values agree with their complete reads in the earlier
cancelled run. No claim is made for the remaining files or unread sectors.

Screenshot `w98read-project-pass.png` shows actual OS4.10/build2222, source
E:, filenames/counts/checksums and final summary. Immediately afterwards,
`w98read-project-properties2.png` reports working project CBI parent and
`w98read-project-driver.png` shows provider usb-wdm-free/date21-Oct-2026.
These are copied unchanged to ignored `out/proof/mitsumi/98read.png`,
`98bind.png`, `98driver.png`, with original paths/hashes in the proof
manifest. `w98read-project-pass.txt` preserves the report. Tested helper
SHA256: `AC8CEB0F40034F0095DE4075E6A399A47D9AA45578E44E7EF67A2D661E103D63`.

## Native control following project pass

Search-for-better-driver chose **Mitsumi USB Floppy** from the guest's
`C:\WINDOWS\INF\USBSTOR.INF`. Its `ExcludeFromSelect=*` hides native
models from the normal manual list. A direct Have Disk attempt did not
offer them, so the search route was used. The wizard requested
`USBNTMAP.SYS`; the mounted Windows98CD lacked it. The guest's already
installed `C:\WINDOWS\SYSTEM32\DRIVERS` satisfied the prompt; no host
driver binary was staged, exported or inspected. Native provider then
reported Microsoft/date16-Nov-2007, but immediate live-rebind General
reported an unspecified problem. This parallels the earlier live-rebind
limitation, so a clean Windows restart was requested before the control.

The native restart remained at the Windows98 splash screen for roughly
three minutes. USB detach did not immediately advance it. A normal ACPI
powerdown request was also attempted. **The native media-read control did
not complete**; no native read pass is claimed and no cause is assigned.
Screenshots `w98read-native-general.png`, `w98read-native-booted.png`,
`w98read-native-bootdetach.png` preserve the negative observations. This
does not invalidate the separately completed project-bound reads, and it
does not establish that the native and project routes differ in their
underlying binary behavior. No speculative INF/registry fix was applied.

## Cleanup and artifacts

The restart did not finish and did not respond to ACPI powerdown. After
the physical USB device had been detached, HMP `quit` closed QEMU; the
overlay is retained as the failed native-rebind/restart experiment, not a
clean shutdown image. PID33108 and localhost listener44338 were verified
absent. The host Mitsumi device remained present/OK with WinUSB. All owned
guests are headless and this run's guest is closed; other agents' guests
were not touched. The physical disk was left inserted and unchanged by
the diagnostic.

The final project report SHA256 is
`0E58D035D7F2679D900432FD5AC4B4A5F1D0093686FE3A52115922CA24102E3A`.
The report and untouched screenshot copies are ignored local evidence;
[proof index](proof-index.md) records the functional scope. Driver source,
INFs and release files were not modified by this media run. The only new
source is the original read-only user-mode diagnostic/build script.
