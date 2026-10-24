/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original guarded portable control-core tests; no driver/device access.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "uacctl.h"
static unsigned int Checks;
static void Check(int condition,const char *expression,unsigned int line)
{Checks++;if(!condition) {printf("FAIL line%u: %s\n",line,expression);ExitProcess(1);}}
#define CHECK(x) Check((x),#x,__LINE__)
static void Put(unsigned char *p,unsigned int value,unsigned int width)
{unsigned int i;for(i=0;i<width;i++) p[i]=(unsigned char)(value>>(i*8));}
int main(void)
{
    SYSTEM_INFO info;unsigned char *memory,*end,*b;unsigned int width,sign,i,j,n,len,v,seed=1;
    unsigned char setup[8],before[8],cur[4],bitmap[64];UAC_CTL_POLICY policy;
    UAC_CTL_TARGET target={3,9,0,0,2};UAC_CTL_RANGE range,saved;UAC_CTL_TRIPLE triple;
    GetSystemInfo(&info);memory=(unsigned char*)VirtualAlloc(NULL,3*info.dwPageSize,MEM_RESERVE,PAGE_NOACCESS);
    CHECK(memory!=NULL);CHECK(VirtualAlloc(memory+info.dwPageSize,info.dwPageSize,MEM_COMMIT,PAGE_READWRITE)!=NULL);
    end=memory+2*info.dwPageSize;
    for(i=0;i<256;i++) {
        end[-1]=(unsigned char)i;
        for(j=1;j<=4;j++) {
            int lane=(i>>(2*(j-1)))&3;
            CHECK(UacCtlAccess(end-1,1,j)==(lane==2?-1:lane));
        }
        CHECK(UacCtlAccess(end-1,1,5)==-1);
    }
    memset(bitmap,0xff,sizeof(bitmap));for(i=1;i<=255;i++) CHECK(UacCtlAccess(bitmap,64,i)==3);
    CHECK(UacCtlAccess(NULL,64,1)==-1);CHECK(UacCtlAccess(end,0,1)==-1);
    CHECK(UacCtlAccess(end,0,0)==-1);CHECK(UacCtlAccess(bitmap,64,256)==-1);
    memset(setup,0x55,8);memcpy(before,setup,8);
    CHECK(UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,3,1,0,4,0,setup));
    CHECK(!memcmp(setup,"\x21\x01\x00\x01\x03\x09\x04\x00",8));
    CHECK(UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,1,2,1,2,0,setup));CHECK(setup[0]==0xa1);
    CHECK(!UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,1,1,0,4,0,setup));
    CHECK(!UacCtlSetup(UAC_CTL_CLOCK_SOURCE,2,&target,3,1,0,1,0,setup));
    CHECK(!UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,3,2,0,14,0,setup));
    CHECK(UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,3,2,0,14,1,setup));
    CHECK(!UacCtlSetup(UAC_CTL_CLOCK_SOURCE,1,&target,3,2,0,2,1,setup));
    target.Channel=2;
    CHECK(UacCtlSetup(UAC_CTL_FEATURE,3,&target,3,1,0,1,0,setup));
    CHECK(UacCtlSetup(UAC_CTL_ENCODER,5,&target,1,1,1,1,0,setup));
    CHECK(UacCtlSetup(UAC_CTL_ENCODER,6,&target,1,1,1,1,0,setup));
    CHECK(!UacCtlSetup(UAC_CTL_CLOCK_SELECTOR,1,&target,3,1,0,1,0,setup));
    target.Channel=3;CHECK(!UacCtlSetup(UAC_CTL_FEATURE,1,&target,3,1,0,1,0,setup));
    CHECK(!UacCtlSetup(UAC_CTL_ENCODER,5,&target,1,1,1,1,0,setup));
    target.Channel=0;target.Entity=0;
    CHECK(UacCtlSetup(UAC_CTL_AS_INTERFACE,3,&target,1,1,1,4,0,setup));CHECK(setup[5]==0);
    target.Interface=0;target.Endpoint=0x83;
    CHECK(UacCtlSetup(UAC_CTL_ENDPOINT,2,&target,1,1,1,1,0,setup));
    CHECK(setup[0]==0xa2 && setup[4]==0x83 && !setup[5]);
    for(i=0;i<=255;i++) {
        int valid=(i&15) && !(i&0x70);
        target.Endpoint=i;
        CHECK(UacCtlSetup(UAC_CTL_ENDPOINT,1,&target,3,1,0,1,0,setup)==valid);
    }
    target.Interface=3;target.Entity=9;target.Endpoint=0;
    for(i=0;i<256;i++) for(j=1;j<=10;j++) {
        if(UacCtlPolicy(j,i,&policy)) {
            UAC_CTL_TARGET t=target;
            if(policy.Recipient!=1) t.Entity=0;
            if(policy.Recipient==3) {t.Interface=0;t.Endpoint=1;}
            CHECK(UacCtlSetup(j,i,&t,1,1,1,policy.Width,0,setup));
            CHECK(!UacCtlSetup(j,i,&t,2,1,1,policy.Width,0,setup));
            CHECK(!UacCtlSetup(j,i,&t,0,1,1,policy.Width,0,setup));
            CHECK(!UacCtlSetup(j,i,&t,3,1,2,policy.Width,0,setup));
            CHECK(!UacCtlSetup(j,i,&t,3,3,1,policy.Width,0,setup));
            CHECK(!UacCtlSetup(j,i,&t,3,1,1,65536,0,setup));
            CHECK(UacCtlSetup(j,i,&t,3,1,0,policy.Width,0,setup)==!policy.ReadOnly);
        }
    }
    memcpy(setup,before,8);target.Interface=256;
    CHECK(!UacCtlSetup(1,1,&target,3,1,0,4,0,setup));CHECK(!memcmp(setup,before,8));
    for(i=0;i<256;i++) {
        cur[0]=(unsigned char)i;
        CHECK(UacCtlValue(UAC_CTL_CLOCK_SELECTOR,1,cur,1,7)==(i>=1 && i<=7));
        CHECK(UacCtlValue(UAC_CTL_ENCODER,2,cur,1,0)==(i<=100));
        CHECK(UacCtlValue(UAC_CTL_ENCODER,4,cur,1,0)==(i>=1 && i<=9));
        CHECK(UacCtlValue(UAC_CTL_FEATURE,17,cur,1,0)==(i<=1));
    }
    CHECK(!UacCtlValue(1,1,end,0,0));cur[0]=cur[1]=0;
    CHECK(!UacCtlValue(3,1,cur,2,0));cur[0]=1;CHECK(UacCtlValue(3,1,cur,2,0));
    cur[0]=0;cur[1]=0x80;CHECK(UacCtlValue(UAC_CTL_FEATURE,2,cur,2,0)); /* silence CUR */
    CHECK(!UacCtlValue(UAC_CTL_FEATURE,11,cur,2,0));
    CHECK(!UacCtlValue(UAC_CTL_FEATURE,12,cur,2,0));
    CHECK(!UacCtlValue(UAC_CTL_PARAM_EQ,4,cur,2,0));
    for(width=1;width<=4;width*=2) for(sign=0;sign<=1;sign++) for(n=1;n<=12;n++) {
        len=2+3*width*n;b=end-len;Put(b,n,2);
        for(i=0;i<n;i++) {Put(b+2+3*width*i,3*i,width);Put(b+2+3*width*i+width,3*i+2,width);Put(b+2+3*width*i+2*width,1,width);}
        CHECK(UacCtlRange(b,len,width,sign,&range));CHECK(range.Count==n);
        CHECK(UacCtlRangeContains(&range,0));CHECK(!UacCtlRangeContains(&range,-1));
        CHECK(UacCtlRangeGet(&range,n-1,&triple));CHECK(triple.Max==3*(n-1)+2);
        CHECK(!UacCtlRangeGet(&range,n,&triple));
        for(i=0;i<len;i++) {saved=range;CHECK(!UacCtlRange(b,i,width,sign,&range));CHECK(!memcmp(&saved,&range,sizeof(range)));}
        /* End-of-page input plus count beyond response and zero resolution. */
        b[0]=(unsigned char)(n+1);CHECK(!UacCtlRange(b,len,width,sign,&range));b[0]=(unsigned char)n;
        Put(b+2+2*width,0,width);CHECK(!UacCtlRange(b,len,width,sign,&range));
    }
    /* Independent signed/unsigned8 oracle over randomized triplets. */
    b=end-5;b[0]=1;b[1]=0;
    for(sign=0;sign<=1;sign++) for(i=0;i<50000;i++) {
        int lo,hi,res,valid;
        for(j=2;j<5;j++) {seed=seed*1664525u+1013904223u;b[j]=(unsigned char)(seed>>24);}
        lo=b[2];hi=b[3];res=b[4];
        if(sign) {if(lo>=128) lo-=256;if(hi>=128) hi-=256;if(res>=128) res-=256;}
        valid=lo<=hi && (lo==hi?res==0:res>0);
        CHECK(UacCtlRange(b,5,1,sign,&range)==valid);
        if(valid) for(v=0;v<256;v++) {
            int value=(int)v;if(sign && value>=128) value-=256;
            CHECK(UacCtlRangeContains(&range,value)==(value>=lo && value<=hi && (!res || (value-lo)%res==0)));
        }
    }
    /* Signed32 full-span subtraction must not overflow; U32 maximum survives. */
    b=end-14;Put(b,1,2);Put(b+2,0x80000000u,4);Put(b+6,0x7fffffffu,4);Put(b+10,1,4);
    CHECK(UacCtlRange(b,14,4,1,&range));CHECK(UacCtlRangeContains(&range,-2147483647-1));
    CHECK(UacCtlRangeContains(&range,2147483647));CHECK(!UacCtlRangeContains(&range,((UAC_CTL_INT)1)<<32));
    Put(b+2,0,4);Put(b+6,0xffffffffu,4);Put(b+10,0xffffffffu,4);
    CHECK(UacCtlRange(b,14,4,0,&range));CHECK(UacCtlRangeContains(&range,4294967295u));
    CHECK(!UacCtlRangeContains(&range,1));CHECK(!UacCtlRange(b,14,4,1,&range));
    Put(b+2,7,4);Put(b+6,7,4);Put(b+10,0,4);CHECK(UacCtlRange(b,14,4,0,&range));
    CHECK(UacCtlRangeContains(&range,7));Put(b+10,1,4);CHECK(!UacCtlRange(b,14,4,0,&range));
    CHECK(!UacCtlRange(end,0,4,0,&range));CHECK(!UacCtlRange(end,1,4,0,&range));
    b=end-8;Put(b,2,2);b[2]=0;b[3]=2;b[4]=1;b[5]=2;b[6]=4;b[7]=1;
    CHECK(!UacCtlRange(b,8,1,0,&range)); /* touching intervals */
    b[5]=1;CHECK(!UacCtlRange(b,8,1,0,&range)); /* overlap */
    b[5]=3;CHECK(UacCtlRange(b,8,1,0,&range));
    b[5]=255;CHECK(!UacCtlRange(b,8,1,0,&range)); /* inverted second interval */
    Put(b,0,2);CHECK(!UacCtlRange(b,8,1,0,&range));
    CHECK(!UacCtlRange(b,8,3,0,&range));CHECK(!UacCtlRange(b,8,1,2,&range));
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
    printf("PASS: %u UAC2 control/request/RANGE checks (guard pages and independent8-bit oracle)\n",Checks);
    return 0;
}


