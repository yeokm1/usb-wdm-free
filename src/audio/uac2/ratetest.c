/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original cadence/feedback regression tests, including guarded inputs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "uacrate.h"
/* uacwatch.h is a header of static helpers; this test uses only the
 * wall-clock ones (watchtest.c covers the bus-time ones), so MSVC 6.0
 * /W4 would refuse the unused rest (C4505). */
#pragma warning(disable: 4505)
#include "uacwatch.h"

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

static void Feedback3(unsigned char *b,unsigned int value)
{
    b[0]=(unsigned char)value; b[1]=(unsigned char)(value>>8); b[2]=(unsigned char)(value>>16);
}

/* Full Speed explicit feedback: 3 bytes 10.14 or 4 bytes 16.16, per frame,
 * decoded by the length received (docs/design/uac2.md section 23). */
static void FullSpeed(void)
{
    UAC_RATE s,t;
    unsigned char b[32];
    unsigned int i,sum,good,bad;
    CHECK(UacRateFeedbackStride(1,4)==4);
    CHECK(UacRateFeedbackStride(1,1024)==4);
    CHECK(UacRateFeedbackStride(1,3)==0);
    CHECK(UacRateFeedbackStride(1,0)==0);
    CHECK(UacRateFeedbackStride(0,4)==4);
    CHECK(UacRateFeedbackStride(0,1023)==4);
    CHECK(UacRateFeedbackStride(0,3)==3);
    CHECK(UacRateFeedbackStride(0,2)==0);
    CHECK(UacRateFeedbackStride(0,0)==0);

    /* 48 kHz both ways: 48 frames a 1 ms frame. */
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    Feedback3(b,48u<<14);
    CHECK(UacRateFeedback(&s,b,3));
    CHECK(Sum(&s,1000,4,200)==192000);
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    Feedback(b,48u<<16);
    CHECK(UacRateFeedback(&s,b,4));
    CHECK(Sum(&s,1000,4,200)==192000);
    /* A 10.14 value and the same value in 16.16 give the same cadence. */
    CHECK(UacRateInitialize(&s,44100,0,1,4,200));
    CHECK(UacRateInitialize(&t,44100,0,1,4,200));
    Feedback3(b,44100u*16384u/1000u);
    CHECK(UacRateFeedback(&s,b,3));
    Feedback(b,(44100u*16384u/1000u)<<2);
    CHECK(UacRateFeedback(&t,b,4));
    for(i=0;i<10000;i++) CHECK(UacRateNextBytes(&s)==UacRateNextBytes(&t));
    /* 16.16 carries 2 bits of fraction 10.14 cannot: 44.1 kHz exactly. */
    CHECK(UacRateInitialize(&s,44100,0,1,4,200));
    Feedback(b,44100u*65536u/1000u+1u);
    CHECK(UacRateFeedback(&s,b,4));
    sum=Sum(&s,10000,4,200)/4;
    CHECK(sum>=440999 && sum<=441001);
    /* A device running 0.5% fast, and one slow, followed in 16.16. */
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    Feedback(b,(48u<<16)+15729u); /* 48.24 samples/frame */
    CHECK(UacRateFeedback(&s,b,4));
    sum=Sum(&s,10000,4,200)/4;
    CHECK(sum>=482399 && sum<=482401);
    Feedback(b,(47u<<16)+49152u); /* 47.75 */
    CHECK(UacRateFeedback(&s,b,4));
    sum=Sum(&s,10000,4,200)/4;
    CHECK(sum>=477499 && sum<=477501);

    /* Values outside the window or the endpoint, either length. */
    CHECK(UacRateInitialize(&s,48000,0,1,4,196)); /* 49 frames at most */
    bad=s.BadFeedback;
    Feedback(b,0); CHECK(!UacRateFeedback(&s,b,4));
    Feedback(b,0xffffffffu); CHECK(!UacRateFeedback(&s,b,4));
    Feedback(b,(42u<<16)-1u); CHECK(!UacRateFeedback(&s,b,4)); /* below -12.5% */
    Feedback(b,(54u<<16)+1u); CHECK(!UacRateFeedback(&s,b,4)); /* above +12.5% */
    Feedback(b,(49u<<16)+1u); CHECK(!UacRateFeedback(&s,b,4)); /* 50 frames > endpoint */
    Feedback3(b,(49u<<14)+1u); CHECK(!UacRateFeedback(&s,b,3));
    Feedback3(b,0xffffffu); CHECK(!UacRateFeedback(&s,b,3));
    Feedback(b,(48u<<14)); CHECK(!UacRateFeedback(&s,b,4)); /* 10.14 padded to 4 bytes */
    CHECK(s.BadFeedback==bad+8 && !s.FeedbackActive && !s.GoodFeedback);
    CHECK(Sum(&s,1000,4,196)==192000); /* nominal kept */
    Feedback(b,49u<<16); CHECK(UacRateFeedback(&s,b,4));
    CHECK(s.FeedbackActive && s.GoodFeedback==1);

    /* Lengths, strides and offsets at the packet level. */
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    bad=s.BadFeedback;
    CHECK(UacRateFeedbackPacket(&s,b,32,0,0,4)==UAC_FEEDBACK_EMPTY); /* no rate yet: serviced, empty */
    CHECK(s.BadFeedback==bad && !s.FeedbackActive);
    for(i=0;i<32;i+=4) Feedback(b+i,48u<<16);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,1,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,2,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,5,4)==UAC_FEEDBACK_BAD); /* above the stride */
    CHECK(UacRateFeedbackPacket(&s,b,32,0,4,3)==UAC_FEEDBACK_BAD); /* above a 3-byte stride */
    CHECK(UacRateFeedbackPacket(&s,b,32,29,4,4)==UAC_FEEDBACK_BAD); /* past the buffer */
    CHECK(UacRateFeedbackPacket(&s,b,32,33,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,NULL,32,0,4,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,0xfffffffcu,4,4)==UAC_FEEDBACK_BAD);
    CHECK(s.BadFeedback==bad+8 && !s.FeedbackActive);
    CHECK(UacRateFeedbackPacket(&s,b,32,28,4,4)==UAC_FEEDBACK_VALUE);
    Feedback3(b+4,48u<<14);
    CHECK(UacRateFeedbackPacket(&s,b,32,4,3,4)==UAC_FEEDBACK_VALUE); /* 3 bytes in a 4-byte stride */
    CHECK(UacRateFeedbackPacket(&s,b,32,8,0,4)==UAC_FEEDBACK_KEEPALIVE);
    good=s.GoodFeedback;
    CHECK(good==2 && s.BadFeedback==bad+8);
    /* Once active, a zero-length packet is still held to the buffer. */
    CHECK(UacRateFeedbackPacket(&s,b,32,33,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,29,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,0xffffffffu,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,NULL,32,0,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,0,0)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,3,0,0,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,b,32,28,0,4)==UAC_FEEDBACK_KEEPALIVE);
    CHECK(s.GoodFeedback==good && s.BadFeedback==bad+14);
    CHECK(UacRateFeedbackPacket(&s,b,24,0,3,3)==UAC_FEEDBACK_BAD); /* b[0..2] is 16.16 low bytes */
    Feedback3(b,48u<<14);
    CHECK(UacRateFeedbackPacket(&s,b,24,0,3,3)==UAC_FEEDBACK_VALUE);

    /* High Speed: 4 bytes per microframe only, as before. */
    CHECK(UacRateInitialize(&s,48000,1,1,4,112));
    Feedback(b,6u<<16);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,3,4)==UAC_FEEDBACK_BAD);
    Feedback3(b,6u<<14);
    CHECK(!UacRateFeedback(&s,b,3));
    Feedback(b,6u<<16);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,4,4)==UAC_FEEDBACK_VALUE);
    CHECK(Sum(&s,8000,4,112)==192000);
    CHECK(UacRateFeedbackPacket(&s,b,32,4,0,4)==UAC_FEEDBACK_KEEPALIVE);
}

/* The worker's deadline as uacusb.c keeps it: refreshed only by a packet
 * UacRateFeedbackPacket accepts (a value, or a keepalive after one). */
static void Deadline(void)
{
    UAC_RATE s;
    unsigned char b[32];
    UAC_WATCH_TICK now,last;
    const UAC_WATCH_TICK ms=10000;
    unsigned int i;
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    for(i=0;i<32;i+=4) Feedback(b+i,0x7fffffffu);
    last=0;
    for(now=ms;now<=1100*ms;now+=ms) {
        if(UacRateFeedbackPacket(&s,b,32,0,4,4)!=UAC_FEEDBACK_BAD) last=now;
        if(UacRateFeedbackPacket(&s,b,32,4,3,4)!=UAC_FEEDBACK_BAD) last=now;
        if(UacRateFeedbackPacket(&s,b,32,8,5,4)!=UAC_FEEDBACK_BAD) last=now;
        CHECK(UacFeedbackLate(now,last,s.FeedbackActive)==(now>1000*ms));
    }
    CHECK(last==0 && !s.FeedbackActive);
    /* Section 25.6: empty answers before the first value keep the start
     * deadline away (the endpoint is serviced, the cadence nominal), for
     * as long as they come; a value then makes the stream active. */
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    last=0;
    for(now=ms;now<=5000*ms;now+=ms) {
        CHECK(UacRateFeedbackPacket(&s,b,32,4,0,4)==UAC_FEEDBACK_EMPTY); last=now;
        CHECK(!UacFeedbackLate(now,last,s.FeedbackActive) && !s.FeedbackActive && !s.BadFeedback);
    }
    Feedback(b,48u<<16);
    CHECK(UacRateFeedbackPacket(&s,b,32,0,4,4)==UAC_FEEDBACK_VALUE && s.FeedbackActive);
    CHECK(UacRateFeedbackPacket(&s,b,32,4,0,4)==UAC_FEEDBACK_KEEPALIVE);

    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    Feedback(b,48u<<16);
    last=0;
    now=500*ms;
    CHECK(UacRateFeedbackPacket(&s,b,32,0,4,4)==UAC_FEEDBACK_VALUE); last=now;
    for(now=600*ms;now<=2000*ms;now+=100*ms) {
        CHECK(UacRateFeedbackPacket(&s,b,32,4,0,4)==UAC_FEEDBACK_KEEPALIVE); last=now;
        CHECK(!UacFeedbackLate(now,last,s.FeedbackActive));
    }
    Feedback(b+8,0);
    for(now=2001*ms;now<=2300*ms;now+=ms) {
        if(UacRateFeedbackPacket(&s,b,32,8,4,4)!=UAC_FEEDBACK_BAD) last=now;
        if(UacRateFeedbackPacket(&s,b,32,12,2,4)!=UAC_FEEDBACK_BAD) last=now;
        if(UacRateFeedbackPacket(&s,b,32,40,0,4)!=UAC_FEEDBACK_BAD) last=now; /* misplaced keepalive */
        CHECK(UacFeedbackLate(now,last,s.FeedbackActive)==(now>2200*ms));
    }
    CHECK(last==2000*ms);
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
    CHECK(!UacRateFeedback(&s,guard+info.dwPageSize,5)); /* reject before dereference */
    Feedback(guard+info.dwPageSize-4,48u<<16);
    CHECK(UacRateInitialize(&s,48000,0,1,4,200));
    CHECK(UacRateFeedback(&s,guard+info.dwPageSize-4,4)); /* FS 16.16 to the page's edge */
    CHECK(UacRateFeedbackPacket(&s,guard,info.dwPageSize,info.dwPageSize-4,4,4)==UAC_FEEDBACK_VALUE);
    CHECK(UacRateFeedbackPacket(&s,guard,info.dwPageSize,info.dwPageSize-3,4,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,guard,info.dwPageSize,info.dwPageSize,3,4)==UAC_FEEDBACK_BAD);
    CHECK(UacRateFeedbackPacket(&s,guard,info.dwPageSize,info.dwPageSize+4,4,4)==UAC_FEEDBACK_BAD);
    CHECK(VirtualFree(guard,0,MEM_RELEASE));

    FullSpeed();
    Deadline();
    printf("PASS %u rate/packet checks\n",checks);
    return 0;
}
