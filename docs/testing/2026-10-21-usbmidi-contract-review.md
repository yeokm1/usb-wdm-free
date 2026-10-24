# USB MIDI pre-load contract review, 2026-10-21

This independent read-only review covers the new exact-simulator
`src/audio/usbmidi/usbmidi.cpp`, `umusb.c/.h` and `usbmidi.inf` before the
first kernel VM load. It does not establish installation, WinMM endpoint
enumeration, callback delivery, MIDI loopback or real-device compatibility.
The implementation owner edits these files; this reviewer changes no MIDI
or UAC driver source and starts no guest.

Sources are published PortCls/KS documentation and the installed
`ntddk/inc/portcls.h`, `unknown.h`, `ksmedia.h`, USB/WDM declarations.
No implementation is adopted from a DDK/WDK sample, Microsoft/vendor
binary code or disassembly.

## Findings sent to the owner

1. **STOP/output ordering requires a drain barrier.** The initial worker
   removed raw bytes from the queue under its spin lock, then encoded and
   submitted USB OUT after releasing that lock. Concurrent render STOP
   cleared the queue and returned, while that previously dequeued data
   could still be transmitted. Closing and reopening a stream could thus
   allow an old stream's MIDI events after STOP or into the next stream's
   lifetime. Queue clearing alone is insufficient. The owner is adding
   serialized state transitions, a generation check and worker/OUT drain
   acknowledgement before completing STOP. The worker must signal the
   barrier on terminal error/exit as well; no spin lock may span a USB wait.
   This is a source-level race, not an observed VM failure.
2. **Final-release cleanup needs a proven IRQL/lifetime contract.** Initial
   `Stream::Release()` immediately invoked its destructor, and owner
   `Mini::Release()` could immediately destroy the USB transport, whose
   stop joins a thread at PASSIVE_LEVEL. A blocking stream-close barrier
   makes direct elevated final destruction illegal as well. The inspected
   inherited `IUnknown` declarations do not provide a PASSIVE-only final
   release guarantee. Protect elevated final release with a referenced-FDO
   I/O work item (as used by the separate UAC stream helper), or establish
   an explicit documented caller guarantee. Pending work must retain the
   device and driver lifetime through its final cleanup. This is an
   unclosed proof obligation, not a claim that PortCls was observed calling
   final Release at DISPATCH_LEVEL.
3. **Port-format channel count was incorrect.** The initial
   `KSDATARANGE_MUSIC` used `KSMUSIC_TECHNOLOGY_PORT` with Channels=16.
   The [published structure members](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-ksdatarange_music)
   define Channels/Notes for internal synthesizers and require zero for a
   port. Notes=0 and ChannelMask=FFFF already match a port transmitting
   all channels. Set Channels=0; the 16-channel MIDI protocol does not make
   that synthesizer-specific field 16.
4. **D3 did not support D0 restart.** The initial power callback called
   permanent `Shutdown()` on non-D0 and did nothing on D0. That closes the
   transport and marks the miniport dead permanently. The owner now
   reports rejecting optional non-D0 power queries and documenting forced
   power-down as drained but non-resumable until restart. This is an
   explicit prototype limit, not a completed suspend/resume implementation.
5. **Data intersection has an endpoint-discovery proof gap.** The initial
   handler returned STATUS_NOT_IMPLEMENTED for every MUSIC/MIDI request.
   [Default data-intersection documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/default-data-intersection-handlers)
   guarantees the generic fallback for mono/stereo PCM; it does not
   establish a MUSIC-format guarantee. A bounded explicit MIDI intersection
   or direct KS/WinMM runtime proof is preferable. This review has not
   established whether the Windows 2000 PortMidi implementation provides
   a further MIDI-specific fallback, so this is not classified as a proven
   kernel-load failure. An explicit handler needs correct wildcard matching
   and size-query/buffer statuses.

## Contracts that match the inspected design

- [IMiniportMidiStream::SetState](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportmidistream-setstate)
  is documented at **PASSIVE_LEVEL**. It may use a passive drain barrier;
  the draft's former blanket DISPATCH claim should not be applied to it.
  STOP means transport is stopped, and NewStream starts in STOP.
- [Read](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportmidistream-read)
  and [Write](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportmidistream-write)
  are DISPATCH_LEVEL callbacks. The ring operations are nonpaged and use
  a spin lock rather than USB waits. Read's empty result is success/zero.
  A partial Write count must be a multiple of four; rounding a partial
  acceptance down to four in the original transport follows this contract
  and should not be removed merely because the stored data are raw bytes.
- [RegisterServiceGroup](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportmidi-registerservicegroup)
  is PASSIVE_LEVEL and follows COM ownership rules. A group from
  PcNewServiceGroup retained by the miniport, plus separate output AddRefs
  from [Init](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportmidi-init)
  and [NewStream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iminiportmidi-newstream),
  is consistent with those contracts. No missing group output AddRef was
  found. PortCls's own registered/member references still need ordinary
  close/teardown testing.
- [IPortMidi::Notify](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/portcls/nf-portcls-iportmidi-notify)
  permits any IRQL and allows methods other than a physical ISR for deciding
  when to notify. A passive USB worker notifying actual input/output-space
  progress is therefore not itself an IRQL violation. The borrowed port
  pointer requires worker drain before port teardown; the pre-forward PnP
  stop design is important to that lifetime argument.
- Pin dataflow and IRP communication direction are distinct. Capture
  DATAFLOW_OUT does not by itself require COMMUNICATION_SOURCE: a capture
  pin can receive read IRPs. No change to capture SINK is justified solely
  from data direction. See
  [pin communication semantics](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/ksproperty-pin-communication).
- MUSIC/MIDI/NONE stream pins, raw MIDI port technology, MIDI_BUS bridge
  ranges, render/capture filter categories and the INF's matching `Midi`
  subdevice reference are internally consistent with an external raw MIDI
  port. [MIDI and DirectMusic filters](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/midi-and-directmusic-filters)
  distinguishes this from an internal synthesizer; no synth-category/node
  addition is justified merely to make a MIDI port.

The exact simulator descriptor/VID/PID checks intentionally reject other
devices. Matching both a whole-device ID and an MI_00 child in the INF is
not proof that both PDO views return the exact whole configuration that
the transport requires; record the actual installed PDO and returned
descriptor bounds in the VM run.

The owner is applying fixes concurrently. This record preserves the
findings and source/API scope, not an assertion that every later revision
has been re-reviewed or that kernel loading is approved by a host build.

## Targeted source re-review after fixes

The subsequent generation/RenderIdle implementation was read again. Its
idle acknowledgement follows old OUT retirement and encoder reset, and
the terminal worker path signals the barrier. Both Mini and Stream now
preallocate device-associated work items and retain separate FDO references
until passive destruction, work-item free and final device dereference.
The nested stream-owner release retains the stream's own FDO reference
through that cleanup. The explicit MUSIC/MIDI_BUS intersection bounds its
result to a KSDATAFORMAT and handles size query, short buffer and wildcard
matching. Port Channels is now zero; optional non-D0 queries are refused.
No additional blocking source issue was found in those targeted fixes.
This re-review does not claim their kernel execution was tested here.
