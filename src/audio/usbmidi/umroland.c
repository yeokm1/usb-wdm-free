/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded profile recognizer using the captured descriptors.
 * Roland F1/02 cable-count and alt1 interrupt semantics adapted from
 * Linux v6.12 sound/usb/midi.c, adc218676eef25575469234709c2d87185ca223a.
 * Upstream notices follow; the complete source remains an ignored reference.
 */
/*
 * usbmidi.c - ALSA USB MIDI driver
 *
 * Copyright (c) 2002-2009 Clemens Ladisch
 * All rights reserved.
 *
 * Based on the OSS usb-midi driver by NAGANO Daisuke,
 *          NetBSD's umidi driver by Takuya SHIOZAKI,
 *          the "USB Device Class Definition for MIDI Devices" by Roland
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed and/or modified under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "umroland.h"
#include <string.h>
int UmMatchUmOne2(const unsigned char *d,unsigned int dn,
    const unsigned char *b,unsigned int n,unsigned int alt,UM_ROLAND *out)
{
    /* Exact observed interface descriptor bodies. Fail closed on unreviewed
     * variants; a shared product name or vendor class is never sufficient. */
    static const unsigned char body[]={
        9,4,0,0,2,0xff,3,0,0,
        6,0x24,0xf1,2,1,1, 6,0x24,0xf1,1,0,0,
        7,5,2,2,64,0,0, 7,5,0x81,2,64,0,0,
        9,4,0,1,2,0xff,3,0,0,
        7,5,2,2,64,0,0, 7,5,0x81,3,64,0,1
    };
    if(!out) return 0;
    memset(out,0,sizeof(*out));
    if(!d || !b || dn!=18 || d[0]!=18 || d[1]!=1 ||
        d[2]!=0x10 || d[3]!=1 || d[4]!=0xff || d[5]!=0 || d[6]!=0xff || d[7]!=64 ||
        d[8]!=0x82 || d[9]!=5 || d[10]!=0x2a || d[11]!=1 ||
        d[17]!=1 || alt>1 || n!=9+sizeof(body) ||
        b[0]!=9 || b[1]!=2 || b[2]!=n || b[3]!=0 ||
        b[4]!=1 || b[5]!=1 || !(b[7]&0x80) || (b[7]&0x1f) ||
        memcmp(b+9,body,sizeof(body))) return 0;
    out->Interface=0;out->Alternate=alt;
    out->InAddress=0x81;out->OutAddress=2;
    out->InType=alt?3:2;out->OutType=2;out->InInterval=alt?1:0;
    out->MaxPacket=64;out->InCables=1;out->OutCables=1;
    return 1;
}
