/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of the failed state's decisions the driver takes from
 * uacgate.h (docs/design/uac2.md 29.5.7, 29.9). Decisions only: the kernel
 * waits, locks and threads are the driver's, read and run on the legs.
 */
#include <stdio.h>
#include "uacgate.h"
static unsigned int checks;
static int Check(int value, unsigned int line)
{
    checks++;
    if (!value) { printf("FAIL failtest line %u\n", line); return 0; }
    return 1;
}
#define CHECK(x) if (!Check((x), __LINE__)) return 1

/* A scripted mutex and gate: try n (1-based) acquires when n == acquireAt
 * (0: never, a holder blocked in a hung request); the failure is published
 * during try failAt (0: never) or is there from the start (failNow). */
typedef struct ACQ {
    int tries, acquireAt, failAt, failNow, held, releases, reads;
} ACQ;
static int AcqFailed(void *c) { ACQ *a = (ACQ *)c; a->reads++; return a->failNow; }
static int AcqTry(void *c)
{
    ACQ *a = (ACQ *)c;
    a->tries++;
    if (a->failAt && a->tries == a->failAt) a->failNow = 1;
    if (a->acquireAt && a->tries == a->acquireAt) { a->held = 1; return 1; }
    return 0;
}
static void AcqRelease(void *c) { ACQ *a = (ACQ *)c; a->held = 0; a->releases++; }
static int Acquire(ACQ *a) { return UacGateAcquire(a, AcqFailed, AcqTry, AcqRelease); }

static int TestAcquire(void)
{
    ACQ a;
    /* A healthy device: the mutex comes free at the third try. */
    a.tries = 0; a.acquireAt = 3; a.failAt = 0; a.failNow = 0; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 1 && a.held && a.tries == 3 && !a.releases && a.reads == 4);
    /* Already failed: refused before any wait. */
    a.tries = 0; a.acquireAt = 1; a.failAt = 0; a.failNow = 1; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 0 && !a.held && !a.tries && !a.releases);
    /* The holder is blocked in a hung request and never releases (a level
     * SET behind it, 29.9): the failure published during the fourth try
     * ends the wait at the next read, holding nothing. */
    a.tries = 0; a.acquireAt = 0; a.failAt = 4; a.failNow = 0; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 0 && !a.held && a.tries == 4 && !a.releases);
    /* The same after a long wait: bounded by the failure, not by the holder. */
    a.tries = 0; a.acquireAt = 0; a.failAt = 1000; a.failNow = 0; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 0 && !a.held && a.tries == 1000);
    /* The failure published while the try that acquires waited: the read
     * after the acquisition refuses and releases (the recheck). */
    a.tries = 0; a.acquireAt = 2; a.failAt = 2; a.failNow = 0; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 0 && !a.held && a.tries == 2 && a.releases == 1);
    /* Published at the first try, acquired later: refused at the next read. */
    a.tries = 0; a.acquireAt = 3; a.failAt = 1; a.failNow = 0; a.held = 0; a.releases = 0; a.reads = 0;
    CHECK(Acquire(&a) == 0 && !a.held && a.tries == 1 && !a.releases);
    return 0;
}

/* The notifier (29.9): the wakes of a failure's life, and its join. */
static int TestNotify(void)
{
    UAC_NOTIFY n;
    unsigned int a;
    /* No failure: a wake does nothing; an exit leaves. */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    CHECK(UacNotifyStep(&n, 0, 0, 0) == 0);
    CHECK(UacNotifyStep(&n, 1, 0, 0) == UAC_NOTIFY_EXIT);
    /* A running device: the expiry's wake writes the first record and asks
     * for the call; started, it is admitted once. */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    a = UacNotifyStep(&n, 0, 1, 0);
    CHECK(a == (UAC_NOTIFY_WRITE | UAC_NOTIFY_FIRST | UAC_NOTIFY_ADMIT));
    CHECK(UacNotifyLife(&n, UAC_LIFE_STARTED) == 1 && n.Invalidated);
    /* The completion's wake writes the final record, once; no second call. */
    a = UacNotifyStep(&n, 0, 1, 1);
    CHECK(a == UAC_NOTIFY_WRITE);
    CHECK(UacNotifyStep(&n, 0, 1, 1) == 0);
    CHECK(UacNotifyLife(&n, UAC_LIFE_STARTED) == 0);
    /* The teardown's join after the drain: the exit with the final record
     * pending writes it before leaving (a join never discards it). */
    n.Recorded = 1; n.FinalRecorded = 0; n.Invalidated = 1;
    a = UacNotifyStep(&n, 1, 1, 1);
    CHECK(a == (UAC_NOTIFY_WRITE | UAC_NOTIFY_EXIT) && n.FinalRecorded);
    /* A failure the thread had not yet seen when the exit came (the join
     * right after the expiry's drain): first and final record, no call. */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    a = UacNotifyStep(&n, 1, 1, 1);
    CHECK(a == (UAC_NOTIFY_WRITE | UAC_NOTIFY_FIRST | UAC_NOTIFY_EXIT));
    CHECK(n.Recorded && n.FinalRecorded && !n.Invalidated);
    /* A startup expiry: Life starting admits nothing and asks again; a start
     * that then succeeds wakes it, and the call is admitted. */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    a = UacNotifyStep(&n, 0, 1, 1);
    CHECK(a == (UAC_NOTIFY_WRITE | UAC_NOTIFY_FIRST | UAC_NOTIFY_ADMIT));
    CHECK(UacNotifyLife(&n, UAC_LIFE_STARTING) == 0 && !n.Invalidated);
    a = UacNotifyStep(&n, 0, 1, 1);
    CHECK(a == UAC_NOTIFY_ADMIT);
    CHECK(UacNotifyLife(&n, UAC_LIFE_STARTED) == 1);
    /* A startup expiry and a failed start: the join writes nothing twice and
     * makes no call (Life never started). */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    a = UacNotifyStep(&n, 0, 1, 1);
    CHECK(UacNotifyLife(&n, UAC_LIFE_STARTING) == 0);
    CHECK(UacNotifyStep(&n, 1, 1, 1) == UAC_NOTIFY_EXIT && !n.Invalidated);
    /* A teardown before the call: never made, not asked again. */
    n.Recorded = n.FinalRecorded = n.Invalidated = 0;
    (void)UacNotifyStep(&n, 0, 1, 0);
    CHECK(UacNotifyLife(&n, UAC_LIFE_SURPRISE) == 0 && n.Invalidated);
    CHECK(UacNotifyStep(&n, 0, 1, 0) == 0);
    n.Invalidated = 0;
    CHECK(UacNotifyLife(&n, UAC_LIFE_TEARDOWN) == 0 && n.Invalidated);
    return 0;
}

/* The record's final status and lateness (29.9). */
static int TestFinal(void)
{
    UAC_GATE_TIME at = 123456789u, ms = 10000u, wrap = (UAC_GATE_TIME)1 << 32;
    /* The caller's mapping; the record keeps the raw status. */
    CHECK(UacGateCallerStatus(UAC_GATE_STATUS_CANCELLED) == UAC_GATE_STATUS_IO_TIMEOUT);
    CHECK(UacGateCallerStatus(0) == 0);
    CHECK(UacGateCallerStatus((long)0xC0000001L) == (long)0xC0000001L);
    CHECK(UacGateCallerStatus(UAC_GATE_STATUS_IO_TIMEOUT) == UAC_GATE_STATUS_IO_TIMEOUT);
    /* Whole milliseconds, rounded down; none before the expiry. */
    CHECK(UacGateLateMs(at, at) == 0 && UacGateLateMs(at - 1, at) == 0);
    CHECK(UacGateLateMs(at + 9999u, at) == 0 && UacGateLateMs(at + ms, at) == 1);
    CHECK(UacGateLateMs(at + 2 * ms + 5000u, at) == 2);
    /* Past 2^32 units of 100 ns (about 7.2 minutes) and past 2^32 * 16
     * (the old conversion narrowed (now - at) >> 4 to 32 bits first, so it
     * wrapped after about 114.5 minutes of drain). */
    CHECK(UacGateLateMs(at + wrap, at) == (unsigned long)(wrap / ms));
    CHECK(UacGateLateMs(at + wrap * 16u, at) == (unsigned long)(wrap * 16u / ms));
    CHECK(UacGateLateMs(at + wrap * 16u + 160000u, at) == (unsigned long)((wrap * 16u + 160000u) / ms));
    CHECK(UacGateLateMs(at + 120u * 60u * 1000u * ms, at) == 120ul * 60ul * 1000ul);
    /* Saturated below "not completed": about 49.7 days and beyond. */
    CHECK(UacGateLateMs(at + (UAC_GATE_TIME)0xFFFFFFFEul * ms, at) == 0xFFFFFFFEul);
    CHECK(UacGateLateMs(at + (UAC_GATE_TIME)0xFFFFFFFFul * ms, at) == 0xFFFFFFFEul);
    CHECK(UacGateLateMs(~(UAC_GATE_TIME)0, 0) == 0xFFFFFFFEul);
    CHECK(UacGateLateMs(at + (UAC_GATE_TIME)0xFFFFFFFDul * ms, at) == 0xFFFFFFFDul);
    return 0;
}

/* The startup latch (29.9, 29.14): the outcomes a start's step can have. */
static int TestLatch(void)
{
    static const long outcomes[] = {
        0,                              /* a success: a late successful completion, ReadName's fallback */
        UAC_GATE_STATUS_NOT_SUPPORTED,  /* no format left (Learn skipped them): uac1's null function */
        (long)0xC0000185L,              /* STATUS_IO_DEVICE_ERROR */
        (long)0xC000009CL,              /* STATUS_DEVICE_DATA_ERROR: a short descriptor */
        (long)0xC00000A3L,              /* STATUS_DEVICE_NOT_READY: refused by the failed gate */
        (long)0xC000009AL,              /* STATUS_INSUFFICIENT_RESOURCES */
        UAC_GATE_STATUS_IO_TIMEOUT,
        UAC_GATE_STATUS_CANCELLED
    };
    unsigned int i;
    for (i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); i++) {
        /* Latched: every outcome ends the start with STATUS_IO_TIMEOUT. */
        CHECK(UacStartVerdict(outcomes[i], 1) == UAC_GATE_STATUS_IO_TIMEOUT);
        /* Not latched: the step's own outcome, as before section 29. */
        CHECK(UacStartVerdict(outcomes[i], 0) == outcomes[i]);
    }
    /* uac1's null function: taken for an unlatched STATUS_NOT_SUPPORTED
     * only, never for a latched one or another outcome. */
    CHECK(UacStartNullFunction(UacStartVerdict(UAC_GATE_STATUS_NOT_SUPPORTED, 0)));
    CHECK(!UacStartNullFunction(UacStartVerdict(UAC_GATE_STATUS_NOT_SUPPORTED, 1)));
    CHECK(!UacStartNullFunction(UacStartVerdict(0, 0)));
    CHECK(!UacStartNullFunction(UacStartVerdict(UAC_GATE_STATUS_IO_TIMEOUT, 0)));
    return 0;
}

int main(void)
{
    if (TestAcquire()) return 1;
    if (TestNotify()) return 1;
    if (TestFinal()) return 1;
    if (TestLatch()) return 1;
    printf("PASS: %u failed-state decision checks (uacgate.h)\n", checks);
    return 0;
}
