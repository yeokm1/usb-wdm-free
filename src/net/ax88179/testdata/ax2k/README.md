# AX88179 descriptors observed inside Windows 2000/QEMU

Captured 2026-10-21 by original `HUBDESC.EXE` via the Windows USB hub
user-mode descriptor IOCTL, before installing an AX function driver.
The target filter was exactly `0B95:1790`; no strings, serial, MAC/EEPROM,
vendor registers, reset or configuration writes were requested.

This is a **separate guest-view fixture**, not a rewrite of the physical
SuperSpeed capture in `../ax179/`. QEMU attached the physical device to
EHCI and reported 480 Mb/s, but the guest still received `bcdUSB=0300`,
1024-byte bulk endpoints and SuperSpeed endpoint companions. That does
not establish working USB2 passthrough or an AX driver.

| File | SHA-256 |
|---|---|
| `device.bin` | `11285e7808d8653fce8d3193d6af84a1896777192e69c8f85bf2b5f6154627df` |
| `config0.bin` | `9fc701e4df09e07ac9eb91eed85014f469aa367cbb9a13d7fdcda8280266a97c` |
| `hubdesc.txt` | `1f56dde1d77e9bf7fce8a00ac46994b829d2cec4cd25aa64be8b23baac151ea0` |

`hubdesc.txt` is the unchanged original guest helper output. Full setup,
byte differences, limitations and cleanup are in the
[run record](../../../../../docs/testing/2026-10-21-ax88179-2k-descriptors.md).
