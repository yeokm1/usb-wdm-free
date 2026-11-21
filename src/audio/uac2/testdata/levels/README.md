# Playback level baselines (design 28.15)

What the shipped `uac2` build (release 0.1.11, `uac2wdm.sys` 0.1.12.0, the
`free/audio/uac2/w2k-xp` files at main's tip) sent to a simulated device's
Feature Units in an XP SP3 guest, in order, with every value, and what
made it send them. `leveltest` replays each file through `uaclevel.c`
against a fake device that answers as the simulator did, and fails at the
first request that differs or is missing.

Each `.lvl` file:

- `#` lines are comments: the guest harness's steps, in time order, as
  `levbase.py` put them in from the harness log.
- Request lines, as `levbase.py` prints them from the simulator's log:
  `bmRequestType bRequest wValue wIndex wLength status actual data`.
  The two reads `uacusb.c` makes when a stream starts (`ReadFeature`) are
  not the level engine's and stay as comments.
- `!` lines are the events, written from the harness steps and the
  `Uac2VolMap` and `Uac2Level` values the guest read after them:
  `!profile <descriptor> <AC interface> <first> <count>`, `!start <render
  formats> <most channels> <terminal>` (Prepare's `LevelSetup` and
  `ApplyLocked`), `!vol <channel> <KS level>` and `!mute <channel> <0|1>`
  (a KS SET at PASSIVE, as WDMAud makes it for a mixer change), `!run`,
  `!stop`, `!d3`, `!d0`, `!dword Uac2VolAboveZero <n>`, `!saved <hex>` /
  `!nosaved` (the registry before a start), `!level`, `!change`, `!diag
  <hex>` and `!savedis <hex>` (checks against the guest's values), `!end`.

The descriptors: `..\cx31993m\config0.bin` and `..\sbx4\config0.bin` are
the simulator's `cx31993m` and `x4full`; `hs24-*.bin` are `uacemu.py`'s
`hs24` with `--fu full`, `master` and `channels`.
