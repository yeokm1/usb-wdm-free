/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Experimental NT2000+ synthetic engine; class serialization ON, no ISR.
 * Native start/cleanup remains unresolved; see the worker run record.
 */
#ifndef UVC_WORK_H
#define UVC_WORK_H
#include "uvcport.h"
typedef struct UVWORK UVWORK;
/* Lower is used only by the UVWORK_USB (M2) build: the attached device
 * object (ConfigInfo->PhysicalDeviceObject) that receives USB requests. */
typedef struct UVWORK_ANCHOR { UVWORK *Engine; PDEVICE_OBJECT Lower; } UVWORK_ANCHOR;
/* All public entrypoints must run in the default Stream Class serialized
 * callback domain. No callback waits/maps/joins. INITIALIZE stays owned
 * until LOW bootstrap and its LowToHigh completion finish it. Caller validates
 * ConfigInfo and sets StreamDescriptorSize before calling Initialize.
 * Success/PENDING transfers INITIALIZE ownership; failure does not.
 */
NTSTATUS UvwInitialize(UVWORK_ANCHOR *anchor, PHW_STREAM_REQUEST_BLOCK init,
    PDEVICE_OBJECT classFdo);
/* Class-serialized opaque query; never dereferences the request. A duplicate
 * callback on the still-owned INIT must not notify or mutate that SRB. */
int UvwOwnsInitialize(UVWORK_ANCHOR *anchor,const void *request);
int UvwControl(UVWORK_ANCHOR *anchor, PHW_STREAM_REQUEST_BLOCK request,
    unsigned int command, unsigned int state);
int UvwRead(UVWORK_ANCHOR *anchor, PHW_STREAM_REQUEST_BLOCK request);
void UvwCancel(UVWORK_ANCHOR *anchor, const void *request);
#ifdef UVWORK_USB
#include "uvcfmt.h"
/* M2: the camera's advertised formats (NULL before INIT completes). */
const UVFMT *UvwFormats(UVWORK_ANCHOR *anchor);
/* M2: record the format an OPEN_STREAM named; returns 0 if invalid. */
int UvwSelectFormat(UVWORK_ANCHOR *anchor, int index);
int UvwSelected(UVWORK_ANCHOR *anchor);
/* M2: PROPSETID_VIDCAP_DROPPEDFRAMES counts (class-serialized caller). */
void UvwFrameCounts(UVWORK_ANCHOR *anchor, ULONG *pictures, ULONG *drops, ULONG *frameBytes);
#endif
#endif
