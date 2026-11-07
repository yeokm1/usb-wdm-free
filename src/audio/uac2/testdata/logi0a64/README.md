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

## Added 2026-11-07: the report descriptor and the class answers

`HIDREPORT.BIN` (142 bytes, the length the HID descriptor declares) is
interface 3's report descriptor as the same unit answered Windows 10
22H2's own HID driver, the device passed to a QEMU guest by `usb-host`
(WinUSB on the host, bound by the owner) and read from `usb-host`'s
`pcap=` capture; the `logi` profile of `uac1emu.py` replays it. SHA-256
`172cbc17e9a3eb707c22acf242edd005d6e677126bddcbd0ee5b85ea17db6d99`.
The serial string is not recorded.

The answers the unit gave Windows' own `usbaudio.sys` (XP SP3 and 10):

| Request | Answer |
|---|---|
| Feature Unit 2 (microphone) volume MIN/MAX/RES/CUR | `E200` (-30 dB) / `0000` / `0100` / `F102` (off the 1 dB grid); mute and AGC CUR `00` |
| Feature Unit 5 (speaker, master) volume MIN/MAX/RES/CUR | `CE00` (-50 dB) / `0000` / `0100` / `EF00` (-17 dB); mute CUR `00` |
| Feature Unit 11 (sidetone) volume MIN/MAX/RES/CUR | `C700` (-57 dB) / `F400` (-12 dB) / `0180` (1.5 dB) / `DD80` |
| Mixer Unit 12 CUR (`0101` to `0203`) | stall |
| String 6 (input terminal 10's iTerminal) | "Sidetone" |
| Device qualifier, string `0xEE` | stall |
| A volume SET_CUR off the grid (XP sent `DA81`) | taken |
| SET_CUR sampling frequency (XP: 192000, 48000, 44100 on `0x01`; 16000, 48000 on `0x81`) | taken |
