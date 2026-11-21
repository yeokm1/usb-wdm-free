/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cfgwalk.h - USBMTP.SYS's walks of the device and configuration
 * descriptors (docs/design/mtp.md 3.3): pure C with no kernel calls, so the
 * build host tests them with truncated and malformed descriptors
 * (cfgtest.c). Every byte of a descriptor is untrusted; nothing of it
 * reaches USBD's helpers before these walks have accepted the whole of it.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef USBMTP_CFGWALK_H
#define USBMTP_CFGWALK_H

/* Speeds, as GET_INFO's speed field reports them. */
#define CS_FULL  1
#define CS_HIGH  2
#define CS_SUPER 3

typedef struct CFG_PICK {
    unsigned long IfOffset;         /* the owned interface descriptor's offset */
    unsigned char NumInterfaces;    /* the configuration's bNumInterfaces */
    unsigned char Interface, Class, SubClass, Protocol;
    unsigned char EpIn, EpOut, EpInt;   /* endpoint addresses: bulk IN, bulk OUT, interrupt IN */
    unsigned short MpsIn, MpsOut, MpsInt;   /* packet sizes, the size bits only */
    unsigned char MultInt;          /* high speed: the interrupt endpoint's additional transactions */
    unsigned char Speed;            /* CS_* */
    unsigned short IntBytes;        /* the interrupt endpoint's bytes per interval (the poll's length) */
    unsigned char IntervalInt;      /* its bInterval */
} CFG_PICK;

/* Results: 0 accepted, else the first rule broken. */
#define CW_OK           0
#define CW_SHORT        1   /* less than a configuration descriptor */
#define CW_HEADER       2   /* bLength, bDescriptorType or wTotalLength wrong */
#define CW_RECORD       3   /* a descriptor with bLength < 2 or past the end */
#define CW_TOO_SHORT    4   /* an interface (< 9), endpoint (< 7) or companion (not 6) descriptor */
#define CW_NESTED       5   /* a second configuration descriptor inside */
#define CW_STRAY        6   /* an endpoint descriptor before any interface */
#define CW_COUNT        7   /* alternate-0 interfaces differ from bNumInterfaces, an (interface,
                               alternate) twice, or an interface with no alternate 0 */
#define CW_WHOLE        8   /* a whole device (no MI_nn) with more than one interface */
#define CW_ABSENT       9   /* the interface the node stands for is not there */
#define CW_CLASS        10  /* the interface is neither 06/01/01 nor FF */
#define CW_ENDPOINTS    11  /* not exactly one bulk IN, one bulk OUT and one interrupt IN, an
                               interface's endpoint records unlike its bNumEndpoints, or more
                               endpoints than USB allows */
#define CW_PACKET       12  /* a wMaxPacketSize not valid for its type and speed */
#define CW_ADDRESS      13  /* endpoint 0, reserved address bits, or an address twice */
#define CW_DEVICE       14  /* the device descriptor: bMaxPacketSize0 (also against the speed the
                               configuration establishes) or bNumConfigurations */
#define CW_SPEED        15  /* SuperSpeed companions missing, misplaced, malformed, or present
                               below SuperSpeed */
#define CW_INTERVAL     16  /* the interrupt endpoint's bInterval outside its speed's range */
#define CW_CONFIG       17  /* bConfigurationValue 0, or bmAttributes with bit 7 clear or a
                               reserved bit (4 to 0) set */
#define CW_ATTRIBUTES   18  /* an owned endpoint's bmAttributes with a reserved bit set: bits 7..2
                               of a bulk endpoint; bits 7..6 and 3..2 of an interrupt endpoint,
                               and its bits 5..4 above 01b at SuperSpeed or nonzero below it */

/* The device descriptor as it arrived (Len the bytes returned): 18 bytes,
   bDescriptorType 1, at least one configuration, bMaxPacketSize0 8, 16,
   32 or 64, or 09h with a bcdUSB of 0300h or more, which is SuperSpeed
   (*Super set). */
int DevCheck(const unsigned char *Cfg, unsigned long Len, int *Super);

/* Cfg, Len: the configuration descriptor as it arrived (Len the bytes the
   device returned). Node: the interface number nn of an "&MI_nn" node, or
   -1 for a whole device. Super: DevCheck's verdict; Mps0: the device
   descriptor's bMaxPacketSize0, checked against the speed established. */
int CfgWalk(const unsigned char *Cfg, unsigned long Len, int Node, int Super, unsigned char Mps0, CFG_PICK *Out);

#endif
