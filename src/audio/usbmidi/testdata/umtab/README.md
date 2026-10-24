# UM-ONE mk2 current TAB descriptors

Read-only libusb standard descriptors captured 2026-10-21 after the owner
reported WinUSB binding, with the physical switch confirmed TAB. No MIDI
payload, strings, class/vendor control, reset or configuration change was
requested. Actual speed is full speed (libusb enum2).

Current identity **0582:0168**, USB1.1, AC interface0 and MIDIStreaming
interface1; one cable, bulk OUT02 and IN81, both maxpacket64. This is a
standard MIDI1 descriptor set and the existing strict UmParseInterface
accepts it without a vendor quirk. Embedded jack IDs are40 (OUT) and20
(IN), hex. The earlier 0582:012A capture remains a separate fixture; its
vendor-specific descriptors do not describe this reconnected identity.

- DEVICE.BIN SHA256: f6575633acdd26f261ea172d00c67ad2c6d9f29679db1be25346ec2bf922a329.
- CONFIG.BIN SHA256: 0b56db383a01a7814a84778e104b2a19045bc6e69d79651f824ebd7391a1b8c9.

The owner has no DIN loopback attached. Physical driver-start/open tests
must not be described as MIDI roundtrip or DIN electrical validation.
