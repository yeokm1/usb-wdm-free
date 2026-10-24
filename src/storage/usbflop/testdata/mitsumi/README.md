# Mitsumi USB floppy descriptors

Captured 2026-10-21 from the owner's `03EE:6901` drive, bound to WinUSB
using Zadig. Python 3 with QEMU 11.1.0's `libusb-1.0.dll`, through
`src/generic/usbprobe/usbdesc.py`: standard configuration GET_DESCRIPTOR
requests only; no interface claim, reset, configuration selection, class
requests, media access or string/serial-number capture.

Full Speed (`libusb_get_device_speed` = 2). Interface 0, alternate 0,
class/subclass/protocol `08/04/00`: UFI, CBI with completion interrupt.
Bulk IN `81`, bulk OUT `02`, both 64-byte packets; interrupt IN `83`,
2-byte packets. Already listed by Microsoft's storage INFs; see
`docs/design/floppy.md` section 2. A manually selected class entry can
exercise this package but cannot establish automatic unlisted-drive rank.

SHA-256:

```
07d2e3aef61f5ca9589c5f8429acbe93fdf008c2d41497c72795d8c2f7244888  device.bin
d08f1c213500d110828d20e519e884d8a971a9ee6ed8f347c5d368860d2ac9e7  config0.bin
```
