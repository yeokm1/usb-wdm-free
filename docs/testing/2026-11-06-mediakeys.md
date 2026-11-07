# Keyboard media keys on 98 SE, ME, 2000 and XP: readings, 2026-11-06

The readings behind `docs/design/mediakeys.md` (xHCI98 task 47.28).
QEMU (TCG) with simulated keyboards (`src/hid/mediakeys/kbdemu.py`),
QEMU's own `usb-kbd`, `usb-audio` and PS/2 keyboard; no real keyboard.
Raw logs, screenshots and scripts are in the kit `E:\xhci98-vm\a347\wdm5\`
(`ev\`, `shots\`, `scripts\`, `pids.txt` per guest folder).

## Rig

- Emulator: the usb4-emu fork build `cb36497` (QEMU 11.1.2 with
  `usb-redir`), `qemu-system-x86_64.exe` SHA-256
  `3e7f6bf5b98fde98f56761c500e73fba62dd9a078b4d6c430546e59c95c4812d`.
- Guests: overlays of the shared clean q35 bases (98 SE `ab1d4d65...`, ME
  `42fea19a...`, 2000 SP4 `7fc9714e...`, XP SP3 `64ada103...`), 98 SE,
  ME and 2000 one vCPU, XP two. xHCI98 3.4.1.0 as published
  (`xhci98.sys` `f67f2bd2b4ab178786f8ee4f3a1b9aa4f701a02a11346cbd17de246898d9e62d`,
  the same file in `98-me` and `nt-x86`), installed with
  `INSTALL.BAT /console /yes /nopause` (98 SE, ME: exit 3, then a cold
  start; 2000: started, exit 0 on a second run; XP: exit 3 after its
  signing prompts and wizards, read before and without the restart).
- `usb-audio` (`audiodev=none`) on the xHCI root port 4 for a mixer:
  98 SE and ME bind it to `USBAUDIO.sys` (`WDMA_USB.INF`), 2000 and XP to
  their own USB audio driver.
- `kbdemu.py` on the xHCI root port 1 through `usb-redir`, Full Speed,
  VID FFFF: `boot` (0040), `twoif` (0041), `twoifid` (0042), `oneif`
  (0043), `bitmap` (0044), and `twoif` as 0045 (`--pid`) for the INF
  reading. The legs ran its first form, with the same descriptors and
  reports; the committed file then keeps the held state apart from
  delivery and answers GET_REPORT, SET_REPORT and SET_IDLE per report
  ID (idle resends, pending changes sent when polling resumes), which no
  leg here exercised (the guests' traces show no non-zero SET_IDLE).
- `KEYTEST.EXE` (`src/hid/mediakeys/keytest.c`, MSVC 6.0, `keytest.cmd`;
  the last legs ran build `2f184cdc...`; the committed source adds only
  the output closing at exit under the lock)
  in the guest, run from the transfer drive: the watch form (200 s full
  leg or 70 s short leg), `/enum`, `/hid`.
- The full leg (kit `scripts\keyleg.sh`): the letter A, Volume Up twice,
  Volume Down, Mute twice, Play/Pause, Next, Previous, Stop, the Keyboard
  page's Volume Up, Volume Down and Mute twice, Volume Up held 1.5 s,
  then 12 s apart AC Home, AL Email Reader, AC Search, AL Calculator, AL
  Local Machine Browser, AL Consumer Control Configuration, then AC Back,
  Forward, Refresh, Bookmarks and Stop. The short leg (`keyleg2.sh`):
  A, Volume Up, Down, Mute twice, Play/Pause, Next, Previous, Stop, AL
  Calculator. QEMU key codes (`qleg.sh`, QMP `send-key`): a, volumeup,
  volumedown, audiomute twice, audioplay, audionext, audioprev,
  audiostop, calculator.

## 98 SE (guest `g\w98`)

| Leg | Result | Evidence |
|---|---|---|
| `twoif`, first plug, no restart | wizards for both interfaces and both collections answered with Enter; consumer collection `HIDDEV.INF` `[HIDSystemConsumer]`, keyboard collection `KEYBOARD.INF` `[HIDKeyboard]` (`kbdhid.vxd`), both started; no mixer yet (USB audio not started before a restart, so this leg cannot tell whether hidserv needs the restart); A arrives, no consumer key does anything | `ev\w98-enum-1.txt`, `ev\w98-twoif-1.txt` |
| `twoif` after a cold start | Volume Up, Down, Mute step the mixer (61680 to 64301 to 65535, 62914, mute 1, 0); nothing else: no WM_APPCOMMAND, no key message, no window | `ev\w98-twoif-2.txt`, `shots\w98-twoif-2-b.png` |
| `twoifid`, `oneif`, `bitmap`, short leg | the same as `twoif` for those keys | `ev\w98-twoifid.txt`, `ev\w98-oneif.txt`, `ev\w98-bitmap.txt` |
| `boot` with the Keyboard page's 7F, 80, 81 | A only | `ev\w98-boot.txt` |
| PS/2 media key codes | WM_KEYDOWN/UP with VK FF and scan codes E0 30, 2E, 20, 20, 22, 19, 10, 24, 21; nothing acts | `ev\w98-ps2.txt` |
| QEMU `usb-kbd` | not read: the guest stopped first (below) | `shots\w98-usbaudio-fault.png` |
| `KEYTEST /hid`, `twoif` | collection 0 page 000C usage 0001 input 3 bytes, read=yes; collection 1 page 0001 usage 0006 read=yes; with overlapped reads no report; with blocking reads in a thread every report (`00ea00`, `000000`, `00cd00`, ...) | `ev\w98-hid.txt`, `ev\w98-hid2.txt` |
| `/hid` and the watch form at once | the watch form logs hidserv's mixer steps while the second instance reads every report into `C:\KEYHID2.TXT` | `ev\w98-coexist.txt`, `shots\w98-coexist-hid.png` |
| Throwaway `KBDCC.INF` (FFFF:0045 interface 1 to `kbdhid.vxd`) | the collection installs from it and starts (problem 0); no consumer key gives a key message; hidserv still steps the mixer | `ev\w98-kbdcc-enum.txt`, `ev\w98-kbdcc.txt` |

## ME (guests `g\me`, then `g\me2`)

| Leg | Result | Evidence |
|---|---|---|
| `twoif`, first plug, no restart | installed with no wizard; the same INF sections as 98 SE; every key works: mixer steps, WM_APPCOMMAND 10, 9, 8, 14, 11, 12, 13 and VK AF, AE, AD, B3, B0, B1, B2; Home opens Internet Explorer (and its connection wizard), Search, Calculator, My Computer, Windows Media Player open; held Volume Up repeats | `ev\me-twoif-1.txt`, `shots\me-twoif-1-b.png` |
| `twoifid`, `oneif`, `bitmap`, short leg | the same for those keys | `ev\me-twoifid.txt`, `ev\me-oneif.txt`, `ev\me-bitmap.txt` |
| `boot` with 7F, 80, 81 | A only | `ev\me-boot.txt` |
| PS/2 media key codes (`me2`, before xHCI98) | VK AF, AE, AD, B3, B0, B1, B2, B7 with the E0 scan codes and the WM_APPCOMMANDs; Calculator opens | `ev\me2-ps2.txt` |
| QEMU `usb-kbd` (`me2`) | A only; the media codes give nothing | `ev\me2-usbkbd.txt` |
| `KEYTEST /hid`, `twoif` (`me2`) | reads every report (overlapped reads complete on ME) | `ev\me2-hid.txt` |

## 2000 SP4 (guest `g\w2k`)

| Leg | Result | Evidence |
|---|---|---|
| `twoif`, first plug | consumer collection `hidserv.inf` `[HIDSystemConsumer]`, keyboard `keyboard.inf` (`kbdhid`); volume, transport and Home keys a WM_APPCOMMAND and a VK, the later keys VK only (the focus had left the tool); the mixer does not move (audio started); Home and Search open; Mail, Calculator, My Computer, media key: VK only | `ev\w2k-enum-1.txt`, `ev\w2k-twoif.txt` |
| `twoif` after a cold start | the same, and the volume keys now step the mixer | `ev\w2k-twoif-2.txt` |

## XP SP3 (guest `g\xp`)

| Leg | Result | Evidence |
|---|---|---|
| `twoif`, first plug, xHCI98 started before the advised restart | volume keys step the mixer; volume, transport and Home keys a WM_APPCOMMAND and a VK, the later keys VK only (the focus had left the tool); Home opens Internet Explorer, Mail Outlook Express, Search, Calculator, My Computer, Windows Media Player 9 | `ev\xp-twoif.txt` |

## Seen on the way (outside this task)

- 98 SE stopped once with "A fatal exception 00 has occurred at
  0028:FF09FED4 in VXD USBAUDIO(01) + 00002ED4" (98 SE's own
  `usbaudio.sys` driving QEMU's `usb-audio` under xHCI98 3.4.1.0), around
  an unplug of the simulator and a plug of QEMU's `usb-kbd` on another
  port (`shots\w98-usbaudio-fault.png`). A fresh cold start with the same
  devices plugged and unplugged did not repeat it. Not followed further.
- This QEMU build aborts on QMP `input-send-event` with a `device`
  argument when it runs with `-display none` ("Property
  'qemu-fixed-text-console.device' not found"); `send-key` works.
- 98 SE on these bases hangs at "Windows is shutting down" now and then
  (the bases' STATE.md); the guest was ended from QEMU there.
- In the watch form, after the first launch key the launched program
  held the focus: later keys show as GetAsyncKeyState lines and the
  windows that opened, not as messages. The key and WM_APPCOMMAND lines
  are logged in arrival order; no order between them is claimed.
