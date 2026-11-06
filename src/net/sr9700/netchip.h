/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The chip interface of the USB Ethernet core (ethernet.md section 3.3).
 * One package supplies one NET_CHIP table; the common NDIS upper edge
 * (netcore.c) and WDM lower edge (netusb.c) call it and own everything
 * else. Nothing here includes an OS header: a chip's code is compiled
 * unchanged into the driver and into the package's host tests.
 *
 * This file, netusb.h, netcore.c and netusb.c are identical copies in
 * src/net/ax88179/ and src/net/rtl815x/. They are written to move to
 * src/net/common/ (ethernet.md section 3.2) once that directory's owner
 * agrees; until then a change to one copy is made to both.
 *
 * Rules for a chip (section 3.3):
 * - Probe is a pure parser of descriptors; it changes nothing.
 * - Start, Link, Poll, SetMac, SetFilter, Stop, Suspend and Resume run
 *   at PASSIVE_LEVEL on the adapter's one control thread, never two at
 *   once, and reach the device only through NET_IO.
 * - Status, RxParse, TxHeader and TxEnd run at IRQL <= DISPATCH_LEVEL from
 *   USB completions; they never call NET_IO, allocate, or block.
 * - RxParse validates a whole transfer before it emits any frame.
 */
#ifndef NETCHIP_H
#define NETCHIP_H

typedef unsigned char nc_u8;
typedef unsigned short nc_u16;
typedef unsigned long nc_u32;

#define NC_OK            0L
#define NC_E_MALFORMED (-1L)   /* a length or offset is out of bounds */
#define NC_E_UNSUPPORTED (-2L) /* well formed, but not a device this chip serves */
#define NC_E_DEVICE    (-3L)   /* the device refused or answered nonsense */
#define NC_E_IO        (-4L)   /* a control transfer failed (NET_IO.Status) */

#define NC_ETH_MIN 14
#define NC_ETH_PAD 60          /* frames shorter than this are zero padded */
#define NC_ETH_MAX 1514

/* NDIS packet filter bits, as ndis.h defines them; a chip maps these to
 * its receive control register. */
#define NC_FILTER_DIRECTED      0x0001
#define NC_FILTER_MULTICAST     0x0002
#define NC_FILTER_ALL_MULTICAST 0x0004
#define NC_FILTER_BROADCAST     0x0008
#define NC_FILTER_PROMISCUOUS   0x0020
#define NC_MULTICAST_MAX 32     /* OID_802_3_MAXIMUM_LIST_SIZE (section 4.6) */

/* USB speed, from the bulk endpoints' wMaxPacketSize (64, 512, 1024). */
#define NC_SPEED_FULL  1
#define NC_SPEED_HIGH  2
#define NC_SPEED_SUPER 3

/* Vendor control requests to the device (bmRequestType 0x40 or 0xC0). */
typedef struct NET_IO {
    void *Context;
    /* One vendor/device control transfer; in = device to host. Returns
     * NC_OK with *actual set, or a negative error. */
    long (*Control)(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                    void *data, nc_u16 length, nc_u16 *actual);
    void (*Sleep)(void *context, nc_u32 ms);
    /* First failure seen; once set, Io* helpers issue nothing further and
     * read back zero (or all ones where the chip code asks for that). */
    long Status;
    nc_u32 Requests;            /* control transfers issued */
} NET_IO;

/* What Probe chose: one configuration, one interface, its endpoints. */
typedef struct NET_PROBE {
    nc_u8 ConfigIndex;          /* descriptor index, not bConfigurationValue */
    nc_u8 ConfigValue;
    nc_u8 Interface, Alternate;
    nc_u8 NumInterfaces;
    nc_u8 In, Out, Interrupt;   /* endpoint addresses */
    nc_u16 InMax, OutMax, IntMax;
    nc_u8 IntInterval;
    nc_u8 Speed;                /* NC_SPEED_* */
    nc_u16 Vendor, Product, Revision;
} NET_PROBE;

/* What Start reports, for the lower edge's buffers and for NDIS. */
typedef struct NET_PARAMS {
    nc_u8 Mac[6];               /* the address the chip holds after Start */
    nc_u32 RxSize;              /* bulk-IN buffer allocation */
    nc_u32 RxCount;             /* bulk-IN transfers outstanding */
    nc_u32 TxSize;              /* bulk-OUT buffer, bytes */
    nc_u32 TxSlots;             /* bulk-OUT transfers outstanding */
    nc_u32 TxBatch;             /* frames per bulk-OUT transfer, at most */
    nc_u32 TxHeader;            /* bytes before each frame */
    nc_u32 TxAlign;             /* each header starts at a multiple (power of 2) */
    nc_u32 Version;             /* chip revision, for the log */
    nc_u32 RxConfig, RxConfigRead; /* receive configuration written and read back (log; 0 if none) */
} NET_PARAMS;

typedef struct NET_LINK {
    int Up;
    nc_u32 Mbps;                /* 10, 100, 1000; 0 when down */
    int FullDuplex;
    nc_u32 RxLength;            /* bulk-IN transfer length to request from now on */
} NET_LINK;

typedef void (*NET_EMIT)(void *context, const nc_u8 *frame, nc_u32 length);

typedef struct NET_RX_STATS {
    nc_u32 Frames, Dropped;     /* delivered; well-formed but refused (CRC, size) */
    nc_u32 FcsStripped;         /* safety net: frames that arrived with an FCS, stripped */
} NET_RX_STATS;

#define NET_CHIP_STATE 256      /* bytes of chip state the lower edge provides */

typedef struct NET_CHIP {
    const char *Name;           /* trace prefix and driver name */
    const char *Description;    /* OID_GEN_VENDOR_DESCRIPTION */
    nc_u16 DriverVersion;       /* OID_GEN_VENDOR_DRIVER_VERSION low word */
    /* Pure: choose a configuration among all of them (config[i], size[i]). */
    long (*Probe)(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
                  const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
    /* Bring-up after SELECT_CONFIGURATION. Reads the chip's own MAC
     * into params->Mac and fills the buffer plan. */
    long (*Start)(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
    long (*SetMac)(void *chip, NET_IO *io, const nc_u8 mac[6]);
    /* Read the link and apply what it needs (speed-dependent registers,
     * receive enable). Called after Start, on each change, and by Poll. */
    long (*Link)(void *chip, NET_IO *io, NET_LINK *link);
    /* Cheap check: 1 if the link differs from what Link last applied. */
    int (*Poll)(void *chip, NET_IO *io);
    long (*SetFilter)(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count);
    long (*Stop)(void *chip, NET_IO *io);       /* halt: receive off, PHY down */
    long (*Suspend)(void *chip, NET_IO *io);    /* before D3 */
    long (*Resume)(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params);
    /* Interrupt-IN data: 1 link up, 0 link down, -1 no link information. */
    int (*Status)(void *chip, const nc_u8 *data, nc_u32 length);
    /* One completed bulk-IN transfer. Returns frames emitted, or a
     * negative error with nothing emitted. */
    long (*RxParse)(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit,
                    void *context, NET_RX_STATS *stats);
    /* Write the header for one frame of length bytes at hdr; single is
     * nonzero when it is the only frame of the transfer. */
    void (*TxHeader)(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single);
    /* After the last frame: may extend total (padding); returns 1 if the
     * transfer must be followed by a zero-length packet. */
    int (*TxEnd)(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket);
} NET_CHIP;

/* The package's chip table, defined by the package. */
extern const NET_CHIP NetPackageChip;

/* ---- pure helpers for chip code (netchip.c) ---- */

/* Ethernet CRC-32 as Linux ether_crc(): the bit-reversed, non-inverted
 * CRC of the address; a chip's 64-bin hash index is the top six bits. */
nc_u32 NcEtherCrc(const nc_u8 *data, nc_u32 length);
/* IEEE 802.3 frame check sequence (zlib CRC-32), for FCS verification. */
nc_u32 NcFcs(const nc_u8 *data, nc_u32 length);
int NcValidMac(const nc_u8 mac[6]);  /* not zero, not multicast */

/* Standard descriptor walk: find, in one configuration, the first
 * interface (alternate 0) of class FF with exactly one bulk IN, one bulk
 * OUT and one interrupt IN; with firstOnly, only the configuration's
 * first interface descriptor is considered. Fills probe's interface and endpoint fields
 * and Speed. Returns NC_OK, NC_E_UNSUPPORTED or NC_E_MALFORMED. */
long NcFindVendorInterface(const nc_u8 *config, nc_u32 size, int firstOnly, NET_PROBE *probe);
long NcParseDevice(const nc_u8 *device, nc_u32 size, NET_PROBE *probe);

/* Vendor control transfers through NET_IO with a sticky status: after
 * the first failure (error or short reply) nothing more is issued and
 * reads return zero-filled data. NcIoTry does not set the sticky status,
 * for reads Linux also allows to fail (an absent EEPROM word). */
long NcIoRead(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, void *data, nc_u16 length);
long NcIoWrite(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, const void *data, nc_u16 length);
long NcIoTry(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, void *data, nc_u16 length);
void NcIoSleep(NET_IO *io, nc_u32 ms);

#endif
