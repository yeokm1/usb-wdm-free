USBFLOP.INF - USB floppy drives (UFI), 98 SE and ME (usb-wdm-free 0.1.2)
========================================================================

An INF only: it binds Windows' own USB storage driver (USBSTOR.SYS, with
USBNTMAP.SYS) to any USB floppy drive that uses the UFI command set
(USB mass storage class 08, subclass 04, protocol 00 or 01). Windows'
own INF lists only some drives by VID/PID; this one matches the class.

Windows 98 SE: install a USB storage driver set first. 98 SE has no USB
storage driver of its own; NUSB 3.3's storage files (USBSTOR.SYS,
USBNTMAP.SYS, USBMPHLP.PDR) are the set this INF was tested with. Without
them, the drive is bound to a driver that is not there and does not work.

Windows ME: use the folder for ME (wme). Its INF copies USBSTOR.SYS and
USBNTMAP.SYS from ME's own files; Windows may ask for the ME CD.

Installing: plug the drive in; when the Add New Hardware Wizard asks,
point it at this folder (or tick "Floppy disk drives" if it is on a
floppy). The drive then appears as a floppy drive letter.

Tested only in virtual machines (QEMU): a real Mitsumi drive passed
through, read under 98 SE's UHCI and under xHCI98 on 98 SE and ME. No
leg on a real PC, and no USB 2.0 (EHCI) leg.

Licence: GPL-2.0-or-later (https://github.com/yeokm1/usb-wdm-free).
