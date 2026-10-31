/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * usbprn.h - the device interface of USBPRN.SYS, shared with USBPRNMN.DLL
 * (docs/design/printing.md 3.2, 5.3, 5.4).
 *
 * Each printer is \Device\UsbPrnF<k> with the link \DosDevices\USBPRN<n>;
 * port USB00n of USBPRNMN.DLL opens \\.\USBPRN<n>.
 *
 * The public codes have the values Windows' own USB printing driver
 * answers on later systems (function numbers 12 to 16, FILE_DEVICE_UNKNOWN,
 * METHOD_BUFFERED, FILE_ANY_ACCESS), so tools written for that interface
 * work here. The values are written out here, not taken from a header.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef USBPRN_H
#define USBPRN_H

/* CTL_CODE(FILE_DEVICE_UNKNOWN (0x22), function, METHOD_BUFFERED, FILE_ANY_ACCESS) */
#define UP_CTL(fn) ((0x22UL << 16) | ((unsigned long)(fn) << 2))

#define IOCTL_USBPRINT_GET_LPT_STATUS       UP_CTL(12)  /* out: 1 status byte */
#define IOCTL_USBPRINT_GET_1284_ID          UP_CTL(13)  /* out: length (2, big-endian) + id */
#define IOCTL_USBPRINT_VENDOR_SET_COMMAND   UP_CTL(14)  /* STATUS_NOT_SUPPORTED */
#define IOCTL_USBPRINT_VENDOR_GET_COMMAND   UP_CTL(15)  /* STATUS_NOT_SUPPORTED */
#define IOCTL_USBPRINT_SOFT_RESET           UP_CTL(16)

/* Private (design 5.4). */
#define IOCTL_USBPRN_SET_TIMEOUTS           UP_CTL(0x800)   /* in: UP_TIMEOUTS */
#define IOCTL_USBPRN_GET_INFO               UP_CTL(0x801)   /* out: UP_INFO */

typedef struct UP_TIMEOUTS {
    unsigned long WriteMs;      /* a write that moves nothing for this long returns short; 0 = never */
    unsigned long ReadMs;       /* a read with no data for this long returns 0 bytes; 0 = 1000 ms */
} UP_TIMEOUTS;

#define UP_INFO_VERSION     1

#define UP_F_PRESENT        0x0001  /* device present and started */
#define UP_F_BIDI           0x0002  /* a bulk IN pipe is in use */
#define UP_F_1284_4         0x0004  /* protocol 03 only: data refused */
#define UP_F_STATUS_VALID   0x0008  /* LastStatus was read */
#define UP_F_WRITE_SHORT    0x0010  /* the last write returned short (timed out) */
#define UP_F_RESET_OTHER    0x0020  /* SOFT_RESET goes as 23h (the 1.0 code) */

typedef struct UP_INFO {
    unsigned long  Version;     /* UP_INFO_VERSION */
    unsigned long  Flags;       /* UP_F_* */
    unsigned short Vid, Pid;
    unsigned char  Interface, Alternate, Protocol, LastStatus;
    unsigned long  PortNumber;  /* n of USB00n */
    unsigned long  LastWriteBytes;
    unsigned long  WriteMs, ReadMs;
    char           Description[96];     /* from the 1284 id */
} UP_INFO;

#endif
