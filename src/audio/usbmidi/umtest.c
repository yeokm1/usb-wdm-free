/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original USB MIDI codec/descriptor/port regression and guarded-buffer tests.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "umcore.h"
static unsigned int checks;
#define CHECK(x) do { checks++; if(!(x)) {printf("FAIL %u: %s\n",__LINE__,#x);exit(1);} } while(0)
/* One cable each way, MS interface only (Appendix B shape, no AC). */
static unsigned char fixture[]={
 9,2,83,0,1,1,0,128,50,
 9,4,1,0,2,1,3,0,0,
 7,0x24,1,0,1,37,0,
 6,0x24,2,1,1,0, 6,0x24,2,2,2,0,
 9,0x24,3,1,3,1,2,1,0, 9,0x24,3,2,4,1,1,1,0,
 9,5,1,2,64,0,0,0,0, 5,0x25,1,1,1,
 9,5,0x81,2,64,0,0,0,0, 5,0x25,1,1,3
};
/* AC + MS, two DIN pairs on one endpoint pair, External jacks named 4/5.
 * The same bytes are midiemu.py's "multi" profile. */
static unsigned char multi[]={
 9,2,133,0,2,1,0,0x80,50,
 9,4,0,0,0,1,1,0,0,
 9,0x24,1,0,1,9,0,1,1,
 9,4,1,0,2,1,3,0,0,
 7,0x24,1,0,1,67,0,
 6,0x24,2,1,1,0, 6,0x24,2,1,2,0,
 9,0x24,3,2,3,1,1,1,4, 9,0x24,3,2,4,1,2,1,5,
 6,0x24,2,2,5,4, 6,0x24,2,2,6,5,
 9,0x24,3,1,7,1,5,1,0, 9,0x24,3,1,8,1,6,1,0,
 9,5,1,2,64,0,0,0,0, 6,0x25,1,2,1,2,
 9,5,0x81,2,64,0,0,0,0, 6,0x25,1,2,7,8
};
static void Event(UM_ENCODER *s,const unsigned char *bytes,unsigned int n,
                  const unsigned char want[4])
{
    unsigned char p[4]; unsigned int i;
    for(i=0;i<n;i++) CHECK(UmEncodeByte(s,bytes[i],p)==(i+1==n));
    CHECK(!memcmp(p,want,4));
}
static void Codec(void)
{
    UM_ENCODER s;
    unsigned char p[4],data[3],bytes[310],recovered[310];
    unsigned int cable,status,a,b,n,i,count,r;
    CHECK(!UmEncoderInit(&s,16));
    for(cable=0;cable<16;cable++) {
        CHECK(UmEncoderInit(&s,cable));
        for(status=0x80;status<=0xef;status++) {
            unsigned int len=(status<0xc0 || status>=0xe0)?3:2;
            unsigned char input[3],want[4];
            input[0]=(unsigned char)status;input[1]=0x21;input[2]=0x7f;
            want[0]=(unsigned char)((cable<<4)|(status>>4));
            want[1]=(unsigned char)status;want[2]=0x21;want[3]=(len==3)?0x7f:0;
            Event(&s,input,len,want);
            CHECK(UmDecodePacket(want,4,1u<<cable,data)==len);
            CHECK(!memcmp(data,input,len));
            CHECK(!UmDecodePacket(want,4,(1u<<cable)^65535,data));
            /* Running status expands to a complete USB event. */
            Event(&s,input+1,len-1,want);
        }
    }
    for(n=0;n<=300;n++) {
        CHECK(UmEncoderInit(&s,7)); bytes[0]=0xf0;
        for(i=0;i<n;i++) bytes[i+1]=(unsigned char)(i&127);
        bytes[n+1]=0xf7; count=0;
        for(i=0;i<n+2;i++) {
            CHECK(UmEncodeByte(&s,0xf8,p)==1); CHECK(p[0]==0x7f && p[1]==0xf8);
            r=(unsigned int)UmEncodeByte(&s,bytes[i],p); CHECK(r<=1);
            if(r) { a=UmDecodePacket(p,4,128,data);CHECK(a>=1 && a<=3);
                memcpy(recovered+count,data,a);count+=a; }
        }
        CHECK(count==n+2 && !memcmp(bytes,recovered,count));
    }
    CHECK(UmEncoderInit(&s,0));
    CHECK(UmEncodeByte(&s,1,p)==-1);
    CHECK(UmEncodeByte(&s,0x90,p)==0);CHECK(UmEncodeByte(&s,1,p)==0);
    CHECK(UmEncodeByte(&s,0xf9,p)==1); CHECK(p[1]==0xf9);
    CHECK(UmEncodeByte(&s,2,p)==1 && p[0]==9 && p[1]==0x90);
    CHECK(UmEncodeByte(&s,0xf4,p)==-1); CHECK(UmEncodeByte(&s,2,p)==-1);
    CHECK(UmEncodeByte(&s,0xf0,p)==0); CHECK(UmEncodeByte(&s,1,p)==0);
    CHECK(UmEncodeByte(&s,0xc1,p)==0);CHECK(UmEncodeByte(&s,2,p)==1 && p[0]==12);
    {
        static const unsigned char common[][4]={
            {2,0xf1,0x23,0},{3,0xf2,0x23,0x45},{2,0xf3,0x23,0},
            {5,0xf6,0,0},{5,0xf7,0,0},{15,0xff,0,0}
        };
        static const unsigned char size[]={2,3,2,1,1,1};
        CHECK(UmEncoderInit(&s,0));
        for(i=0;i<sizeof(size);i++) {
            Event(&s,common[i]+1,size[i],common[i]);
            CHECK(UmDecodePacket(common[i],4,1,data)==size[i]);
            CHECK(!memcmp(data,common[i]+1,size[i]));
        }
    }
    for(a=0;a<16;a++) for(b=0;b<256;b++) {
        p[0]=(unsigned char)a;p[1]=(unsigned char)b;p[2]=p[3]=0;
        r=UmDecodePacket(p,4,1,data); CHECK(r<=3);
        if(a==0 || a==1) CHECK(r==0);
        if(a==15) CHECK(r==1 && data[0]==b);
    }
}
static int Zero(const UM_INTERFACE *out)
{
    static UM_INTERFACE zero;
    return !memcmp(out,&zero,sizeof(zero));
}
static void Invariants(const UM_INTERFACE *out)
{
    unsigned int k;
    CHECK(out->Count>=1 && out->Count<=UM_MAX_ENDPOINTS);
    for(k=0;k<out->Count;k++) {
        const UM_ENDPOINT *e=&out->Endpoint[k];
        CHECK(e->Cables>=1 && e->Cables<=16);
        CHECK(e->MaxPacket>=4 && e->MaxPacket<=0x7ff);
        CHECK(e->Type==2 || e->Type==3);
        CHECK((e->Address&0x70)==0 && (e->Address&15));
    }
}
static void Descriptors(void)
{
    UM_INTERFACE out; unsigned int i,j,success=0;
    unsigned char copy[sizeof(multi)];
    CHECK(sizeof(fixture)==83 && sizeof(multi)==133);
    CHECK(UmParseInterface(fixture,sizeof(fixture),1,0,&out));
    CHECK(out.Count==2 && out.Endpoint[0].Jack[0]==1 && out.Endpoint[1].Jack[0]==3);
    CHECK(out.Endpoint[0].Name[0]==0 && out.Endpoint[0].Type==2 && out.Declared==2);
    CHECK(UmFindInterface(fixture,sizeof(fixture),&out) && out.Number==1 && out.Alternate==0);
    /* wTotalLength is recorded, not enforced (either reading, or neither). */
    memcpy(copy,fixture,sizeof(fixture));copy[23]=65;
    CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out));
    copy[23]=200;CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out));
    CHECK(!UmParseInterface(fixture,sizeof(fixture),1,1,&out));CHECK(Zero(&out));
    CHECK(!UmParseInterface(fixture,sizeof(fixture),0,0,&out));CHECK(Zero(&out));
    {   /* Fatal: config header, class, bcdMSC, duplicate/zero jack ID, bounds. */
        static const unsigned char offset[]={1,2,14,15,22,35,29,9,18,42};
        static const unsigned char value[]={3,84,2,1,2,1,0,1,1,2};
        for(i=0;i<sizeof(offset);i++) {
            memcpy(copy,fixture,sizeof(fixture)); copy[offset[i]]=value[i];
            CHECK(!UmParseInterface(copy,sizeof(fixture),1,0,&out));
            CHECK(Zero(&out));
        }
    }
    /* Skipped, not fatal: an isochronous endpoint, a CS endpoint with the
     * wrong subtype, zero cables; interrupt endpoints are accepted. */
    memcpy(copy,fixture,sizeof(fixture));copy[58]=1;
    CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out) && out.Count==1 && out.Endpoint[0].Address==0x81);
    memcpy(copy,fixture,sizeof(fixture));copy[66]=2;
    CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out) && out.Count==1 && out.Endpoint[0].Address==0x81);
    memcpy(copy,fixture,sizeof(fixture));copy[67]=0;
    CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out) && out.Count==1);
    memcpy(copy,fixture,sizeof(fixture));copy[72]=3;copy[73]=8;
    CHECK(UmParseInterface(copy,sizeof(fixture),1,0,&out) && out.Count==2 &&
        out.Endpoint[1].Type==3 && out.Endpoint[1].Interval==0);
    memcpy(copy,fixture,sizeof(fixture));copy[58]=1;copy[72]=1;
    CHECK(!UmParseInterface(copy,sizeof(fixture),1,0,&out));CHECK(Zero(&out));
    /* Names from External jacks; ports and filters. */
    CHECK(UmParseInterface(multi,sizeof(multi),1,0,&out));
    CHECK(out.Count==2 && out.Endpoint[0].Cables==2 && out.Endpoint[1].Cables==2);
    CHECK(out.Endpoint[0].Name[0]==4 && out.Endpoint[0].Name[1]==5);
    CHECK(out.Endpoint[1].Name[0]==4 && out.Endpoint[1].Name[1]==5);
    {
        UM_PORTMAP map;
        CHECK(UmMapPorts(&out,&map)==2);
        CHECK(map.Outputs==2 && map.Inputs==2 && map.OutPipes==1 && map.InPipes==1);
        CHECK(map.OutPipe[0]==0 && map.InPipe[0]==1);
        CHECK(map.Out[1].Cable==1 && map.Out[1].Name==5 && map.In[0].Name==4);
        CHECK(UmMapPorts(NULL,&map)==0 && map.Outputs==0);
    }
    /* An embedded jack's own iJack wins over the External one. */
    memcpy(copy,multi,sizeof(multi));copy[48]=9;
    CHECK(UmParseInterface(copy,sizeof(multi),1,0,&out) && out.Endpoint[0].Name[0]==9);
    /* Alternate choice: MIDI 2.0 alternate 1 is ignored; a broken alt 0
     * falls back to a MIDI 1.0 alt 1. */
    {
        static unsigned char two[83+9+7];
        memcpy(two,fixture,sizeof(fixture));
        memcpy(two+83,"\x09\x04\x01\x01\x00\x01\x03\x00\x00\x07\x24\x01\x00\x02\x07\x00",16);
        two[2]=(unsigned char)sizeof(two);
        CHECK(UmFindInterface(two,sizeof(two),&out) && out.Alternate==0 && out.Count==2);
        two[22]=2; two[83+13]=1;  /* alt0 becomes 2.0; alt1 1.0 without endpoints */
        CHECK(!UmFindInterface(two,sizeof(two),&out));CHECK(Zero(&out));
    }
    /* Every truncation and single-byte mutation: zero output or invariants. */
    for(i=0;i<sizeof(multi);i++) {
        CHECK(!UmParseInterface(multi,i,1,0,&out));CHECK(Zero(&out));
        for(j=0;j<256;j++) {
            memcpy(copy,multi,sizeof(multi));copy[i]=(unsigned char)j;
            if(UmFindInterface(copy,sizeof(multi),&out)) {
                UM_PORTMAP map;unsigned int f;
                success++;Invariants(&out);
                f=UmMapPorts(&out,&map);
                CHECK(f<=UM_MAX_PORTS && map.Outputs<=UM_MAX_PORTS && map.Inputs<=UM_MAX_PORTS);
                CHECK(map.OutPipes<=UM_MAX_PIPES && map.InPipes<=UM_MAX_PIPES);
            } else CHECK(Zero(&out));
        }
    }
    CHECK(success>0);
}
static unsigned int Wide(const char *s,unsigned short *w)
{
    unsigned int n=0; while(s[n]) {w[n]=(unsigned char)s[n];n++;} return n;
}
static void Name(const char *product,const char *jack,int several,unsigned int index,const char *want)
{
    unsigned short p[64],j[64],out[32];unsigned int pn=0,jn=0,n,i;
    if(product) pn=Wide(product,p);
    if(jack) jn=Wide(jack,j);
    for(i=0;i<32;i++) out[i]=0xffff;
    n=UmPortName(product?p:NULL,pn,jack?j:NULL,jn,several,index,out);
    CHECK(n==strlen(want) && n<=31 && out[n]==0);
    for(i=0;i<n;i++) CHECK(out[i]==(unsigned char)want[i]);
}
static void Names(void)
{
    Name("UM-ONE","",0,0,"UM-ONE");
    Name("UM-ONE",NULL,1,1,"UM-ONE MIDI 2");
    Name("Keystation ","",1,9,"Keystation MIDI 10");
    Name("MIDI 2x2 SIM","Port A",1,0,"MIDI 2x2 SIM Port A");
    Name("MIDI 2x2","MIDI 2x2 Port B",1,1,"MIDI 2x2 Port B");
    Name(NULL,NULL,0,0,"USB MIDI");
    Name("",NULL,1,3,"USB MIDI MIDI 4");
    Name(NULL,"DIN 1",1,0,"USB MIDI DIN 1");
    Name("A very long product name string here","",1,15,"A very long product name string");
    Name("Thirty-one characters exactly!!","x",0,0,"Thirty-one characters exactly!!");
}
static void Guarded(void)
{
    SYSTEM_INFO si; unsigned char *base,*edge; DWORD old;
    unsigned int i; UM_INTERFACE out; unsigned char data[3];
    GetSystemInfo(&si);
    base=(unsigned char*)VirtualAlloc(NULL,si.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(base!=NULL);
    CHECK(VirtualProtect(base+si.dwPageSize,si.dwPageSize,PAGE_NOACCESS,&old));
    edge=base+si.dwPageSize;
    for(i=0;i<=sizeof(multi);i++) {
        memcpy(edge-i,multi,i);
        CHECK(UmFindInterface(edge-i,i,&out)==(i==sizeof(multi)));
        CHECK(UmParseInterface(edge-i,i,1,0,&out)==(i==sizeof(multi)));
    }
    for(i=0;i<4;i++) CHECK(!UmDecodePacket(edge-i,i,65535,data));
    CHECK(VirtualFree(base,0,MEM_RELEASE));
}
int main(void)
{
    Codec();Descriptors();Names();Guarded();
    printf("PASS %u checks; MIDI1 portable core only, no kernel/VM claim\n",checks);
    return 0;
}
