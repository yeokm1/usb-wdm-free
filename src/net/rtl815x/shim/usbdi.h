/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host-test stand-in for the USB declarations netusb.c uses (see
 * shim/wdm.h). The field names are those of the documented URB
 * structures; the layouts are this test's own. URB function codes and
 * USBD status values are the documented ones. Identical copy in both
 * packages.
 */
#ifndef SHIM_USBDI_H
#define SHIM_USBDI_H

#include "wdm.h"

typedef LONG USBD_STATUS;
typedef PVOID USBD_PIPE_HANDLE, USBD_CONFIGURATION_HANDLE, USBD_INTERFACE_HANDLE;
#define USBD_SUCCESS(s) ((USBD_STATUS)(s) >= 0)
#define USBD_STATUS_SUCCESS ((USBD_STATUS)0)
#define USBD_STATUS_CANCELED ((USBD_STATUS)0xC0010000L)

#define URB_FUNCTION_SELECT_CONFIGURATION          0x0000
#define URB_FUNCTION_SELECT_INTERFACE              0x0001
#define URB_FUNCTION_ABORT_PIPE                    0x0002
#define URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER    0x0009
#define URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE    0x000B
#define URB_FUNCTION_VENDOR_DEVICE                 0x0017
#define URB_FUNCTION_RESET_PIPE                    0x001E
#define USBD_TRANSFER_DIRECTION_IN 0x01
#define USBD_SHORT_TRANSFER_OK     0x02
#define IOCTL_INTERNAL_USB_SUBMIT_URB 0x00220003UL

#define USB_DEVICE_DESCRIPTOR_TYPE        0x01
#define USB_CONFIGURATION_DESCRIPTOR_TYPE 0x02
#define USB_STRING_DESCRIPTOR_TYPE        0x03

#pragma pack(push, 1)
typedef struct _USB_CONFIGURATION_DESCRIPTOR {
    UCHAR bLength, bDescriptorType;
    USHORT wTotalLength;
    UCHAR bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
} USB_CONFIGURATION_DESCRIPTOR, *PUSB_CONFIGURATION_DESCRIPTOR;
#pragma pack(pop)

typedef enum { UsbdPipeTypeControl, UsbdPipeTypeIsochronous, UsbdPipeTypeBulk, UsbdPipeTypeInterrupt } USBD_PIPE_TYPE;

typedef struct _USBD_PIPE_INFORMATION {
    USHORT MaximumPacketSize;
    UCHAR EndpointAddress, Interval;
    USBD_PIPE_TYPE PipeType;
    USBD_PIPE_HANDLE PipeHandle;
    ULONG MaximumTransferSize, PipeFlags;
} USBD_PIPE_INFORMATION, *PUSBD_PIPE_INFORMATION;

typedef struct _USBD_INTERFACE_INFORMATION {
    USHORT Length;
    UCHAR InterfaceNumber, AlternateSetting, Class, SubClass, Protocol, Reserved;
    USBD_INTERFACE_HANDLE InterfaceHandle;
    ULONG NumberOfPipes;
    USBD_PIPE_INFORMATION Pipes[1];
} USBD_INTERFACE_INFORMATION, *PUSBD_INTERFACE_INFORMATION;

struct _URB;
struct _URB_HEADER { USHORT Length, Function; USBD_STATUS Status; PVOID UsbdDeviceHandle; ULONG UsbdFlags; };
struct _URB_SELECT_CONFIGURATION {
    struct _URB_HEADER Hdr;
    PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor;
    USBD_CONFIGURATION_HANDLE ConfigurationHandle;
    USBD_INTERFACE_INFORMATION Interface;
};
struct _URB_PIPE_REQUEST { struct _URB_HEADER Hdr; USBD_PIPE_HANDLE PipeHandle; ULONG Reserved; };
struct _URB_CONTROL_DESCRIPTOR_REQUEST {
    struct _URB_HEADER Hdr;
    PVOID Reserved; ULONG Reserved0;
    ULONG TransferBufferLength; PVOID TransferBuffer; PVOID TransferBufferMDL; struct _URB *UrbLink;
    USHORT Reserved1; UCHAR Index, DescriptorType; USHORT LanguageId, Reserved2;
};
struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST {
    struct _URB_HEADER Hdr;
    PVOID Reserved; ULONG TransferFlags;
    ULONG TransferBufferLength; PVOID TransferBuffer; PVOID TransferBufferMDL; struct _URB *UrbLink;
    UCHAR RequestTypeReservedBits, Request; USHORT Value, Index, Reserved1;
};
struct _URB_BULK_OR_INTERRUPT_TRANSFER {
    struct _URB_HEADER Hdr;
    USBD_PIPE_HANDLE PipeHandle; ULONG TransferFlags;
    ULONG TransferBufferLength; PVOID TransferBuffer; PVOID TransferBufferMDL; struct _URB *UrbLink;
};
typedef struct _URB {
    union {
        struct _URB_HEADER UrbHeader;
        struct _URB_SELECT_CONFIGURATION UrbSelectConfiguration;
        struct _URB_PIPE_REQUEST UrbPipeRequest;
        struct _URB_CONTROL_DESCRIPTOR_REQUEST UrbControlDescriptorRequest;
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST UrbControlVendorClassRequest;
        struct _URB_BULK_OR_INTERRUPT_TRANSFER UrbBulkOrInterruptTransfer;
    };
} URB, *PURB;

#endif
