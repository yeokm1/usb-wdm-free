# Host process wait-resolution diagnostic

`src/generic/usbprobe/hostwait.c` is an original, standalone Win32 diagnostic.
It opens no audio or USB device and changes no registry, process-power policy,
driver binding or system configuration in default mode. Its only timer-resolution request is
one successful `timeBeginPeriod(timeGetDevCaps().wPeriodMin)`, paired with
`timeEndPeriod` using the same value before any subsequent phase or normal
exit, including cancellation and error paths. If the first end call fails,
the request remains marked unconfirmed and cleanup retries once. Only a
successful end call clears that marker. A failed API return is an error,
not evidence that the requested policy took effect or was restored. If both
end attempts fail, the report explicitly records unknown request state at
process exit; process termination does not certify API restoration.

The build uses the repository-named `tools/MSVC600/VC98` headers, libraries and
compiler. `src/generic/usbprobe/hostwait.cmd` builds only; it never executes the
experiment. Default output is `E:\usb-wdm-free\wave2\host-waits`. The first
build passed `/W3 /WX /MD /O1`; executable SHA256:
`58929BD4EE30EDC15BE70E5CFC45CE9AAB815A9A403445ACB0F91D12D3B89060`.
The corrected source was rebuilt and run once during the coordinated quiet
window; results and the replacement executable identity appear below.

### Optional explicit timer-request honoring

The later `/honor` option changes only the current process's
`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` policy bit. It does not
request any execution-speed change. Baseline distributions are measured first
with the original policy. Immediately before `timeBeginPeriod`, the utility
snapshots the complete public power-throttling state, sets that control bit
and clears its state bit, preserving every other captured bit. API success
and complete readback equality are required. After ending the timer request,
it restores the captured state and requires matching readback before the
after-end measurements. Cancellation/error paths attempt restoration too;
failed restoration leaves an explicit unknown-state marker and nonzero exit.

Both documented APIs are resolved by `GetProcAddress` from the already loaded
Kernel32 module, only when `/honor` is requested. No DLL bytes are read.
The VC6 compatibility ABI uses three 32-bit ULONG fields, version 1,
`ProcessPowerThrottling=4`, and `IGNORE_TIMER_RESOLUTION=0x4`, verified against
the installed public MinGW `mingw64/include/processthreadsapi.h` declarations
and Microsoft's public API documentation. There is no build-time dependency
on that MinGW installation and no API example implementation was copied.

Optional-mode executable built with `/W3 /WX`:
`E66D6FCD04E2F14E9BCC75E2A70C8627C2B43F8F6901A36B72D33DAFC2E0482C`.
The coordinated `/quiet /honor` run of this later executable is recorded below.

Primary contracts:

- [SetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation) describes explicitly disabling timer-request ignoring, process scope, and returning to system-managed behavior.
- [GetProcessInformation](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocessinformation) documents state retrieval and error reporting.
- [PROCESS_POWER_THROTTLING_STATE](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_power_throttling_state) documents the Version/ControlMask/StateMask layout.

Example after that coordination:

```powershell
$p = Start-Process -FilePath E:\usb-wdm-free\wave2\host-waits\HOSTWAIT.EXE `
  -ArgumentList '/quiet E:\usb-wdm-free\wave2\host-waits\quiet.txt' `
  -WindowStyle Hidden -PassThru
```

Omit `/quiet` and hidden-window launch to measure a visible GUI process. Record
whether that window remains visible, becomes occluded or minimized, and whether
the display is off. The program deliberately does not override Windows 11's
visibility-related policy. Report paths must resolve to drive E; the program
rejects paths resolving elsewhere. It does not create report parent folders.

Each of three phases (baseline, requested resolution, after matching end)
measures 256 `Sleep(1)` calls and 256 `WaitForSingleObject(event,1)` calls. The
event is private, manual-reset, initially unsignalled, and never signalled.
Any event wait result other than `WAIT_TIMEOUT` rejects the measurement. QPC
readings surround each wait, not report I/O or GUI message pumping. A decreasing
or failed QPC reading rejects the run. Sample count is bounded rather than
depending on QPC reaching a future deadline. An external host process watchdog
is still appropriate for OS stalls or suspension.

The report includes every sample and sorted min, median (middle-pair mean),
nearest-rank p90/p99, max and arithmetic mean. Six distributions total are
expected. GUI messages are pumped between short waits. Exit codes: 0 completed
measurement, 1 GUI cancellation, 2 argument/API/clock error, 3 report write,
flush or close error. Exit 0 is not an accuracy certification. Compare the
after-end phase as an observation, not proof that no other process influenced
the host scheduler.

This measures only this executable's waits. In particular it does **not** show
that QEMU inherited this process's timer request, that QEMU's GLib poll uses the
same primitive, or that any observed audio packet loss has been fixed.

Primary Microsoft contracts consulted on 2026-10-21, without kit samples:

- [Sleep](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep): scheduling and clock resolution affect actual sleep duration.
- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject): timeout units/results, object lifetime and window message-pumping obligations.
- [timeGetDevCaps](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timegetdevcaps): reported minimum/maximum period and success return.
- [timeBeginPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod): requests must be paired; Windows 10 2004 and later process scope, Windows 11 occlusion caveat, and no improvement to QPC precision.
- [timeEndPeriod](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timeendperiod): release uses the same period and its return must be checked.

The Microsoft pages describe public API contracts. No Microsoft or vendor
binary code, disassembly, sample driver, sample library or sample INF was read
or adapted for this diagnostic.

## Quiet/hidden host run

Host: Windows 11 Pro 10.0.26300. Corrected executable SHA256
`43C007890BA8040C0056EC1A92BEFBACD931A254429D29C454FB0124D44AB80D`.
Report `E:\usb-wdm-free\wave2\host-waits\HOST.TXT` SHA256
`DA37485163F61CF090877B2CE67CA932FA7AE0AB91FE0589432DC3B60D3F19AF`.
PID 19708 exited normally with code 0 after host Stopwatch 24178.8704 ms;
the process was confirmed absent afterward. There was no audio guest or
active USB experiment. A separate floppy guest briefly booted just before the
hold and was then CPU-paused; initial boot disk I/O may have overlapped this
probe's start. Do not attribute maximum jitter solely to the wait API.

| Phase | Wait | n | Min ms | Median ms | p90 ms | p99 ms | Max ms | Mean ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | Sleep(1) | 256 | 14.126300 | 15.608800 | 16.033300 | 16.333700 | 16.706200 | 15.630896 |
| Baseline | Event timeout(1) | 256 | 8.094200 | 15.616650 | 16.081300 | 16.368500 | 16.470400 | 15.603007 |
| Requested | Sleep(1) | 256 | 13.983200 | 15.616400 | 16.103600 | 16.401500 | 16.462500 | 15.641527 |
| Requested | Event timeout(1) | 256 | 14.219300 | 15.634650 | 16.062700 | 16.352600 | 16.756100 | 15.634867 |
| After end | Sleep(1) | 256 | 14.051900 | 15.553700 | 16.056400 | 16.415700 | 16.546400 | 15.600838 |
| After end | Event timeout(1) | 256 | 14.028400 | 15.569950 | 16.016900 | 16.481100 | 16.809200 | 15.615357 |

`timeBeginPeriod(1)` and matching `timeEndPeriod(1)` both returned 0; the final
report records `timer_request_release_unconfirmed=0`. Requesting 1 ms did not
improve this quiet/hidden process's measured waits. This is compatible with
the documented Windows 11 caveat but does not establish its cause, the effect
of a visible window, or the behavior of QEMU's different process/event loop.

## Explicit-honor quiet/hidden host run

After the audio owner confirmed its physical guest PID 17068 and monitor
44333 closed, and the webcam owner held its next guest/build, the same host
ran `/quiet /honor` once. PID 39400 exited normally with code 0 after host
Stopwatch 17078.4921 ms and was confirmed absent. No VM was started during
this probe. Executable SHA256 remains
`E66D6FCD04E2F14E9BCC75E2A70C8627C2B43F8F6901A36B72D33DAFC2E0482C`.
`E:\usb-wdm-free\wave2\host-waits\HONOR.TXT` SHA256:
`8454E88963378500029D851D893A7E4D7F9FF03583311DB4E409C2CF31A2F7F5`.

| Phase | Wait | n | Min ms | Median ms | p90 ms | p99 ms | Max ms | Mean ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | Sleep(1) | 256 | 6.384100 | 15.720250 | 16.065100 | 16.567400 | 16.661700 | 15.643444 |
| Baseline | Event timeout(1) | 256 | 7.110100 | 15.723950 | 16.073900 | 16.459400 | 16.521200 | 15.681577 |
| Requested, explicit honor | Sleep(1) | 256 | 1.007700 | 1.999400 | 2.027600 | 2.470200 | 2.521600 | 1.873068 |
| Requested, explicit honor | Event timeout(1) | 256 | 0.702100 | 1.370300 | 2.019300 | 2.174800 | 2.283900 | 1.487606 |
| After end and policy restore | Sleep(1) | 256 | 14.779000 | 15.868450 | 16.049800 | 16.361900 | 16.665000 | 15.721927 |
| After end and policy restore | Event timeout(1) | 256 | 14.620200 | 15.687150 | 16.196200 | 16.661100 | 16.775500 | 15.681448 |

All three power-policy readbacks reported Version 1. Original ControlMask/
StateMask were `00000000/00000000`; honored state was `00000004/00000000`;
restored state exactly matched `00000000/00000000`. `timeBeginPeriod(1)` and
`timeEndPeriod(1)` both returned 0. Final markers were
`timer_request_release_unconfirmed=0 power_restore_unconfirmed=0`.

The process's wait distributions improved during the combined explicit-honor
policy and 1 ms request, then returned to roughly 15.7 ms after restoration.
The earlier hidden run's request alone did not show that improvement. This is
direct evidence about this process on this host, consistent with the public
policy contract. It does not prove QEMU uses the same waits, establish that
QEMU inherited any request, or certify audio transport reliability. No QEMU
source, host driver binding or persistent policy was changed by either run.

## Exact-PID external policy helper (prepared)

Source review found that the experimental QEMU source already calls
`timeGetDevCaps` / `timeBeginPeriod(min)` at startup and registers an end call
in `os-win32.c`. To isolate timer-request honoring without replacing that
QEMU executable or adding a duplicate period request, the original
`src/generic/usbprobe/hostpolicy.c` helper temporarily controls only bit 4 in
one explicitly supplied process. It makes **no timer-resolution API call**.

`hostpol.cmd` builds `E:\usb-wdm-free\wave2\host-policy\HOSTPOL.EXE`.
Initial executable SHA256:
`9C92DCFC79B0455137AF23A8985AA61326DE569714576852C9A28C2E8AB8E2B7`.
The source builds cleanly under VC6 `/W3 /WX`. Before invocation, the owner
must verify the exact target PID, executable path and hash independently.
The helper holds the opened process handle with query, set-information and
synchronize rights, preventing PID recycling from selecting a replacement.
It snapshots the complete public state, preserves every bit except timer
ignoring, checks readback, and flushes both its E: report and console marker.
Unsupported API or state retrieval fails closed before applying policy.

Run in a persistent interactive process:

```text
HOSTPOL.EXE <verified-QEMU-PID> E:\absolute\POLICY.TXT
```

Wait for `HONOR_ACTIVE` before measurement. After audio stops, send Enter
while QEMU is still running. Require `RESTORED` and successful helper exit
before shutting down QEMU. EOF also attempts restoration but returns a
nonzero result. Failed apply/readback and report errors take restoration
paths; failed restoration gets one cleanup retry and retains an explicit
unknown-state flag if unsuccessful. A target that exits before restoration
cannot provide verified readback and is reported as an error.

Forced termination of this external helper cannot guarantee rollback in a
different live process; use the Enter/verified-restoration lifecycle. The
helper does not claim to survive termination, inject into QEMU, set registry
policy, or alter machine-wide settings. No target process has been attached
by the preparation/build/host-test work recorded here.

`poltest.cmd` includes the **actual `hostpolicy.c` core** with target/API mocks.
All 3400 checks passed in `E:\usb-wdm-free\wave2\host-policy-test`. Coverage
includes preservation of unrelated mask/state bits, absent APIs, query/set
and readback failures, report-error rejection, partial-apply rollback,
restoration retry, idempotent successful cleanup and target exit. These are
core contract tests, not a real cross-process policy test; OpenProcess,
interactive stdin and actual Windows policy effects still require the
coordinated target run. No mocked test calls a real process-policy API.
