/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original guarded tests for the Linux-adapted Mixer Unit helper.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "uacmix.h"
static unsigned int Checks;
static void Check(int condition,const char *expression,unsigned int line)
{Checks++;if(!condition) {printf("FAIL line%u: %s\n",line,expression);ExitProcess(1);}}
#define CHECK(x) Check((x),#x,__LINE__)
int main(void)
{
    SYSTEM_INFO info;unsigned char *memory,*end,*d,setup[8],before[8];
    unsigned int inputs,outputs,cells,pins,bytes,length,i,j,k,at,count,mcn;
    unsigned int channels[2];UAC_MIX mix,saved;
    GetSystemInfo(&info);memory=(unsigned char*)VirtualAlloc(NULL,3*info.dwPageSize,MEM_RESERVE,PAGE_NOACCESS);
    CHECK(memory!=NULL);CHECK(VirtualAlloc(memory+info.dwPageSize,info.dwPageSize,MEM_COMMIT,PAGE_READWRITE)!=NULL);
    end=memory+2*info.dwPageSize;
    for(inputs=1;inputs<=256;inputs++) for(outputs=1;outputs<=255 && outputs<=256/inputs;outputs++) {
        cells=inputs*outputs;pins=inputs==256?2:1;bytes=(cells+7)/8;length=13+pins+bytes;
        d=end-length;memset(d,0,length);d[0]=(unsigned char)length;d[1]=0x24;d[2]=4;d[3]=9;d[4]=(unsigned char)pins;
        d[5]=1;if(pins==2) d[6]=2;d[5+pins]=(unsigned char)outputs;
        channels[0]=inputs==256?255:inputs;channels[1]=1;
        d[6+pins]=3;d[10+pins]=7;d[length-1]=11;
        CHECK(UacMixParse(d,length,channels,pins,&mix));
        CHECK(mix.Inputs==inputs && mix.Outputs==outputs && mix.Cells==cells);
        CHECK(mix.ChannelConfig==3 && mix.ChannelNames==7 && mix.MixerName==11);
        CHECK(!UacMixAccess(&mix,inputs,0));CHECK(!UacMixAccess(&mix,0,outputs));
        for(i=0;i<length;i++) {saved=mix;CHECK(!UacMixParse(d,i,channels,pins,&mix));CHECK(!memcmp(&mix,&saved,sizeof(mix)));}
        for(i=0;i<inputs;i++) for(j=0;j<outputs;j++) {
            at=i*outputs+j;CHECK(UacMixControlNumber(&mix,i,j,&mcn));CHECK(mcn==at);
            CHECK(UacMixAccess(&mix,i,j)==1); /* fixed nodes remain readable */
            CHECK(UacMixSetup(&mix,3,i,j,1,1,2,0,setup));
            CHECK(setup[0]==0xa1 && setup[1]==1 && setup[2]==at && setup[3]==1 && setup[4]==3 && setup[5]==9 && setup[6]==2 && !setup[7]);
            memcpy(before,setup,8);CHECK(!UacMixSetup(&mix,3,i,j,1,0,2,0,setup));CHECK(!memcmp(before,setup,8));
            d[mix.MatrixOffset+at/8]=(unsigned char)(0x80u>>(at%8));
            CHECK(UacMixAccess(&mix,i,j)==3);CHECK(UacMixSetup(&mix,3,i,j,1,0,2,0,setup));
            CHECK(setup[2]==at && setup[0]==0x21); /* MCN0 and255 included */
            CHECK(!UacMixSetup(&mix,3,i,j,2,0,8,0,setup));
            CHECK(UacMixSetup(&mix,3,i,j,2,0,8,1,setup));
            CHECK(UacMixSetup(&mix,3,i,j,2,1,2,0,setup));
            d[mix.MatrixOffset+at/8]=0;
        }
        if(cells%8) {
            d[mix.MatrixOffset+bytes-1]=1;
            CHECK(!UacMixParse(d,length,channels,pins,&mix));d[mix.MatrixOffset+bytes-1]=0;
        }
    }
    /* Reserved tail fields and invalid descriptor IDs/source relationships. */
    d=end-15;memset(d,0,15);d[0]=15;d[1]=0x24;d[2]=4;d[3]=9;d[4]=1;d[5]=1;d[6]=1;channels[0]=1;
    for(i=0;i<256;i++) {
        int valid=!(i&0xc0);
        for(j=0;j<3;j++) if(((i>>(j*2))&3)==2) valid=0;
        d[13]=(unsigned char)i;CHECK(UacMixParse(d,15,channels,1,&mix)==valid);
    }
    d[13]=0;CHECK(UacMixParse(d,15,channels,1,&mix));
    CHECK(!UacMixParse(d,15,channels,0,&mix));CHECK(!UacMixParse(d,15,NULL,1,&mix));
    channels[0]=0;CHECK(!UacMixParse(d,15,channels,1,&mix));
    channels[0]=256;CHECK(!UacMixParse(d,15,channels,1,&mix));
    channels[0]=0xffffffffu;CHECK(!UacMixParse(d,15,channels,1,&mix));channels[0]=1;
    d[5]=9;CHECK(!UacMixParse(d,15,channels,1,&mix));d[5]=0;CHECK(!UacMixParse(d,15,channels,1,&mix));d[5]=1;
    for(k=0;k<5;k++) CHECK(!UacMixParse(end,k,channels,1,&mix));
    /* All 256 matrix bit positions independently compared with a row/column
     * oracle. In particular catches LSB-first or one-based MCN mistakes. */
    d=end-46;memset(d,0,46);d[0]=46;d[1]=0x24;d[2]=4;d[3]=9;d[4]=1;d[5]=1;d[6]=16;channels[0]=16;
    CHECK(UacMixParse(d,46,channels,1,&mix));
    for(at=0;at<256;at++) {
        d[12+at/8]=(unsigned char)(128>>(at%8));count=0;
        for(i=0;i<16;i++) for(j=0;j<16;j++) {
            int access=UacMixAccess(&mix,i,j);CHECK(access==((16*i+j==at)?3:1));if(access==3) count++;
        }
        CHECK(count==1);d[12+at/8]=0;
    }
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
    printf("PASS: %u Linux-adapted UAC2 mixer matrix/request checks\n",Checks);return 0;
}
