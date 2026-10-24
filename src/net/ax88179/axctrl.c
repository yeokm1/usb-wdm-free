/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 * Request/register/link parameter facts adapted from Linux v6.12
 * adc218676eef25575469234709c2d87185ca223a ax88179_178a.c. See NOTICE.md.
 * Original bounded pure functions. No USB, kernel, NDIS or allocation.
 */
#include "axctrl.h"
static unsigned int R16(const unsigned char *p)
{ return p[0] | ((unsigned int)p[1] << 8); }
static void Zero(void *p, unsigned int n)
{ unsigned int i; for(i=0;i<n;++i) ((unsigned char*)p)[i]=0; }
int AxCtrlDescribe(const unsigned char *dev,unsigned int dn,const unsigned char *b,
    unsigned int bn,unsigned int speed,AX_CTRL_DEVICE *out)
{
    AX_CTRL_DEVICE d={0}; unsigned int n,o,size,haveInterface=0,seen=0,previous=0;
    if(!out) return AX_CTRL_BAD; *out=d;
    if(!dev || dn!=18 || dev[0]!=18 || dev[1]!=1 || !b || bn<9 || b[0]!=9 || b[1]!=2) return AX_CTRL_BAD;
    if(R16(dev+8)!=0x0b95 || R16(dev+10)!=0x1790 || dev[17]!=1) return AX_CTRL_UNSUPPORTED;
    if(speed<AX_SPEED_FULL || speed>AX_SPEED_SUPER) return AX_CTRL_UNSUPPORTED;
    d.Speed=speed;d.UsbRevision=R16(dev+2);
    if((speed==AX_SPEED_SUPER && dev[7]!=9) || (speed==AX_SPEED_HIGH && dev[7]!=64) ||
       (speed==AX_SPEED_FULL && dev[7]!=8 && dev[7]!=16 && dev[7]!=32 && dev[7]!=64)) return AX_CTRL_SPEED_MISMATCH;
    n=R16(b+2);if(n<9 || n>bn || !b[5]) return AX_CTRL_BAD;
    if(b[4]!=1) return AX_CTRL_UNSUPPORTED;
    d.ConfigBytes=n;d.Configuration=b[5];
    for(o=9;o<n;o+=size) {
        if(n-o<2 || (size=b[o])<2 || size>n-o) return AX_CTRL_BAD;
        if(previous && speed==AX_SPEED_SUPER && b[o+1]!=0x30) return AX_CTRL_BAD;
        if(b[o+1]==4) {
            if(size!=9 || haveInterface++) return AX_CTRL_UNSUPPORTED;
            if(b[o+2]!=0 || b[o+3]!=0 || b[o+4]!=3 || b[o+5]!=255 || b[o+6]!=255 || b[o+7]!=0) return AX_CTRL_UNSUPPORTED;
        } else if(b[o+1]==5) {
            unsigned int address,type,packet,bit;
            if(!haveInterface || size!=7) return AX_CTRL_BAD;
            address=b[o+2];type=b[o+3]&3;packet=R16(b+o+4);
            if(address==0x81 && type==3) bit=1;
            else if(address==0x82 && type==2) bit=2;
            else if(address==3 && type==2) bit=4;
            else return AX_CTRL_UNSUPPORTED;
            if((seen&bit) || (packet&0xf800) || !packet) return AX_CTRL_BAD;
            seen|=bit;previous=bit;
            if(type==2) {
                if((speed==AX_SPEED_SUPER && packet!=1024) || (speed==AX_SPEED_HIGH && packet!=512) ||
                   (speed==AX_SPEED_FULL && packet!=8 && packet!=16 && packet!=32 && packet!=64)) return AX_CTRL_SPEED_MISMATCH;
                if(bit==2) {d.BulkIn=address;d.InPacket=packet;} else {d.BulkOut=address;d.OutPacket=packet;}
            } else {
                if(packet!=8 || !b[o+6] || (speed!=AX_SPEED_FULL && b[o+6]>16)) return AX_CTRL_UNSUPPORTED;
                d.StatusIn=address;d.StatusPacket=packet;d.StatusInterval=b[o+6];
            }
            if(speed!=AX_SPEED_SUPER) previous=0;
        } else if(b[o+1]==0x30) {
            unsigned int burst,interval;
            if(speed!=AX_SPEED_SUPER) return AX_CTRL_SPEED_MISMATCH;
            if(size!=6 || !previous || b[o+2]>15 || b[o+3]) return AX_CTRL_UNSUPPORTED;
            burst=b[o+2]+1;interval=R16(b+o+4);
            if(previous==1) {
                if(interval>d.StatusPacket*burst) return AX_CTRL_BAD;
                d.StatusBurst=burst;d.StatusBytes=interval;
                if(!interval) d.Warnings|=AX_WARN_STATUS_INTERVAL_ZERO;
            } else {
                if(interval) return AX_CTRL_BAD; /* reserved for bulk */
                if(previous==2) d.InBurst=burst;else d.OutBurst=burst;
            }
            previous=0;
        } else return AX_CTRL_UNSUPPORTED;
    }
    if(!haveInterface || seen!=7 || previous) return AX_CTRL_BAD;
    if(speed!=AX_SPEED_SUPER) d.InBurst=d.OutBurst=d.StatusBurst=1;
    d.Valid=1;*out=d;return AX_CTRL_OK;
}
static int Device(const AX_CTRL_DEVICE *d)
{
    return d && d->Valid && d->Speed>=AX_SPEED_FULL && d->Speed<=AX_SPEED_SUPER &&
        d->BulkIn==0x82 && d->BulkOut==3 && d->StatusIn==0x81;
}
static void Request(AX_CTRL_STEP *s,unsigned int kind,unsigned int req,
    unsigned int value,unsigned int index,unsigned int length,unsigned int tag)
{
    s->Kind=kind;s->RequestType=kind==AX_STEP_READ?0xc0:0x40;
    s->Request=req;s->Value=value;s->Index=index;s->Length=length;s->ResultTag=tag;
}
int AxCtrlReadPlan(const AX_CTRL_DEVICE *d,AX_CTRL_PLAN *p)
{
    if(!p) return AX_CTRL_BAD;Zero(p,sizeof(*p));if(!Device(d)) return AX_CTRL_BAD;
    Request(&p->Step[0],AX_STEP_READ,1,0x02,1,1,AX_TAG_USB_STATUS);
    Request(&p->Step[1],AX_STEP_READ,1,0x10,6,6,AX_TAG_MAC);
    Request(&p->Step[2],AX_STEP_READ,2,3,0x11,2,AX_TAG_PHY_STATUS);
    p->Count=3;p->NextGate=AX_GATE_OBSERVATIONS;p->Valid=1;return AX_CTRL_OK;
}
int AxCtrlPowerPrefix(const AX_CTRL_DEVICE *d,AX_CTRL_PLAN *p)
{
    if(!p) return AX_CTRL_BAD;Zero(p,sizeof(*p));if(!Device(d)) return AX_CTRL_BAD;
    Request(&p->Step[0],AX_STEP_WRITE,1,0x26,2,2,0); /* PHY power reset0 */
    Request(&p->Step[1],AX_STEP_WRITE,1,0x26,2,2,0);p->Step[1].Data[0]=0x20;
    p->Step[2].Kind=AX_STEP_DELAY;p->Step[2].DelayMs=500;
    Request(&p->Step[3],AX_STEP_WRITE,1,0x33,1,1,0);p->Step[3].Data[0]=3;
    p->Step[4].Kind=AX_STEP_DELAY;p->Step[4].DelayMs=200;
    Request(&p->Step[5],AX_STEP_READ,4,0x43,1,2,AX_TAG_AUTODETACH_EEPROM);
    p->Count=6;p->HasWrites=1;p->NextGate=AX_GATE_AUTODETACH_DECISION;p->Valid=1;return AX_CTRL_OK;
}
int AxCtrlSetup(const AX_CTRL_STEP *s,unsigned char setup[8])
{
    unsigned char raw[8];unsigned int i;
    if(!s || !setup || (s->Kind!=AX_STEP_READ && s->Kind!=AX_STEP_WRITE) ||
       s->RequestType!=(s->Kind==AX_STEP_READ?0xc0U:0x40U) || s->Request>255 ||
       s->Value>65535 || s->Index>65535 || !s->Length || s->Length>8) return 0;
    raw[0]=(unsigned char)s->RequestType;raw[1]=(unsigned char)s->Request;
    raw[2]=(unsigned char)s->Value;raw[3]=(unsigned char)(s->Value>>8);
    raw[4]=(unsigned char)s->Index;raw[5]=(unsigned char)(s->Index>>8);
    raw[6]=(unsigned char)s->Length;raw[7]=0;
    for(i=0;i<8;++i) setup[i]=raw[i];return 1;
}
int AxCtrlResponse(const AX_CTRL_STEP *s,unsigned int actual)
{
    unsigned char setup[8];return AxCtrlSetup(s,setup) && actual==s->Length;
}
int AxCtrlMac(const unsigned char *b,unsigned int n)
{
    unsigned int i,any=0;
    if(!b || n!=6 || (b[0]&1)) return 0;
    for(i=0;i<6;++i) any|=b[i];return any!=0;
}
int AxCtrlPhy(const unsigned char *b,unsigned int n,AX_CTRL_LINK *out)
{
    AX_CTRL_LINK link={0};unsigned int status,speed;
    if(!out) return 0;*out=link;if(!b || n!=2) return 0;
    status=R16(b);if(status==65535) return 0;
    if(status&0x400) {
        speed=status&0xc000;if(speed==0xc000) return 0;
        link.Up=1;link.Megabits=speed==0x8000?1000:speed==0x4000?100:10;
        link.FullDuplex=(status&0x2000)!=0;
    }
    *out=link;return 1;
}
int AxCtrlLinkPlan(const AX_CTRL_DEVICE *d,const unsigned char mac[6],
    const unsigned char *usb,unsigned int un,const unsigned char *phy,unsigned int pn,
    AX_CTRL_LINK_PLAN *p)
{
    static const unsigned char queue[4][5]={{7,0x4f,0,0x12,0xff},{7,0x20,3,0x16,0xff},
        {7,0xae,7,0x18,0xff},{7,0xcc,0x4c,0x18,8}};
    unsigned int speed,index=3,i;
    if(!p) return AX_CTRL_BAD;Zero(p,sizeof(*p));
    if(!Device(d) || !AxCtrlMac(mac,6) || !usb || un!=1 || !AxCtrlPhy(phy,pn,&p->Link)) return AX_CTRL_BAD;
    speed=(*usb&4)?AX_SPEED_SUPER:(*usb&2)?AX_SPEED_HIGH:AX_SPEED_FULL;
    if(speed!=d->Speed) {Zero(p,sizeof(*p));return AX_CTRL_SPEED_MISMATCH;}
    if(p->Link.Up) {
        p->Medium=0x130; /* receive + TX/RX flow control, as Linux link_reset */
        if(p->Link.Megabits==1000) {p->Medium|=9;index=speed==AX_SPEED_SUPER?0:speed==AX_SPEED_HIGH?1:3;}
        else if(p->Link.Megabits==100) {p->Medium|=0x200;index=speed==AX_SPEED_FULL?3:2;}
        if(p->Link.FullDuplex) p->Medium|=2;
        for(i=0;i<5;++i) p->Queue[i]=queue[index][i];
        p->ReceiveBufferBytes=1024U*(p->Queue[3]+2);
    }
    p->Valid=1;return AX_CTRL_OK;
}
