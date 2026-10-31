# usbprn on 98 SE and ME with the QEMU printer model, 2026-10-31

The first legs of `usbprn` (`docs/design/printing.md`): milestone M0 on
stock ME, then the 98 SE package under three USB stacks and the ME INF.
Every leg ran in QEMU against `usb-printer`, this repository's test-only
printer-class model (owner, 2026-10-31; `printing.md` 14). No real printer
was used: the real-printer bench (a USB-to-parallel cable with a printer,
or a USB PCL or PostScript laser) is still to come.

## The printer model

[qemu-printer.patch](qemu-printer.patch), applied to QEMU 11.1.0 after
`qemu-usbnet-hs.patch` and `qemu-xhci-cancel.patch` (the build of
[2026-10-23-qemu-xhci-cancel.md](2026-10-23-qemu-xhci-cancel.md), plus
the patch), MSYS2 MINGW64, the same configure line. `usb-printer`:

- interface 07/01/02 (bulk OUT 0x01, bulk IN 0x82), or 07/01/01 with
  `protocol=1`; Full Speed, or High Speed beside it with `highspeed=on`;
- `GET_DEVICE_ID` from `device-id=` (here
  `MFG:Hewlett-Packard;CMD:PJL,PCL;MDL:HP LaserJet 4;CLS:PRINTER;DES:Hewlett-Packard HP LaserJet 4;`);
- `GET_PORT_STATUS` 18h (Select, Not Error); `paper-empty`, `error`,
  `offline` and `busy` set at run time with `qom-set`; while one is set,
  bulk OUT is NAKed, as a printer that has stopped taking data;
- `SOFT_RESET` as 21h and 23h (`reset-other-only=on` stalls 21h);
- bulk OUT into the host file `file=` (`append=on` kept every job of a
  leg in one file); `out-rate=` paces it (500 bytes/s for the mid-job
  unplugs);
- bulk IN: a PJL status reply to `@PJL INFO STATUS`, bytes written to
  `in-inject`, a zero-length packet after a reply that fills its last
  packet;
- `log=`: one line per request (the per-leg logs below).

qtest `tests/qtest/usb-printer-test.c` on `qemu-xhci`: 8 of 8 pass
(descriptors for protocols 2 and 1, the device id and its truncation, port
status bits, both reset codes, the host file, OUT held on paper-empty and
busy and resumed, OUT pacing, IN replies with the zero-length packet).

| File | SHA-256 |
|---|---|
| `qemu-printer.patch` | `65466906765680f1f67ad232e77dfc9bc81dd82015171cb838892a3daf91ca73` |
| `qemu-system-x86_64.exe` (patched) | `42575c4cc154a6ae3ef18a477495c1c8abf4e51010bf04014579efb04f1f83ab` |
| `usb-printer-test.exe` | `c1d3ef1063675307025b007346e3e87a9d96756bfefa0e691ccf37a9f838a31f` |

## Rig

Kit `E:\xhci98-vm\p45\uwf-usbprn\` (scripts `launch.ps1`, `step.ps1`,
`plug.py`, `inst98.ps1`, `addprn2.py`, `statusleg.ps1`, `plugleg.ps1`,
`cyclef.ps1`, `pclcheck.py`; QMP ports 49640 to 49643; every QEMU start
and stop in `pids.txt`). Headless TCG, `-machine pc -cpu pentium3,-apic`,
1 vCPU, 256 MB, a `usb-tablet` for the mouse, one fresh overlay per leg:

| Guest | Base (read-only) | USB stack the printer was on |
|---|---|---|
| ME (M0, me1) | `E:\usb-wdm-free\vm\wme.qcow2`, stock ME | Windows' own UHCI (`piix3-usb-uhci`) |
| A1 | 98 SE + xHCI98 3.2.0.0 | xHCI98 (`qemu-xhci`), Full Speed |
| B1 | 98 SE + NUSB 3.3 | NUSB's EHCI (`usb-ehci`), High Speed |
| C1 | 98 SE + xHCI98 3.2.0.0, `piix3-usb-uhci` added | Windows' own UHCI (installed from `USB.INF` at first boot), Full Speed |

Build: `src\print\usbprn\build.cmd free` at `0477e4f`, tree clean, all
gates PASS (32 SYS imports in the 98 DDK WDM.H, 27 DLL imports in the 98
SE and ME export tables, exports exactly `InitializeMonitorEx`,
`DllEntryPoint`, `InstallMonitor`, 31 INF mutations rejected, 95 host
checks):

| File | SHA-256 |
|---|---|
| `USBPRN.SYS` | `DBB195A873C9D7E0A39DC965AEA94AADA1BD7BDF86BACF5760867D81A52A839A` |
| `USBPRNMN.DLL` | `2C175137FBFAC20EF22BFD704CA1A763E118B0930A1ABAC0966B5147ADF9D6DE` |
| `USBPRN.INF` | `70117BB0C63345312332E253C1B0660C2F0EC248EAAF69235414BC9F67F61A5B` |
| `USBPRNME.INF` | `FECB97B1808A952C1DB69F94376548E322B3551E968B1E2DADE41BE44F8CE7FE` |

Development runs before the final build used the `trace` flavour
(`USBPRN.SYS` lines on QEMU's port E9h, `USBPRNMN.DLL` lines in
`C:\USBPRNMN.LOG`); what they showed is in `printing.md` 18.

A job counts as "well-formed" when `pclcheck.py` finds it starting with
the PJL UEL and `@PJL ... ENTER LANGUAGE=PCL`, holding `ESC E`, and ending
with form feed, `ESC E` and the UEL.

## M0: stock ME

- Plug: Add New Hardware Wizard "QEMU USB Printer"; automatic search with
  the ME CD present: "Windows was unable to locate the software for this
  device". Registry export: the devnode `USB\VID_46F4&PID_0010\PRN0001`
  has the compatible ids `USB\CLASS_07&SUBCLASS_01&PROT_02`,
  `USB\CLASS_07&SUBCLASS_01`, `USB\CLASS_07`, class Unknown, no `Driver`.
  `HKLM\...\Control\Print\Monitors\usbmon` is registered; `USBPRINT.SYS`
  and `USBMON.DLL` are on disk (directory listing). Add Printer: COM1,
  FILE, LPT1. **ME cannot print to a USB printer as installed.**
- With `USBPRNME.INF` copied to `C:\WINDOWS\INF` and the devnode deleted:
  the printer bound silently ("USB Printing Support"), `PortName=USB001`
  appeared in its key, the printer child
  `USBPRINT\HEWLETT-PACKARDHP_LA22F9` got no driver, Add Printer listed
  "USB001 Virtual printer port for USB", and the HP LaserJet 4 test page
  arrived: 71,023 bytes, well-formed.

## ME: the package leg (me1)

Fresh stock ME overlay; `USBPRNME.INF` on drive A: (same hash as above).

| Step | Result |
|---|---|
| Plug, automatic search | the wizard found the INF on A: and installed "USB printing support (bidirectional)"; then a second wizard for the printer child, no driver found (Finish) |
| Add Printer, HP LaserJet 4 on USB001, test page | PASS, 71,023 bytes, well-formed |
| Notepad page | PASS, 18,570 bytes, well-formed |
| paper-empty during a job | ME's own driver reads no status: the job waited with no dialog for 3 minutes, then finished when the paper came back |
| unplug and replug idle, then a Notepad page | PASS |

## 98 SE: the legs

| Step | A1 xHCI98 | B1 NUSB EHCI (High Speed) | C1 Windows' UHCI |
|---|---|---|---|
| Plug, wizard: floppy search finds `USBPRN.INF` ("USB printer (bidirectional)"), installs | PASS | PASS | PASS |
| Add Printer lists USB001 at once (no restart; the INF's RunOnce registered the monitor) | PASS (`A1-port-usb001.png`) | PASS | PASS (`C1-port-usb001.png`) |
| HP LaserJet 4 (in-box, PJL language monitor) on USB001, test page | PASS, 48,707 bytes, well-formed | PASS, same bytes | PASS |
| Notepad page | PASS | PASS | PASS |
| paper-empty during a job | "The printer is out of paper. Add more paper." with Retry (`A1-paper-empty.png`); the job finished when the paper came back | same (`B1-paper-empty.png`) | same (`C1-paper-empty.png`) |
| error during a job | "The printer is not ready. Make sure it is turned on and online." with Retry (`A1-error.png`); finished after | same (`B1-error.png`) | same (`C1-error.png`) |
| unplug and replug idle | PASS, back on USB001 | PASS | PASS |
| unplug 12 s into a job paced at 500 bytes/s | "not ready" dialog with Retry (`A1-midjob.png`); after the replug the retry finished the job: the two parts join into one well-formed job of the expected size (8,192 + 15,085 = 23,277 bytes), no byte lost or doubled | same (8,192 + 14,162 = 22,354) | same (6,144 + 17,483 = 23,627) |
| a second printer at once | USB002 listed beside USB001 (`A1-two-ports.png`); a second HP LaserJet 4 on USB002 printed its test page (50,068 bytes, well-formed) | same (`B1-two-ports.png`) | - |
| idle unplug and replug cycles | 15 of 15 | 10 of 10 | 10 of 10 |

All job files: `E:\xhci98-vm\p45\uwf-usbprn\evidence\` (`x-A1-p1.prn`,
`n-B1-p1.prn`, `C1-p1.prn`, `*-mj1.prn`/`*-mj2.prn` and their joins,
`A1-p2.prn`, `B1-p2.prn`), with the model's request logs (`*.log`) and
the screenshots named above; the M0 registry exports and screenshots in
`evidence\m0\`.

## A removal that did not finish

With the build before `0477e4f` (`fd19583`), one idle unplug on 98 SE
under xHCI98 (guest F1, right after the paper-empty and error jobs) never
finished: the busy cursor stayed and the replugged printer was never
started; QEMU was stopped. An earlier development build had done the same
once. The same sequence and 38 further unplugs of that code (trace and
free flavours, under xHCI98) did not repeat it, so its cause was not found. `0477e4f`
sends the pipe aborts one at a time and, while a stop or removal waits
for I/O, once a second instead of every 100 ms (concurrent and rapid
aborts were the suspect); every unplug of the final legs above (35
cycles and 9 single unplugs) finished.

## Verdict

98 SE (xHCI98, NUSB's EHCI, Windows' UHCI): install without a restart,
port USB001, in-box PCL printer added by hand, test page and Notepad page
as well-formed PJL/PCL jobs, paper-empty and error shown by the spooler
with Retry and recovered, unplug and replug idle and mid-job with the
job completed after Retry, two printers on USB001 and USB002: PASS.
ME with `USBPRNME.INF`: install, USB001 (ME's own monitor), test page and
Notepad page, replug: PASS; status is ME's own driver's (none shown).

Not covered: a real printer or USB-to-parallel cable (the owner's bench),
standby and resume, an MFP, PnP printer install (not built), the
removal stall above (not reproduced since).
