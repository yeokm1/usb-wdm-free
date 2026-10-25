/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * USB MIDI 1.0 transport for one MIDIStreaming function; include wdm.h first.
 */
#ifndef UMUSB_H
#define UMUSB_H
typedef struct UM_USB UM_USB;
typedef VOID (*UM_NOTIFY)(PVOID context);
typedef struct UM_USB_STATS {
    ULONG TxBytes, RxBytes, TxUrbs, RxUrbs, BadEvents, Overruns, Timeouts;
    NTSTATUS LastStatus;
    ULONG Stalls, Errors, Elevated;
} UM_USB_STATS;
typedef struct UM_USB_INFO {
    USHORT Vendor, Product, Revision, Speed; /* Speed: MaxPacket of the first pipe */
    UCHAR Interface, Alternate, OutPipes, InPipes;
    ULONG Outputs, Inputs, Filters;
} UM_USB_INFO;
/* Create reads the descriptors, chooses the MIDIStreaming interface and its
 * ports, selects the configuration and reads the names. PASSIVE_LEVEL.
 * Start launches the OUT worker and the IN requests; Stop is permanent and
 * waits for cancellation and the worker's exit; Destroy stops and frees.
 * Power(FALSE) pauses (D3: IN cancelled, OUT held), Power(TRUE) resumes.
 * Caller serializes Create/Start/Stop/Power/Destroy.
 */
NTSTATUS UmUsbCreate(PDEVICE_OBJECT lower, UM_USB **out);
VOID UmUsbGetInfo(UM_USB *s, UM_USB_INFO *info);
/* Name of filter i (pairs output port i and input port i), NUL-terminated. */
VOID UmUsbFilterName(UM_USB *s, ULONG filter, WCHAR name[32]);
BOOLEAN UmUsbHasPort(UM_USB *s, BOOLEAN capture, ULONG port);
NTSTATUS UmUsbStart(UM_USB *s);
VOID UmUsbStop(UM_USB *s);
VOID UmUsbPower(UM_USB *s, BOOLEAN on);
VOID UmUsbDestroy(UM_USB *s);
VOID UmUsbGetStats(UM_USB *s, UM_USB_STATS *out);
/* Bind/unbind (notify NULL) a port's data/space notification. PASSIVE;
 * unbinding waits for a notification already in progress. */
VOID UmUsbBind(UM_USB *s, BOOLEAN capture, ULONG port, UM_NOTIFY notify, PVOID context);
/* Read/Write: <= DISPATCH_LEVEL. State: PASSIVE_LEVEL; a render STOP or
 * PAUSE returns only after OUT data already taken from the ring retired. */
NTSTATUS UmUsbWrite(UM_USB *s, ULONG port, PVOID bytes, ULONG size, PULONG used);
NTSTATUS UmUsbRead(UM_USB *s, ULONG port, PVOID bytes, ULONG size, PULONG used);
NTSTATUS UmUsbState(UM_USB *s, BOOLEAN capture, ULONG port, ULONG state);
#endif
