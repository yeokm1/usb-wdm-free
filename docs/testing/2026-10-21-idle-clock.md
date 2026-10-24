# Idle Win32 clock diagnostic, 2026-10-21

The original `src/audio/uac2/timertst.c` helper compares GetTickCount with
QueryPerformanceCounter over twenty QPC seconds. It uses Sleep10 and
flushes intermediate observations roughly once per QPC second. It does
not open audio or USB, load a driver, change timer resolution or adjust
the clock. QPC is an independent observation here, not a general claim
that every virtual counter is trustworthy. External host timing remains
useful when assessing guest runs.

Build with `cmd /c src\audio\uac2\timer.cmd`; optional first argument
selects an output directory. Default scratch is
`E:/usb-wdm-free/wave2/timer-probe`. The build uses the named local MSVC6
kit with /W4 /WX and Windows subsystem4.0. No other repository is used.

In a guest, run `start /wait A:\TIMERTST.EXE A:\CLOCK.TXT` and record
the process exit code separately. The optional `/quiet` before the output
path suppresses its progress window. It exits automatically after twenty
QPC seconds, instead of sleeping twenty potentially drifting guest seconds.
Window close cancels. Exit0 means diagnostic completion only;1 is cancelled,
2 is invalid invocation or counter/UI failure,3 is report failure. Neither
its exit0 nor COMPLETE line is a playback PASS.
If QPC itself freezes without moving backward, the helper cannot prove
twenty seconds elapsed. Apply an external host watchdog/cancel in that
case; automatic exit is QPC-bounded, not independently host-bounded.

## Host control

Helper SHA256:
`B030ED64427090C46ED074A2BD3608CB82AAED94906CE01FBEADC479ACC938B9`.
An explicitly hidden native host launch with `/quiet` completed and exited;
report `E:/usb-wdm-free/wave2/timer-probe/HOST.TXT` records QPC20001.651ms,
GetTickCount20000ms,1277 Sleep10 loops and maximum loop interval16.659ms.
This control's original process exit code was not retained; no exit-code
claim is made from the COMPLETE line. The helper process was verified gone.

Guest WHPX/TCG comparisons use this exact helper with physical X4 detached.
Their results will be recorded with the physical audio investigation.

The first WHPX one-CPU/HPET-off idle run, with X4 detached and AudioSrv
stopped, completed at QPC20002.639ms/GetTick18657ms: approximately6.7%
undercount,1183 Sleep10 loops and maximum interval79.203ms. Original report
is `out/audio/uac2/w7-whpx-clock.txt`. That undercount is much smaller than
the same guest's audio run (875ms versus5579ms). This does not identify
the cause; audio-related VM scheduling and instrumentation load require
separate controls. Guest counter drift should not be assumed a fixed ratio.

The subsequent TCG/pentium3/one-CPU/HPET-off idle run after the guest's
required restart recorded QPC20010.858ms/GetTick19172ms,1146 loops and
maximum first-loop interval1026.974ms; QPC frequency3579545. Report:
`out/audio/uac2/w7-tcg-clock.txt`. WHPX reported10000000 for QPC frequency.
These figures are observations, not an assumption that the two counters
use the same emulated hardware.

Important control limit: the native host used `/quiet`, while these initial
guest measurements displayed the helper window. The initial clock sample
precedes CreateWindow, so first-loop latency includes GUI creation as well
as Sleep10 and observation overhead. Repeat `/quiet` in both guests for a
cleaner steady idle comparison. The first-loop outlier is not alone evidence
of a slow Sleep call or a constant timer-scale error. Intermediate records
remain available to distinguish initial delay from later observations.
