/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation from USB-IF MIDI 1.0 section 4, tables 4-1/4-2.
 */
#include "umcore.h"

static unsigned int MessageSize(unsigned char b)
{
    if (b >= 0x80 && b <= 0xef) return (b < 0xc0 || b >= 0xe0) ? 3 : 2;
    if (b == 0xf1 || b == 0xf3) return 2;
    if (b == 0xf2) return 3;
    return 0;
}
static int Emit(UM_ENCODER *s, unsigned int cin, unsigned int n,
                unsigned char out[4])
{
    unsigned int i;
    out[0]=(unsigned char)((s->Cable << 4) | cin);
    for(i=0;i<3;i++) out[i+1]=(i<n)?s->Data[i]:0;
    return 1;
}
int UmEncoderInit(UM_ENCODER *s, unsigned int cable)
{
    unsigned int i;
    if (!s || cable>15) return 0;
    s->Cable=(unsigned char)cable;
    s->Running=s->Status=s->Need=s->Count=s->Sysex=0;
    for(i=0;i<3;i++) s->Data[i]=0;
    return 1;
}
int UmEncodeByte(UM_ENCODER *s, unsigned char b, unsigned char out[4])
{
    unsigned int cin;
    if (!s || !out || s->Cable>15 || s->Count>2) return -1;
    if (b>=0xf8) { /* Defined and undefined realtime bytes remain single. */
        out[0]=(unsigned char)((s->Cable<<4)|15);
        out[1]=b; out[2]=out[3]=0;
        return 1;
    }
    if (s->Sysex) {
        if (b<0x80 || b==0xf7) {
            s->Data[s->Count++]=b;
            if (b==0xf7) {
                cin=4+s->Count; s->Sysex=0;
                Emit(s,cin,s->Count,out); s->Count=0; return 1;
            }
            if (s->Count==3) { Emit(s,4,3,out); s->Count=0; return 1; }
            return 0;
        }
        /* A new status abandons an incomplete SysEx, then starts normally. */
        s->Sysex=0; s->Count=0;
    }
    if (b>=0x80) {
        s->Count=s->Need=s->Status=s->Running=0;
        if (b==0xf0) { s->Sysex=1; s->Data[0]=b; s->Count=1; return 0; }
        if (b==0xf6 || b==0xf7) { s->Data[0]=b; return Emit(s,5,1,out); }
        s->Need=(unsigned char)MessageSize(b);
        if (!s->Need) return -1;
        s->Status=b;
        if(b<0xf0) s->Running=b;
        s->Data[0]=b; s->Count=1; return 0;
    }
    if (!s->Count) {
        if (!s->Running) return -1;
        s->Status=s->Running; s->Need=(unsigned char)MessageSize(s->Status);
        s->Data[0]=s->Status; s->Count=1;
    }
    s->Data[s->Count++]=b;
    if (s->Count<s->Need) return 0;
    cin=(s->Status<0xf0)?(s->Status>>4):((s->Need==2)?2:3);
    Emit(s,cin,s->Need,out); s->Count=0; return 1;
}
unsigned int UmDecodePacket(const unsigned char *p,unsigned int length,
                           unsigned int cables,unsigned char data[3])
{
    static const unsigned char sizes[16]={0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
    unsigned int cin,n,i;
    if (!p || !data || length!=4 || !(cables & (1u<<(p[0]>>4)))) return 0;
    cin=p[0]&15; n=sizes[cin];
    if (!n) return 0;
    if (cin>=8 && cin<=14) {
        if (((unsigned int)p[1]>>4)!=cin) return 0;
        for(i=2;i<=n;i++) if(p[i]&0x80) return 0;
    } else if (cin==2 || cin==3) {
        if ((cin==2 && p[1]!=0xf1 && p[1]!=0xf3) ||
            (cin==3 && p[1]!=0xf2)) return 0;
        for(i=2;i<=n;i++) if(p[i]&0x80) return 0;
    } else if (cin==4) {
        if(p[1]!=0xf0 && p[1]>=0x80) return 0;
        if((p[2]|p[3])&0x80) return 0;
    } else if(cin==5) {
        if(p[1]!=0xf6 && p[1]!=0xf7) return 0;
    } else if(cin==6 || cin==7) {
        if(p[n]!=0xf7 || (p[1]>=0x80 && p[1]!=0xf0)) return 0;
        if(cin==7 && p[2]>=0x80) return 0;
    }
    for(i=0;i<n;i++) data[i]=p[i+1];
    return n;
}
