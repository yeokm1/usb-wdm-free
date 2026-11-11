/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host-test stand-in for the USB declarations cdcusb.c uses beyond the
 * shared core's (shim/usbdi.h): the URBs of a class request to an
 * interface, GET_STATUS and SELECT_INTERFACE. The field names are those
 * of the documented URB structures; the layouts are this test's own. The
 * function codes are the documented ones. Host-test code only (cdcusbtest).
 */
#ifndef USBTEST_USBDI_H
#define USBTEST_USBDI_H

#include "../shim/usbdi.h"

#define URB_FUNCTION_GET_STATUS_FROM_DEVICE 0x0013
#define URB_FUNCTION_CLASS_INTERFACE        0x001B

struct _URB_CONTROL_GET_STATUS_REQUEST {
    struct _URB_HEADER Hdr;
    PVOID Reserved; ULONG Reserved0;
    ULONG TransferBufferLength; PVOID TransferBuffer; PVOID TransferBufferMDL; struct _URB *UrbLink;
    UCHAR Reserved1[4]; USHORT Index, Reserved2;
};

struct _URB_SELECT_INTERFACE {
    struct _URB_HEADER Hdr;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;
    USBD_INTERFACE_INFORMATION Interface;
};

#endif
