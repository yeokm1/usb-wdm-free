# Physical ASIX AX88179 descriptor capture

Captured 2026-10-21 after the owner connected the adapter and configured
WinUSB with Zadig. The device reports VID/PID `0B95:1790`; libusb reports
SuperSpeed (enum value 4), host bus 4/address 3 at capture time. Host
addresses may change after replug.

The repository's read-only `src/generic/usbprobe/usbdesc.py` requested only
standard device/configuration descriptors. No configuration selection,
interface claim, reset, vendor register request, MAC/EEPROM read or string
request was performed. No serial number was captured. The X4 remained
assigned to its separate audio test.

| File | SHA-256 |
|---|---|
| `device.bin` | `098dc327a0bdf75036e337566369db7d4fd5274dcc013dd22dfd07d36eba6e83` |
| `config0.bin` | `e5b31952c1bbccf229dc5ac4be8b57989b0e6e5e6827310bea8f7c0bd5b71e13` |

Interface 0 alternate 0 is vendor class `FF/FF/00`, with interrupt IN
`81` (8-byte maximum), bulk IN `82` and bulk OUT `03` (1024-byte maxima
at this actual SuperSpeed connection). This is not a CDC-ECM device;
the experimental ECM driver is not an install candidate for it.

These are descriptor facts. No ASIX driver installation, register setup,
link status, packet transfer or QEMU passthrough result follows from this
capture. A later USB2 capture must be recorded separately rather than
rewriting the SuperSpeed descriptors to look like a high-speed device.
