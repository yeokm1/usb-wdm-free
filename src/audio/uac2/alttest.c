/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test: the kept alternate's state machine (uacalt.h,
 * docs/design/uac2.md 29.4, 29.5.4, 29.8) with an injected clock: the
 * settings, the deadline at its boundaries, adoption and its refusals,
 * the hold in either direction, idempotent stops, every failed step, the
 * gate, the alt thread's scan and claim, the start's commit, the
 * conflict classes of the clock walk (to the mask's 32 records), the retry
 * after an alternate 0 that could not be submitted, full-width times and
 * the saturation limit, the gate's authority over a late success, the
 * outcome matrix from Idle and Unknown with the failure setting on and
 * off, settings across RUN, adoption and hand-back, the generation count,
 * and the retained geometry. Decisions only: kernel lifetime, locking and
 * the transport's requests are the adapter's and its harness's.
 */
#include <stdio.h>
#include "uacalt.h"

static int failures, checks;

static void Expect(const char *name, unsigned long got, unsigned long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s (got %lu, want %lu)\n", name, got, want);
    }
}

/* Full-width time comparison, printed as two 32-bit halves. */
static void ExpectT(const char *name, UAC_ALT_TIME got, UAC_ALT_TIME want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s (got %08lX%08lX, want %08lX%08lX)\n", name,
               (unsigned long)(got >> 32), (unsigned long)got,
               (unsigned long)(want >> 32), (unsigned long)want);
    }
}

/* Short form for the small-time tests: never truncates silently. A time
 * that does not fit 32 bits (other than "never") fails the check here;
 * the full-width tests use ExpectT. */
static unsigned long T(UAC_ALT_TIME t)
{
    if (t == UAC_ALT_NEVER) return 0xFFFFFFFFUL;
    if (t >= 0xFFFFFFFFUL) {
        failures++;
        printf("FAIL a time did not fit 32 bits in a short-form check\n");
        return 0xFFFFFFFEUL;
    }
    return (unsigned long)t;
}

static UAC_ALT_GATE gate;

static void Want(UAC_ALT_WANT *w, unsigned long linger, unsigned long hold, unsigned int dir)
{
    w->Alt = 1; w->Terminal = 2; w->Rate = 48000;
    w->Need.HasFeedback = 0;
    w->Need.Data.Transfer = 1536; w->Need.Data.Packet = 192;
    w->Need.Feedback.Transfer = 0; w->Need.Feedback.Packet = 0;
    UacAltSnap(&w->Snap, linger, hold, dir);
}

/* A record selected at `at` and running. */
static void Running(UAC_ALT_REC *r, const UAC_ALT_WANT *w, UAC_ALT_TIME at)
{
    UAC_ALT_TIME when;
    UacAltInit(r, 0);
    UacAltClaim(r, &gate, UAC_OWN_START, at);
    UacAltStart(r, &gate, w, at, &when);
    UacAltSelectDone(r, w, 0, UAC_SEL_OK, at);
}

/* Selected at 1000 with w, stopped clean at 1300, started at 1310 with x. */
static int Restart(UAC_ALT_REC *r, const UAC_ALT_WANT *w, const UAC_ALT_WANT *x, UAC_ALT_TIME *at)
{
    Running(r, w, 1000);
    UacAltHandBack(r, &gate, 1, 1300, at);
    UacAltClaim(r, &gate, UAC_OWN_START, 1310);
    return UacAltStart(r, &gate, x, 1310, at);
}

static void Settings(void)
{
    UAC_ALT_SNAP s;
    Expect("linger 0", UacAltLingerMs(0), 0);
    Expect("linger 1", UacAltLingerMs(1), 1);
    Expect("linger top", UacAltLingerMs(10000), 10000);
    Expect("linger top+1 clamped", UacAltLingerMs(10001), 10000);
    Expect("linger 0xFFFFFFFE clamped", UacAltLingerMs(0xFFFFFFFEUL), 10000);
    Expect("linger none: default", UacAltLingerMs(UAC_ALT_RAW_NONE), 2000);
    Expect("hold 0", UacAltHoldMs(0), 0);
    Expect("hold top", UacAltHoldMs(1000), 1000);
    Expect("hold top+1 clamped", UacAltHoldMs(1001), 1000);
    Expect("hold none: default", UacAltHoldMs(UAC_ALT_RAW_NONE), 200);
    Expect("probe 0 off", UacAltProbeMs(0), 0);
    Expect("probe 1 -> 2000", UacAltProbeMs(1), 2000);
    Expect("probe 1999 -> 2000", UacAltProbeMs(1999), 2000);
    Expect("probe 2000", UacAltProbeMs(2000), 2000);
    Expect("probe top", UacAltProbeMs(60000), 60000);
    Expect("probe top+1 clamped", UacAltProbeMs(60001), 60000);
    Expect("probe none: default off", UacAltProbeMs(UAC_ALT_RAW_NONE), 0);
    Expect("fail 0 off", UacAltFailOn(0), 0);
    Expect("fail 1 on", UacAltFailOn(1), 1);
    Expect("fail 2 default", UacAltFailOn(2), 1);
    Expect("fail none default", UacAltFailOn(UAC_ALT_RAW_NONE), 1);
    UacAltSnap(&s, UAC_ALT_RAW_NONE, UAC_ALT_RAW_NONE, 7);
    Expect("snap defaults linger", s.LingerMs, 2000);
    Expect("snap defaults hold", s.HoldMs, 200);
    Expect("snap unknown direction -> to zero", s.HoldDir, UAC_HOLD_TO_ZERO);
    UacAltSnap(&s, 0, 0, UAC_HOLD_BOTH);
    Expect("snap both", s.HoldDir, UAC_HOLD_BOTH);
}

static void Baseline(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at, t;
    int i, zeros = 0, selects = 0, other = 0;
    Want(&w, 0, 0, UAC_HOLD_TO_ZERO);
    UacAltInit(&r, 0);
    /* The rapid open and close pattern: today's sequence, request for
     * request: SET_INTERFACE(N) at each start, (0) at each stop, at once. */
    for (i = 0, t = 10; i < 20; i++, t += 36) {
        UacAltClaim(&r, &gate, UAC_OWN_START, t);
        if (UacAltStart(&r, &gate, &w, t, &at) == UAC_ACT_SELECT && at == t) selects++; else other++;
        UacAltSelectDone(&r, &w, 0,UAC_SEL_OK, t + 1);
        if (UacAltHandBack(&r, &gate, 1, t + 6, &at) == UAC_ACT_ZERO && at == t + 6) zeros++; else other++;
        UacAltZeroDone(&r, UAC_SEL_OK, t + 7);
        UacAltRelease(&r);
    }
    Expect("baseline selects at once", (unsigned long)selects, 20);
    Expect("baseline zeros at once", (unsigned long)zeros, 20);
    Expect("baseline nothing else", (unsigned long)other, 0);
    Expect("baseline ends idle", r.State, UAC_ALT_IDLE);
    Expect("baseline owner none", r.Owner, UAC_OWN_NONE);
}

static void Deadlines(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    /* L 0, H 200, selected at 1000. */
    Want(&w, 0, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    Expect("H-1: linger", (unsigned long)UacAltHandBack(&r, &gate, 1, 1199, &at), UAC_ACT_LINGER);
    Expect("H-1: deadline", T(at), 1200);
    Running(&r, &w, 1000);
    Expect("H: zero now", (unsigned long)UacAltHandBack(&r, &gate, 1, 1200, &at), UAC_ACT_ZERO);
    Expect("H: at now", T(at), 1200);
    Running(&r, &w, 1000);
    Expect("H+1: zero now", (unsigned long)UacAltHandBack(&r, &gate, 1, 1201, &at), UAC_ACT_ZERO);
    /* L 2000, H 200. */
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    Expect("L and H: linger", (unsigned long)UacAltHandBack(&r, &gate, 1, 1100, &at), UAC_ACT_LINGER);
    Expect("L and H: deadline stop+L", T(r.Deadline), 3100);
    Expect("L and H: stop time", T(r.StopAt), 1100);
    /* H 0, L 2000. */
    Want(&w, 2000, 0, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1050, &at);
    Expect("H 0: deadline stop+L", T(at), 3050);
    /* L 0 with the hold running, the record unclean: AttemptAt + H. */
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 0, 1050, &at);
    Expect("unclean: deadline AttemptAt+H", T(at), 1200);
    Expect("unclean: not adoptable", r.Adoptable, 0);
}

static void Idempotent(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    unsigned long gen;
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    Expect("pause: linger", (unsigned long)UacAltHandBack(&r, &gate, 1, 1500, &at), UAC_ACT_LINGER);
    gen = r.Gen;
    /* Close of the paused pin, UacUsbDestroy's stop, the next start's
     * leading stop: nothing, the deadline unchanged. */
    Expect("close after pause: none", (unsigned long)UacAltHandBack(&r, &gate, 1, 1900, &at), UAC_ACT_NONE);
    Expect("destroy's stop: none", (unsigned long)UacAltHandBack(&r, &gate, 1, 1950, &at), UAC_ACT_NONE);
    Expect("deadline unchanged", T(r.Deadline), 3500);
    Expect("no change counted", r.Gen, gen);
    /* A settings edit after the hand-back does not move the deadline: the
     * record keeps its own snapshot. */
    Want(&w, 10000, 1000, UAC_HOLD_BOTH);
    Expect("edit: deadline unchanged", T(r.Deadline), 3500);
    Expect("edit: record snapshot kept", r.Snap.LingerMs, 2000);
}

static void Adoption(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w, x;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1300, &at);
    Expect("linger claim by start", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_START, 1310), 1);
    Expect("same format adopts", (unsigned long)UacAltStart(&r, &gate, &w, 1310, &at), UAC_ACT_ADOPT);
    Expect("adopted active", r.State, UAC_ALT_ACTIVE);
    Expect("adopted owner stream", r.Owner, UAC_OWN_STREAM);
    Expect("adopted commit", (unsigned long)UacAltCommit(&r, &gate), 1);
    /* A start failing after the adoption: lingers again, same deadline. */
    UacAltStartFailed(&r, 1320);
    Expect("adopt failed: lingering", r.State, UAC_ALT_LINGERING);
    Expect("adopt failed: earlier deadline", T(r.Deadline), 3300);
    Expect("adopt failed: still adoptable", r.Adoptable, 1);
    Expect("adopt failed: owner none", r.Owner, UAC_OWN_NONE);

    x = w; x.Rate = 44100;
    Expect("other rate: ended first", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    Expect("other rate: zero after the hold", T(at), 1310);
    x = w; x.Alt = 2;
    Expect("other alternate", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    x = w; x.Terminal = 3;
    Expect("other terminal", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    x = w; x.Need.Data.Transfer = 1537;
    Expect("data transfer larger", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    x = w; x.Need.Data.Packet = 193;
    Expect("data packet larger", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    x = w; x.Need.HasFeedback = 1;
    Expect("feedback where none was selected", (unsigned long)Restart(&r, &w, &x, &at), UAC_ACT_ZERO);
    /* Feedback geometry, per pipe. */
    w.Need.HasFeedback = 1; w.Need.Feedback.Transfer = 32; w.Need.Feedback.Packet = 4;
    Running(&r, &w, 1000); UacAltHandBack(&r, &gate, 1, 1300, &at);
    x = w; x.Need.Feedback.Packet = 8; UacAltClaim(&r, &gate, UAC_OWN_START, 1310);
    Expect("feedback packet larger", (unsigned long)UacAltStart(&r, &gate, &x, 1310, &at), UAC_ACT_ZERO);
    UacAltRelease(&r);
    x = w; x.Need.Feedback.Transfer = 64; UacAltClaim(&r, &gate, UAC_OWN_START, 1310);
    Expect("feedback transfer larger", (unsigned long)UacAltStart(&r, &gate, &x, 1310, &at), UAC_ACT_ZERO);
    UacAltRelease(&r);
    x = w; x.Need.Feedback.Transfer = 16; UacAltClaim(&r, &gate, UAC_OWN_START, 1310);
    Expect("feedback smaller adopts", (unsigned long)UacAltStart(&r, &gate, &x, 1310, &at), UAC_ACT_ADOPT);
    w.Need.HasFeedback = 0;
    /* Unclean end. */
    Running(&r, &w, 1000);
    Expect("unclean, hold over: zero at once", (unsigned long)UacAltHandBack(&r, &gate, 0, 1300, &at), UAC_ACT_ZERO);
    Running(&r, &w, 1000);
    Expect("unclean, hold running: lingers", (unsigned long)UacAltHandBack(&r, &gate, 0, 1100, &at), UAC_ACT_LINGER);
    UacAltClaim(&r, &gate, UAC_OWN_START, 1110);
    Expect("unclean: not adopted", (unsigned long)UacAltStart(&r, &gate, &w, 1110, &at), UAC_ACT_ZERO);
    Expect("unclean: zero after the hold", T(at), 1200);
    /* Another power epoch. */
    Running(&r, &w, 1000); UacAltHandBack(&r, &gate, 1, 1300, &at);
    gate.Epoch++;
    UacAltClaim(&r, &gate, UAC_OWN_START, 1310);
    Expect("other epoch: not adopted", (unsigned long)UacAltStart(&r, &gate, &w, 1310, &at), UAC_ACT_ZERO);
    gate.Epoch = 0;
    /* Without the claim: busy. */
    Running(&r, &w, 1000); UacAltHandBack(&r, &gate, 1, 1300, &at);
    Expect("unclaimed start: busy", (unsigned long)UacAltStart(&r, &gate, &w, 1310, &at), UAC_ACT_BUSY);
    Expect("start while running", (unsigned long)(Running(&r, &w, 1000), UacAltStart(&r, &gate, &w, 1010, &at)),
           UAC_ACT_RUNNING);
}

static void Holds(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w, x;
    UAC_ALT_TIME at;
    /* Symmetric: selected at 0, format change at 50: alternate 0 at 200,
     * done at 210, the new alternate not before 410 (2H). */
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    Running(&r, &w, 0);
    UacAltHandBack(&r, &gate, 1, 40, &at);
    x = w; x.Rate = 44100;
    UacAltClaim(&r, &gate, UAC_OWN_START, 50);
    Expect("both: zero first", (unsigned long)UacAltStart(&r, &gate, &x, 50, &at), UAC_ACT_ZERO);
    Expect("both: zero after the hold", T(at), 200);
    Expect("both: zero done idle", UacAltZeroDone(&r, UAC_SEL_OK, 210), UAC_ALT_IDLE);
    Expect("both: select", (unsigned long)UacAltStart(&r, &gate, &x, 210, &at), UAC_ACT_SELECT);
    Expect("both: select after a second hold", T(at), 410);
    /* One direction: the select at once. */
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 0);
    UacAltHandBack(&r, &gate, 1, 40, &at);
    x = w; x.Rate = 44100;
    UacAltClaim(&r, &gate, UAC_OWN_START, 50);
    UacAltStart(&r, &gate, &x, 50, &at);
    UacAltZeroDone(&r, UAC_SEL_OK, 210);
    UacAltStart(&r, &gate, &x, 210, &at);
    Expect("to zero: select at once", T(at), 210);
    /* Symmetric from idle long after: no wait. */
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    UacAltInit(&r, 0);
    UacAltClaim(&r, &gate, UAC_OWN_START, 5000);
    UacAltStart(&r, &gate, &w, 5000, &at);
    Expect("both: idle long: at once", T(at), 5000);
}

static void FailedSteps(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    UacAltInit(&r, 100);
    UacAltClaim(&r, &gate, UAC_OWN_START, 500);
    UacAltStart(&r, &gate, &w, 500, &at);
    Expect("unsubmitted: state unchanged", UacAltSelectDone(&r, &w, 0,UAC_SEL_UNSUBMITTED, 510), UAC_ALT_IDLE);
    Expect("unsubmitted: AttemptAt unchanged", T(r.AttemptAt), 100);
    Expect("refused: unknown", UacAltSelectDone(&r, &w, 0,UAC_SEL_REFUSED, 520), UAC_ALT_UNKNOWN);
    Expect("refused: AttemptAt moved", T(r.AttemptAt), 520);
    UacAltStart(&r, &gate, &w, 530, &at);
    Expect("from unknown: symmetric hold from the attempt", T(at), 720);
    Expect("bad pipes: lingering", UacAltSelectDone(&r, &w, 0,UAC_SEL_BAD_PIPES, 730), UAC_ALT_LINGERING);
    Expect("bad pipes: not adoptable", r.Adoptable, 0);
    Expect("bad pipes: deadline AttemptAt+H", T(r.Deadline), 930);
    UacAltRelease(&r);
    Expect("bad pipes: owner none", r.Owner, UAC_OWN_NONE);
    /* After a fresh selection, an allocation failure. */
    Running(&r, &w, 1000);
    UacAltStartFailed(&r, 1005);
    Expect("start failed: lingering", r.State, UAC_ALT_LINGERING);
    Expect("start failed: not adoptable", r.Adoptable, 0);
    Expect("start failed: deadline", T(r.Deadline), 1200);
    /* Alternate 0 outcomes, by the thread at the deadline. */
    Expect("thread claim at deadline", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, 1200), 1);
    Expect("thread end: zero", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_THREAD, 1200, &at), UAC_ACT_ZERO);
    Expect("zero unsubmitted: still lingering", UacAltZeroDone(&r, UAC_SEL_UNSUBMITTED, 1201), UAC_ALT_LINGERING);
    Expect("zero unsubmitted: AttemptAt kept", T(r.AttemptAt), 1000);
    Expect("zero refused: unknown", UacAltZeroDone(&r, UAC_SEL_REFUSED, 1202), UAC_ALT_UNKNOWN);
    Expect("zero refused: AttemptAt moved", T(r.AttemptAt), 1202);
    UacAltRelease(&r);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1000, &at);
    UacAltClaim(&r, &gate, UAC_OWN_START, 1300);
    UacAltStart(&r, &gate, &w, 1300, &at);   /* adopt */
    UacAltHandBack(&r, &gate, 1, 1400, &at);
    Expect("adopted then stopped: deadline from the new stop", T(r.Deadline), 3400);
    /* Expiries. */
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1000, &at);
    UacAltClaim(&r, &gate, UAC_OWN_THREAD, 3000);
    UacAltEnd(&r, &gate, UAC_OWN_THREAD, 3000, &at);
    Expect("zero expired: failed", UacAltZeroDone(&r, UAC_SEL_EXPIRED, 8000), UAC_ALT_FAILED);
    UacAltInit(&r, 0);
    UacAltClaim(&r, &gate, UAC_OWN_START, 10);
    UacAltStart(&r, &gate, &w, 300, &at);
    Expect("select expired: failed", UacAltSelectDone(&r, &w, 0,UAC_SEL_EXPIRED, 5300), UAC_ALT_FAILED);
    Expect("failed: start refused", (unsigned long)UacAltStart(&r, &gate, &w, 5400, &at), UAC_ACT_REFUSE);
}

static void Gate(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    /* Removal: nothing sent. */
    Running(&r, &w, 1000);
    gate.Admit = UAC_ADMIT_CLOSED;
    Expect("closed: hand-back discards", (unsigned long)UacAltHandBack(&r, &gate, 1, 1500, &at), UAC_ACT_DISCARD);
    Expect("closed: start refused", (unsigned long)UacAltStart(&r, &gate, &w, 1500, &at), UAC_ACT_REFUSE);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1500, &at);
    Expect("closed: pnp end refused", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_PNP, 1500, &at), UAC_ACT_REFUSE);
    gate.Admit = UAC_ADMIT_OPEN;
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1500, &at);
    Expect("remove: discard", (unsigned long)UacAltRemove(&r), UAC_ACT_DISCARD);
    /* Power: the hand-back lingers for the power caller, who ends it after
     * the hold; the thread may not. */
    Running(&r, &w, 1000);
    gate.Admit = UAC_ADMIT_BELOW_D0;
    gate.Epoch++;
    Expect("below D0: hand-back lingers", (unsigned long)UacAltHandBack(&r, &gate, 1, 1050, &at), UAC_ACT_LINGER);
    Expect("below D0: thread may not claim", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, 9999), 0);
    Expect("below D0: thread end refused", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_THREAD, 9999, &at), UAC_ACT_REFUSE);
    Expect("below D0: start refused", (unsigned long)(UacAltClaim(&r, &gate, UAC_OWN_POWER, 1060),
           UacAltStart(&r, &gate, &w, 1060, &at)), UAC_ACT_REFUSE);
    Expect("below D0: power end", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_POWER, 1060, &at), UAC_ACT_ZERO);
    Expect("below D0: power waits the hold", T(at), 1200);
    Expect("below D0: idle", UacAltZeroDone(&r, UAC_SEL_OK, 1210), UAC_ALT_IDLE);
    UacAltRelease(&r);
    /* PnP stop. */
    gate.Admit = UAC_ADMIT_OPEN;
    gate.Epoch = 0;
    Running(&r, &w, 1000);
    gate.Admit = UAC_ADMIT_QUIESCING;
    UacAltHandBack(&r, &gate, 1, 1500, &at);
    Expect("quiescing: power end refused", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_POWER, 1500, &at), UAC_ACT_REFUSE);
    UacAltClaim(&r, &gate, UAC_OWN_PNP, 1500);
    Expect("quiescing: pnp end", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_PNP, 1500, &at), UAC_ACT_ZERO);
    gate.Admit = UAC_ADMIT_OPEN;
    /* Failure: nothing sends from any path. */
    Running(&r, &w, 1000);
    gate.Failed = 1;
    Expect("failed: commit refused", (unsigned long)UacAltCommit(&r, &gate), 0);
    Expect("failed: hand-back refused", (unsigned long)UacAltHandBack(&r, &gate, 1, 1500, &at), UAC_ACT_REFUSE);
    Expect("failed: record failed", r.State, UAC_ALT_FAILED);
    Expect("failed: end refused", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_PNP, 1500, &at), UAC_ACT_REFUSE);
    gate.Failed = 0;
    Running(&r, &w, 1000);
    UacAltFail(&r);
    Expect("record fail: commit refused", (unsigned long)UacAltCommit(&r, &gate), 0);
    Expect("record fail: start refused", (unsigned long)UacAltStart(&r, &gate, &w, 1500, &at), UAC_ACT_REFUSE);
    /* The commit sees an epoch change too. */
    Running(&r, &w, 1000);
    gate.Epoch++;
    Expect("epoch: commit refused", (unsigned long)UacAltCommit(&r, &gate), 0);
    gate.Epoch = 0;
    Expect("commit", (unsigned long)UacAltCommit(&r, &gate), 1);
}

static void Thread(void)
{
    UAC_ALT_REC r[3];
    UAC_ALT_WANT w;
    UAC_ALT_TIME at, next;
    unsigned int i;
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    for (i = 0; i < 3; i++) {
        Running(&r[i], &w, 1000);
        UacAltHandBack(&r[i], &gate, 1, 1000 + 100 * i, &at);   /* deadlines 3000, 3100, 3200 */
    }
    Expect("scan early: none", (unsigned long)(UacAltScan(r, 3, &gate, 2000, &next) + 1), 0);
    Expect("scan early: next", T(next), 3000);
    Expect("thread claim early refused", (unsigned long)UacAltClaim(&r[0], &gate, UAC_OWN_THREAD, 2999), 0);
    Expect("scan at 3100: first", (unsigned long)UacAltScan(r, 3, &gate, 3100, &next), 0);
    Expect("scan at 3100: next pending", T(next), 3200);
    /* An owned record is not runnable and adds no time: no zero-length
     * wait loop while its owner works. */
    UacAltClaim(&r[0], &gate, UAC_OWN_START, 3100);
    UacAltClaim(&r[1], &gate, UAC_OWN_POWER, 3100);
    Expect("owned: scan finds r2 only later", (unsigned long)(UacAltScan(r, 3, &gate, 3150, &next) + 1), 0);
    Expect("owned: next is r2's", T(next), 3200);
    UacAltClaim(&r[2], &gate, UAC_OWN_PNP, 3150);
    Expect("all owned: nothing", (unsigned long)(UacAltScan(r, 3, &gate, 9000, &next) + 1), 0);
    Expect("all owned: untimed wait", T(next), 0xFFFFFFFFUL);
    for (i = 0; i < 3; i++) UacAltRelease(&r[i]);
    /* Held back by the gate: no time either. */
    gate.Admit = UAC_ADMIT_BELOW_D0;
    Expect("below D0: nothing", (unsigned long)(UacAltScan(r, 3, &gate, 9000, &next) + 1), 0);
    Expect("below D0: untimed wait", T(next), 0xFFFFFFFFUL);
    gate.Admit = UAC_ADMIT_QUIESCING;
    next = 1;   /* not "never", so the scan must set it */
    Expect("quiescing: nothing", (unsigned long)(UacAltScan(r, 3, &gate, 9000, &next) + 1), 0);
    ExpectT("quiescing: untimed wait", next, UAC_ALT_NEVER);
    gate.Admit = UAC_ADMIT_OPEN;
    gate.Failed = 1;
    Expect("failed: nothing", (unsigned long)(UacAltScan(r, 3, &gate, 9000, &next) + 1), 0);
    gate.Failed = 0;
    /* Claim and revalidate in one step: a record adopted after the scan is
     * not taken by the thread. */
    Expect("scan sees r0", (unsigned long)UacAltScan(r, 3, &gate, 3300, &next), 0);
    UacAltClaim(&r[0], &gate, UAC_OWN_START, 3300);
    UacAltStart(&r[0], &gate, &w, 3300, &at);   /* adopted */
    Expect("adopted after scan: thread claim refused", (unsigned long)UacAltClaim(&r[0], &gate, UAC_OWN_THREAD, 3300), 0);
    Expect("thread takes r1", (unsigned long)UacAltClaim(&r[1], &gate, UAC_OWN_THREAD, 3300), 1);
    Expect("thread end r1", (unsigned long)UacAltEnd(&r[1], &gate, UAC_OWN_THREAD, 3300, &at), UAC_ACT_ZERO);
    Expect("thread end r1 at once (hold long over)", T(at), 3300);
    Expect("end by non-owner: busy", (unsigned long)UacAltEnd(&r[2], &gate, UAC_OWN_THREAD, 3300, &at), UAC_ACT_BUSY);
}

static void Conflict(void)
{
    UAC_ALT_REC r[3];
    UAC_ALT_WANT w, x;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_TO_ZERO);
    /* r0 playback lingering at 48 kHz; r1 capture wants 44.1 kHz on the
     * same source; r2 active. */
    Running(&r[0], &w, 1000);
    UacAltHandBack(&r[0], &gate, 1, 1500, &at);
    UacAltInit(&r[1], 0);
    Running(&r[2], &w, 1000);
    Expect("no conflict", (unsigned long)UacAltConflict(r, 3, 0), UAC_CLAIM_FREE);
    Expect("lingering only", (unsigned long)UacAltConflict(r, 3, 1UL), UAC_CLAIM_LINGERING);
    Expect("with an active one", (unsigned long)UacAltConflict(r, 3, 5UL), UAC_CLAIM_ACTIVE);
    Expect("beyond the records", (unsigned long)UacAltConflict(r, 3, 8UL), UAC_CLAIM_ACTIVE);
    /* The eviction: the starting caller takes r0, ends it after its hold,
     * then its own start selects. */
    Expect("evict claim", (unsigned long)UacAltClaim(&r[0], &gate, UAC_OWN_START, 1600), 1);
    Expect("evict end", (unsigned long)UacAltEnd(&r[0], &gate, UAC_OWN_START, 1600, &at), UAC_ACT_ZERO);
    Expect("evict end after the hold", T(at), 1600);
    Expect("evicted idle", UacAltZeroDone(&r[0], UAC_SEL_OK, 1610), UAC_ALT_IDLE);
    UacAltRelease(&r[0]);
    Expect("now free", (unsigned long)UacAltConflict(r, 3, 0), UAC_CLAIM_FREE);
    x = w; x.Rate = 44100; x.Alt = 1; x.Terminal = 5;
    UacAltClaim(&r[1], &gate, UAC_OWN_START, 1620);
    Expect("capture selects", (unsigned long)UacAltStart(&r[1], &gate, &x, 1620, &at), UAC_ACT_SELECT);
}

/* A due alternate 0 that cannot be submitted: the record and AttemptAt
 * stay, and it is held back 100 ms, doubling to 1 s (29.4.6). */
static void Retry(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at, next, t;
    int i;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1000, &at);   /* deadline 3000 */
    UacAltClaim(&r, &gate, UAC_OWN_THREAD, 3000);
    UacAltEnd(&r, &gate, UAC_OWN_THREAD, 3000, &at);
    Expect("unsubmitted: lingering", UacAltZeroDone(&r, UAC_SEL_UNSUBMITTED, 3001), UAC_ALT_LINGERING);
    Expect("unsubmitted: AttemptAt kept", T(r.AttemptAt), 1000);
    Expect("unsubmitted: deadline kept", T(r.Deadline), 3000);
    UacAltRelease(&r);
    Expect("unsubmitted: not runnable at once", (unsigned long)(UacAltScan(&r, 1, &gate, 3001, &next) + 1), 0);
    Expect("unsubmitted: next is the retry", T(next), 3101);
    Expect("unsubmitted: thread claim refused before the retry", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, 3100), 0);
    /* Keep failing: 100, 200, 400, 800, 1000, 1000 ms apart. */
    t = 3001;
    for (i = 0; i < 6; i++) {
        static const unsigned long gap[6] = { 100, 200, 400, 800, 1000, 1000 };
        char name[64];
        t += gap[i];
        sprintf(name, "retry %d: runnable at its time", i);
        Expect(name, (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, t), 1);
        UacAltEnd(&r, &gate, UAC_OWN_THREAD, t, &at);
        UacAltZeroDone(&r, UAC_SEL_UNSUBMITTED, t);
        UacAltRelease(&r);
        sprintf(name, "retry %d: next interval", i);
        Expect(name, r.RetryMs, i < 4 ? gap[i + 1] : 1000);
    }
    /* A successful submission clears it. */
    t = r.RetryAt;
    UacAltClaim(&r, &gate, UAC_OWN_THREAD, t);
    UacAltEnd(&r, &gate, UAC_OWN_THREAD, t, &at);
    Expect("retry then success: idle", UacAltZeroDone(&r, UAC_SEL_OK, t + 2), UAC_ALT_IDLE);
    Expect("retry cleared", r.RetryMs, 0);
    Expect("retry time cleared", T(r.RetryAt), 0);
    /* The same after the stopping caller's immediate alternate 0. */
    Want(&w, 0, 0, UAC_HOLD_TO_ZERO);
    Running(&r, &w, 1000);
    UacAltHandBack(&r, &gate, 1, 1010, &at);
    UacAltZeroDone(&r, UAC_SEL_UNSUBMITTED, 1010);
    UacAltRelease(&r);
    Expect("immediate unsubmitted: held back", (unsigned long)(UacAltScan(&r, 1, &gate, 1010, &next) + 1), 0);
    Expect("immediate unsubmitted: retry at", T(next), 1110);
}

/* Full-width times: a non-wrapping 64-bit clock, saturation the limit. */
static void Time(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at, next, big = (UAC_ALT_TIME)1 << 40, t62 = (UAC_ALT_TIME)1 << 62;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    /* 2^40 ms (about 35 years of uptime): exact arithmetic. */
    Running(&r, &w, big);
    UacAltHandBack(&r, &gate, 1, big + 100, &at);
    ExpectT("2^40: deadline exact", at, big + 2100);
    Expect("2^40: not runnable a ms early", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, big + 2099), 0);
    Expect("2^40: runnable at it", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, big + 2100), 1);
    /* Beyond 2^32: the low halves alone would order these wrongly. */
    Running(&r, &w, ((UAC_ALT_TIME)1 << 32) - 50);
    UacAltHandBack(&r, &gate, 1, ((UAC_ALT_TIME)1 << 32) - 10, &at);
    ExpectT("across 2^32: deadline", at, ((UAC_ALT_TIME)1 << 32) + 1990);
    UacAltScan(&r, 1, &gate, ((UAC_ALT_TIME)1 << 32), &next);
    ExpectT("across 2^32: pending", next, ((UAC_ALT_TIME)1 << 32) + 1990);
    /* 2^62 ms, inside the contract (below UAC_ALT_TIME_LIMIT). */
    Running(&r, &w, t62);
    UacAltHandBack(&r, &gate, 1, t62, &at);
    ExpectT("2^62: deadline exact", at, t62 + 2000);
    Expect("limit is above 2^62", (unsigned long)(t62 < UAC_ALT_TIME_LIMIT), 1);
    /* Outside the contract: a deadline past the clock's range saturates to
     * "never", and the record never becomes runnable (the chosen limit). */
    Running(&r, &w, UAC_ALT_NEVER - 1500);
    UacAltHandBack(&r, &gate, 1, UAC_ALT_NEVER - 1000, &at);
    ExpectT("saturated: never", at, UAC_ALT_NEVER);
    next = 0;
    Expect("saturated: not runnable", (unsigned long)(UacAltScan(&r, 1, &gate, UAC_ALT_NEVER - 1, &next) + 1), 0);
    ExpectT("saturated: no pending time", next, UAC_ALT_NEVER);
}

/* The gate is the authority: a late success after the failure. */
static void LateSuccess(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    UacAltInit(&r, 0);
    UacAltClaim(&r, &gate, UAC_OWN_START, 500);
    UacAltStart(&r, &gate, &w, 500, &at);
    /* The SET_INTERFACE(N) expires: the hook fails the gate and the
     * record; the request completes successfully much later. */
    gate.Failed = 1;
    UacAltFail(&r);
    Expect("late OK: record stays failed", UacAltSelectDone(&r, &w, 0, UAC_SEL_OK, 9000), UAC_ALT_FAILED);
    Expect("late OK: commit refused", (unsigned long)UacAltCommit(&r, &gate), 0);
    Expect("late OK: start refused", (unsigned long)UacAltStart(&r, &gate, &w, 9001, &at), UAC_ACT_REFUSE);
    Expect("late OK: no end", (unsigned long)UacAltEnd(&r, &gate, UAC_OWN_START, 9001, &at), UAC_ACT_REFUSE);
    Expect("late OK: no thread work", (unsigned long)UacAltClaim(&r, &gate, UAC_OWN_THREAD, 9001), 0);
    /* Even a record not marked failed refuses on the gate alone. */
    Running(&r, &w, 1000);   /* gate failed: the start refused, state set by the late OK */
    Expect("gate alone: commit refused", (unsigned long)UacAltCommit(&r, &gate), 0);
    Expect("gate alone: hand-back sends nothing", (unsigned long)UacAltHandBack(&r, &gate, 1, 1100, &at), UAC_ACT_REFUSE);
    Expect("gate alone: zero late OK keeps failed", UacAltZeroDone(&r, UAC_SEL_OK, 1200), UAC_ALT_FAILED);
    gate.Failed = 0;
}

/* Every SET_INTERFACE(N) outcome from Idle and from Unknown, with the
 * failure setting on and off (29.4.6, 29.8). */
static void Outcomes(void)
{
    static const struct { int sel; unsigned int failOn; unsigned int want; int moves; } m[] = {
        { UAC_SEL_OK, 1, UAC_ALT_ACTIVE, 1 },
        { UAC_SEL_BAD_PIPES, 1, UAC_ALT_LINGERING, 1 },
        { UAC_SEL_REFUSED, 1, UAC_ALT_UNKNOWN, 1 },
        { UAC_SEL_UNSUBMITTED, 1, 99, 0 },          /* 99: the state it had */
        { UAC_SEL_EXPIRED, 1, UAC_ALT_FAILED, 1 },
        { UAC_SEL_EXPIRED, 0, UAC_ALT_UNKNOWN, 1 }, /* setting off: that request's failure */
    };
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    unsigned int from, k;
    char name[96];
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    Expect("expiry, setting on", (unsigned long)UacAltExpiry(1), UAC_SEL_EXPIRED);
    Expect("expiry, setting off", (unsigned long)UacAltExpiry(0), UAC_SEL_REFUSED);
    for (from = 0; from < 2; from++)
        for (k = 0; k < sizeof(m) / sizeof(m[0]); k++) {
            unsigned int start;
            int outcome = m[k].sel == UAC_SEL_EXPIRED ? UacAltExpiry(m[k].failOn) : m[k].sel;
            UacAltInit(&r, 100);
            if (from) {   /* to Unknown first: a refused selection at 400 */
                UacAltClaim(&r, &gate, UAC_OWN_START, 400);
                UacAltStart(&r, &gate, &w, 400, &at);
                UacAltSelectDone(&r, &w, 0, UAC_SEL_REFUSED, 400);
                UacAltRelease(&r);
            }
            start = r.State;
            UacAltClaim(&r, &gate, UAC_OWN_START, 1000);
            UacAltStart(&r, &gate, &w, 1000, &at);
            sprintf(name, "from %s, outcome %d, fail %u: state", from ? "unknown" : "idle", m[k].sel, m[k].failOn);
            Expect(name, UacAltSelectDone(&r, &w, 0, outcome, 1050), m[k].want == 99 ? start : m[k].want);
            sprintf(name, "from %s, outcome %d, fail %u: attempt", from ? "unknown" : "idle", m[k].sel, m[k].failOn);
            Expect(name, T(r.AttemptAt), m[k].moves ? 1050 : (from ? 400UL : 100UL));
        }
}

/* Settings across RUN, adoption and hand-back: one snapshot per RUN. */
static void SettingsAcross(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT run1, run2;
    UAC_ALT_TIME at;
    Want(&run1, 2000, 200, UAC_HOLD_BOTH);
    Running(&r, &run1, 1000);
    /* The registry is edited while the stream runs: no effect yet. */
    Want(&run2, 5000, 1000, UAC_HOLD_BOTH);
    UacAltHandBack(&r, &gate, 1, 1500, &at);
    Expect("RUN 1 snapshot at its hand-back", T(at), 3500);
    /* RUN 2 adopts with the edited snapshot; its hand-back uses it. */
    UacAltClaim(&r, &gate, UAC_OWN_START, 1600);
    Expect("RUN 2 adopts", (unsigned long)UacAltStart(&r, &gate, &run2, 1600, &at), UAC_ACT_ADOPT);
    UacAltHandBack(&r, &gate, 1, 1700, &at);
    Expect("RUN 2 snapshot at its hand-back", T(at), 6700);
    Expect("RUN 2 snapshot kept", r.Snap.LingerMs, 5000);
    /* Its forced end uses the record's (RUN 2's) hold. */
    UacAltClaim(&r, &gate, UAC_OWN_START, 1800);
    UacAltEnd(&r, &gate, UAC_OWN_START, 1800, &at);
    Expect("forced end with RUN 2's hold", T(at), 2000);
}

/* Gen counts every change of state, owner or retry, and nothing else. */
static void Generation(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w;
    UAC_ALT_TIME at;
    unsigned long g0;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    UacAltInit(&r, 0);
    Expect("init gen", r.Gen, 0);
    g0 = r.Gen; UacAltClaim(&r, &gate, UAC_OWN_START, 10);
    Expect("claim bumps", r.Gen, g0 + 1);
    g0 = r.Gen; UacAltClaim(&r, &gate, UAC_OWN_PNP, 10);
    Expect("refused claim does not", r.Gen, g0);
    g0 = r.Gen; UacAltStart(&r, &gate, &w, 10, &at);
    Expect("SELECT decision does not", r.Gen, g0);
    g0 = r.Gen; UacAltSelectDone(&r, &w, 0, UAC_SEL_UNSUBMITTED, 20);
    Expect("unsubmitted select does not", r.Gen, g0);
    g0 = r.Gen; UacAltSelectDone(&r, &w, 0, UAC_SEL_OK, 20);
    Expect("select OK bumps", r.Gen, g0 + 1);
    g0 = r.Gen; UacAltRelease(&r);
    Expect("release of a stream's record does not", r.Gen, g0);
    g0 = r.Gen; UacAltHandBack(&r, &gate, 1, 30, &at);
    Expect("hand-back bumps", r.Gen, g0 + 1);
    g0 = r.Gen; UacAltHandBack(&r, &gate, 1, 40, &at);
    Expect("repeated hand-back does not", r.Gen, g0);
    g0 = r.Gen; UacAltScan(&r, 1, &gate, 50, &at);
    Expect("scan does not", r.Gen, g0);
    g0 = r.Gen; UacAltCommit(&r, &gate);
    Expect("commit does not", r.Gen, g0);
    UacAltClaim(&r, &gate, UAC_OWN_START, 60);
    g0 = r.Gen; UacAltZeroDone(&r, UAC_SEL_UNSUBMITTED, 60);
    Expect("retry time bumps", r.Gen, g0 + 1);
    g0 = r.Gen; UacAltFail(&r);
    Expect("fail bumps", r.Gen, g0 + 1);
    g0 = r.Gen; UacAltFail(&r);
    Expect("second fail does not", r.Gen, g0);
    g0 = r.Gen; UacAltZeroDone(&r, UAC_SEL_OK, 70);
    Expect("late completion on failed does not", r.Gen, g0);
}

/* Geometry: the record stores what the pipes retain. */
static void Kept(void)
{
    UAC_ALT_REC r;
    UAC_ALT_WANT w, x;
    UAC_ALT_GEOM kept;
    UAC_ALT_TIME at;
    Want(&w, 2000, 200, UAC_HOLD_BOTH);
    kept = w.Need;
    kept.Data.Packet = 384;          /* the stack returned 384 */
    kept.Data.Transfer = 3072;       /* asked */
    UacAltInit(&r, 0);
    UacAltClaim(&r, &gate, UAC_OWN_START, 1000);
    UacAltStart(&r, &gate, &w, 1000, &at);
    UacAltSelectDone(&r, &w, &kept, UAC_SEL_OK, 1000);
    UacAltHandBack(&r, &gate, 1, 1300, &at);
    x = w; x.Need.Data.Packet = 384; x.Need.Data.Transfer = 3072;
    UacAltClaim(&r, &gate, UAC_OWN_START, 1310);
    Expect("needs within what is kept adopt", (unsigned long)UacAltStart(&r, &gate, &x, 1310, &at), UAC_ACT_ADOPT);
    /* The 32-record limit of the conflict mask. */
    {
        static UAC_ALT_REC many[UAC_ALT_MAX_RECORDS];
        unsigned int i;
        for (i = 0; i < UAC_ALT_MAX_RECORDS; i++) { UacAltInit(&many[i], 0); many[i].State = UAC_ALT_LINGERING; }
        Expect("bit 31 of 32 lingering", (unsigned long)UacAltConflict(many, UAC_ALT_MAX_RECORDS, 0x80000000UL), UAC_CLAIM_LINGERING);
        Expect("all 32 lingering", (unsigned long)UacAltConflict(many, UAC_ALT_MAX_RECORDS, 0xFFFFFFFFUL), UAC_CLAIM_LINGERING);
        many[31].State = UAC_ALT_ACTIVE;
        Expect("bit 31 active", (unsigned long)UacAltConflict(many, UAC_ALT_MAX_RECORDS, 0x80000000UL), UAC_CLAIM_ACTIVE);
        Expect("bit 31 past n=31", (unsigned long)UacAltConflict(many, 31, 0x80000000UL), UAC_CLAIM_ACTIVE);
    }
}

int main(void)
{
    gate.Admit = UAC_ADMIT_OPEN; gate.Failed = 0; gate.Epoch = 0;
    Settings();
    Baseline();
    Deadlines();
    Idempotent();
    Adoption();
    Holds();
    FailedSteps();
    Gate();
    Thread();
    Conflict();
    Retry();
    Time();
    LateSuccess();
    Outcomes();
    SettingsAcross();
    Generation();
    Kept();
    printf("%s alttest: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
