/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * SR9700F chip core (ethernet.md section 6.9.2). Written for this project
 * from facts in Linux drivers/net/usb/sr9700.c and sr9700.h at v7.3 (GPL
 * version 2 only: facts only). The facts and where they are:
 * - the requests and registers (sr9700.h);
 * - the bring-up: chip reset, the address from the address registers, a
 *   PHY reset of 20 ms then 2 ms, receive on (sr9700_bind);
 * - the link: network status bit 6, 10 Mb/s half duplex only
 *   (sr9700_get_link, sr9700_get_link_ksettings); the status report's
 *   first byte is the network status (sr9700_status);
 * - the filter: no multicast filter, so all multicast when any is wanted
 *   (sr9700_set_multicast); the address change (sr9700_set_mac_address);
 * - receive: packets of a status byte (0x40), a 16-bit length that
 *   counts the FCS, the frame and its FCS, back to back in one transfer
 *   of 3 KiB (sr9700_rx_fixup, rx_urb_size);
 * - transmit: a 16-bit length before the frame, one more when header and
 *   frame end on a packet boundary, where usbnet then adds a zero byte
 *   (sr9700_tx_fixup; usbnet_start_xmit).
 * Stop, Suspend and Resume are this core's own (Linux's driver has none):
 * receive off, and the bring-up again.
 */
#include "srchip.h"

typedef char SR7_STATE_FITS[(sizeof(SR7_STATE) <= NET_CHIP_STATE) ? 1 : -1];

static void Set(NET_IO *io, nc_u8 reg, nc_u8 value)
{
    NcIoWrite(io, SR7_REQ_WRITE1, value, reg, 0, 0);
}

static nc_u8 Get(NET_IO *io, nc_u8 reg)
{
    nc_u8 v = 0;
    NcIoRead(io, SR7_REQ_READ, 0, reg, &v, 1);
    return v;
}

static void Zero(void *p, nc_u32 n)
{
    nc_u8 *b = p;
    while (n--) *b++ = 0;
}

long Sr7Probe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    long status;
    nc_u32 i;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    /* One vendor interface with a bulk IN, a bulk OUT and an interrupt IN,
     * whichever its number (the 0FE6:9702 has it on interface 1). */
    for (i = 0; i < count; i++) {
        if (!config[i] || !size[i]) continue;
        status = NcFindVendorInterface(config[i], size[i], 0, probe);
        if (status == NC_OK) {
            if (probe->Speed == NC_SPEED_SUPER) return NC_E_UNSUPPORTED;
            probe->ConfigIndex = (nc_u8)i;
            return NC_OK;
        }
        if (status == NC_E_MALFORMED) return status;
    }
    return NC_E_UNSUPPORTED;
}

long Sr7Start(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    SR7_STATE *s = chip;
    nc_u32 i;
    (void)probe;
    Zero(s, sizeof(*s));
    Set(io, SR7_NET_CTRL, SR7_NET_CTRL_RESET);
    NcIoSleep(io, 1);
    NcIoRead(io, SR7_REQ_READ, 0, SR7_ADDRESS, s->Mac, 6);
    Set(io, SR7_PHY_RESET, SR7_PHY_RESET_ON);
    NcIoSleep(io, 20);
    Set(io, SR7_PHY_RESET, 0);
    NcIoSleep(io, 2);
    s->RxCtrl = SR7_RX_DEFAULT;
    Set(io, SR7_RX_CTRL, s->RxCtrl);
    s->RxCtrlRead = Get(io, SR7_RX_CTRL);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    params->RxSize = SR7_RX_TRANSFER;
    params->RxCount = 4;          /* no frame spans transfers */
    params->TxHeader = 2;
    params->TxAlign = 1;
    params->TxBatch = 1;          /* one frame per transfer */
    params->TxSlots = 4;
    params->TxSize = 2 + NC_ETH_MAX + 1;
    params->Version = 0x9700;
    params->RxConfig = s->RxCtrl;
    params->RxConfigRead = s->RxCtrlRead;
    return NC_OK;
}

long Sr7SetMac(void *chip, NET_IO *io, const nc_u8 mac[6])
{
    SR7_STATE *s = chip;
    nc_u32 i;
    /* A write that fails stays pending: Poll asks for Link, which writes
     * the address again before it reads the link. */
    for (i = 0; i < 6; i++) s->WantMac[i] = mac[i];
    s->MacPending = 1;
    NcIoWrite(io, SR7_REQ_WRITE, 0, SR7_ADDRESS, mac, 6);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) s->Mac[i] = mac[i];
    s->MacPending = 0;
    return NC_OK;
}

long Sr7Link(void *chip, NET_IO *io, NET_LINK *link)
{
    SR7_STATE *s = chip;
    nc_u8 status;
    link->Up = 0; link->Mbps = 0; link->FullDuplex = 0;
    link->RxLength = SR7_RX_TRANSFER;
    if (s->MacPending) {
        long r = Sr7SetMac(s, io, s->WantMac);
        if (r < 0) { s->LinkUp = 0; return r; }
    }
    status = Get(io, SR7_NET_STATUS);
    if (io->Status < 0) { s->LinkUp = 0; return io->Status; }
    s->Status = status;
    s->LinkUp = (nc_u8)((status & SR7_NET_STATUS_LINK) != 0);
    if (s->LinkUp) {
        link->Up = 1;
        link->Mbps = 10;          /* the chip runs 10 Mb/s half duplex only */
    }
    return NC_OK;
}

int Sr7Poll(void *chip, NET_IO *io)
{
    SR7_STATE *s = chip;
    nc_u8 status = Get(io, SR7_NET_STATUS);
    if (io->Status < 0) return 0;
    /* A filter write that failed is applied again: the core runs Link and,
     * with the link up, the filter. */
    if (s->MacPending || (s->FilterPending && s->LinkUp)) return 1;
    return ((status & SR7_NET_STATUS_LINK) != 0) != (s->LinkUp != 0);
}

long Sr7SetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count)
{
    SR7_STATE *s = chip;
    nc_u8 rx = SR7_RX_DEFAULT;
    (void)multicast;
    /* Directed and broadcast always; no multicast filter in the chip. */
    if (filter & NC_FILTER_PROMISCUOUS) rx |= SR7_RX_PROMISC;
    else if ((filter & NC_FILTER_ALL_MULTICAST) || ((filter & NC_FILTER_MULTICAST) && count))
        rx |= SR7_RX_ALL_MULTI;
    Set(io, SR7_RX_CTRL, rx);
    if (io->Status < 0) { s->FilterPending = 1; return io->Status; }
    s->RxCtrl = rx;
    s->FilterPending = 0;
    return NC_OK;
}

long Sr7Stop(void *chip, NET_IO *io)
{
    SR7_STATE *s = chip;
    Set(io, SR7_RX_CTRL, 0);
    s->RxCtrl = 0;
    s->LinkUp = 0;
    return io->Status < 0 ? io->Status : NC_OK;
}

long Sr7Suspend(void *chip, NET_IO *io)
{
    return Sr7Stop(chip, io);
}

long Sr7Resume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    long r = NC_E_DEVICE;
    int i;
    for (i = 0; i < 3; i++) {
        io->Status = NC_OK;
        r = Sr7Start(chip, io, probe, params);
        if (r >= 0) break;
    }
    return r;
}

int Sr7Status(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    if (length < 8) return -1;
    return (data[0] & SR7_NET_STATUS_LINK) ? 1 : 0;
}

/* One pass over a transfer: with emit NULL only the checks. Packets are
 * back to back; a tail shorter than a packet's overhead is ignored (Linux
 * counts it as an error after it has delivered the packets before it). */
static long Walk(const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                 nc_u32 *frames, nc_u32 *dropped, int *tail)
{
    nc_u32 offset = 0;
    *frames = 0; *dropped = 0; *tail = 0;
    while (length - offset > SR7_RX_OVERHEAD) {
        nc_u32 total, frame;
        if (data[offset] != SR7_RX_GOOD) return NC_E_MALFORMED;
        total = (nc_u32)data[offset + 1] | ((nc_u32)data[offset + 2] << 8);
        if (total < 4) return NC_E_MALFORMED;
        frame = total - 4;
        if (frame > NC_ETH_MAX || frame > length - offset - SR7_RX_OVERHEAD) return NC_E_MALFORMED;
        if (frame < NC_ETH_MIN) {
            (*dropped)++;
        } else {
            (*frames)++;
            if (emit) emit(context, data + offset + 3, frame);
        }
        offset += frame + SR7_RX_OVERHEAD;
    }
    if (offset != length) *tail = 1;
    return NC_OK;
}

long Sr7RxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats)
{
    SR7_STATE *s = chip;
    nc_u32 frames, dropped;
    int tail;
    stats->Frames = 0; stats->Dropped = 0; stats->FcsStripped = 0;
    if (Walk(data, length, 0, 0, &frames, &dropped, &tail) < 0) {
        s->Malformed++;
        return NC_E_MALFORMED;
    }
    (void)Walk(data, length, emit, context, &frames, &dropped, &tail);
    if (tail) s->Tails++;
    stats->Frames = frames;
    stats->Dropped = dropped;
    return (long)frames;
}

void Sr7TxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    nc_u32 n = length;
    (void)chip; (void)single;
    /* Header and frame on a packet boundary: the length says one more,
     * for the zero byte TxEnd adds. */
    if (maxPacket && (length + 2) % maxPacket == 0) n++;
    hdr[0] = (nc_u8)n; hdr[1] = (nc_u8)(n >> 8);
}

int Sr7TxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    (void)chip;
    if (maxPacket && *total % maxPacket == 0 && *total + 1 <= capacity) {
        buffer[*total] = 0;
        *total += 1;
    }
    return 0;
}

const NET_CHIP NetPackageChip = {
    "sr9700f",
    "usb-wdm-free SR9700F (CoreChips SR9700 USB Ethernet)",
    0x0001,
    Sr7Probe, Sr7Start, Sr7SetMac, Sr7Link, Sr7Poll, Sr7SetFilter, Sr7Stop, Sr7Suspend, Sr7Resume,
    Sr7Status, Sr7RxParse, Sr7TxHeader, Sr7TxEnd
};
