/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original cadence/feedback regression tests, including guarded inputs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "uacrate.h"

static unsigned int checks;
static void Verify(int result,unsigned int line,const char *expression)
{
    checks++;
    if(!result) { printf("FAIL line%u: %s\n",line,expression); exit(1); }
}
#define CHECK(x) Verify(!!(x),__LINE__,#x)

static unsigned int Sum(UAC_RATE *s,unsigned int count,unsigned int frameBytes,unsigned int maximum)
{
    unsigned int i,total=0;
    for(i=0;i<count;i++) {
        unsigned int n=UacRateNextBytes(s);
        CHECK(n && n%frameBytes==0 && n<=maximum);
        total+=n;
    }
    return total;
}

static void Feedback(unsigned char *b,unsigned int value)
{
    b[0]=(unsigned char)value; b[1]=(unsigned char)(value>>8);
    b[2]=(unsigned char)(value>>16); b[3]=(unsigned char)(value>>24);
}

int main(void)
{
    UAC_RATE s;
    unsigned char feedback[4];
    unsigned int i,sum;
    SYSTEM_INFO info;
    DWORD old;
    unsigned char *guard;
    CHECK(sizeof(unsigned int)==4);
    CHECK(!UacRateInitialize(NULL,48000,1,1,4,112));
    CHECK(!UacRateInitialize(&s,48000,1,1,4,23));
    CHECK(UacRateNextBytes(&s)==0);
    CHECK(!UacRateInitialize(&s,0,1,1,4,112));
    CHECK(!UacRateInitialize(&s,384001,1,1,4,112));
    CHECK(!UacRateInitialize(&s,48000,2,1,4,112));
    CHECK(!UacRateInitialize(&s,48000,1,0,4,112));
    CHECK(!UacRateInitialize(&s,48000,1,5,4,112));
    CHECK(!UacRateInitialize(&s,48000,0,2,4,112));
    CHECK(!UacRateInitialize(&s,48000,1,1,0,112));
    CHECK(!UacRateInitialize(&s,48000,1,1,65,112));
    CHECK(!UacRateInitialize(&s,48000,1,1,4,3073));
    CHECK(UacRateInitialize(&s,48000,1,1,4,112));
    CHECK(Sum(&s,8000,4,112)==192000);
    CHECK(UacRateInitialize(&s,44100,1,1,4,112));
    CHECK(Sum(&s,8000,4,112)==176400);
    CHECK(UacRateInitialize(&s,44100,0,1,4,200));
    CHECK(Sum(&s,10,4,200)==441*4);
    CHECK(Sum(&s,990,4,200)==43659*4);
    CHECK(UacRateInitialize(&s,44100,1,3,4,112));
    CHECK(Sum(&s,2000,4,112)==176400);
    CHECK(UacRateInitialize(&s,384000,1,4,1,3072));
    CHECK(Sum(&s,1000,1,3072)==384000);
    CHECK(UacRateInitialize(&s,8000,1,1,4,4));
    Feedback(feedback,57344); /*0.875 samples/microframe*/
    CHECK(UacRateFeedback(&s,feedback,4));
    sum=0;
    for(i=0;i<8;i++) {
        unsigned int bytes=UacRateNextBytes(&s);
        CHECK(bytes<=4 && bytes%4==0); sum+=bytes;
    }
    CHECK(sum==28); /* zero packets are valid at this fractional cadence */

    CHECK(UacRateInitialize(&s,48000,1,1,4,112));
    Feedback(feedback,6u<<16);
    CHECK(UacRateFeedback(&s,feedback,4));
    CHECK(Sum(&s,8000,4,112)==192000);
    Feedback(feedback,(6u<<16)+32768u);
    CHECK(UacRateFeedback(&s,feedback,4));
    CHECK(Sum(&s,8000,4,112)==208000);
    Feedback(feedback,0);
    CHECK(!UacRateFeedback(&s,feedback,4));
    Feedback(feedback,0xffffffffu);
    CHECK(!UacRateFeedback(&s,feedback,4));
    CHECK(!UacRateFeedback(&s,NULL,4));
    CHECK(!UacRateFeedback(&s,feedback,3));
    CHECK(s.GoodFeedback==2 && s.BadFeedback==4);
    CHECK(Sum(&s,2,4,112)==52); /* invalid input retains last cadence */

    CHECK(UacRateInitialize(&s,48000,1,3,4,100));
    Feedback(feedback,(6u<<16)+32768u);
    CHECK(!UacRateFeedback(&s,feedback,4)); /* plausible rate, pipe too small */
    CHECK(Sum(&s,2,4,100)==192);
    CHECK(UacRateInitialize(&s,44100,1,1,4,112));
    CHECK(UacRateNextBytes(&s)==20);
    Feedback(feedback,(5u<<16)+49152u);
    CHECK(UacRateFeedback(&s,feedback,4));
    CHECK(UacRateNextBytes(&s)==24); /* retain pre-feedback fractional phase */

    GetSystemInfo(&info);
    guard=VirtualAlloc(NULL,info.dwPageSize*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(guard!=NULL);
    CHECK(VirtualProtect(guard+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&old));
    Feedback(guard+info.dwPageSize-4,6u<<16);
    CHECK(UacRateInitialize(&s,48000,1,1,4,112));
    CHECK(UacRateFeedback(&s,guard+info.dwPageSize-4,4));
    CHECK(!UacRateFeedback(&s,guard+info.dwPageSize,3)); /* reject before dereference */
    CHECK(UacRateInitialize(&s,44100,0,1,4,200));
    sum=44100u*16384u/1000u;
    for(i=0;i<3;i++) guard[info.dwPageSize-3+i]=(unsigned char)(sum>>(i*8));
    CHECK(UacRateFeedback(&s,guard+info.dwPageSize-3,3));
    sum=Sum(&s,10000,4,200)/4;
    CHECK(sum>=440998 && sum<=441001);
    CHECK(!UacRateFeedback(&s,guard+info.dwPageSize,4));
    CHECK(VirtualFree(guard,0,MEM_RELEASE));
    printf("PASS %u rate/packet checks\n",checks);
    return 0;
}
