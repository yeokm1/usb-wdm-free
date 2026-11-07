/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original implementation from USB-IF Audio2 sections5.2.1-5.2.6, AppendixA.17.
 */
#include "uacctl.h"
typedef char UacCtlRequires32BitUnsigned[(sizeof(unsigned int)==4)?1:-1];
static unsigned int Le(const unsigned char *p,unsigned int n)
{
    unsigned int i,v=0;for(i=0;i<n;i++) v|=(unsigned int)p[i]<<(8*i);return v;
}
static UAC_CTL_INT Number(const unsigned char *p,unsigned int n,unsigned int sign)
{
    unsigned int raw=Le(p,n),bits=n*8;
    UAC_CTL_INT full=((UAC_CTL_INT)1)<<bits;
    if(sign && (raw&(1u<<(bits-1)))) return -((UAC_CTL_INT)full-raw);
    return (UAC_CTL_INT)raw;
}
int UacCtlAccess(const unsigned char *bitmap,unsigned int length,unsigned int selector)
{
    unsigned int bit,lane;
    if(!bitmap || !selector || selector>255) return -1;
    bit=(selector-1)*2;if(bit/8>=length) return -1;
    lane=(bitmap[bit/8]>>(bit%8))&3;
    return lane==2?-1:(int)lane;
}
int UacCtlPolicy(unsigned int family,unsigned int selector,UAC_CTL_POLICY *out)
{
    UAC_CTL_POLICY p={0,0,0,0,0,UAC_CTL_ENTITY_RECIPIENT};
    if(!out || !selector || selector>255) return 0;
    switch(family) {
    case UAC_CTL_CLOCK_SOURCE:
        if(selector==1) {p.Width=4;p.Range=1;}
        else if(selector==2) {p.Width=1;p.ReadOnly=1;}break;
    case UAC_CTL_CLOCK_SELECTOR:
        if(selector==1) p.Width=1;break;
    case UAC_CTL_CLOCK_MULTIPLIER:
        if(selector<=2) {p.Width=2;p.ReadOnly=1;}break;
    case UAC_CTL_SELECTOR:
        if(selector==1) p.Width=1;
        else if(selector==2) {p.Width=4;p.ReadOnly=1;}break;
    case UAC_CTL_FEATURE:
        p.Channels=1;
        switch(selector) {
        case 1:case 7:case 9:case 10:case 13:case 17:p.Width=1;break;
        case 2:case 11:case 12:p.Width=2;p.Signed=1;p.Range=1;break;
        case 3:case 4:case 5:p.Width=1;p.Signed=1;p.Range=1;break;
        case 14:case 15:p.Width=1;p.ReadOnly=1;break;
        case 16:p.Width=4;p.ReadOnly=1;p.Channels=0;break;
        }break;
    case UAC_CTL_PARAM_EQ:
        p.Channels=1;
        switch(selector) {
        case 1:p.Width=1;p.Channels=0;break;
        case 2:case 3:p.Width=4;p.Range=1;break;
        case 4:p.Width=2;p.Signed=1;p.Range=1;break;
        case 5:case 6:p.Width=1;p.ReadOnly=1;p.Channels=1;break;
        case 7:p.Width=4;p.ReadOnly=1;p.Channels=0;break;
        }break;
    case UAC_CTL_ENCODER:
        switch(selector) {
        case 1:p.Width=4;p.Range=1;break;
        case 2:p.Width=1;p.Range=1;break;
        case 3:case 4:p.Width=1;break;
        case 5:case 6:p.Width=1;p.ReadOnly=1;p.Channels=1;break;
        case 7:p.Width=2;p.Signed=1;p.ReadOnly=1;break;
        default:if(selector>=8 && selector<=15) {p.Width=4;p.Signed=1;p.Range=1;}
        }break;
    case UAC_CTL_AS_INTERFACE:
        p.Recipient=UAC_CTL_INTERFACE_RECIPIENT;p.ReadOnly=1;
        if(selector==1) p.Width=1;else if(selector==3) p.Width=4;break;
    case UAC_CTL_ENDPOINT:
        p.Recipient=UAC_CTL_ENDPOINT_RECIPIENT;
        if(selector<=3) {p.Width=1;p.ReadOnly=selector!=1;}break;
    case UAC_CTL_MIXER:
        if(selector==1) {p.Width=2;p.Signed=1;p.Range=1;p.Channels=1;}break;
    }
    if(!p.Width) return 0;*out=p;return 1;
}
static void Put(unsigned char *p,unsigned int n) {p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
int UacCtlSetup(unsigned int family,unsigned int selector,const UAC_CTL_TARGET *t,
    unsigned int access,unsigned int attribute,unsigned int read,unsigned int length,
    unsigned int rangeWritable,unsigned char setup[8])
{
    UAC_CTL_POLICY p;unsigned int index,recipient,i;unsigned char b[8];
    if(!setup || !t || !UacCtlPolicy(family,selector,&p) || read>1 || rangeWritable>1 ||
       (access!=1 && access!=3) || (!read && (access!=3 || p.ReadOnly)) ||
       t->Interface>255 || t->Entity>255 || t->Endpoint>255 || t->Channel>255 ||
       t->MaxChannel>255 || t->Channel>t->MaxChannel || (!p.Channels && t->Channel) ||
       length>65535) return 0;
    if(attribute==UAC_CTL_CUR) {if(length!=p.Width) return 0;}
    else if(attribute==UAC_CTL_RANGE_ATTRIBUTE) {
        if(!p.Range || length<2 || (length-2)%(3*p.Width) ||
           (!read && (!rangeWritable || length==2))) return 0;
    } else return 0;
    recipient=1;
    if(p.Recipient==UAC_CTL_ENTITY_RECIPIENT) {
        if(!t->Entity || t->Endpoint) return 0;index=(t->Entity<<8)|t->Interface;
    } else if(p.Recipient==UAC_CTL_INTERFACE_RECIPIENT) {
        if(t->Entity || t->Endpoint) return 0;index=t->Interface;
    } else {
        if(t->Entity || t->Interface || !t->Endpoint || (t->Endpoint&0x70) || !(t->Endpoint&15)) return 0;
        index=t->Endpoint;recipient=2;
    }
    b[0]=(unsigned char)(0x20|recipient|(read?0x80:0));b[1]=(unsigned char)attribute;
    Put(b+2,(selector<<8)|t->Channel);Put(b+4,index);Put(b+6,length);
    for(i=0;i<8;i++) setup[i]=b[i];return 1;
}
int UacCtlValue(unsigned int family,unsigned int selector,const unsigned char *bytes,
    unsigned int length,unsigned int pinCount)
{
    UAC_CTL_POLICY p;UAC_CTL_INT value;
    if(!bytes || !UacCtlPolicy(family,selector,&p) || length!=p.Width) return 0;
    value=Number(bytes,p.Width,p.Signed);
    if(family==UAC_CTL_CLOCK_SELECTOR || (family==UAC_CTL_SELECTOR && selector==1))
        return pinCount>=1 && pinCount<=255 && value>=1 && value<=pinCount;
    if(family==UAC_CTL_CLOCK_MULTIPLIER) return value!=0;
    if((family==UAC_CTL_FEATURE && (selector==11 || selector==12)) ||
       (family==UAC_CTL_PARAM_EQ && selector==4)) return value!=-32768;
    if(family==UAC_CTL_ENCODER) {
        if(selector==2) return value<=100;
        if(selector==4) return value>=1 && value<=9;
    }
    if((family==UAC_CTL_CLOCK_SOURCE && selector==2) || family==UAC_CTL_ENDPOINT ||
       (family==UAC_CTL_FEATURE && (selector==1 || selector==7 || selector==9 || selector==10 ||
        selector==13 || selector==14 || selector==15 || selector==17)) ||
       (family==UAC_CTL_PARAM_EQ && (selector==1 || selector==5 || selector==6)) ||
       (family==UAC_CTL_ENCODER && (selector==3 || selector==5 || selector==6))) return value<=1;
    if(family==UAC_CTL_AS_INTERFACE && selector==3) {
        unsigned int bits=Le(bytes,4);return bits && !(bits&(bits-1));
    }
    return 1;
}
static int Triple(const unsigned char *b,unsigned int w,unsigned int sign,UAC_CTL_TRIPLE *t)
{
    t->Min=Number(b,w,sign);t->Max=Number(b+w,w,sign);t->Res=Number(b+2*w,w,sign);
    if(t->Min>t->Max) return 0;
    return t->Min==t->Max?t->Res==0:t->Res>0;
}
int UacCtlRange(const unsigned char *bytes,unsigned int length,unsigned int width,
    unsigned int sign,UAC_CTL_RANGE *out)
{
    unsigned int count,i,stride;UAC_CTL_TRIPLE t;UAC_CTL_INT previous=0;
    UAC_CTL_RANGE r;
    if(!bytes || !out || length<2 || (width!=1 && width!=2 && width!=4) || sign>1) return 0;
    count=Le(bytes,2);stride=3*width;
    /* Divide before multiply: no unchecked count/length multiplication. */
    if(!count || (length-2)%stride || count!=(length-2)/stride) return 0;
    for(i=0;i<count;i++) {
        if(!Triple(bytes+2+i*stride,width,sign,&t) || (i && t.Min<=previous)) return 0;
        previous=t.Max;
    }
    r.Bytes=bytes;r.Length=length;r.Count=count;r.Width=width;r.Signed=sign;*out=r;return 1;
}
int UacCtlRangeGet(const UAC_CTL_RANGE *r,unsigned int index,UAC_CTL_TRIPLE *out)
{
    UAC_CTL_TRIPLE t;
    if(!r || !out || !r->Bytes || (r->Width!=1 && r->Width!=2 && r->Width!=4) || r->Signed>1 ||
       r->Length<2 || !r->Count || r->Count>65535 || index>=r->Count ||
       (r->Length-2)%(3*r->Width) || r->Count!=(r->Length-2)/(3*r->Width)) return 0;
    if(!Triple(r->Bytes+2+index*3*r->Width,r->Width,r->Signed,&t)) return 0;
    *out=t;return 1;
}
int UacCtlRangeContains(const UAC_CTL_RANGE *r,UAC_CTL_INT value)
{
    unsigned int i;UAC_CTL_TRIPLE t;
    if(!r || !r->Count || r->Count>65535) return 0;
    for(i=0;i<r->Count;i++) {
        if(!UacCtlRangeGet(r,i,&t)) return 0;
        if(value>=t.Min && value<=t.Max && (!t.Res || (value-t.Min)%t.Res==0)) return 1;
    }
    return 0;
}
