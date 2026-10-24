# Separate strict playback verifiers, 2026-10-21

These are diagnostic tools, not released drivers. Historical physical
comparisons retain UACPLAY SHA256
`9D671A99C7D3E4C24CD7BD9E958CB8852EC01C4FB4128C9626A40574D00F6AF9`.
Its application-level buffer PASS can coexist with severe independent
clock/transport failure, as the TCG run demonstrates. Preserve its reports;
do not retroactively claim it checked wall-clock quality.

The default branch of `uacplay.c` preserves the two-second experiment:
8 buffers of12000 stereo16/48000 frames, minimum GetTickCount1500ms,
position91200..100800, completion deadline10000 guest milliseconds.
Optional QPC observation remains diagnostic in that branch. The original
EXE was not overwritten by the builds below.

New separate builds:

| Tool | Build | Additional policy | Report |
|---|---|---|---|
| UACCHK.EXE | `wallchk.cmd`, UACPLAY_WALLGATE | Mandatory QPC1500..2500ms, monotonic observations, independent10000ms completion deadline | A:\PLAYLOG.TXT |
| UACSOAK.EXE | `soak.cmd`, UACPLAY_SOAK, implies wall gate |120 buffers/1440000 frames/5760000 bytes; minimum GetTick29500ms, position1435200..1444800; mandatory QPC29500..30500ms, independent38000ms completion deadline | A:\SOAKLOG.TXT |

Both builds remain fixed stereo16/48000 with the same quiet440Hz triangle.
The 30-second branch allocates its full PCM and header arrays before queueing.
Neither helper changes timer resolution or a system clock. QPC comparisons
reject backward steps between observations, not just values below the start.
Final wall bounds reject late completion even when all headers are already
DONE before the loop checks its deadline. These thresholds are explicit test
policies, not USB Audio class limits. QPC trust must still be assessed against
independent host timing and raw transport evidence for the actual VM.

Run with `/playexit` for automatic completion/report/exit. Exit0 is a strict
WinMM test pass, not audible fidelity or zero USB errors. Exit1 is test failure,
2 no endpoint,3 save failure,4 retained resources. Automatic exit is suppressed
when resources remain retained, preserving buffers until external cleanup.
Use an external host watchdog for a wedged guest. Never free still-owned
headers merely to force test termination.

For the floppy-I/O isolation experiment, `/playexitlocal` performs the same
automatic test but creates `C:\UACTEST` and writes `PLAYLOG.TXT` there
(`SOAKLOG.TXT` for the soak build). `/playlocal` selects that path while
leaving the GUI open. Copy the executable and launch script to C: before
starting audio, eject A: for the entire timed run, and copy reports out only
after audio stops. The normal flags retain the A: report paths above.

Both build scripts use the repository-local MSVC6 kit, /W3 /WX, GUI4.0,
and default to separate scratch directories on E:. Their optional first
argument selects a different output directory. No VM or audio device is
opened by a build.

Reviewed build identities after adding the local-report flags. UACCHK was
staged for the next physical experiment; building/staging is not a test pass:

| Artifact | SHA256 |
|---|---|
| E:/usb-wdm-free/wave2/uac-wall/UACCHK.EXE | A057CC1ED6268A3B7ABFBC9426663434BF3A31F7A46A2EB04F734C5FD1CE7169 |
| E:/usb-wdm-free/wave2/uac-soak/UACSOAK.EXE | 72D31E449EE79FD2D12A98DEF029D419F826A395907810D240B5F67C6D765ACF |

## Separate write-phase diagnostic

The original measurement begins before the first `waveOutWrite`. That call
can enter the driver's RUN path and perform USB configuration, clock and
interface work before the first physical ISO callback. Consequently, a
large player QPC interval with a short host ISO window does not by itself
locate a steady playback stall or prove an inaccurate performance counter.

`phase.cmd` builds separate `UACPHAS.EXE` with `UACPLAY_PHASES` (implies
the strict wall gate). Before and after each of eight writes it appends
`Phase=before_write/after_write index=N tick_ms=N qpc_ms=N` to memory.
There are no additional timed-run file writes. Each QPC observation is
checked for failure/backstep. It retains the same overall deadline and
duration criteria; it does not turn startup delay into a playback PASS.
Normal UACCHK/UACSOAK binaries above were not overwritten.

Prepared artifact `E:/usb-wdm-free/wave2/uac-phase/UACPHAS.EXE` SHA256:
`8E70826CA5ED5F6382D9565C3130215B26E6CCBCC67AC7730E4C847CA3C12F8D`.
Use `/playexitlocal` with C:-local executable/script and ejected A: as above.
The subsequent physical phase-helper run failed its unchanged strict gates:
all16before/after-write observations were0ms, but8submitted/0completed,
10080samples, GetTick6532ms/QPC10127ms and the independent completion
deadline. Reset/Close returned0. Thus that run does not support the proposed
long synchronous first-write delay. See the per-write phase section in
`2026-10-21-uac2-vm.md` and `out/audio/uac2/w7-phases1-play.txt`.

## Direct actual-source regression

`playtest.cmd` includes the actual `uacplay.c`, replacing all audio,
clock/event and report-file operations with deterministic test doubles.
The test executable has no WINMM import, performs no audio/USB operations
and writes build/log artifacts only under its E: output directory. It checks
PCM channel agreement, bounded/nonconstant waveform, buffer/header counts,
four completion batches, positions, premature/late/stretched elapsed time,
counter startup/final/frequency failure and both kinds of intermediate/final
backstep. It also checks retained ownership, report-create/short-write failure
and allocation failure. Local-mode checks assert the selected C: report
path and automatic-exit policy using mocked file operations.

Root independently ran `playtest.cmd E:\usb-wdm-free\wave2\uac-playtest-independent`:
legacy8 passes2496728 checks, strict8 passes3072962, strict120 passes46088578.
The subsequent independent `uac-playtest-phases` run also passed these
unchanged counts and the new phase branch3289059 checks, including a
3000ms first-write delay visibly stamped and still rejected, plus phase
QPC failure. All four builds use /W3 /WX; import gates confirm no WINMM.DLL.
These are deterministic host checks, not kernel/USB/device proof.

After the write-failure injection additions, an independent run waited for
the audio owner to close guest45904 and explicitly release its quiet window,
then ran `playtest.cmd E:\usb-wdm-free\wave2\uac-playtest-writefail`:

| Variant | Checks | Failures |
|---|---:|---:|
| Legacy8 |2520770 |0 |
| Strict8 |3097004 |0 |
| Phase8 |3337145 |0 |
| Strict120 |46112844 |0 |

Both the failed second-write case and simultaneous failed-write/after-write
QPC case assert the actual player's `Submitted=1 completed=0` report and
exactly one Reset, alongside mock acceptance counts and complete unprepare/
close/free accounting. The simultaneous failure retains both error messages;
its phase error cannot hide the failed Write. All four builds passed /W3 /WX
and import checks without WINMM.DLL. No audio, USB, host A: or real report-file
operation was used. Source hashes at this run:

- `playtest.c`: `CC26E08C924F1DC2E820BFD62E13CED0EF4072D5E281CACCCE9F514E6D0D7C85`
- `uacplay.c`: `382ADD3AE846603CFC68BBF5A2DB9E15492ACD1B3161293DE3E47B335AD3BC59`

The source tests did not rebuild, replace or restage the physical phase
artifact8E70826C...C12F8D. The host mock window was then released.

`x4-evidence.py` defaults to the two-second raw/trace/player configuration.
Its explicit `--soak` mode instead requires120/120 buffers and1440000
samples, GetTick at least29500ms, mandatory QPC and host OUT windows
29500..30500ms, at least240000 OUT packets/5760000 requested bytes, and
at least29500 feedback packets with29500ms coverage. It retains all raw
error, matching-count and cleanup gates. These are fixed X4 experiment
policies; this mode is prepared and regression-tested, not physically passed.
