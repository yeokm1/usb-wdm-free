# STREAM.SYS capture buffer contract review, 2026-10-21

This is a source/design review for the original ROOT\UVCSIM98 software-frame
experiment, not evidence of a USB camera driver or a successful kernel run.
Only published Microsoft documentation and installed public headers were used.
No kit samples, Microsoft binary internals, or ReactOS implementation were read
for this review.

## Mapping decision

`HW_STREAM_OBJECT.Pio = TRUE` identifies a programmed-I/O stream. Its documented
definition does not itself promise that `KSSTREAM_HEADER.Data` is an address
usable by an arbitrary system worker. The generic header documentation describes
`Data` as a virtual address, without that stronger address-space guarantee.

The explicit, bounded alternative is to map the existing stream-buffer MDL.
Microsoft's [KsProbeStreamIrp documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ks/nf-ks-ksprobestreamirp)
separates the two objects: headers are placed in
`Irp->AssociatedIrp.SystemBuffer`, while stream-buffer MDLs are linked from
`Irp->MdlAddress`. Thus the first MDL represents stream data, rather than the
header array, under this documented KS IRP preparation contract. This is not a
reason to call KsProbeStreamIrp again or replace the class driver's MDLs.

The [Stream Class overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/streaming-minidrivers2)
assigns buffer locking and flushing for PIO to the class driver. The
[SRB structure contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/strmini/ns-strmini-_hw_stream_request_block)
identifies `Irp` as the request IRP, and `DataBufferArray` plus `NumberOfBuffers`
as its stream-data headers. Taken together, these support a deliberately narrow
single-header, single-MDL implementation. Actual target behavior still needs a
recorded kernel run; reject unsupported layouts rather than guessing an address.

## Bounded first implementation

- Require `SRB_READ_DATA`, one header, nonnull request IRP, one nonnull MDL,
  and `Mdl->Next == NULL`. Reject multiple buffers until their exact association
  is implemented.
- Check the header's declared size, ordinary data-buffer semantics, fixed-format
  frame extent, and the MDL byte count before writing. Require at least the
  complete frame in both bounds. Requiring the MDL to cover the entire advertised
  `FrameExtent` is a conservative consistency check. Reject unsupported format
  changes or special buffer representations; a virtual pointer alone proves
  neither accessibility nor capacity.
- Use `MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority)`; handle NULL as a
  mapping failure. Its returned address includes the MDL's original byte offset:
  **do not add `MmGetMdlByteOffset` again**. Serialize mapping/access to each MDL.
- Write only the fixed frame length through the mapped system address; leave
  the original header `Data` field intact. Return actual valid bytes in
  `DataUsed`, never exceeding `FrameExtent`.
- Do not unlock, free, replace, or explicitly unmap the class-owned MDL. Retain
  the request until the worker has finished all accesses and then complete it
  once. Never retain its mapped address or header after completion.

The [mapping API contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmgetsystemaddressformdlsafe)
provides a nonpaged system-space mapping for already locked pages and permits
calls through DISPATCH_LEVEL. It explicitly excludes Windows 98. This choice
therefore defines an **NT 2000-or-later prototype**, even if the package name
contains `98`; it supplies no Windows 98 runtime claim. A separate 98-compatible
mapping and lifetime design would need review before claiming that target.

## Queue and teardown obligations

The [SRB_READ_DATA contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/srb-read-data)
permits accepting reads in PAUSE and RUN and requires immediate return in STOP.
Queueing a PAUSE buffer is distinct from publishing a frame while paused.
The SRB structure documentation directs a minidriver to disable request timeout
while queued (`TimeoutCounter = 0`) and restore the original timeout when
processing resumes. Timeout, cancellation and worker completion must share one
ownership state so that only one path completes a request.

Setting `TurnOffSynchronization = TRUE` transfers synchronization responsibility
to the minidriver. A passive worker does not make cancel or timeout callbacks
passive. Those callbacks may mark cancellation and wake work, but must not wait
for that same worker at elevated IRQL. Close, STOP, flush and uninitialize must
close admission, retire active work, drain queued reads and establish worker
rundown before releasing stream/device state. A completion callback can trigger
teardown; capture any required notification context beforehand and do not access
the completed SRB afterwards.

The proposed implementation was communicated directly to its owner before code
integration. This document records the contract and review limits; it is not a
claim that the implementation has passed those runtime races.

## Local header checks

`tools/ntddk/inc/ddk/wdm/strmini.h` declares `CommandData.DataBufferArray`,
`NumberOfBuffers`, the original `Irp`, and the `Dma`/`Pio` booleans consistently
with these published interfaces. `tools/ntddk/inc/ks.h` declares the stream header
and `KsProbeStreamIrp` flags. These are installed public headers, not sample
drivers. No source was adapted from either header into this review.
