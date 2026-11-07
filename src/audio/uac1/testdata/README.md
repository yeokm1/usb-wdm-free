# UAC 1.0 descriptor fixtures

Copies of recorded configuration descriptors, kept here so the `uac1` host
tests do not depend on other packages' test data. Descriptor data, not driver
binaries.

| File | Device | Copied from | SHA-256 |
|---|---|---|---|
| `brio100.bin` | Logitech Brio 100 (`046D:094C`), High Speed; UAC 1.0 microphone on interfaces 2/3 | `src/video/uvc98/testdata/brio100/CONFIG.BIN` | `4c6975d2262f3267ccd639c23a0843c4c101f71ea99d6c3768d6eb8d14d7eeb7` |
| `logi0a64.bin` | Logi USB Headset (`046D:0A64`), Full Speed | `src/audio/uac2/testdata/logi0a64/CONFIG.BIN` | `74cb6143c1983c29ffbe04adfd88c6233bed0c40a1d460b7631da72b280f5208` |
| `play2.bin` | Creative Sound Blaster Play! 2 (`041E:323D`), Full Speed | `src/audio/uac2/testdata/play2/config0.bin` | `fbc200d75b2f552fdb3b34ae83cb4c739fee471d4fa6dae92f1924d6682c72df` |
