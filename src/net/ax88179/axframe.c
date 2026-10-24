/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 * RX/TX wire semantics adapted from Linux v6.12 ax88179_178a.c, pinned
 * adc218676eef25575469234709c2d87185ca223a. See NOTICE.md.
 * Original two-pass bounds validation and caller-buffer implementation.
 */
#include "axframe.h"
static unsigned long R32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
        ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static void W32(unsigned char *p, unsigned long v)
{
    unsigned int i;
    for (i = 0; i < 4; ++i) { p[i] = (unsigned char)v; v >>= 8; }
}
static unsigned long EthernetCrc(const unsigned char *b,unsigned int n)
{
    unsigned long crc=0xffffffffUL;unsigned int i,bit;
    for(i=0;i<n;i++) {
        crc^=b[i];
        for(bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1)?0xedb88320UL:0);
    }
    return crc^0xffffffffUL;
}
static int Receive(const unsigned char *b, unsigned int bytes,
    unsigned int maxFrame, AX_FRAME_RECEIVE receive, void *context,
    AX_FRAME_STATS *stats,unsigned int fcs)
{
    AX_FRAME_STATS result = { 0 };
    unsigned int count, metadata, position = 0, i, length, padded, remainder, strip;
    unsigned long trailer, status;
    if (!stats) return 0;
    *stats = result;
    if (!b || !receive || bytes < 4 || bytes > AX_FRAME_MAX_AGGREGATE ||
        maxFrame < 14 || maxFrame > AX_FRAME_MAX_ETHERNET) return 0;
    trailer = R32(b + bytes - 4);
    count = (unsigned int)(trailer & 65535UL); metadata = (unsigned int)(trailer >> 16);
    if (!count || count > AX_FRAME_MAX_HEADERS || metadata > bytes - 4 ||
        (metadata & 7) || count > (bytes - 4 - metadata) / 4) return 0;
    remainder = bytes - 4 - metadata - count * 4;
    if (remainder > 7) return 0; /* optional final alignment storage only */
    /* No callback until every length and padded extent is validated. */
    for (i = 0; i < count; ++i) {
        status = R32(b + metadata + i * 4);
        length = (unsigned int)((status >> 16) & 8191UL);
        if (!length) continue; /* optional dummy metadata consumes no data */
        padded = (length + 7) & ~7U;
        if (padded > metadata - position) return 0;
        position += padded;
    }
    if (position != metadata) return 0;
    result.Headers = count; position = 0;
    for (i = 0; i < count; ++i) {
        status = R32(b + metadata + i * 4);
        length = (unsigned int)((status >> 16) & 8191UL);
        if (!length) { ++result.Dummy; continue; }
        padded = (length + 7) & ~7U;
        /* fcs 1 (per frame): strip the last 4 bytes when they are the
         * frame's FCS, else deliver the record whole. */
        strip = fcs;
        if (fcs == 1) strip = (length >= 16+4 &&
            EthernetCrc(b+position+2,length-6)==R32(b+position+length-4)) ? 4 : 0;
        if (fcs == 1 && strip) ++result.FcsStripped;
        if ((status & 0xa0000000UL) || length < 16+strip || length - 2-strip > maxFrame ||
            (fcs == 4 && EthernetCrc(b+position+2,length-6)!=R32(b+position+length-4))) ++result.Dropped;
        else {
            if (status & 3UL) ++result.ChecksumErrors;
            receive(context, b + position + 2, length - 2-strip, status);
            ++result.Delivered;
        }
        position += padded;
    }
    *stats = result; return 1;
}
int AxFrameReceive(const unsigned char *b,unsigned int bytes,unsigned int maxFrame,
    AX_FRAME_RECEIVE receive,void *context,AX_FRAME_STATS *stats)
{return Receive(b,bytes,maxFrame,receive,context,stats,0);}
int AxFrameReceiveFcs(const unsigned char *b,unsigned int bytes,unsigned int maxFrame,
    AX_FRAME_RECEIVE receive,void *context,AX_FRAME_STATS *stats)
{return Receive(b,bytes,maxFrame,receive,context,stats,4);}
int AxFrameReceiveAuto(const unsigned char *b,unsigned int bytes,unsigned int maxFrame,
    AX_FRAME_RECEIVE receive,void *context,AX_FRAME_STATS *stats)
{return Receive(b,bytes,maxFrame,receive,context,stats,1);}
int AxFrameTransmit(const unsigned char *frame, unsigned int bytes,
    unsigned int maxFrame, unsigned int maxPacket, unsigned char *out,
    unsigned int capacity, unsigned int *written)
{
    unsigned int i;
    unsigned long flags = 0;
    if (!written) return 0;
    *written = 0;
    if (!frame || !out || maxFrame < 14 || maxFrame > AX_FRAME_MAX_ETHERNET ||
        bytes < 14 || bytes > maxFrame || capacity < 8 || bytes > capacity - 8 ||
        (maxPacket != 8 && maxPacket != 16 && maxPacket != 32 && maxPacket != 64 &&
         maxPacket != 512 && maxPacket != 1024)) return 0;
    if ((bytes + 8) % maxPacket == 0) flags = 0x80008000UL;
    W32(out, bytes); W32(out + 4, flags);
    for (i = 0; i < bytes; ++i) out[i + 8] = frame[i];
    *written = bytes + 8; return 1;
}
