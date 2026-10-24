# Physical Logi USB Headset: UAC1 negative fixture

Captured 2026-10-21 before any Zadig change from connected046D:0A64,
identified by Windows as Logi USB Headset. Native audio/HID/composite
bindings were left unchanged. Our explicit-target HUBDESC helper requested
only standard device/configuration descriptors through the hub. No audio,
strings, class/vendor requests, configuration changes or interface claims.

The configuration is280 bytes. AudioControl interface0 has protocol00 and
class-specific bcdADC0100: **USB Audio Class1.0, not2.0**. Interfaces1/2 are
audio streaming and interface3 HID. Host libusb reports full speed. The
device's bcdUSB0200 denotes the USB bus specification, not its audio-class
revision. This is a useful negative classification fixture, not a UAC2
target or a physical playback/capture test.

SHA-256:

- DEVICE.BIN: `c70b8ac48369f927c804ad86cc7c840d87f9bb67eac05e55c601987ef0d0ae5d`
- CONFIG.BIN: `74cb6143c1983c29ffbe04adfd88c6233bed0c40a1d460b7631da72b280f5208`

No device serial string was requested. Files are USB descriptor data, not
driver binaries. HUBDESC.TXT records the acquisition and endpoint facts.
