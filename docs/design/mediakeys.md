# Keyboard volume and media keys on Windows 98 SE and ME (mediakeys)

Status: readings and design of 2026-11-06 (xHCI98 task 47.28). Owner,
2026-11-06: option A, documentation only (section 7); B, C and D are
kept as designs and are not built. No driver code: the
simulator (`src/hid/mediakeys/kbdemu.py`) and the guest reading tool
(`src/hid/mediakeys/keytest.c`, `KEYTEST.EXE`) only.

## 1. The question

A keyboard's volume and media keys (Volume Up and Down, Mute, Play/Pause,
Next and Previous Track, Stop) and its browser, mail and launch keys are
reported on the HID Consumer page (usage page 0x0C) in a top-level
"Consumer Control" collection (usage 0x01), either on a second USB
interface or under a report ID of the keyboard's own interface. Does
Windows 98 SE and ME act on them under xHCI98 3.4.1.0, and where it does
not, is there an INF or a small HID filter that closes the gap?

## 2. Readings

Record: `docs/testing/2026-11-06-mediakeys.md` (rig, command lines,
hashes, every leg). The readings ran in QEMU, q35 guests of the shared
clean bases with xHCI98 3.4.1.0 as published (`xhci98.sys` `f67f2bd2...`),
QEMU's `usb-audio` for a mixer to watch, and:

- `kbdemu.py`, a usb-redir keyboard simulator: a boot keyboard (03/01/01)
  plus a Consumer Control collection in four layouts real keyboards use:
  `twoif` (a second interface, a 16-bit usage array), `twoifid` (the
  second interface with report IDs, Consumer Control and System Control),
  `oneif` (one interface, report ID 1 the keyboard, 2 Consumer Control),
  `bitmap` (a second interface, one bit per usage, read with the short
  leg's keys only: volume, mute, transport, Calculator); and `boot`, the
  keyboard alone, as the control;
- QEMU's own `usb-kbd` (no Consumer Control collection) and the PS/2
  keyboard, sent QEMU's media key codes, as controls;
- `KEYTEST.EXE` in the guest: a window that logs WM_KEYDOWN/UP (virtual
  key, scan code), WM_APPCOMMAND (passed on to DefWindowProc), the
  GetAsyncKeyState of VK A6..B7, the mixer's master volume and mute, and
  the foreground window's title (a launched program); `/enum` lists the
  HID and USB device nodes with their INF section and driver; `/hid`
  opens each HID collection through hid.dll and reads the Consumer
  Control ones.

| Key (Consumer usage) | 98 SE | ME | 2000 SP4 | XP SP3 |
|---|---|---|---|---|
| Volume Up, Down (E9, EA) | mixer steps, after a restart (hidserv) | mixer steps; WM_APPCOMMAND 10, 9; VK AF, AE | mixer steps after a restart; WM_APPCOMMAND; VK | mixer steps; WM_APPCOMMAND; VK |
| Mute (E2) | mixer mute toggles, after a restart | toggles; WM_APPCOMMAND 8; VK AD | toggles after a restart; WM_APPCOMMAND; VK | toggles; WM_APPCOMMAND; VK |
| Play/Pause, Next, Previous, Stop (CD, B5, B6, B7) | nothing | WM_APPCOMMAND 14, 11, 12, 13; VK B3, B0, B1, B2 | the same | the same |
| AC Home (223) | nothing | WM_APPCOMMAND 7; VK AC; Internet Explorer opens | the same | the same |
| AC Search (221) | nothing | VK AA; Search opens | the same | the same |
| AL Email Reader (18A) | nothing | VK B4; no window seen | VK B4; no window | VK B4; Outlook Express opens |
| AL Calculator (192) | nothing | WM_APPCOMMAND 18; VK B7; Calculator opens | VK B7; no window | VK B7; Calculator opens |
| AL Local Machine Browser (194) | nothing | VK B6; My Computer opens | VK B6; no window | VK B6; My Computer opens |
| AL Consumer Control Configuration (183) | nothing | VK B5; Windows Media Player opens | VK B5; no window | VK B5; Windows Media Player 9 opens |
| AC Back, Forward, Refresh, Bookmarks, Stop (224-227, 22A) | nothing | not read cleanly (the focus had left the tool; VK A7 seen) | VK A6, A7, A8, AB, A9 | VK A6, A7, A8, AB, A9 |
| Keyboard page Mute, Volume Up, Down (7F-81) | nothing | nothing | not read | not read |

WM_APPCOMMAND numbers are the APPCOMMAND_* values (Microsoft Learn,
"WM_APPCOMMAND message"): 8 VOLUME_MUTE, 9 VOLUME_DOWN, 10 VOLUME_UP, 11
MEDIA_NEXTTRACK, 12 MEDIA_PREVIOUSTRACK, 13 MEDIA_STOP, 14
MEDIA_PLAY_PAUSE, 7 BROWSER_HOME, 18 LAUNCH_APP2. After AC Home the
launched program held the focus, so for the keys below it `KEYTEST.EXE`
saw the virtual key (GetAsyncKeyState) and the window that opened, not
the messages; "VK" there means that, and says nothing of a WM_APPCOMMAND.
The full leg (every key) ran on `twoif` only; the other layouts ran the
short leg (volume, mute, transport, Calculator).

### 2.1 Windows 98 SE

- The Consumer Control collection gets `HID-compliant consumer control
  device` from 98 SE's own `HIDDEV.INF`, `[HIDSystemConsumer]`, by the
  compatible id `HID_DEVICE_UP:000C_U:0001`, with no driver of its own;
  the keyboard collection gets `kbdhid.vxd` (`KEYBOARD.INF`,
  `[HIDKeyboard]`). Both start (problem 0).
- `[HIDSystemConsumer]` copies `hidserv.exe` and adds it to
  `RunServices` (`"Hidserv.exe run"`) and `RunOnce` (`"Hidserv.exe
  install"`). On the first plug, before a restart, no key did anything,
  but the USB audio device had not started either (no mixer until the
  restart), so that reading does not tell whether hidserv itself needs
  the restart. After one restart Volume Up, Down and Mute change the
  mixer.
- No key does anything else: no WM_APPCOMMAND, no key message, nothing
  opens. The same for every layout read and before and after the
  restart.
- The PS/2 keyboard's media scan codes (E0 30, E0 2E, E0 20, E0 22, E0 19,
  E0 10, E0 24, E0 21) reach the window as WM_KEYDOWN with virtual key FF
  (unassigned) and the scan code; nothing acts on them either.
- An INF that binds the Consumer Control collection to 98 SE's own
  keyboard mapper (`DevLoader=*ntkern`, `NTMPDriver=kbdhid.vxd`, as
  `HIDDEV.INF` `[HIDSystemControl]` does for System Control) installs and
  starts; `kbdhid.vxd` then maps none of the consumer usages (no key
  message); hidserv still changes the volume (a throwaway INF in the
  record, not part of any package).
- A user-mode program opens the collection through hid.dll and SetupAPI
  (shared read) and reads every report (`report=00ea00` for Volume
  Decrement: report ID 0, then the usage) with blocking reads; 98 SE
  completed no overlapped read on that handle. hidserv keeps acting while
  it reads: both get every report.

### 2.2 Windows ME

- The same INF sections as 98 SE (`HIDDEV.INF` `[HIDSystemConsumer]`;
  `KEYBOARD.INF` `[HIDKeyboard]`), and every key works on the first plug,
  no restart: the volume keys step the mixer, and the volume, transport,
  Home and Calculator keys arrive in the focus window as a WM_APPCOMMAND
  and a virtual key (VK AD..B7, scan code 0, extended). The launch keys
  open Internet Explorer, Search, Calculator, My Computer and Windows
  Media Player; Mail gave its virtual key and no window that the tool
  saw. Every layout read behaves the same.
- The PS/2 keyboard's media scan codes become the same virtual keys and
  WM_APPCOMMANDs.
- A user-mode program reads the collection as on 98 SE.

### 2.3 Windows 2000 and XP (reference)

- 2000 SP4: the collection gets `hidserv.inf` `[HIDSystemConsumer]`
  (`hidserv.exe`, installed through `RunOnce` `rundll32.exe
  hid.dll,HidservInstaller /install`). On the first plug the volume,
  transport and Home keys arrive as a WM_APPCOMMAND and a virtual key, but
  the volume keys do not move the mixer (the audio device was started
  there, so on 2000 the restart is hidserv's); after a restart they do.
  Calculator, My Computer, Mail and the media-player key arrive as
  virtual keys and open nothing; Home and Search open.
- XP SP3: the same `hidserv.inf` section; on the first plug, no restart,
  the volume keys step the mixer, the volume, transport and Home keys
  arrive as a WM_APPCOMMAND and a virtual key, and the launch keys open
  Internet Explorer, Outlook Express, Search, Calculator, My Computer
  and Windows Media Player 9.

### 2.4 What follows

1. ME, 2000 and XP act on the keys with their own components: nothing to
   do there.
2. 98 SE acts on Volume Up, Down and Mute only, through `hidserv.exe`.
   They worked after a restart; whether hidserv alone needs that restart
   was not separated from the audio device's own start (a release-notes
   wording point, section 7 A, not a code one).
3. The 98 SE gap is the other keys, and no system component consumes
   them: 98 SE generated no WM_APPCOMMAND and mapped no media key to a
   virtual key, from HID or from PS/2. A kernel mapper that turned
   consumer usages into the media scan codes would deliver virtual key
   FF, which nothing acted on, and the in-box `kbdhid.vxd` mapped none of
   them. A filter could still turn a usage into some other input that a
   given program already understands (an application's own shortcut
   key), but that is a per-program mapping in the kernel, with a report
   descriptor rewrite under every keyboard (section 5).
4. A user-mode program can read the collection beside hidserv.

## 3. What Microsoft documented

- 98 SE's `HIDDEV.INF` (Copyright 1998) lists `HID_DEVICE_UP:000C_U:0001`
  as `HID-compliant consumer control device`, installed with
  `hidserv.exe`, and binds Microsoft's and Altec Lansing's USB speakers'
  "HID Audio Controls" interfaces to the HID stack: the 98 support is
  for audio controls.
- "Keyboard Scan Code Specification", Windows Platform Design Notes,
  Revision 1.3a, March 16, 2000 (Microsoft; formerly
  `download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/scancode.doc`),
  section "Usage Page and Usages for Audio Control": a device "must
  declare itself as being a Consumer Control device (usage 0x01)" on the
  Consumer page; "When such a device is enumerated by the operating
  system, the supporting software (HIDSERVE.EXE) is installed and loaded
  on the host system"; Table 1 lists the Consumer page audio controls
  supported in Windows 2000 (Volume 0xE0, Mute 0xE2, Bass 0xE3, Treble
  0xE4, Bass Boost 0xE5, Loudness 0xE7, Volume Increment and Decrement
  0xE9 and 0xEA, and the Bass and Treble Increment and Decrement pairs
  0x152 to 0x155), and "These controls are supported in Windows 98
  (original release and Service Pack 1 release)" marks Volume, Mute, Bass
  Boost, Volume Increment and Volume Decrement. The
  controls "Affect audio playback by the default system audio device
  (same device as sndvol32.exe controls)" and must sit right beneath the
  top-level collection. The document names no transport, browser or
  launch usage.
- Microsoft Learn, "WM_APPCOMMAND message": minimum supported client
  Windows 2000; a window should return TRUE when it handles the message,
  "Doing so will allow software that simulates this message on Windows
  systems earlier than Windows 2000 to determine whether the window
  procedure processed the message". That is the period's own statement
  that on earlier systems (98 SE) a keyboard maker's software generated
  the message. (The readings show ME generating it too.)
- Microsoft Learn, "Top-Level Collections Opened by Windows for System
  Use": usage page 0x000C usage 0x0001 "Consumer controls", access mode
  Shared, which is why a second reader can open it (section 2.1).
- 2000 SP4's and XP SP3's `hidserv.inf` match the same compatible id to
  `[HIDSystemConsumer]`; XP's lists keyboards' consumer collections by id
  (`HID\VID_045E&PID_001D&MI_01&COL01` and others).

## 4. Makers' drivers (owner policy of 2026-11-06)

Checked on 2026-11-06 at the makers' own sites.

| Maker | What the maker offers for these systems | Carried in usb-wdm-nonfree? |
|---|---|---|
| Logitech (its keyboards only) | iTouch 2.22 build 289, "Windows XP (or older)" (support.logi.com, article 360025121974); the English package `it222enu.exe` (4429593 bytes, SHA-256 `48208ced...0a0c`, from `download01.logi.com/web/ftp/pub/techsupport/keyboards/itouch/`) holds `Win98`, `WinME`, `Win2K_XP` and `WinNT4` folders with Logitech's own HID minidriver and filter for 72 Logitech ids (`USB\VID_046D&...`); its README says "This software supports the following platforms: Windows(R) 95, 98, 98 SE, ME, NT 4.0 (Service Pack 6 or higher), Windows(R) 2000 and XP" and lists the media players it drives (Windows Media Player up to 9.x, Winamp 2.x, 3 and 5, RealOne, MUSICMATCH and others) | No: no licence or redistribution terms on the page or in the package's README (owner question Q3); listed as "Not shipped" with the link |
| Microsoft (its keyboards only) | IntelliType Pro 8.2 for XP, Vista and 7 on the Download Center; no IntelliType for 98 SE or ME is offered there any more | No (nothing for 98 SE or ME to carry) |
| Other keyboard makers | not surveyed one by one: their 98-era utilities, where they existed, served their own ids | - |

The Logitech package was unpacked with 7-Zip into its own folder to read
its file list and README; nothing in it was run.

Who covers what, then, as far as these readings (simulated keyboards of
the usual shapes, section 2) and this survey reach:

- **ME, 2000, XP**: Windows itself, for a keyboard of those shapes
  (section 2), with the launch keys as read there (2000 opens no
  Calculator, My Computer, Mail or media player; ME opened no mail
  window).
- **98 SE, volume and mute**: Windows itself (`hidserv.exe`), after a
  restart.
- **98 SE, other keys, the 72 Logitech ids iTouch lists**: Logitech's
  iTouch, which the user downloads from Logitech.
- **98 SE, other keys, other keyboards**: no maker's software found;
  other makers were not surveyed one by one. This is the gap options B
  and C (section 7) would serve.

## 5. Why not an INF or a HID filter

- **An INF only**: the one in-box mapper an INF could bind the collection
  to, `kbdhid.vxd`, mapped none of the consumer usages on 98 SE (section
  2.1). No other 98 SE component was seen to consume them.
- **A HID filter or minidriver** that rewrites the consumer usages into
  the media scan codes or keyboard usages: 98 SE turned the media scan
  codes into virtual key FF, which nothing acted on (section 2.1, the PS/2
  reading). A filter could instead emit some other key a given program
  already knows (say a player's own shortcut), but that is a per-program
  mapping chosen in the kernel, under every keyboard, with a report
  descriptor rewrite, and it fires whatever window has the focus. Not
  impossible; the wrong place for it.
- **A VxD calling VKD services** to inject keys: the same choices as the
  filter, in a VxD.

What is missing is a user-mode consumer that decides per key what to do,
which is what Logitech's iTouch and Microsoft's IntelliType were on 98.

## 6. A user-mode helper for 98 SE (options B and C)

If the owner wants the gap closed, the smallest piece that does it:

- **What**: `MKEY98.EXE`, a Win32 program with a hidden window (for
  device notifications) and its message loop, started at logon, 98 SE
  only (it exits on any other system). AGENTS.md allows user-mode code
  "to fill a gap Windows leaves".
- **Input**: every HID collection with usage page 0x0C, usage 0x01,
  found through SetupAPI (`HidD_GetHidGuid`, `SetupDiGetClassDevs`) and
  opened for shared reading; one thread per collection with blocking
  `ReadFile` (98 SE completed no overlapped read, section 2.1); the
  reports decoded with `HidP_GetUsages` (button arrays and bitmaps) from
  the preparsed data, per report ID, so the array, bitmap and report-ID
  layouts all decode the same way. The `KEYTEST /hid` reading shows the
  open and the reads; it does not decode.
- **Key transitions**: the helper keeps, per collection and report ID,
  the set of usages down in the last report of that ID. A usage present
  now and absent before is a press; absent now and present before is a
  release; a report of one ID changes nothing for another ID; a report
  repeated unchanged (an idle resend) is nothing. An action fires on the
  press only, never on the release, and launches do not auto-repeat.
- **Arrival and removal**: the hidden window registers for HID interface
  arrivals (`RegisterDeviceNotification` with the HID class GUID; where
  98 SE gives only the broadcast `WM_DEVICECHANGE`, that) and on each
  arrival rescans, keeping a table of open collections by interface path
  so an open one is not opened twice. A reader whose `ReadFile` fails
  (the device left) closes its handle, removes its table entry under a
  lock, and then posts a rescan request to the hidden window before it
  ends. A re-plug whose arrival rescan ran while the old entry was still
  there is therefore picked up by that second rescan; a periodic rescan
  (every few seconds while any reader ended in the last minute) covers a
  notification that never came.
- **Volume Up, Down, Mute**: left to `hidserv.exe`; the helper does not
  touch them, so a key never steps twice.
- **Option B (actions only)**, each resolved when the key is pressed,
  with a fixed fallback, through `ShellExecute` or `CreateProcess`:
  - AL Calculator: `calc.exe` from the Windows folder.
  - AL Local Machine Browser: `explorer.exe` with the My Computer folder
    (`::{20D04FE0-3AEA-1069-A2D8-08002B30309D}`).
  - AC Home: the default browser's start page: the command of the
    `http` protocol's `shell\open\command` with its argument removed if
    it runs a known browser that opens its home page with no argument
    (Internet Explorer: `iexplore.exe` from `App Paths`); otherwise
    `iexplore.exe` itself. DDE-only associations are not followed.
  - AC Search: the shell's Find Files (`ShellExecute` of the `find`
    verb on the desktop folder).
  - AL Email Reader: the command under `HKLM\Software\Clients\Mail\<the
    default client>\shell\open\command`; if absent, nothing.
  - AL Consumer Control Configuration: `mplayer2.exe` from `App Paths`
    (Windows Media Player 6.4 on 98 SE), or the newer player's `App
    Paths` entry when present.
  The exact resolution is part of the owner's Q2; the readings show only
  which programs ME and XP open, not how they look them up.
  - Media transport and browser navigation keys: nothing in B.
- **Option C (B plus WM_APPCOMMAND)**: for Play/Pause, Next, Previous,
  Stop and the browser navigation keys, the helper sends WM_APPCOMMAND
  (the APPCOMMAND_* value, FAPPCOMMAND_OEM) to the foreground window, as
  Microsoft's WM_APPCOMMAND page says software did "on Windows systems
  earlier than Windows 2000". Players and browsers that handle
  WM_APPCOMMAND then act on them on 98 SE; ones that do not, do not (the
  same as on 2000 and XP). Which period players handle it on 98 SE is to
  be read in the legs (Windows Media Player 6.4 of 98 SE is expected not
  to). The per-player commands Logitech's iTouch used are not copied.
- **Install**: an INF with a `[DefaultInstall]` section (right-click,
  Install) that copies `MKEY98.EXE` to the system folder and adds it to
  `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`; an uninstall
  section that removes both. It binds no device: hidserv keeps its INF
  binding, and nothing changes under the keyboard. The registry value it
  writes is its own Run entry only (no tuning value read by a driver).
- **With iTouch or IntelliType installed**: those programs read the same
  collection for their own keyboards; both would act. The helper skips
  collections whose device id is in a small list (Logitech's 046D ids the
  iTouch INF lists, Microsoft's 045E keyboards), or the owner prefers a
  documented "do not install both" (Q4).
- **Size**: one C file of a few hundred lines, MSVC 6.0 as `KEYTEST.EXE`
  is built, no kernel code, no new driver import.
- **Legs**: 98 SE under xHCI98 and under NUSB's EHCI, each simulator
  layout (the bitmap profile given the keys it lacks), `KEYTEST.EXE`
  reading the WM_APPCOMMANDs and the launched windows, a held key and an
  idle resend firing once, two keys of different report IDs, hidserv's
  volume steps unchanged, a hot plug after logon, an unplug and re-plug,
  two keyboards at once, the helper on ME/2000/XP exiting at once.

## 7. Options for the owner

**Decision (owner, 2026-11-06): A.** B, C and D below stay as designs; none
is built. The release-notes sentence, a draft for the owner's wording,
for usb-wdm-free's release notes (and xHCI98's):

> On Windows 98 SE, a keyboard's volume and mute keys work after a
> restart; its other media keys need the keyboard maker's software. ME,
> 2000 and XP handle them.

The options as put:

- **A. No code: document it.** The sentence above in the release notes.
  Cost: text only.
- **B. A plus the helper with direct actions only** (Calculator, My
  Computer, browser home, Search, Mail, media player). Small; the launch
  keys of keyboards of the shapes read work on 98 SE; the transport keys
  still do nothing.
- **C. B plus WM_APPCOMMAND** for the transport and browser keys, so
  that players and browsers that handle the message work on 98 SE as on
  2000. A few lines more than B; its value depends on which 98-era
  players handle WM_APPCOMMAND (read in the legs).
- **D. A kernel HID filter or an INF** (the task's first idea): not
  recommended. The INF bindings and scan codes read gave nothing on
  98 SE; a filter would have to pick other keys per program, in the
  kernel, under every keyboard (section 5).

**Recommendation: A now, for every system; C if the owner wants the 98 SE
gap closed for keyboards no maker covers.** A costs nothing and is true
today; of the options that serve the transport keys (C, or D's per-program
keys), C is the general one, using the message Windows defines for them,
and it is user-mode code outside the keyboard's driver stack.

## 8. Owner questions

Answered 2026-11-06: Q1 A (so Q2 and Q4 do not arise); Q3 yes, iTouch is
in usb-wdm-nonfree's "Not shipped" list.

- **Q1.** A, B, C or D? Recommended: A, then C.
- **Q2.** (B/C) The launch targets of section 6, and whether AC Home
  opens the default browser or always Internet Explorer. Recommended: the
  default browser.
- **Q3.** Logitech iTouch 2.22 (98 SE to XP, Logitech keyboards): no
  terms readable on Logitech's page or in the package's README. Leave it
  out of usb-wdm-nonfree, listed as "Not shipped" with Logitech's link?
  Recommended: yes.
- **Q4.** (B/C) Skip collections of Logitech and Microsoft keyboards
  (their makers' software may be installed), or act on every keyboard
  and document "do not install both"? Recommended: act on every keyboard
  and document it (a skip list cannot tell whether the maker's software
  is installed).

## 9. Not covered

The readings ran on QEMU's xHCI with simulated keyboards and QEMU's USB
audio device; a real keyboard with media keys on 98 SE and ME is the
bench's. NUSB's EHCI and Windows' own UHCI were not read: the gap is
above the HID class driver, so the host controller driver does not
change it, but a helper's legs (section 6) run under both host
controller drivers, as AGENTS.md requires.
