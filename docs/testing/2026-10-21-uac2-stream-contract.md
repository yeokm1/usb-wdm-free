# Original UAC2 stream/DMA contract checks, 2026-10-21

`src/audio/uac2/uacstream.h` is an original bounded stream implementation.
The input format is stereo signed 16-bit PCM at 48 kHz, including the
equivalent extensible representation. A zeroed 16,384-byte nonpaged ring
backs the stream. Submission and USB-completion positions are separate.
Only completion callbacks advance the reported position or trigger
accumulated byte-based notifications; there is no simulated playback clock.

The implementation follows public kit headers and Microsoft's published
[DMA channel object contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/dma-channel-objects),
which permits miniport-owned IDmaChannel implementations. The
[NewStream contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportwavecyclic-newstream)
specifies an initially stopped stream, output reference ownership and the
six DMA methods consumed by WaveCyclic. Unused physical-DMA accessors do
not invent a hardware adapter or physical address.

Create requires the FDO, preallocates a device-associated I/O work item,
and retains the FDO and owner. Final Release at elevated IRQL defers Close,
transport drain and object destruction to passive cleanup. Hook publication
occurs only after allocation succeeds: failure cleanup must not reenter
the caller's Wave mutex. Independent stream/DMA output references share
one object, and its active-stream gate stays set until final cleanup.

`streamtest.cmd` builds/runs `streamtest.cpp` with repository MSVC6 and
public DDK headers. The test includes the actual stream header, with
deterministic kernel/service-group test doubles. Its recorded PASS covers:

- Truncated format rejection and inconsistent PCM byte rate.
- Object/ring/service-group/work-item allocation failure cleanup.
- One-stream gate and independent COM release ordering.
- Ring bounds, wraparound and preservation of output canaries.
- USB-completion notification accumulation, paused submission rewind and
  STOP position/ring reset.
- Boundary values and 5,000 random byte-to-time normalization cases,
  compared with an independent 64-bit arithmetic oracle.
- Final Release at simulated DISPATCH_LEVEL: resources and gate remain
  owned until the queued passive callback runs.

VC6 x86 and WDK 7.1 x64 compile probes of the header pass checked-build
flags with `/W3 /WX`. The x86 object introduces no implicit CRT arithmetic
or delete helpers. Logs/probes are in ignored `out/audio/uac2/`; the
repeatable host check is `cmd /c src\audio\uac2\streamtest.cmd`.

These checks establish source contracts and deterministic ownership cases.
They do not establish kernel scheduling, PnP/power races, USB completion
timing, audible playback, or compatibility with every target OS. Actual
device/VM results belong in their runtime logs. No Microsoft sample,
driver binary code, disassembly or memory dump was used.
