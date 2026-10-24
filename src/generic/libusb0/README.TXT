LIBUSB0 - libusb-win32 0.1 for Windows 98 SE and ME (usb-wdm-free 0.1.0)
=========================================================================

libusb-win32's driver (LIBUSB0.SYS) and library (LIBUSB0.DLL), rebuilt
from upstream 0.1.12.2 for Windows 98 SE and ME. Programs written for
libusb-0.1 use LIBUSB0.DLL to talk to a USB device that LIBUSB0.SYS
drives.

Devices LIBUSB0.INF lists (by VID/PID only, never by class):
  USBasp and other V-USB devices on the shared libusb PID  16C0:05DC
  USBtiny                                                  1781:0C9F
  Atmel AVRISP mkII                                        03EB:2104
  Atmel JTAGICE mkII                                       03EB:2103
  Atmel AVR Dragon                                         03EB:2107
Programs to pair them with: avrdude and other programs built against
libusb-0.1 (the "libusb-win32" or "libusb0" build of the program).

Installing (98 SE or ME): plug the device in; when the Add New Hardware
Wizard asks, point it at this folder (or "Search for the best driver" with
the floppy or this folder as the location). LIBUSB0.SYS goes to
SYSTEM32\DRIVERS and LIBUSB0.DLL to SYSTEM. TESTLIBU.EXE lists the devices
libusb sees, with their descriptors.

Another device: copy LIBUSB0.INF, add a line in its [Devices] section
with the device's own id, for example
  %MyDevice%=LibUsb,USB\VID_1234&PID_5678
and a MyDevice="..." line under [Strings], then install that copy for the
device. Only do this for a device no other driver should own.

Developer files: USB.H (the libusb-0.1 header), LIBUSB0.LIB (import
library for MSVC) and LIBUSB0.A (for MinGW).

Licences: the driver LIBUSB0.SYS is GPL (version 2 or later); the library
LIBUSB0.DLL, its headers and import libraries, and TESTLIBU.EXE are LGPL
(2.1 or later). Upstream: libusb-win32 0.1.12.2 by Stephan Meyer, with
the libusb-0.1 core by Johannes Erdfelt. Source, the licence texts and the
changes made for 98 SE and ME: https://github.com/yeokm1/usb-wdm-free
(src/generic/libusb0, docs/design/libusb.md).

Tested only in virtual machines (QEMU): install, load and the device list
under xHCI98, NUSB's EHCI and 98 SE's UHCI; no transfers to a real
programmer yet, and no leg on ME.
