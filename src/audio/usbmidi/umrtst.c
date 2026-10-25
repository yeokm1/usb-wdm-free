/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Captured-profile, malformed-buffer and guard-page regression tests.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "umroland.h"
#include "umcore.h"
static unsigned int checks;
#define CHECK(x) do {checks++;if(!(x)){printf("FAIL %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static void Load(const char *path,unsigned char *bytes,unsigned int n)
{
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);
    CHECK(fread(bytes,1,n,f)==n);CHECK(fgetc(f)==EOF);CHECK(!fclose(f));
}
int main(int argc,char **argv)
{
    unsigned char d[18],b[67],copy[67],zero[sizeof(UM_ROLAND)];
    unsigned int i,j,alt; UM_ROLAND out;
    SYSTEM_INFO si;unsigned char *base,*edge;DWORD old;
    CHECK(argc==5);Load(argv[1],d,sizeof(d));Load(argv[2],b,sizeof(b));
    memset(zero,0,sizeof(zero));
    for(alt=0;alt<2;alt++) {
        CHECK(UmMatchUmOne2(d,18,b,67,alt,&out));
        CHECK(out.Interface==0 && out.Alternate==alt);
        CHECK(out.InAddress==0x81 && out.OutAddress==2 && out.MaxPacket==64);
        CHECK(out.InType==(alt?3u:2u) && out.OutType==2);
        CHECK(out.InCables==1 && out.OutCables==1);
    }
    for(i=0;i<67;i++) {
        CHECK(!UmMatchUmOne2(d,18,b,i,0,&out));CHECK(!memcmp(&out,zero,sizeof(out)));
        for(j=0;j<256;j++) {
            memcpy(copy,b,67);copy[i]=(unsigned char)j;
            if(UmMatchUmOne2(d,18,copy,67,0,&out)) {
                CHECK(out.MaxPacket==64 && out.InCables==1 && out.OutCables==1);
                CHECK(i==6 || i==7 || i==8 || copy[i]==b[i]);
            } else CHECK(!memcmp(&out,zero,sizeof(out)));
        }
    }
    d[8]^=1;CHECK(!UmMatchUmOne2(d,18,b,67,0,&out));d[8]^=1;
    CHECK(!UmMatchUmOne2(d,18,b,67,2,&out));
    CHECK(!UmMatchUmOne2(NULL,18,b,67,0,&out));
    CHECK(!UmMatchUmOne2(d,18,NULL,67,0,&out));
    CHECK(!UmMatchUmOne2(d,18,b,67,0,NULL));
    GetSystemInfo(&si);
    base=(unsigned char*)VirtualAlloc(NULL,si.dwPageSize*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(base!=NULL);CHECK(VirtualProtect(base+si.dwPageSize,si.dwPageSize,PAGE_NOACCESS,&old));
    edge=base+si.dwPageSize;
    for(i=0;i<=67;i++) {
        memcpy(edge-i,b,i);
        CHECK(UmMatchUmOne2(d,18,edge-i,i,0,&out)==(i==67));
    }
    for(i=0;i<=18;i++) {
        memcpy(edge-i,d,i);
        CHECK(UmMatchUmOne2(edge-i,i,b,67,0,&out)==(i==18));
    }
    CHECK(VirtualFree(base,0,MEM_RELEASE));
    {
        unsigned char tab[101],tabDevice[18]; UM_INTERFACE parsed;
        Load(argv[3],tabDevice,18);Load(argv[4],tab,101);
        CHECK(tabDevice[8]==0x82 && tabDevice[9]==5 && tabDevice[10]==0x68 && tabDevice[11]==1);
        CHECK(!UmMatchUmOne2(tabDevice,18,tab,101,0,&out));
        CHECK(UmParseInterface(tab,101,1,0,&parsed));
        CHECK(parsed.Count==2 && parsed.Endpoint[0].Address==2 && parsed.Endpoint[1].Address==0x81);
        CHECK(parsed.Endpoint[0].Cables==1 && parsed.Endpoint[1].Cables==1);
        CHECK(parsed.Endpoint[0].Jack[0]==0x40 && parsed.Endpoint[1].Jack[0]==0x20);
        for(i=0;i<101;i++) CHECK(!UmParseInterface(tab,i,1,0,&parsed));
    }
    printf("PASS %u checks; captured UM-ONE mk2 profile only, no USB I/O\n",checks);
    return 0;
}
