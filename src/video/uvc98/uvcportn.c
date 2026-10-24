/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Actual NT2000+ hooks; not linked into M0. M1 native start/cleanup and
 * mapped frame delivery remain unresolved; see the worker run record.
 */
#include "uvcport.h"
#include "uvctrace.h"
PUCHAR UvPortNtMap(PMDL mdl, PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    /* MmGetSystemAddressForMdlSafe imports MmMapLockedPagesSpecifyCache,
     * which Windows 98 SE's NTKERN does not export; MmMapLockedPages is in
     * every WDM system (the 2000 DDK's own MmGetSystemAddressForMdl). */
    if(mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA|MDL_SOURCE_IS_NONPAGED_POOL))
        return (PUCHAR)mdl->MappedSystemVa;
    return (PUCHAR)MmMapLockedPages(mdl,KernelMode);
}
VOID UvPortNtNotify(PHW_STREAM_REQUEST_BLOCK s, PHW_STREAM_OBJECT stream,
    int deviceRequest, PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    if(deviceRequest) {
        PVOID device=s->HwDeviceExtension;
        StreamClassDeviceNotification(DeviceRequestComplete,device,s);
    } else StreamClassStreamNotification(StreamRequestComplete,stream,s);
    /* No ReadyForNext: this adapter requires TurnOffSynchronization=TRUE. */
}
VOID UvPortNtSerialNotify(PHW_STREAM_REQUEST_BLOCK s, PHW_STREAM_OBJECT stream,
    int deviceRequest, PVOID context)
{
    UVHEX("COMPLETE_SRB ",s->Command);
    UVHEX("COMPLETE_STATUS ",s->Status);
    UNREFERENCED_PARAMETER(stream);
    UNREFERENCED_PARAMETER(deviceRequest);
    UNREFERENCED_PARAMETER(context);
    /* Called ONLY from a class-serialized callback/HIGH priority routine,
     * with TurnOffSynchronization=FALSE and no ISR in the first prototype.
     * One published operation transfers SRB and reopens its class queue.
     * No caller-side class-pointer access occurs after this handoff. */
#ifndef UVWORK_USB
    /* M2 (UVWORK_USB) reopens the device queue after UNINITIALIZE too. On
     * 98 SE and 2000 a surprise removal uninitializes the device while a
     * client still has its stream open; that client's later CLOSE_STREAM
     * (and everything it waits on) needs the queue. Completed without
     * reopening it, the client hung for good (VfW mapper, CamCheck;
     * docs/testing/2026-10-24-uvc98-vfw.md). The one call completes and
     * reopens, so there is no class access after the device may go. */
    if(s->Command==SRB_UNINITIALIZE_DEVICE) {
        PVOID device=s->HwDeviceExtension;
        StreamClassDeviceNotification(DeviceRequestComplete,device,s);
    } else
#endif
    StreamClassCompleteRequestAndMarkQueueReady(s);
}
