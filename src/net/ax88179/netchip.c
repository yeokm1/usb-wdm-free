/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Pure helpers shared by the chip cores (netchip.h): CRCs, address
 * checks and the standard USB descriptor walk (USB 2.0 chapter 9; the
 * SuperSpeed endpoint companion of USB 3.x section 9.6.7 is skipped).
 * Identical copy in src/net/ax88179/ and src/net/rtl815x/.
 */
#include "netchip.h"

nc_u32 NcEtherCrc(const nc_u8 *data, nc_u32 length)
{
    nc_u32 crc = 0xffffffffUL, reversed = 0, i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    for (bit = 0; bit < 32; bit++) { reversed = (reversed << 1) | (crc & 1); crc >>= 1; }
    return reversed;
}

nc_u32 NcFcs(const nc_u8 *data, nc_u32 length)
{
    nc_u32 crc = 0xffffffffUL, i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return crc ^ 0xffffffffUL;
}

int NcValidMac(const nc_u8 mac[6])
{
    if (mac[0] & 1) return 0;
    return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
}

static nc_u16 Le16(const nc_u8 *p) { return (nc_u16)(p[0] | (p[1] << 8)); }

long NcParseDevice(const nc_u8 *d, nc_u32 size, NET_PROBE *probe)
{
    if (!d || size < 18 || d[0] != 18 || d[1] != 1) return NC_E_MALFORMED;
    probe->Vendor = Le16(d + 8);
    probe->Product = Le16(d + 10);
    probe->Revision = Le16(d + 12);
    return NC_OK;
}

long NcFindVendorInterface(const nc_u8 *c, nc_u32 size, int firstOnly, NET_PROBE *probe)
{
    nc_u32 at, total;
    int inInterface = 0, seenInterface = 0, endpoints = 0, declared = 0;
    nc_u8 in = 0, out = 0, irq = 0, interval = 0, number = 0, value;
    nc_u16 inMax = 0, outMax = 0, irqMax = 0;

    if (!c || size < 9 || c[0] < 9 || c[1] != 2) return NC_E_MALFORMED;
    total = Le16(c + 2);
    if (total < 9 || total > size) return NC_E_MALFORMED;
    value = c[5];
    for (at = c[0]; at < total; at += c[at]) {
        const nc_u8 *p = c + at;
        if (total - at < 2 || p[0] < 2 || p[0] > total - at) return NC_E_MALFORMED;
        if (p[1] == 4) {                                  /* INTERFACE */
            if (p[0] < 9) return NC_E_MALFORMED;
            if (inInterface) break;                       /* the candidate ended */
            if (firstOnly && seenInterface) return NC_E_UNSUPPORTED;
            seenInterface = 1;
            if (p[3] == 0 && p[5] == 0xff) {
                inInterface = 1; number = p[2]; declared = p[4];
            }
        } else if (p[1] == 5 && inInterface) {            /* ENDPOINT */
            nc_u16 max;
            if (p[0] < 7) return NC_E_MALFORMED;
            max = (nc_u16)(Le16(p + 4) & 0x7ff);
            endpoints++;
            if ((p[3] & 3) == 2 && (p[2] & 0x80) && !in) { in = p[2]; inMax = max; }
            else if ((p[3] & 3) == 2 && !(p[2] & 0x80) && !out) { out = p[2]; outMax = max; }
            else if ((p[3] & 3) == 3 && (p[2] & 0x80) && !irq) { irq = p[2]; irqMax = max; interval = p[6]; }
            else return NC_E_UNSUPPORTED;
        }
    }
    if (!inInterface) return NC_E_UNSUPPORTED;
    if (endpoints != 3 || declared != 3 || !in || !out || !irq) return NC_E_UNSUPPORTED;
    if (inMax != outMax || !irqMax) return NC_E_UNSUPPORTED;
    if (inMax == 1024) probe->Speed = NC_SPEED_SUPER;
    else if (inMax == 512) probe->Speed = NC_SPEED_HIGH;
    else if (inMax == 8 || inMax == 16 || inMax == 32 || inMax == 64) probe->Speed = NC_SPEED_FULL;
    else return NC_E_UNSUPPORTED;
    probe->ConfigValue = value;
    probe->NumInterfaces = c[4];
    probe->Interface = number;
    probe->Alternate = 0;
    probe->In = in; probe->Out = out; probe->Interrupt = irq;
    probe->InMax = inMax; probe->OutMax = outMax; probe->IntMax = irqMax;
    probe->IntInterval = interval;
    return NC_OK;
}

static void Zero(void *data, nc_u16 length)
{
    nc_u8 *b = data;
    while (length--) *b++ = 0;
}

static long Transfer(NET_IO *io, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                     void *data, nc_u16 length, int sticky)
{
    nc_u16 actual = 0;
    long status;
    if (io->Status < 0) {
        if (in) Zero(data, length);
        return io->Status;
    }
    io->Requests++;
    status = io->Control(io->Context, in, request, value, index, data, length, &actual);
    if (status >= 0 && actual != length) status = NC_E_DEVICE;
    if (status < 0) {
        if (in) Zero(data, length);
        if (sticky) io->Status = status;
    }
    return status;
}

long NcIoRead(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, void *data, nc_u16 length)
{
    return Transfer(io, 1, request, value, index, data, length, 1);
}

long NcIoWrite(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, const void *data, nc_u16 length)
{
    return Transfer(io, 0, request, value, index, (void *)data, length, 1);
}

long NcIoTry(NET_IO *io, nc_u8 request, nc_u16 value, nc_u16 index, void *data, nc_u16 length)
{
    return Transfer(io, 1, request, value, index, data, length, 0);
}

void NcIoSleep(NET_IO *io, nc_u32 ms)
{
    if (io->Status >= 0 && ms) io->Sleep(io->Context, ms);
}
