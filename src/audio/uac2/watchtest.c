/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Direct test of the predicate used by the actual guest USB worker.
 */
#include <stdio.h>
#include "uacwatch.h"
static unsigned int checks;
static int Check(int value,unsigned int line)
{checks++;if(!value) {printf("FAIL watchdog line%u\n",line);return 0;}return 1;}
#define CHECK(x) if(!Check((x),__LINE__)) return 1
int main(void)
{
    unsigned int i;UAC_WATCH_TICK last,now,base,oldWall;
    CHECK(!UacFeedbackExpired(0,0));CHECK(!UacFeedbackExpired(2000000,0));
    CHECK(UacFeedbackExpired(2000001,0));CHECK(!UacFeedbackExpired(0,1));
    CHECK(!UacFeedbackExpired(0,~(UAC_WATCH_TICK)0));
    for(i=0;i<100000;i++) {
        base=((UAC_WATCH_TICK)i<<32)+123;
        CHECK(!UacFeedbackExpired(base,base));
        CHECK(!UacFeedbackExpired(base+1999999,base));
        CHECK(!UacFeedbackExpired(base+2000000,base));
        CHECK(UacFeedbackExpired(base+2000001,base));
        CHECK(!UacFeedbackExpired(base,base+1));
    }
    last=(~(UAC_WATCH_TICK)0)-4000000;
    CHECK(UacFeedbackExpired(last+2000001,last));
    CHECK(!UacFeedbackExpired(last+2000000,last));
    /* A wall-clock correction must not change either result. Only the
     * independently advancing interrupt count is passed to the worker test.
     * Show why using old wall-clock differences can disagree in both ways. */
    last=10000000;now=last+1000000;oldWall=now+36000000000;
    CHECK(oldWall-last>2000000);CHECK(!UacFeedbackExpired(now,last));
    now=last+3000000;oldWall=now-10000000;
    CHECK(oldWall<last);CHECK(UacFeedbackExpired(now,last));
    /* A successful unchanged feedback packet refreshes this timestamp,
     * whereas no packet leaves it fixed. Predicate behavior is identical. */
    last=now;CHECK(!UacFeedbackExpired(now+2000000,last));
    CHECK(UacFeedbackExpired(now+2000001,last));
    /* Start of stream: 1 s before the first valid rate, then 200 ms. */
    base=5000000;
    CHECK(!UacFeedbackLate(base+2000001,base,0));
    CHECK(!UacFeedbackLate(base+10000000,base,0));
    CHECK(UacFeedbackLate(base+10000001,base,0));
    CHECK(UacFeedbackLate(base+2000001,base,1));
    CHECK(!UacFeedbackLate(base+2000000,base,1));
    CHECK(!UacFeedbackLate(base,base+1,0));
    CHECK(!UacFeedbackLate(base,base+1,1));
    /* Section 25: the deadlines in bus time. */
    CHECK(!UacFeedbackStale(0,1)); CHECK(!UacFeedbackStale(0,0));
    CHECK(!UacFeedbackStale(200000,1)); CHECK(UacFeedbackStale(200001,1));
    CHECK(!UacFeedbackStale(1000000,0)); CHECK(UacFeedbackStale(1000001,0));
    CHECK(UacFeedbackPacketUs(1,1)==125 && UacFeedbackPacketUs(1,2)==250 &&
          UacFeedbackPacketUs(1,3)==500 && UacFeedbackPacketUs(1,4)==1000);
    CHECK(UacFeedbackPacketUs(1,0)==125 && UacFeedbackPacketUs(1,9)==1000);
    CHECK(UacFeedbackPacketUs(0,1)==1000 && UacFeedbackPacketUs(0,4)==1000);
    /* 8-packet URBs a frame each: 25 back unusable are 200 ms (not yet
     * stale once active), the 26th is; at start 125 then 126. */
    {
        unsigned long us=0; int n;
        for(n=0;n<25;n++) us=UacFeedbackAdd(us,8,1000);
        CHECK(us==200000 && !UacFeedbackStale(us,1));
        us=UacFeedbackAdd(us,8,1000); CHECK(UacFeedbackStale(us,1));
        for(us=0,n=0;n<125;n++) us=UacFeedbackAdd(us,8,1000);
        CHECK(!UacFeedbackStale(us,0));
        us=UacFeedbackAdd(us,8,1000); CHECK(UacFeedbackStale(us,0));
    }
    CHECK(!UacFeedbackStaleBus(0,0,1) && !UacFeedbackStaleBus(200000,200000,1));
    CHECK(UacFeedbackStaleBus(200001,0,1) && UacFeedbackStaleBus(0,200001,1));
    CHECK(!UacFeedbackStaleBus(1000000,1000000,0) && UacFeedbackStaleBus(0,1000001,0));
    /* Feedback held while 4 ms data URBs come back: 50 are 200 ms, 51 stale. */
    {
        unsigned long q=0; int n;
        for(n=0;n<50;n++) q=UacFeedbackAdd(q,1,4000);
        CHECK(!UacFeedbackStaleBus(0,q,1));
        q=UacFeedbackAdd(q,1,4000); CHECK(UacFeedbackStaleBus(0,q,1));
    }
    /* 49 data URBs of 4 ms, then one unusable 8-packet feedback URB of
     * 125 us packets, repeated: the data count is not reset by it, so the
     * 51st data URB is stale. */
    {
        unsigned long u=0,q=0; int n;
        for(n=1;n<=51;n++) {
            q=UacFeedbackAdd(q,1,4000);
            if(n%49==0) u=UacFeedbackAdd(u,8,125);
            CHECK(UacFeedbackStaleBus(u,q,1)==(n>=51));
        }
    }
    /* A 128 ms backlog of data (32 URBs of 4 ms, stamps 1-32) came back
     * before a usable feedback URB (stamp 33), which the worker retires
     * first: none of the backlog counts, so 50 newer URBs are not stale
     * and the 51st is. Stamps wrap. */
    {
        unsigned long q=0,from=33,st; int n;
        for(st=1;st<=32;st++) if(UacFeedbackAfter(st,from)) q=UacFeedbackAdd(q,1,4000);
        CHECK(q==0);
        for(n=0,st=34;n<50;n++,st++) if(UacFeedbackAfter(st,from)) q=UacFeedbackAdd(q,1,4000);
        CHECK(!UacFeedbackStaleBus(0,q,1));
        if(UacFeedbackAfter(st,from)) q=UacFeedbackAdd(q,1,4000);
        CHECK(UacFeedbackStaleBus(0,q,1));
        CHECK(UacFeedbackAfter(2,0xfffffffful) && !UacFeedbackAfter(0xfffffffful,2) && !UacFeedbackAfter(5,5));
    }
    /* Both feedback slots usable, retired by index in reverse of their
     * stamps (slot 0 came back as 34, slot 1 as 1, data 2-33 between):
     * the older one is not after the newest retired (34) and moves
     * nothing, so the backlog still does not count. */
    {
        unsigned long newest=0,from=0,q=0,st;
        if(UacFeedbackAfter(34,newest)) { newest=34; from=34; q=0; }
        if(UacFeedbackAfter(1,newest)) { newest=1; from=1; q=0; }
        CHECK(newest==34 && from==34);
        for(st=2;st<=33;st++) if(UacFeedbackAfter(st,from)) q=UacFeedbackAdd(q,1,4000);
        CHECK(q==0);
    }
    /* Retired oldest first: slot 1 usable as 999, slot 0 unusable as 1000,
     * quiet at 196 ms. The worker's pick (the oldest stamp) takes slot 1:
     * it resets both counts; slot 0 then adds its 8 packets of 1 ms. */
    {
        unsigned long stamp[2]={1000,999},q=196000,u=0,newest=998; int pick,usable[2]={0,1},k;
        for(k=0;k<2;k++) {
            pick=(stamp[1]!=0&&(stamp[0]==0||UacFeedbackAfter(stamp[0],stamp[1])))?1:0;
            if(UacFeedbackAfter(stamp[pick],newest)) {
                newest=stamp[pick];
                if(usable[pick]) u=q=0; else u=UacFeedbackAdd(u,8,1000);
            }
            stamp[pick]=0;
        }
        CHECK(q==0 && u==8000 && newest==1000);
        q=UacFeedbackAdd(UacFeedbackAdd(q,1,4000),1,4000);
        CHECK(!UacFeedbackStaleBus(u,q,1));
    }
    /* Both clocks must be past the deadline (98 SE under NUSB): bus time
     * past it with 100 ms of interrupt time is not stale, nor 2 s of
     * interrupt time with no bus time; both past it is. */
    CHECK(!UacFeedbackStaleBoth(base+1000000,base,8000000ul,0,1));
    CHECK(!UacFeedbackStaleBoth(base+1000000,base,0,8000000ul,0));
    CHECK(!UacFeedbackStaleBoth(base+20000000,base,0,0,1));
    CHECK(UacFeedbackStaleBoth(base+2000001,base,200001,0,1));
    CHECK(!UacFeedbackStaleBoth(base+2000000,base,200001,0,1));
    CHECK(!UacFeedbackStaleBoth(base+2000001,base,200000,0,1));
    CHECK(UacFeedbackStaleBoth(base+10000001,base,0,1000001,0));
    CHECK(!UacFeedbackStaleBoth(base+10000000,base,0,1000001,0));
    CHECK(UacFeedbackAdd(0xfffffff0ul,8,1000)==0xfffffffful);
    CHECK(UacFeedbackAdd(0,0x80000000ul,4)==0xfffffffful);
    printf("PASS: %u monotonic feedback watchdog checks\n",checks);return 0;
}
