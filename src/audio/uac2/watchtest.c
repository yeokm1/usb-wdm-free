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
    printf("PASS: %u monotonic feedback watchdog checks\n",checks);return 0;
}
