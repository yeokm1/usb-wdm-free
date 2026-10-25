# Handoff - usb-wdm-free, 2026-10-21

Written for: whoever picks this work up next - any coding agent or a
human developer. No particular tool is assumed.

Read first: `AGENTS.md` (binding rules), `README.md` (status, wave order,
"Tested by the author" table, layout), then the design docs in
`docs/design/`. Every owner decision is recorded in the docs as
"Owner, 2026-10-20: ...". Nothing here is pushed; there is no remote yet.

## Update 2026-10-22 afternoon (read first, then the resume point below)

- UVC M2 item (c) committed (b5251f5): formats from the descriptors;
  160x120, 176x144, 320x240 PASS on 2000/EHCI with the Brio 100. Run 9's
  failure was a stop whose alternate-0 select failed. 640x480 fails on the
  host side of the passthrough (every high-bandwidth packet
  ISO_NOT_ACCESSED_BY_HW), so 2000's own high-bandwidth support is still
  unverified (needs a real PC). Record: docs/testing/2026-10-22-uvc-m2.md.
- usbmidi 98 SE leg run (e4fd9e4): PASS after fixing a fault in the stop
  path (thread-object wait). Open: MIDI input after a new-port replug,
  an intermittent 98 SE guest hang. 2000 legs owe a rerun with the change.
- docs/testing/hcd-matrix.md: legs per host controller driver. No driver
  has an xHCI98 leg yet.

## Resume point, wind-down 2026-10-22 ~08:00 (read this first)

Branch `working` (local only, never pushed). No QEMU guest or build of this
agent is running.

- **UAC2 (X4), item (b), measured in quiet slot #4 (2026-10-22 09:16-09:30).**
  - e1 (strict 2 s) VM-rule gate **PASS**. e2 (30 s soak) **FAIL**: 64
    raw OUT packet errors and OUT coverage (host got 5,746,164 B against
    5,760,000 B).
  - Three counts, e2: guest CompletedBytes 5,763,064; EHCI-executed ok
    5,746,548 B (239,248 packets, fail 0); ring-accepted 5,746,548 B
    (refused 0). e1: 387,068 / 385,724 / 385,724. EHCI-executed equals
    ring-accepted exactly, and every EHCI skip event comes after the
    stream.
  - So the in-stream loss (16,516 B in e2, 1,344 B in e1) is **on the
    guest side, before EHCI executes**: iTD transactions the guest counted
    as completed but the controller never ran. It is not EHCI emulation
    and not QEMU's ring. The stop discard was only 8/16 packets.
  - uac2wdm's CompletedBytes adds whole-URB `slot->Bytes`, so the 144
    PacketErrors (~3.5 KB) are inside the gap. About 540 packets remain
    unexplained.
  - Next: a uac2wdm diagnostic build counting IsoPacket[].Status by value
    and summing success-only lengths, then a quiet slot to rerun e2.
  - HOSTPOL 0/0 -> HONOR 4/0 -> restored 0/0, readback matched, exit 0,
    in all three windows (one e1 window was aborted before play by a
    script bug and redone).
  - Evidence: `E:\usb-wdm-free\evidence\w7-e1\`, `w7-e2\`. Details:
    uac2-vm.md, "Quiet slot #4".
- **UVC M2, item (c), in progress and UNCOMMITTED.**
  - Done in code:
    - M2 builds its format table from the camera's descriptors at
      bootstrap (`uvcfmt.c/.h`, YUY2 frames up to 640x480).
    - GET_STREAM_INFO advertises them; DATA_INTERSECTION and OPEN_STREAM
      select one; SET_DATA_FORMAT accepts only the format already open.
    - The port's READ size follows the selection (`UVPORT.FrameBytes`,
      `UvPortMapBytes`).
    - The transport gained `UvuStartFrame`, `UvuDescriptors` and per-stream
      buffers freed at every stop, so the device stays open across sessions.
    - M1CAP takes `ID WxH`, lists every advertised range and connects with
      the driver's own intersection.
  - Builds and host tests pass (M1, M2 free and checked; worktest, m1test,
    porttest).
  - Guest run 8 (Windows 2000, Brio 100, evidence
    `E:\usb-wdm-free\evidence\w2k-m2\run8\`):
    - 9 YUY2 formats advertised, from 640x480 down to 160x120.
    - 640x480 opened, but its alternate 10 (3 x 896, high bandwidth)
      stalled after 48 packets; no frame. This is the known
      high-bandwidth stall on 2000, and the Brio quirk could not avoid it
      because 640x480 at 30 fps needs about 2.3 KB per microframe.
    - Clean detach and safe-off.
  - Next:
    - **Done 08:00 (run 9, driver A5CAB5CB, M1CAP 075EC184): 320x240 and
      160x120 both FAILED** with no frame. Evidence:
      `E:\usb-wdm-free\evidence\w2k-m2\run9\`. Clean detach and safe-off.
      - 320x240: alternate 4, payload 640, 95 frames dropped.
      - 160x120: alternate 1, payload 192, but the transport's FrameBytes was
        153600, so the PROBE answer resolved to another frame.
      - Hypothesis, unproven: the camera keeps its previously committed
        frame (for example 640x480 from run 8), or the answer's bFrameIndex
        differs from the request.
      - Item (c) stays UNCOMMITTED; it regresses the fixed-320x240 path that
        passed in runs 5-7.
      - First step: log the PROBE answer's bFormatIndex/bFrameIndex and each
        dropped frame's size.
    - decide how to handle sizes whose default interval needs a
      high-bandwidth alternate on 2000 (choose a longer listed interval, or
      leave them out);
    - then commit (src/video/uvc98: m1cap.c, m2build.cmd, uvcm1.c,
      uvcport.c/.h, uvcusb.c/.h, uvcwork.c/.h, new uvcfmt.c/.h).
  - The M2 cycle helper is `E:\usb-wdm-free\wave1\m2run.sh` (boot, install,
    attach, `cap TAG WxH`, detach, off).
- **Then (d):** Windows 98 SE for UAC2. Later: MJPEG and bulk cameras, and
  the stock-Scoop-QEMU question for M2.
- **Package agents (wound down, committed on `working`):**
  - **cdcnet** 0.1.0.0 (a63cff0): new ECM/NCM/RNDIS NDIS 5 driver.
    - Passes in QEMU (usb-net on UHCI: DHCP, ICMP and TFTP, removal and
      replug) on 2000 (ECM, RNDIS; the free binary also under Verifier),
      98 SE (ECM, RNDIS), XP (ECM) and 7 x64 (ECM).
    - CDCRNDIS.INF binds Windows' own RNDIS driver, but QEMU's model
      blocks it from starting (Code 10) on XP and 7 x64.
    - Owed: NCM, EHCI, tethering and sleep on hardware, x64 signing. ME,
      XP x64, Vista and 7 x86 are built only.
    - Record: docs/testing/2026-10-22-cdcnet.md.
  - **usbmidi** M1 (b9362c7; run record and design update committed in
    ca6cade): generic driver, one x86 binary on the 98 SE import set, plus
    an x64 build.
    - The 2000 UHCI simulator legs pass with the traced checked build:
      both ports, unplug with ports open, replug, restart and shutdown.
    - Owed: 98 SE, EHCI, XP, 7 x64.
    - Known: a new port instance on 2000 asks for a restart.
- **Open owner questions:**
  - `release/` vs `free/` layout;
  - when to copy the x64 test-signing tooling from xHCI98;
  - cdcnet's shared core into src/net/common (recommended: when rtl815x or
    ax88179 needs it);
  - accept usbmidi's restart on new instances for M1 (recommended);
  - how the X4 soak's data loss should be fixed once it is located;
  - the M2 high-bandwidth formats on 2000;
  - when a driver is release-ready, whether xHCI98's installer may offer
    usb-wdm-free, which conflicts with the repo-independence rule.

## Earlier checkpoint: STOPPED at owner's request, 2026-10-21

**Read this checkpoint before the chronological history below. It supersedes
older active-VM, pending-build and uninstalled-driver statements.** The owner
asked to stop and save this handoff for another session. All agents stopped;
do not continue this session automatically.

### Resumed by the owner, 2026-10-21 ~23:35 (latest; read first)

- The owner authorized the resume. Other agents now work in this tree on
  usbmidi, cdcnet, usbcom and xpad98, each in its own package and test log.
  This agent owns README.md, handoff.md, NOTICE.md, .gitignore,
  docs/toolchain.md and src/audio/common, and keeps UAC2 and then UVC.
  Owner rule: all scratch files, VM disks and large logs go on E:\usb-wdm-free.
- The prepared telemetry run was taken twice **on a loaded host** (owner: "try
  first"; other TCG guests and builds were running, recorded). wait1 and wait2:
  8/8 buffers and 96000 samples completed, QPC 2018/2008 ms. Raw host OUT
  had 502/499 callbacks with zero errors, the host window was 2013/2003 ms and
  the largest OUT gap 19 ms. No main-loop stall of 50 ms or more fell inside
  the ISO window. The strict gate still FAILs. The main reason is
  GetTick 1375/1312 ms. The rest is known feedback trace-accounting and, in
  wait2, OUT coverage 15968 < 16000. An idle TIMERTST control in the same guest
  (before, during HONOR and after playback) shows GetTick tracking QPC, so the
  shortfall occurs only during ISO playback. The likely cause is lost guest
  ticks under emulation load, which is unproven. A VM gate rule is proposed,
  not applied, and needs an owner decision. HOSTPOL was restored to 0/0 and
  read back on the live target both times. Details and evidence paths:
  docs/testing/2026-10-21-uac2-vm.md, last section;
  E:\usb-wdm-free\evidence\w7-wait{1,2}\.
- Next for audio: a quiet-slot retake (the coordinator arranges it). Add a
  driver-independent USB-load control with a concurrent TIMERTST. Look into
  the guest-side PacketErrors/BadFeedback that occur while raw host packets
  are clean (QEMU EHCI/ISO emulation suspected). The CLKSER serial clock
  probe remains designed only.
- Owner rule (2026-10-22): no programs on the host. That means no native
  probes, no HOSTPOL or other host policy/timer changes, and no host-side
  X4 access. Build toolchains, host unit tests, git and headless QEMU guests
  remain allowed. Owner decision: keep the strict gate. A quiet-slot retake
  comes first, and the proposed VM clock rule is not applied.
- Webcam: the M1CAP pin 87 was the checker's KSPRIORITY subclass 0. Pin 0's
  interface, medium and data range all match, and DATAINTERSECTION returns
  exactly the checker's format. With subclass 1, Win2000 guest run w2kuvck
  passed three changing synthetic frames, RUN/STOP, close and then removal
  (0x10E/0x109 SUCCESS) through SYS C0799AEA. That is synthetic frames only:
  no USB or UVC payload. Details: docs/testing/2026-10-21-uvc-worker.md, last
  section. Next: connect the worker to real UVC payload parsing over USB
  (uvcrun/uvcpay), then the Brio 100 in a guest, then 98 SE. The first
  transport pieces exist in `uvciso.c`: the isochronous-IN packet walker
  that feeds the payload core, and `UvcIsoPlan`, which gives the transfer
  shape of uvc.md section 5.4. `isotest.cmd` passes 2486164 host checks,
  including all 11 alternates of the Brio 100 fixture. Neither is wired to
  USB yet. `uvcneg.c` (PROBE/COMMIT sequence through one class-request
  callback) passes `negtest.cmd`: 3228 checks (with payload-limit cases), with 228 good negotiations
  over the Brio 100 fixture and 7 misbehaviours that never commit.
  **M2 works (2026-10-22, Windows 2000 TCG guest, Brio 100 passed through
  on emulated EHCI).**
  - `uvcusb.c` plus the M1 worker built with `UVWORK_USB`
    (`m2build.cmd`, `UVCM2.INF` per-camera, `m2gate.ps1`) delivered three
    complete, changing 320x240 YUY2 camera frames to M1CAP (PASS). They
    were saved and converted with `yuy2png.py`; the image is dark grey
    noise, the same as the native Windows 7 capture, so no scene claim.
  - Removal and shutdown are clean.
  - Fixes found on the way:
    - A per-camera bandwidth quirk: the Brio answers payload 3060, and the
      high-bandwidth stream stalls on 2000.
    - ABORT_PIPE before draining on removal; 2000 never completes cancelled
      iso URBs before REMOVE.
    - Stop after 8 consecutive failed transfers.
  - Run log: docs/testing/2026-10-22-uvc-m2.md. Evidence:
    `E:\usb-wdm-free\evidence\w2k-m2\`.
  - Next for UVC:
    - stop the camera on pin close and power-down (it now runs until
      UNINITIALIZE);
    - format and size selection beyond the fixed 320x240 YUY2;
    - MJPEG, bulk cameras and controls;
    - a 98 SE port of the transport (no IoAllocateWorkItem or
      PsCreateSystemThread assumptions);
    - a test under the stock QEMU; frames never completed there, and the
      cause is not isolated. The uvc98 and uac2 test scripts now run their EXE by
  explicit path (`.\NAME.EXE`); all ten pass.
- **X4 quiet-slot retake done (2026-10-22, about 00:30-00:45).**
  - q2 (host CPU 0%): the strict player's **first guest PASS**. 8/8
    buffers, 96000 samples, GetTick 1594 / QPC 2016 ms. Host transport was
    clean: OUT 502 callbacks/16064 packets, 0 raw errors, a 2013 ms window,
    max gap 17 ms. The combined gate still FAILs, only on the known
    feedback trace-accounting gap (the orphan-free branch at stop drops 10
    feedback callbacks from the normal trace).
  - q1 (host CPU about 51%, from something non-QEMU): FAILed on GetTick at
    1360 ms, with transport just as clean.
  - HOSTPOL was restored to 0/0 with readback, on the live target, both times.
  - The trace gap is fixed, observation only: `qemu-orphan.patch` and QEMU
    2EDD5707 add an orphan-completion trace event, and the parser
    attributes each orphan record only through its own submit record.
    Checked by the harness and by a mock on q2 data, which gives a gate
    PASS. It is now confirmed physically: r1, r2 and s1 have complete host
    accounting.
  - **Quiet slot #2 done (2026-10-22 ~01:05-01:20).** r1 and r2 (strict
    2 s) FAILed only on GetTick (1359 and 1422 ms, against QPC 2019 ms).
    s1 (30 s soak) completed 120/120 buffers but FAILed on GetTick
    (22094 ms against QPC 30006 ms) and on OUT coverage: about 18.6 KB
    (~97 ms) never reached the host, with 162 guest PacketErrors and all
    raw host packets successful.
  - Open items:
    - (a) **Answered by CLKSER (quiet slot #3), and owner rule applied
      (2026-10-22 ~07:00).** Guest QPC is exact against the host clock.
      Guest GetTick runs at about 0.70 only while ISO playback runs. In a
      VM the gate (`x4-evidence.py --vm`) now judges duration by QPC
      +/-500 ms plus host-window agreement within 100 ms; GetTick is a
      diagnostic there. On physical systems GetTick stays a hard check.
      Re-judged results: **r1 and r2 PASS** (the first complete gate
      passes). q2 fails only on the old trace gap. s1, c1 and c2 fail on
      real data loss: c1 lost its last 24 packets inside QEMU's OUT ring
      at stop, and c2 also had raw OUT errors.
    - (b) Soak data loss, measured by the accounting QEMU (31905625):
      about 11.9 KB is lost before QEMU's OUT ring (EHCI emulation or
      guest schedule) and about 5 KB is the stop-time discard. EHCI skip
      events fall after the stream. Ring restarts: 454. The next step is
      guest-side iTD/packet-status evidence. 
    - CLKSER (`clkser.c`/`clkser.py`) and launch-clk.ps1 are the tools for
      both. Then 98 SE. See the uac2-vm.md sections "Quiet slot #2" and
      "Quiet slot #3".
  - Details: docs/testing/2026-10-21-uac2-vm.md, "Quiet-slot retake q1/q2".
    The driver script is `E:\usb-wdm-free\wave1\x4run.py`. Its post phase
    used to overwrite the probe's fresh UACLOG with a stale copy from
    C:\UACTEST; that is fixed. The conc1 and q1 stats were corrected from the
    probe screenshots.
- **Base images moved (2026-10-22, approved by the owner):** all nine base
  images (33,923,661,824 bytes) now live in `E:\usb-wdm-free\vm\`.
  - The E: copies were SHA-256 verified against the originals.
  - All 42 overlays were rebased metadata-only, with every usb-wdm-free
    guest down. All passed `qemu-img check` and `--backing-chain`, and one
    per base compared identical against its old chain.
  - The `vm/` originals in this repo were deleted. Records are in
    `E:\usb-wdm-free\vm\` and docs/toolchain.md.
  - New overlays must back onto `E:/usb-wdm-free/vm/<image>`.
- Guest driving: the w7 and w2k guest keyboards are US Dvorak. Use
  E:\usb-wdm-free\wave1\hmp.py --type (it maps the layout). Dismiss the w7
  activation dialog with Tab, Enter, then alt-s (Dvorak "o", the OK
  button). Plain Enter on the second page can open IE. Open an elevated cmd with
  ctrl-esc, "cmd", ctrl-shift-Enter, then alt-t (Yes). Absolute mouse clicks
  are unreliable.

### Clean shutdown and standing instructions

- Root independently confirmed no `qemu-system-*` or `HOSTPOL.EXE` process
  and no listeners on44333/44338 after cleanup. Last webcam guest47076
  reached "safe to turn off" before HMP quit. No test VM remains open.
- X4 is released to host libusbK, StatusOK, oem79.inf. Every temporary
  HOSTPOL change was restored/read back to original0/0 while its QEMU was
  still alive, with helper exit0 before normal guest shutdown. Native host
  probes also drained and restored their own device/timer/policy state.
- On a later user-authorized resume: prioritize reliable UAC2 playback,
  then webcam and remaining drivers. Multiple agents are authorized. Use
  headless QEMU and E:\usb-wdm-free for VM overlays/scratch; close guests
  after testing. Give physical audio measurements an exclusive quiet slot;
  no competing VM boots or heavy builds during measurement.
- Full standardized UAC2 and UVC through1.5 remain the requested scope.
  Portable parser/control tests, native Windows drivers and synthetic video
  tests are not completed project drivers. Final proof screenshots should
  be in Windows98SE. No final driver release is ready or claimed.
- Final-only release request remains version0.0.0.1 with actual final
  compilation date and complete driver/OS packages under release/. Reconcile
  that later owner instruction with the older free/ layout documentation
  before packaging. No release, commit, tag or push was made here.
- Keep the large existing worktree intact. Respect AGENTS.md source rules:
  public specs/DDI/headers and compatible sources only, no kit sample sources
  or Microsoft/vendor binary code/disassembly. References are ignored under
  docs/references/. Consult the earlier recorded accidental sample-header
  search exposure; do not misdescribe that historical search as clean.

### UAC2: verified native baseline, guest still fails

1. **Native physical X4 playback passed twice without a VM.** The original
   src/audio/uac2/natprobe.py uses the actual uacrate.c core, pinned libusb
   and libusbK, and explicit cancel/drain/restore gates. Both runs completed
   384000 requested bytes in2005.171/2004.953ms, with1984 valid feedback
   values and zero API packet errors. Native2 full raw records independently
   parse as500 OUT callbacks/16000 packets and64 feedback callbacks/2048
   packet slots, all raw packet/URB errors zero. API feedback includes62
   successful plus2 canceled transfers; do not call2048 slots2048 valid
   feedback values. No audible/fidelity claim; Windows OUT lengths are
   synthesized requested lengths. See docs/testing/2026-10-21-uac2-native.md
   and E:\usb-wdm-free\wave2\uac-native\native{1,2}*.
2. **Guest playback is not reliable.** Latest w7-clockmono-* run installed
   the monotonic-only diagnostic SYS35F039...F912, but failed8 submitted/
   0 completed/6240 samples, GetTick109ms/QPC11586ms. Stop stats had
   LastStatus0, PacketErrors0,37 data/19 feedback URBs,96 good/40 bad
   feedback. The clock change is a valid source correction, not a proven
   playback fix. Earlier paired baseline1D9D runs sometimes reported
   IO_TIMEOUT; that status alone cannot identify the watchdog versus other
   completion/control paths. Combined stream-cleanup fix remains uninstalled.
3. Phase helper UACPHAS shows all eight waveOutWrite calls returning within
   cumulative5QPCms on this Win7 shared path. Therefore synchronous write
   startup blocking was NOT observed. Do not loosen gates based on that
   rejected hypothesis. Guest clock accuracy versus early transport stop
   remains unresolved: clockmono's first host ISO was~2.257s after HMP Enter,
   last OUT~2.427s, while guest QPC reported11.586s. HMP Enter is not a
   helper-finish marker; transport could stop before a later client timeout.
4. Host timer experiment showed hidden-process waits~15.7ms despite a
   successful period request; explicit process-local honor reduced them to
   ~1-2ms. QEMU already requests its minimum period in os-win32.c. HOSTPOL
   changes only the verified target's timer-ignore policy bit, preserves
   other bits and restores/readbacks before guest exit. Honor alone did not
   make guest playback pass. See2026-10-21-host-waits.md.

**Next audio action is prepared, not run:** use
E:\usb-wdm-free\host-qemu\launch-wait.ps1 and immutable qemu-system-wait.exe
with QEMU_HOST_WAIT_DIAG=1 and LIBUSB_DEBUG=2 set in the PARENT PowerShell
before launch. Python os.environ alone did not enable the DLL's raw logger
in native1; parent environment fixed native2. Retain the latest failed
controls: w7clock overlay, installed mono35F039, PHAS8E708, HONOR4/0,
DLL622FC, lean tracing, isobufs16, player/scripts/reports on guest C:\UACTEST,
and A: ejected throughout playback. Change only QEMU observability.

The memory candidate records slow main-loop/control/libusb stages in8192
bounded memory slots and dumps at exit; no timed-run log writes are added.
Full QEMU build, actual-core24672 mock checks and existing ISO/poll source
regressions pass. It has NOT been physically run. Correlate startup/config/
alt, host poll, replay/BQL acquisition, GLib/libusb, notifier and timer
intervals with ISO and host Enter timestamps. Intervals can overlap and are
not additive; idle waits may legitimately be long; the50ms threshold cannot
measure16ms granularity. Overflow/backsteps/incomplete dump limit evidence;
forced kill may lose the dump. Restore HOSTPOL before closing normally.

Key prepared identities (verify complete hashes against manifests):

| Artifact | SHA256 |
|---|---|
| New memory-telemetry QEMU, not physically run | 28A2BDF5CDD806F71531AB1457C3C7B6C7160663E0AE6AF63D3AC14DF9025641 |
| Prior tested poll QEMU | 4BCC83D69230267CE44E3C3C2BB17313437B686B84B15C26A560E68CE9F572A7 |
| Corrected raw-status libusb DLL | 622FC378B496A5F348F06DF94DB07E9BA8CE21C63E8C303CC49A9B16EBD1F29B |
| Loaded host libusbK.dll | 0690CD87D0069EDAC3A867E0B51F379AFA77159EDA3B45CD02740FC6AFB546D8 |
| Current w7clock installed monotonic diagnostic SYS | 35F03961554237CC5C89D8C31A972C991DAE9A21D02960D722E4339D86DBF912 |
| Paired adjustable-clock baseline SYS | 1D9D97097AB1D7DB1C541A2D854403AC66420E5B0AF2942362E40F3F07047306 |
| Preserved original C7 SYS/backing w7uac image | C7A172067C3D9B540C84D965664AB9BDCA90AC435A1374B0759EF85D7E61126E |
| Separate UACPHAS.EXE on E:/usb-wdm-free/wave2/uac-phase | 8E70826CA5ED5F6382D9565C3130215B26E6CCBCC67AC7730E4C847CA3C12F8D |

Authoritative details: docs/testing/2026-10-21-uac2-vm.md,
2026-10-21-uac2-guest-audit.md,2026-10-21-uac2-play-verifier.md,
out/host/qemu-wait-manifest.json and out/audio/uac2/w7-clockmono-*.
The clock pair uses retained D201 objects; it is not a literal C7 relink.
qemu-wait.patch applies after qemu-iso.patch+qemu-poll.patch; notices updated.
Current player mock tests pass2520770 legacy/3097004 strict/3337145 phases/
46112844 soak checks. The separate30s helper and --soak evidence gate are
prepared but no physical30s PASS exists. No ordinary player binary was
overwritten by the separate phase build.

### Webcam: synthetic startup/removal pass, no project capture yet

- Latest ROOT\UVCSIM1 Win2000 noUSB guest47076 started with CMproblem0,
  NewDev success/no reboot. Idle targeted removal passed (root absent1168),
  with own trace SURPRISE_REMOVAL0x10E and UNINITIALIZE0x109 completing
  SUCCESS. This verifies idle lifecycle only, not streaming rundown.
- M1CAP KsCreatePin returned Win32 error87 BEFORE any SRB_OPEN_STREAM,
  RUN or READ reached the miniport. No frame or physical-camera PASS.
  Next investigate/query m1cap.c connection interface/medium/format parameters
  through public KS APIs before blaming the worker frame path.
- Two source bugs were fixed: pending INIT could not depend on the initial
  class timer, now using documented LOW->LowToHigh handoff with tested
  cancellation/join ownership; and M1 zeroed ActualBytesTransferred before
  reading the aliased NumberOfBytesToTransfer size field. Native incoming
  value was0x78, NOT zero. The fix tracks the buffer budget registered at
  INIT rather than reading the clobbered union. M0 captured input before
  zeroing and remains unchanged. Same-owned INIT duplicates cannot be
  completed twice; different duplicate inputs are rejected before inspection.
- Latest host gates: worktest733, m1test152, free/checked/VM-trace native
  import gates and amd64 object compilation pass. Latest tested VM-trace SYS:
  C0799AEAB343F27A4EAE117A6583A3C29387FFF2D68B2DB856A36127817C5EAA.
- Sources: src/video/uvc98/uvcm1.c,uvcwork.c,uvcport*.c,m1cap.c.
  Evidence/source snapshot: out/video/uvc98/m1-native/ and budget-src/ there;
  overlay/exchange E:\usb-wdm-free\wave2\w2kuvcg*. The worker run doc and
  proof index still need the latest startup/idle-removal PASS and pin87
  negative appended; agent stopped before editing those after owner stop.
  Prior Brio native usbvideo capture is reference evidence, not our driver.

### Other devices and remaining preparation

- Floppy physical reads passed in98SE with three matching file CRCs and
  screenshot proof; no write/format test. See2026-10-21-usbflop-media.md.
- AX88179 physical host DHCP/ARP/ICMP passed, not guest NDIS/98 validation.
- UM-ONE mk2 TAB project Win7 startup/open/close passed; no DIN loopback is
  connected, so MIDI data roundtrip remains untested.98 baseline failed
  before project installation. See2026-10-21-umone2.md.
- CDCnet/serial simulated tests have earlier OS-specific results and known
  gaps; see their run logs. Do not promote host/simulator evidence to all-OS
  or98 proof. Logitech headset disconnected; Brio100/AX/UM-ONE WinUSB,
  X4 libusbK, Mitsumi WinUSB with disk inserted per last owner report.
- Full UAC2 controls/mixer portable code and tests exist but are not runtime
  integration. Feature ledgers remain in docs/design/; all-spec work remains.
- Independent guest-clock calibration CLKSER was only DESIGNED: no source,
  receiver, binary or VM run was created. If needed later, build an original
  x86 COM1 BEGIN/sample/END clock probe plus host TCP serial receiver with
  host monotonic arrival timestamps, noUSB/separate overlay. Test public
  Win32 UART configuration/restoration, framing and frozen-clock watchdog
  before use; distinguish END measurement from cleanup/guest exit.

## Historical continuation, 2026-10-21: physical devices and full feature scope

This section supersedes older milestones below; use each run log's exact
tested binary identity rather than assuming the latest build was installed.

- X4 now uses host libusbK. Two controlled physical Win7x86 runs completed
  8/8 buffers and 96000 samples, including reopen, but each had nine packet
  errors. A later strict traced run failed duration; QEMU reports actual
  output-buffer starvation. This is not reliable/fidelity validation.
  Audio agent owns 44333 and follow-up source/trace work; close its guest.
- Brio100 WinUSB physical native-Win7 capture produced increasing frame
  callbacks and successful stop/reopen; image gray/noisy. This is native
  usbvideo evidence, not our project capture driver. That guest was closed.
- Full standardized UAC2 and UVC<=1.5 remain requested. The new
  `docs/design/uac2-features.md` and `uvc-features.md` inventory missing
  work; earlier narrow exclusions no longer define completion. UVC typed
  1.5 probe-tail accessors pass 6089 codec checks and seven model tests.
- Owner connected AX88179 Ethernet to router and authorized autonomous
  testing. Host DHCP/ARP/verified ICMP exchange has passed; network agent
  is validating observed FCS handling and repeating. No NDIS guest claim.
- UM-ONE mk2 switch is TAB, confirmed by owner despite vendor descriptors.
  Fixture and bounded Roland profile recognizer added; portable 17980
  new checks, 203668 existing checks and eight model tests pass. Kernel
  alt0/profile path and exact VID/PID INF now build free/checked version3;
  physical Roland not tested. Latest root host inventory lacked Roland.
  Owner was advised to use WinUSB for Roland, keep X4 libusbK, Brio/AX
  WinUSB and headset native. No host binding change was performed by us.
- Root's fresh-base Windows2000 regression `w2kmidir3.qcow2` exited during
  version3 installation without a verdict; QEMU16176/model36196 are closed.
  Physical TAB0168 then passed **project version4 startup and open/close
  twice on Win7x86/UHCI**, no MIDI events sent/noDINloopback. Installed own
  SYS B34E7510...BEDF3 verified, CMproblem0, removal Add1/Remove1/Destroy1.
  `w7umtab.qcow2` had a WHPX host restart error with default IRQ handling;
  `kernel-irqchip=off` continued normally. QEMU43920/8924 both closed,
  44337/44412 released, Roland host WinUSB/statusOK. Full record:
  `docs/testing/2026-10-21-umone2.md`; proof image under
  `out/audio/usbmidi/physical/w7umtab2-project-proof-final.png`.
- Owner now asks proof screenshots per working device, Windows98SE enough.
  Existing Win7/host results are interim evidence, not requested98proof.
  An agent is auditing MIDI98 imports; another indexes actual screenshots.
- Owner reiterated full specification coverage beyond what X4 advertises,
  for both UAC2 and UVC. A C930 webcam has been ordered; verify its exact
  identity/UVC revision/capabilities from descriptors when it arrives.
  Additional portable UAC2 control/RANGE and UVC planar-format work is
  assigned separately from current tested hardware paths.
- Root is now running a Windows98SE native UM-ONE reference, monitor44337,
  `E:\usb-wdm-free\wave2\w98umtab2.qcow2` child of closed w98flop,
  QEMU6784 (defaultACPI/stdVGA/256MB/UHCI). Earlier fresh-base acpi-off
  attempt asked for baseline BIOS files and was closed untested (36552).
  Native USB composite installed; native USB audio is copying its normal
  files from existing `D:\isos\Win98SE.iso` (named in repo design docs).
  No project98MIDI binary is available yet; imported IoWorkItems and
  cleanup-image-lifetime are concrete blockers. The new umclean prototype
  remains unintegrated/research-guarded, not safe-to-install code.
- Source-search boundary correction: root accidentally searched the whole
  extracted98DDK for NTKERN/work-item keywords and surfaced only two
  `#ifdef NTKERN` lines in sample-header paths. No sample design/body was
  consulted or adapted. All further kit searches are restricted to INCS
  header directories, and this accidental exposure must not be described
  as a clean sample-free search. No implementation decision used those hits.
- No release, commit/tag, `free/` edits or author-Tested-table promotion.

## Continuation, 2026-10-21: wave 1 implementation

This section supersedes the older "no package driver code" status below.
See `docs/testing/2026-10-21-wave1.md` for commands, evidence and limitations.

- `src/audio/uac2/`: original descriptor/clock-path parser; guarded host
  tests; M0 PortCls wave/topology registration driver and INF. x86/x64,
  free/checked builds pass. No configuration selection, DMA implementation
  or streaming. The M0 driver requires a function `&MI_nn` PDO and now
  handles XP USBCCGP's filtered configuration without an IAD. Passive X4
  startup reaches stage 11 with problem 0, but WinMM reports no playback
  device. One earlier combined installation/probe produced a SysAudio
  STOP; the later isolated query did not reproduce it. Streaming remains
  unimplemented. See `docs/testing/2026-10-21-uac2-vm.md`.
- `src/storage/usbflop/`: original NT, 98 SE and ME CB/CBI experimental
  INFs. ME has a separate system-layout file-copy route; 98 SE requires
  preinstalled storage files. Additional results are recorded in
  `docs/testing/2026-10-21-usbflop-more.md`.
  Controlled ME first installation starts parent/child and copies its
  own cached system files. On 98 SE native binding worked, but immediate
  manual rebind to ours showed an unspecified error; after-reboot status
  remains untested. These results do not establish media I/O.
  `src/serial/usbcom/`: original NT CDC-ACM experimental INF. Both use
  `build.cmd inf free` (or `checked`); outputs under `out/`, never `free/`.
- Owner connected a Mitsumi `03EE:6901` USB floppy using Zadig/WinUSB.
  Read-only descriptor capture confirms Full Speed UFI/CBI (`08/04/00`);
  saved under `src/storage/usbflop/testdata/mitsumi/`.
  This model is already listed by Windows, so its automatic binding is
  a control, not evidence for an unlisted drive.
- The Windows 2000 floppy run is complete and its guest closed. Our CBI
  INF started the parent and in-box child; no-medium behavior and idle
  removal were recorded. A new-port reattach chose Microsoft's specific
  entry. See `docs/testing/2026-10-21-usbflop-vm.md`.
- Owner, 2026-10-21: new VM scratch storage belongs on `E:\`.
  Overlay: `E:\usb-wdm-free\wave1\w2kflop.qcow2`; base remains the repo's
  `vm/w2k.qcow2`. Guests run headless and must be closed when finished.
- A serial board is not available. An original Python usb-redir CDC-ACM
  model now supplies Windows 2000 guest serial tests: our INF bound
  usbser as COM3 and six generated echo lengths passed. Guest and model
  exited cleanly. See `docs/testing/2026-10-21-usbcdc-vm.md`; no physical
  board or EHCI result follows from this simulation.
- The real X4's CDC `MI_01` function also binds our INF to XP's usbser,
  allocates COM3 and passes open/GetCommState/close. No UART data or
  explicit line-control writes were performed on that physical function.
- Wave 2 `src/net/cdcnet/` has an original Windows 2000/QEMU ECM M0/M1
  prototype. M1 passed DHCP, three ICMP replies and a 32 KiB deterministic
  TCP receive, then repeated successfully after idle unplug/replug.
  Both TCP streams and six ICMP replies are verified in PCAP. The guest
  and peers exited. See `docs/testing/2026-10-21-cdcnet.md` and its M0 log.
- No release, catalog, signing/trust claim, `free/` files or README
  "Tested" cells were created. Next: finish the floppy system matrix,
  repair X4 endpoint exposure, exercise M0 in 98 SE, finish target-export
  validation, then implement a real WaveCyclic buffer and
  streaming. Simulated serial and Ethernet success are limited to their
  recorded Windows 2000 runs.
- End-of-batch cleanup: all QEMU guests and serial/network model peers
  exited. No QEMU process remains; all new overlays and exchange files
  are retained on E:. No further guest was left for the owner to close.

## Resumed experiments

- Latest owner steering: returns around 6:30 pm with a real USB webcam;
  continue other devices meanwhile. Owner also connected a real AX88179
  using Zadig and authorized QEMU passthrough, Linux reference sources and
  any useful device datasheets. Keep all downloaded references ignored.
  ASIX capture is `src/net/ax88179/testdata/ax179/`, VID/PID0B95:1790,
  actual SuperSpeed; no register/network/passthrough result yet.
- Webcam helper is named **CAMCHECK.EXE**, independent of xHCI98. Original
  AVICap/VfW preview utility builds and loads on Windows2000 and98SE;
  current revised SHA2b4cc420... fixes640x480 sizing and errorID0 handling.
  No camera/livefeed yet. Root owns monitor44336 for utility QA and closes
  each guest after test. See `docs/testing/2026-10-21-camcheck.md`.
- `src/video/uvc98/` now has original portable payload assembly (94104
  guarded host checks) and descriptor/probe codecs (6059 synthetic checks).
  These are not yet an installable webcam driver. UVC audit records limits.
- `src/audio/usbmidi/` now has an original MIDI1 codec/descriptor core
  (203668 checks), usbredir loopback model backed by the owned C codec DLL
  (8 host tests), and MIDITEST WinMM helper. Agentcdcnetvm owns headless
  Win7x86 simulation monitor44337/E:\usb-wdm-free\wave2\w7midi.qcow2;
  native installation succeeded, reboot stalls under investigation. No
  end-to-end pass or project kernel MIDI driver yet; close guest/model at end.
- Owner: keep going until UAC2 is working. Audio is the active priority;
  floppy and network follow-ups have finished and their guests are closed.
  XP is too slow for primary iteration; use Win7 x86 now, XP at final
  compatibility testing. The audio agent owns X4/monitor44333 and
  `E:\usb-wdm-free\wave1\w7uac.qcow2`.
- UAC2 M1 is being integrated, not yet a playback pass: `uacusb.c/.h`
  provide bounded X4 stereo16/48k clock/config/iso/feedback transport;
  `uacstream.h` provides a software cyclic ring and USB-completion-based
  positions/notifications; deferred IO work-item cleanup protects final
  release. This experiment adds NT2000+ cleanup imports and XP+ runtime
  speed-interface requirements; 98 SE/ME M1 is not claimed.
- `uacrate.c/.h` implement exact nominal cadence and bounded normative
  explicit feedback. `rtest.cmd` passes46067 checks, including guarded
  feedback inputs and44.1k/48k cadence. `play.cmd` builds `UACPLAY.EXE`,
  a quiet2-second real-buffer test requiring eight completions, monotonic
  near96000-sample position, real duration and clean close. USB counters
  must corroborate it; no audible or playback result yet.
- Win7 x86 M0 reaches NewStream during EndpointBuilder discovery and
  receives deliberate STATUS_NOT_SUPPORTED. This is direct evidence for
  the missing stopped-stream implementation blocking endpoint creation
  there. XP's earlier multimedia initialization issue remains separate.
- Win7 x86 M1 now reaches successful NewStream, exposes the X4 stereo
  playback endpoint, and passes format query/open in the diagnostic probe.
  The first explicit UACPLAY attempt returned MMSYSERR_ALLOCATED before
  buffer submission; no real playback pass yet. The audio agent is
  rebuilding with feedback/control fixes and testing after a clean reboot
  without the probe's preceding WinMM open. Preserve single-stream USB
  ownership while diagnosing this.
- Official UAC2 (September 2026), USB 2.0, audio formats/terminals and UVC
  1.1/1.5 references now live under ignored `docs/references/`. Pinned Linux
  v6.12 audio/UVC source subset is also retained there with original notices.
  Tracked provenance/audit records: `2026-10-21-uac2-spec.md`,
  `2026-10-21-uvc-spec.md`, `2026-10-21-linux-refs.md` in `docs/testing/`.
  Linux source was not incorporated by the download task. Spec review
  corrected legal unchanged-feedback ZLP handling and clock RANGE rules;
  descriptor boundary review is in progress. Final VM results must name
  the actual tested binary, because the first endpoint build predates fixes.
- The 98 SE floppy's unspecified live-rebind error cleared after clean
  reboot; project parent/child now load. A fresh USB arrival returns to
  the native specific Mitsumi match, so this is not persistence evidence
  for the class INF on a listed drive.
- `docs/testing/2026-10-21-exports.md` records permitted names/ordinals-only
  export reads from pristine 98 SE/ME PortCls and KS. Current M0 PortCls
  imports exist on both. `PcRegisterIoTimeout` is absent only on 98 SE;
  NTKERN imports and actual old-OS audio loading remain to check.

## Where things stand

Read `docs/testing/2026-10-21-device-checkpoint.md` for the latest cross-device
status. The subsequent MIDI diagnostic/resource test and final serial
regression supersede provisional summaries below. MIDI Mini destruction
and unload presence were measured; its replug was gated by file copying
before AddDevice. Version metadata now builds, but validation was stopped
by STOP1E and a subsequent no-USB boot reporting portcls.sys load status
C0000221. Failed overlays are preserved, with no attribution to project
code or repair of system binaries. Serial's final Win7 run passes all six
echo sizes, while the 2000 ZLP regression remains a recorded negative.
All agents' guests/models and root's guests/models are now closed.

Latest continuation: the original USB MIDI PortCls driver has two actual
Windows 2000 loopback passes (355 bytes, 32 short messages, one SysEx,
zero errors, clean close). Replug exposed a whole-composite-parent INF
match; the owner is narrowing it to MI_00 and retesting a clean overlay.
Do not claim replug pass yet. The synthetic STREAM.SYS camera M0 also
loads on 2000, appears in CamCheck's source dialog and exposes its YUY2
format; RUN is still being developed. See the dedicated MIDI and UVC
miniport run records.

CamCheck now displays moving 160x120 YUY2 bars from our original simulated
UVC camera through Windows 7's native usbvideo driver. Stop/disconnect
works; reopen is under investigation (error418 before new class controls).
This is simulator/native-driver/utility evidence, not project USB video
transport or real-camera evidence. Seven simulator integration tests pass;
the legitimate final 62-byte packet after fifteen 64-byte reads is supported.
See `docs/testing/2026-10-21-camcheck.md` for exact hashes and VM evidence.

ASIX descriptor/control planning has 66,542 host checks, in addition to
62,861 framing checks. Both captured SuperSpeed configurations are rejected
when the caller-observed connection is EHCI/high-speed. No real register
writes, USB2 transport or NDIS-driver pass follows from this planning core.
The X4 remains host-bound to WinUSB; its libusbK switch is still awaiting
the user's required host administrator action. Keep it detached meanwhile.

After the shared audio shim move, all four UAC2 build/import gates and the
direct stream tests pass again. The recorded simulation used installed
version3 SYS C7A172...1126E, preserved as `out/audio/uac2/w7-installed-v3.sys`
and in its E: staging folder. Newly rebuilt `out/.../wdmx86/free` hashes
D201B9...6A1910 and was not installed; do not conflate that build with the
recorded VM run or silently trust INF version equality as file identity.

AX host controls now have a separate read-only acquisition record:
`docs/testing/2026-10-21-ax88179-host-reads.md`. Three vendor IN reads returned
exact lengths; no reset/configuration/vendor OUT/claim or guest networking
was performed. `docs/design/uvcrun.md` records why the first unwired frame
worker must be redesigned before RUN integration (notification reentry,
ownership lookup, cancellation and teardown rundown).

- Development has started. The build skeleton is done (2026-10-21):
  `docs/toolchain.md` records the kits and guests, and
  `src/generic/wdmsmoke/` (a disposable root-enumerated WDM driver, never
  released) builds with `build.cmd <wdmx86|wdm64> <free|checked>` into
  `out/` with a log, a record and an import gate. It installs, reports
  "working properly" and removes cleanly on 98 SE, 2000 SP4, XP SP3 and
  7 x64 (with signature enforcement disabled):
  `docs/testing/2026-10-21-wdmsmoke.md`. No package driver code yet.
- Docs: `uvc`, `uac2`, `usbmidi`, `ethernet`, `serial`, `gamepad`,
  `printing`, `floppy`, `libusb`, `installer`, `candidates`, `existing`,
  and the new `build` draft.
- `existing.md` is the survey of drivers that already exist; packages were
  narrowed to the gaps it found.
- Many points in every doc are marked "unverified"; each package's M0
  milestone is experiments that settle them before real code.
- Owner, 2026-10-20: the installer is deferred; users install each
  driver by hand from its `free/` folder. `installer.md` is kept, marked
  deferred, and the x64 signing policy is restated in `build.md` section 7.
- Owner, 2026-10-20: every driver must work with EHCI controllers
  (Microsoft's EHCI on 2000+, NUSB's on 98 SE); each package's tests have
  an EHCI leg. ME has no known EHCI stack.
- Owner, 2026-10-20: tested systems are 98 SE, 2000 SP4, XP SP3 x86 and
  7 x64 (plus ME for `usbflop`), each within the package's range; other
  systems ship "Built, not tested" (README, "Tested by the author").
- Kits are now in `tools/` (MSVC600, ntddk, win98ddk-extracted,
  win2kddk-extracted, WinDDK71) and fresh guests in `vm/` (`build.md`
  section 8), both git-ignored, both copied from xHCI98's host copies.
  The guests were extracted from these snapshots: 98 SE `post-nusb`, ME
  `winme-clean-install`, 2000 `win2k-xonly-clean-install`, and the
  `*-clean-autologon` snapshots for XP to 7 (logons as in xHCI98's
  build-and-test notes: `test`/`test`, XP `test` with no password, XP x64
  `Administrator`/`test`).
- NUSB for 98 SE: the `w98se.qcow2` guest already has NUSB 3.3 installed
  (snapshot `post-nusb`). The installer `nusb33e.exe` and its extracted
  files are also in `tools/` (third-party, test use only, never shipped).
- Kits checked on this host, 2026-10-20: a minimal WDM `DriverEntry`
  compiled and linked with MSVC 6.0 + the 2000 DDK (i386, native 4.00,
  imports `NTOSKRNL.EXE!DbgPrint`) and with WDK 7.1's amd64 compiler
  against `lib\wnet\amd64` (x64, native 5.02). The 2000 DDK build needs
  `MSVC600\VC98\Include` after `ntddk\inc\ddk` and `ntddk\inc` (for
  `excpt.h`). Neither image has been loaded in a guest yet.
- The priority `installer`, `ethernet` (`cdcnet`) and `floppy` drafts
  have been checked and revised. Their historical-system and hardware
  experiments remain pending; the other package drafts still need that
  design check before implementation.

## Drafts expanded in this continuation

- `docs/design/build.md`: local kit inventory, isolated build profiles,
  package command/output contract, per-system import and ABI evidence,
  disposable smoke-driver acceptance, signing/catalog/hash release order
  and test-record requirements. It specifies future scripts; no kit or
  source skeleton was installed or written.
- `installer.md`: immutable NT source generations, current-binding-only
  device removal, Windows dependency exemptions, pure-batch 9x byte
  comparisons, interrupted-operation records and checked cleanup after
  restart. No queued original-path deletion; no signature claim based
  only on a PE certificate table.
- `ethernet.md`: parser and asynchronous ownership contracts, ordered
  payload/ZLP submission, NCM constraints, RNDIS control/offset validation,
  target/build consistency and explicit M0 evidence/exit conditions.
- `floppy.md`: first-release CB/CBI models only, function-specific quirk
  IDs, Windows component ownership, package trust separate from driver
  load policy, and per-system M0 evidence/exit conditions.
- README links the build draft. No code, `free/` files, test claims or
  release tags were created. Documentation whitespace, local links and
  LF line endings passed checks before this continuation was committed.

## Rules the owner set (also in AGENTS.md / the docs)

- GPL-2.0-or-later (owner, 2026-10-25; GPL-2.0 before). Sources: specs, Microsoft's published DDK docs and headers,
  this repo's own code, GPL-compatible code (Linux, ReactOS) with notices
  and a NOTICE.md row. DDK SAMPLE sources may NOT be read; export tables
  (names/ordinals) of in-box binaries MAY be. Never Microsoft or vendor
  binaries' code or disassembly.
- Standalone: no build, release or run-time dependency on any other repo.
  Don't describe this repo in relation to xHCI98 or the non-free pack;
  xHCI98 may appear only as one tested host stack. Code copied from xHCI98
  keeps its GPL header.
- User-mode code, VxDs and INF-only packages are allowed when they exist
  to make a driver here work or fill a gap Windows leaves.
- Multi-function devices: INFs match interface-level IDs only (exception:
  serial composite boards on Microsoft's 98 SE/2000 stacks, by VID/PID).
- Test on real devices (one or two mainstream ones per class). NUSB's
  EHCI stack on 98 SE is fully supported, alongside xHCI98 and Microsoft's.
- x64 signing: the same approach as xHCI98/Stor98 (test-signing documented).
- 8.3 names for everything under `free/`; folder scheme in README "Layout".
- No attribution trailer in commits (no `Co-Authored-By` or similar);
  commit messages from a file (`git commit -F <file>`); LF line endings.
- A README "Tested" cell becomes "Tested" only from a recorded run
  (device, host stack, date).

## Wave plan

Owner, 2026-10-21: the waves swapped, and the INF-only pieces moved
into wave 1. `uvc98` then moved to wave 2: there is no UVC camera on the
bench yet.

- Wave 1: `uac2` (USB Audio Class 2.0, PortCls WaveCyclic, 98 SE-8). It
  needs High Speed isochronous transfers, so its EHCI legs are on real
  hardware. With it, the INF-only pieces: `usbflop` (USB floppy INF binding Windows'
  `usbstor.sys`) and `usbcom`'s `USBCDC.INF` (2000-8, binding in-box
  `usbser.sys`; the `w2k-8` folder ships first as `usbcom`'s first
  release).
- Wave 2: `uvc98` (USB Video Class, 98 SE/ME/2000; waits for a camera),
  `usbcom`'s 98 SE/ME driver (`USBCOM.SYS`/`.VXD`), `usbmidi`
  (class MIDI on 98 SE/2000), `cdcnet` (ECM/NCM/RNDIS, 98 SE-7, NDIS 5;
  RNDIS on XP-7 = INF over Windows' driver; keeps RTL8153 IDs on XP-7 as
  a fallback), `xpad98` (Xbox One/Series pads 98 SE-Vista, 360 receiver
  on 98 SE/ME, 360-protocol pads outside x360c's 25 IDs).
- Later: the installer (deferred); `libusb0` (rebuild libusb-win32
  0.1.12.2 for 98 SE/ME), `rtl815x`/`ax88179` (98 SE/ME/2000 only), `usbprn` (low priority).

## Start here (suggested order)

1. **Build skeleton: done.** Left for the first package build: a
   version resource (Device Manager shows no file version), whether a
   `$Windows NT$` INF shows `DriverVer`'s date on 2000, a negative test
   of the missing-kit message, and the `ndis5`/`ndis51` profiles. When
   a second package needs it, move `chkimp.ps1` out of `wdmsmoke` into a
   shared place and agree where (`AGENTS.md` names only per-class
   `common/`).
2. **Prove the draft gates** of `build.md`, `uac2.md`,
   `floppy.md` and `serial.md` (its NT INF) with the installed kits and
   the named guests and devices. The design check has not replaced these experiments.
   Before implementing later packages, check their designs against the
   allowed specs, documentation, headers and other docs too. Tracked design
   docs do not mention the reviewer, AI tools or review rounds.
3. **Wave 1 next: `uac2` M0** (PortCls over a USB function PDO, read
   from the DDK documentation and headers; `uac2.md` sections 10 and
   14). Its real legs need High Speed UAC 2 hardware on EHCI and
   xHCI98; the Sound Blaster X4 (`041E:3278`) is on the bench. The
   INF-only
   pieces need a USB floppy drive (`usbflop` T0/T1) and an Arduino Uno R3
   or Pi Pico (`usbcom`'s `USBCDC.INF`, `serial.md` M0) on 2000, XP and 7
   x64.
4. Wave 2: `cdcnet` M0/M1 in QEMU (`usb-net` gives ECM and RNDIS; the one
   package testable without hardware), then `uvc98` once a camera is
   on the bench, `usbcom`'s 9x driver,
   `usbmidi` and `xpad98` as hardware arrives.

## The Sound Blaster X4 (uac2's bench device), 2026-10-21

- Its descriptors are saved as the first `uac2` host-test input:
  `src/audio/uac2/testdata/sbx4/` (device, qualifier, configuration,
  strings without the serial number, a decode, and a README with the
  capture method and hashes). The capture scripts were throwaway: a
  read-only libusb-1.0 `GET_DESCRIPTOR` dump through QEMU's
  `libusb-1.0.dll`, with ctypes. Rewrite one in the repo if more devices
  are captured.
- It is High Speed (480 Mb/s), class `EF`, interface 0 HID, 1-2 CDC ACM
  (IAD), 3-6 UAC 2 (IAD `01/00/20`): playback 2/6/8 ch with explicit
  feedback, capture 2 ch on an IN endpoint marked implicit feedback,
  S/PDIF out with a Type III alternate. All iso packets <= 1024 bytes,
  one transaction per microframe (easy on EHCI).
- **On the host it is now on WinUSB** (owner switched the composite
  parent and all interfaces with Zadig), so QEMU `usb-host` can pass it
  through: `info usbhost` lists it as bus 1, addr 7 before the replug
  (re-read the address; it changes on every replug). It no longer works
  as audio on the host until the WinUSB driver is uninstalled with
  "delete the driver software" and the device replugged.
- **Checks against `uac2.md` still to make** (not yet edited):
  - section 4.5, implicit feedback: the rule "usage type 10b on the IN
    data endpoint" would put the X4's playback into implicit mode,
    although playback has its own explicit feedback endpoint `0x83`.
    Implicit mode should apply only to an OUT stream with no explicit
    feedback endpoint, paired with a named IN endpoint.
  - section 1 scope: Type III is out of scope, so alternate setting
    selection (section 4.3) must skip interface 6 alt 2 (and any
    non-Type-I alt), not fail the function.
  - matching (section 7): on XP+ the function is
    `USB\Class_01&SubClass_00&Prot_20`; on 98 SE/2000 without IAD
    grouping, interface 3 is `01/01/20` and 4-6 are `01/02/20`. The INF
    already lists both forms; confirm on a guest.
  - three clock domains, each a clock source behind a one-input
    selector: the clock-path walk of section 4.2 must handle this.
- Optional next look: attach the X4 to the 98 SE or 2000 guest
  (`-device usb-ehci` plus `-device usb-host,hostbus=1,hostaddr=<n>`)
  to see how they enumerate it before any `uac2` code exists; passthrough
  timing is not a valid EHCI test leg.

## Running the guests

Guests are installed by hand through each system's Add Hardware wizard
(owner, 2026-10-21: the owner drives the GUI; the agent launches QEMU
with a window, takes `screendump`s through the monitor port on request,
and records results). Launch settings that worked, each on a throwaway
overlay of the `vm/` image, with test files on a QEMU virtual floppy
(`-drive file=fat:floppy:rw:<dir>,format=raw,if=floppy`):

- 98 SE: `qemu-system-i386 -machine pc -cpu pentium3 -m 256` (TCG).
  Do not add `-usb` casually: the image has never seen PIIX3 UHCI and
  starts the new-hardware wizard for it.
- 2000 SP4: `qemu-system-i386 -machine pc,acpi=off -cpu pentium3,-apic
  -m 256 -vga cirrus -rtc base=localtime` (TCG).
- XP SP3: `qemu-system-x86_64 -machine pc -accel whpx,kernel-irqchip=off
  -cpu pentium3 -smp 4 -m 512 -vga std`. This host's `qemu-system-i386`
  has no WHPX.
- 7 x64: `qemu-system-x86_64 -machine pc -accel tcg,thread=multi -cpu
  qemu64 -smp 4 -m 2048 -vga std`; choose "Disable Driver Signature
  Enforcement" at the boot menu for unsigned drivers. Slow.
- `-monitor tcp:127.0.0.1:<port>,server=on,wait=off` for screenshots.

## Hardware still to choose (owner)

- uac2 (wave 1): a common asynchronous High Speed UAC 2 DAC with explicit
  feedback (XMOS or C-Media CM6631 class); the Sound Blaster X4 is held.
- uvc98 (wave 2, none on the bench): a mainstream YUY2+MJPEG UVC camera with high-bandwidth
  alternates, and a second mainstream camera, ideally with a microphone.
- usbflop: a USB floppy drive on no Microsoft list + a listed TEAC/Y-E Data.
- usbcom: Arduino Uno R3 and Pi Pico (QEMU has no CDC-ACM device).
- usbmidi: a class-compliant MIDI keyboard or cable.
- xpad98: an Xbox One/Series pad + a 360-protocol pad outside x360c's list.
- cdcnet/rtl815x/ax88179: an RTL8153 and an AX88179 dongle; an Android phone.
- libusb0 (later): a loopback board (FX2LP or RP2040).

## Related, outside this repo

- xHCI98 branch `isoch-ring` (Phase 43, 43.38-43.41) enlarges the
  isochronous ring for uac2/uvc98; 43.41 (larger event ring) is the owner's
  call.
- The non-free collection (`usb-wdm-nonfree`) holds vendor drivers that
  cover what these packages deliberately leave out (x360c/x360wc, Realtek,
  ASIX, FTDI, CH341); our drivers must never require it.

## 2026-10-21 X4 host-ISO follow-up (latest)

- Preserved installed project UAC2 build3 C7A172...1126E; no driver restage.
  Built isolated source QEMU11.1.0 and libusb1.0.30 tools on E:, preserving
  installed QEMU/DLL. See docs/testing/2026-10-21-qemu-iso.md and
  docs/testing/2026-10-21-uac2-vm.md; experimental patches are not releases.
- Confirmed upstream folded-KISO-status bug with actual mapper source and
  physical raw logging: nonzero USHORT errors had been classified success.
  docs/testing/libusb-kiso.patch keeps LGPL notices and reports nonzero as
  generic packet errors. Exact original 32-bit USB status cannot be recovered.
- WHPX guest GetTickCount substantially undercounts wall/QPC time; one CPU
  did not fix it, HPET-off reduced discrepancy. Strict criteria unchanged.
- Latest physical X4 repeat w7-kiso4 passed unchanged WinMM test (8/8,
  96000 samples, GetTick1500ms/QPC2015ms, close0, automatic exit0), but raw
  host has24 failed OUT packets within callback455/500 and24 QEMU empty-ring
  events; guest records9errors. Not a clean or audible/fidelity claim.
  Native proof out/audio/uac2/w7-kiso4-pass.png displays actual saved report.
- Existing ASAP=true host policy succeeded on IF4 OUT01/FB83. First24
  packets of the failing transfer failed, last8 succeeded; scheduling versus
  buffer/layout cause remains unresolved. Further source-backed host frame
  scheduling diagnostics are more useful than weakening watchdog/tests.
- All my guests closed normally; final inventory had no QEMU process or
  HMP44333 listener. X4 released StatusOK, libusbK/oem79.inf preserved.
  Current M1 is not proven on98SE; Win7 results do not establish98 support.

## Latest maintainer direction and completed media checkpoint

- Continue autonomously with multiple agents. Reliable physical UAC2 playback
  takes priority, followed by webcam and remaining drivers. All scratch and
  VM overlays remain on E:. Close QEMU guests and release devices after runs.
- Final release is deferred until completion: maintainer requests version
  0.0.0.1, the actual final compilation date, and complete driver/OS packages
  under release/, mirroring the nonfree collection's class/driver/system
  structure. Reconcile the older free/ packaging documentation when adding
  release tooling; do not hand-edit historical free/ artifacts or pretend a
  development test version is the final release. No release/tag yet.
- Physical Mitsumi floppy with media now passes three complete file reads
  on98SE/NUSB3.3/project CBI INF:133557 bytes, zero failures and repeated
  matching CRC32. See docs/testing/2026-10-21-usbflop-media.md and actual
  out/proof/mitsumi/98read.png. No physical writes/formatting. The separate
  native-control reboot stalled, cause unknown. QEMU33108/HMP44338 closed,
  drive released; the old no-medium-only checkpoint is superseded.
- Windows98 native UM-ONE baseline faulted before any project MIDI driver
  install; negative screenshot and cleanup are in the Roland run record.
  No MIDI guest remains from that test.

## Active reliability and webcam work, later 2026-10-21

- Multiple agents explicitly reauthorized. uac2vm owns host QEMU/libusb and
  physical X4/HMP44333. floppyvm now owns webcam adapter/worker M1, not more
  floppy media work. cdcnetvm performs independent audio safety/test audits.
- Increasing usb-host isobufs4 to16 removed immediate empty-ring drops in
  two WHPX runs but both retained raw OUT/feedback errors and severe clock
  drift. TCG plus lean tracing also gave WinMM buffer PASS while QPC took
  about20seconds for2seconds of samples; this is NOT reliable playback.
  Latest exact run records/artifacts are in the audio run log.
- Root added original docs/testing/kiso-raw.py (strict token/status/count
  validation) and x4-evidence.py (paired raw/host/player gate). Parser5 and
  gate6 host regressions pass; independent review reproduced/rejected old
  false positives. Gate deliberately rejects raw4 and ring16 evidence.
  It is fixed to the two-second X4 test, not a general UAC2/soak qualifier.
- Source review found full QEMU log tracing fflush on every event, roughly
  18000 guest-request/completion events/sec. Lean traces remove those two
  event types but keep ISO submit/callback/ring/error evidence. No causal
  fix is claimed from source alone; avoid HMP/screenshots during playback.
- Host polling candidate is documented in
  docs/testing/2026-10-21-qemu-poll-plan.md: existing main-loop notifier
  pattern, host wait cap, BQL/runstate guard, submission/resume wakeup,
  preserved orphan accounting/close-drain semantics. Coordinate actual
  source/build changes with uac2vm. The subsequently implemented poll patch
  passed the actual-source harness but physical w7-poll1 still failed:
  813ms GetTick/4765ms QPC, OUT raw errors0/feedback errors50, no empty-ring
  losses. It is not a reliability fix. See the plan's superseding section.
- Guest source now has monotonic interrupt-time feedback watchdog and always
  native-work-item cleanup for published streams, including PASSIVE final
  Release from Notify. Root reviewed and independently passed500013 cases,
  actual-header cleanup regressions and four current-source native compile/
  relink/import profiles on E:. Installed C7 and normal artifacts unchanged.
  See docs/testing/2026-10-21-uac2-guest-audit.md.
- Original TIMERTST clock helper/build are in src/audio/uac2/timertst.c and
  timer.cmd; no audio/USB/timer-resolution changes. Native host probe exited;
  guest initial GUI-mode controls include window creation overhead. Repeat
  /quiet for cleaner comparisons; frozen QPC needs a host watchdog.
- Root prepared separate UACCHK (mandatory QPC two-second wall bounds) and
  UACSOAK (120blocks/30seconds) build branches/scripts on E:. UACCHK staged,
  neither physically qualified. New /playexitlocal writes C:\UACTEST reports;
  the next audio test runs all player/report I/O on C: with A: ejected.
  The old9D671A UACPLAY stays preserved for host
  A/B; default source branch retains original two-second thresholds. New
  direct mock-WinMM tests passed independently in all three branches.
  Record exact fresh
  helper hashes before use, not intermediate pre-wallgate hashes.
- Webcam actual-SRB adapter porttest883 and timer/worker engine worktest486
  pass independently, with actual public-DDK object compilation. M0 remains
  unchanged. M1 uses class synchronization ON, passive private generator,
  class timer pump, externally joined worker and publication-last cleanup.
  It is synthetic only; physical USB capture remains later. Pending target
  gate: timer behavior while INITIALIZE/UNINITIALIZE is pending. First M1
  native Win2000 install stayed pending over2min before any READ/RUN, though
  UI remained responsive. Guest41268/listener44338 closed. Agent prepares
  VM-only debugcon stage tracing to locate the boundary before redesign.
  No webcam native M1 pass is claimed.

- HOSTWAIT native quiet process retained ~15.6ms waits despite successful
  timeBeginPeriod(1)/matching end. Subsequent /quiet /honor exited0 and
  reduced waits to~1-2ms; exact policy restored and request ended. This
  establishes only the probe process behavior, not QEMU/audio causality.
- x4-evidence now also requires one mandatory QPC report within1500..2500ms;
  diagnostic-only legacy timing cannot qualify. Explicit --soak mode adds
  the fixed30-second coverage/window policy. Eight gate regressions pass;
  no physical soak pass is claimed.
- C-local/A-ejected strict physical run still failed: 8/8 buffers and96000
  samples, GetTick750ms versus mandatory QPC3474ms, Reset/Close0. Player
  exited1, AudioSrv stopped, installed C7 hash unchanged. Audio guest17068
  shut down normally and44333 closed. Native HOSTWAIT /honor quiet slot
  released. Translated OUT errors0/feedback101, no ring drops, host OUT
  window3429ms. LIBUSB_DEBUG was omitted, so raw warning records absent;
  this is negative timing evidence, not a matched raw-quality comparison.
- QEMU os-win32.c already requests min period. Next experiment uses an
  original external exact-PID HOSTPOL policy helper, no QEMU build change
  or duplicate period request. cdcnetvm owns helper and mocked lifecycle
  tests, uac2vm owns physical comparison. Webcam debugcon guest30736/44338
  owns the current VM slot until closed.
- Webcam trace guest30736 is now closed. Nine own-driver debugcon stages
  show LOW entry, successful thread creation, BootstrapDone and initial
  timer arm, but no timer entry before installation stalled. Agent reviews
  documented LowToHigh handoff (public strmini.h/DDI) to avoid initial
  timer dependency, including cancelled/failed bootstrap cleanup.
- Policy-only audio guest29812 is active with exact QEMU4BCC/DLL622FC
  and HOSTPOL9C92. Helper verified original0/0 ->control4/state0; it must
  remain alive until audio stopped, then restore+readback BEFORE guest
  exit. No heavy tests/VMs while this timed run is active.
- Policy-only result supersedes active-run note: FAIL8 submitted/1completed/
  20160samples, GetTick6688ms/QPC10122ms (mandatory wall deadline).
  Reset/Close0, AudioSrv stopped. HOSTPOL restored exact original0/0 while
  QEMU still live, exited0 with unconfirmed0. Timer honor alone is not a
  fix. Audio owner collects fullraw/trace then closes guest; next candidate
  is low-volume main-loop stage timing with bounded memory buffering and
  post-run dump, original policy0/0 and unchanged C7/DLL/lean/ring16.
- Full policy1 raw records narrow the result:100 OUT callbacks/3200 packets
  and27 feedback/864 packets all raw/translated zero errors, no ring drops;
  OUT417ms/FB868ms host windows, max gaps32/59ms. Guest had362 good
  feedback values, last0x5FFCF/length4, but LastStatusIO_TIMEOUT. This
  status does not uniquely identify the watchdog versus other timeout paths.
  Next priority is paired adjustable/monotonic watchdog-only relinks with
  exact retained D201 main/desc/rate/resource objects, not literal C7 objects.
  Baseline x86free1D9D97097AB1D7DB1C541A2D854403AC66420E5B0AF2942362E40F3F07047306;
  monotonic35F03961554237CC5C89D8C31A972C991DAE9A21D02960D722E4339D86DBF912.
  Eight compile/link/import gates pass. clockpair.ps1 records inputs; root
  reviewed. Audio owner will preserve old overlay and use fresh w7clock.
- cdcnetvm now prepares native-host async libusb ISO playback/feedback
  baseline (none previously exists), with pinned DLL, process-local timer
  lifecycle, fake-backend checks and cancel/drain/restore gates. No physical
  native test run yet. Coordinate with audio VM owner before X4/timing use.
- Webcam LOW->LowToHigh fix passed711 actualengine guards and124 dispatch
  checks plus nativeimport/object profiles. Fresh guest31368 now completes
  INIT and runs timers, but installer reports Code10/reboot required; agent
  performs one bounded reboot before any started-device claim.
- Paired1D9D baseline did NOT reproduce IO_TIMEOUT: player8submitted/
  0completed/4862samples, GetTick110ms/QPC11726ms deadline; stoppedstats
  LastStatus0, GoodFB99/Bad35/Keepalive2, Data38/FB19. Raw35OUT/19FB all0;
  normal QEMU trace lacks10 orphanFB callback events because early free
  precedes existing tracepoint. Do not claim matched full host accounting.
  Guest44024 closed normally, HOSTPOL restored0/0 before exit. Mono35F039
  remains unstaged. Clock comparison paused because control did not reproduce.
- Important verifier interpretation: perfStart precedes firstwaveOutWrite,
  which enters UacUsbStart configuration/clock/feature/alt setup. Large
  QPC with short ISO window may include slow startup, not just steady
  playback stalls. Separate UACPHAS before/after eachwrite clocks prepared;
  unchanged strict deadlines, memory-only reporting, four mockvariantsPASS.
  E:/usb-wdm-free/wave2/uac-phase/UACPHAS.EXE SHA
  8E70826CA5ED5F6382D9565C3130215B26E6CCBCC67AC7730E4C847CA3C12F8D.
  Phasevariant3289059 checks includes3s firstWrite delay stamped+rejected.
- Native host X4 baseline natprobe.py now passed twice with unchanged
  DLL622FC/libusbK0690/rate59ED, noVM. Native1/2 complete384000 bytes in
  2005.171/2004.953ms, zeroAPIpacketerrors,1984validFB/0bad, drained all
  callbacks and restored alt/clock/policy/timer. Native1 stderr empty;
  native2 parentLIBUSB_DEBUG2 produced fullraw. Root independent parser
  confirms500OUT/16000packets64FB/2048packets allraw/URBzero, noinvalid.
  OUTactual lengths are synthesizedAPIaccounting, no audible/fidelity claim.
  Nativequietwindowreleased. Webcam cappedSRBtraceguest12904/44338 is
  alreadybooted; ONEboundedtestthenclose, audioUACPHASnextslotpriority.
- UACPHAS1 guest42272: all16 before/afterwrite stamps
  tick0/QPC0, so synchronous firstWrite startup cost NOT observed on this
  Win7 shared-audio path. Failed8submitted/0completed/10080samples at
  GetTick6532ms/QPC10127ms. Poststats stage31success, LastStatusIO_TIMEOUT,
  Data53/FB26, Completed37724, Good155/Bad37, LastFB0x60744/4bytes,
  PacketErrors0. Raw49OUT/20FB allzero and normaltrace fullymatched; host
  OUT202ms/FB645ms. This paired baseline now reproducesIO_TIMEOUT, so
  next is the monoonly35F039 underidentical8E708/HONOR/4BCC/DLL settings.
  Guest42272 closed andpolicyrestored. Mono stage/reboot guest45904 now
  owns quietVMslot; noheavybuilds/otherVMs. Mainlooptelemetrysource reviewed
  afterunits/clockepoch/guardedexit fixes, stillnotbuilt orrun.
- Latest playtest.c adds real failedWrite and simultaneous failedWrite/
  afterstamp failure coverage requested by review. Sourceonly currently;
  cdcnetvm will independently compile/runfourvariants AFTERmonoquietrelease
  and updatefreshcounts. UACPHAS8E708binary/sourceimplementationunchanged.
- Webcam cappedtrace identified actualCode10guardfailure: streambuffer
  present, sizefield0, registereddescriptor120; ourguardreturned
  BUFFER_TOO_SMALL. Independentreviewcorrectedattribution: NumberOfBytesToTransfer
  aliasesActualBytesTransferred in strmini.h; M1 itselfzeroedtheoutput
  BEFOREreading/tracinginput. ThisdoesNOTproveclasssupplied0. M0snapshots
  inputbeforezeroing, explainingdifference. M1-onlyfix tracksdocumented
  registeredbudgetfromINIT, avoidingalias entirely. Sourcealsoavoidsdoublecompletingsame
  still-ownedINITpointer; newtests prepared. M0unchanged. Guest12904 reached
  safetooff andclosed. No newfixednativeRUNproofyet.
- Mono35F039 comparison didnotfixclient playback:8submitted/0completed/
  6240samples,GetTick109ms/QPC11586msdeadline; phasewrite0afterQPC2ms
  throughwrite7after5ms. Stopstats stage31success,LastStatus0,PacketErrors0,
  Data37/FB19,Completed25352,Good96/Bad40,LastFB0x60189/4bytes. Thisdoesnot
  establishsameIO_TIMEOUTcauseasPHAS1. Guest45904closednormally,HOSTPOL
  restored0/0exit0. NextmemorytelemetryQEMU shouldretaincurrentmono35F039/
  HONOR4/0/PHAS8E708/DLL622FC/ring16 baseline, changingonlyQEMUobservability.
- CDC independent4variantwritefailuretestsPASS afterquietrelease:
  legacy2520770/strict3097004/phases3337145/soak46112844checks,zeroerrors,
  W3WX/noWINMMimport. BothfailedwritecasesproveactualSubmitted1/completed0
  andReset1, simultaneousclockandwriteerrorsretained. Verifierdocupdated.
