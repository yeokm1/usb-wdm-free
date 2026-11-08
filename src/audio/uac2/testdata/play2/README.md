# Sound Blaster Play! 2 descriptors

Test input for the `play2` profile of `uac1emu.py`. Read 2026-11-07 from
the owner's unit (`041E:323D`, `bcdDevice` 1.14, Full Speed), bound to
WinUSB on the host by the owner and passed to QEMU guests by `usb-host`
with its `pcap=` capture. Windows' own drivers in the guests (XP SP3
`usbaudio.sys` on UHCI, Windows 10 22H2 `usbaudio.sys` on xHCI) made every
request; the files are the device's answers as the capture holds them.
Nothing else was written to the device. The serial number (string 3) is
not recorded; the model gives index 0.

| File | What |
|---|---|
| `device.bin` | Device descriptor, 18 bytes: USB 2.00, class 0, 64-byte EP0, `041E:323D`, strings 1/2/3, one configuration |
| `config0.bin` | Configuration 1, 273 bytes (`wTotalLength`) |
| `hidreport.bin` | Interface 3's HID report descriptor, 109 bytes |

SHA-256:

```
319bd14a625b6059ec30625716734b83b4ce6c0808b632e0be5fd43f9c585f63  device.bin
fbc200d75b2f552fdb3b34ae83cb4c739fee471d4fa6dae92f1924d6682c72df  config0.bin
660c7ba72de44df803fe8109da328691aafdac9c5d8015c6c23c4f83beb9178a  hidreport.bin
```

What it holds: four interfaces, bus powered, 100 mA. Interface 0 is
AudioControl (ADC 1.00, two streaming interfaces 1 and 2). Capture: a
stereo microphone input terminal 1 through Feature Unit 6 (master mute and
volume) and Selector Unit 10 to the USB streaming output terminal 3.
Playback: the USB streaming input terminal 2 and a second stereo
microphone terminal 7 (through Selector Unit 12) into Mixer Unit 9 (two
channels out, no programmable crosspoint), then Feature Unit 5 (master
mute, volume per channel) to the speaker 4. Interface 1 alternate 1:
capture, 2 channels 16-bit, 44.1 and 48 kHz, asynchronous IN `0x81`
(200 bytes). Interface 2 alternate 1: playback, the same format,
asynchronous OUT `0x01` (200 bytes) whose `bSynchAddress` names the sync
endpoint `0x83` (bmAttributes `01`, 3 bytes, `bRefresh` 5: every 32 ms).
Both data endpoints declare the sampling-frequency control. Interface 3:
HID 1.00, interrupt IN `0x84` (4 bytes, 10 ms); the report has consumer
Volume Up/Down (report 1), output reports, and telephony usages.

The device's answers (the model gives the same):

| Request | Answer |
|---|---|
| GET_DESCRIPTOR device qualifier, string `0xEE` | stall |
| Feature Unit 6 volume (master) MIN/MAX/RES/CUR | `F100` (-15 dB) / `0F00` (+15 dB) / `00C0` (0.75 dB) / `0000` |
| Feature Unit 5 volume (channels 1, 2) MIN/MAX/RES/CUR | `CC00` (-52 dB) / `0000` / `0100` (1 dB) / `F800` (-8 dB) |
| Feature Units 5, 6 mute CUR | `00` |
| Selector Unit 10 CUR | `01` |
| Selector Unit 12 CUR | stall |
| Mixer Unit 9 CUR, every crosspoint Windows 10 asks (`0101` to `0204`) | stall |
| SET_CUR sampling frequency, endpoints `0x01` and `0x81`, 48000 | taken |
| HID SET_IDLE, SET_REPORT (output report 2) | taken |
| Sync endpoint `0x83` | `00 00 0C` (48.0 in 10.14) |
