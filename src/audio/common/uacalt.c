/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The kept alternate's state machine (uacalt.h; docs/design/uac2.md 29.4,
 * 29.5.4, 29.8). Plain C, no kernel calls: additions and comparisons of
 * 64-bit times only, no 64-bit division, so it links in a kernel image
 * without the C runtime's helpers.
 */
#include "uacalt.h"

static unsigned long Clamp(unsigned long raw, unsigned long def, unsigned long max)
{
    if (raw == UAC_ALT_RAW_NONE) return def;
    return raw > max ? max : raw;
}

unsigned long UacAltLingerMs(unsigned long raw)
{
    return Clamp(raw, UAC_LINGER_DEFAULT_MS, UAC_LINGER_MAX_MS);
}

unsigned long UacAltHoldMs(unsigned long raw)
{
    return Clamp(raw, UAC_HOLD_DEFAULT_MS, UAC_HOLD_MAX_MS);
}

unsigned long UacAltProbeMs(unsigned long raw)
{
    if (raw == UAC_ALT_RAW_NONE) return UAC_PROBE_DEFAULT_MS;
    if (!raw) return 0;
    if (raw < UAC_PROBE_MIN_MS) return UAC_PROBE_MIN_MS;
    return raw > UAC_PROBE_MAX_MS ? UAC_PROBE_MAX_MS : raw;
}

unsigned int UacAltFailOn(unsigned long raw)
{
    if (raw == 0) return 0;
    if (raw == 1) return 1;
    return UAC_FAIL_DEFAULT;
}

void UacAltSnap(UAC_ALT_SNAP *snap, unsigned long lingerRaw, unsigned long holdRaw,
                unsigned int holdDir)
{
    snap->LingerMs = UacAltLingerMs(lingerRaw);
    snap->HoldMs = UacAltHoldMs(holdRaw);
    snap->HoldDir = holdDir == UAC_HOLD_BOTH ? UAC_HOLD_BOTH : UAC_HOLD_TO_ZERO;
}

static UAC_ALT_TIME AddMs(UAC_ALT_TIME t, unsigned long ms)
{
    return t > UAC_ALT_NEVER - ms ? UAC_ALT_NEVER : t + ms;
}

static UAC_ALT_TIME Later(UAC_ALT_TIME a, UAC_ALT_TIME b)
{
    return a > b ? a : b;
}

static void Bump(UAC_ALT_REC *r)
{
    r->Gen++;
}

void UacAltInit(UAC_ALT_REC *r, UAC_ALT_TIME now)
{
    unsigned char *p = (unsigned char *)r;
    unsigned int i;
    for (i = 0; i < sizeof(*r); i++) p[i] = 0;
    r->State = UAC_ALT_IDLE;
    r->Owner = UAC_OWN_NONE;
    r->AttemptAt = now;
    r->Deadline = r->SavedDeadline = UAC_ALT_NEVER;
}

static int Admits(const UAC_ALT_GATE *g)
{
    return !g->Failed && g->Admit == UAC_ADMIT_OPEN;
}

static int PipeFits(const UAC_ALT_PIPE *have, const UAC_ALT_PIPE *need)
{
    return need->Transfer <= have->Transfer && need->Packet <= have->Packet;
}

static int CanAdopt(const UAC_ALT_REC *r, const UAC_ALT_GATE *g, const UAC_ALT_WANT *w)
{
    if (!r->Adoptable || r->Epoch != g->Epoch) return 0;
    if (r->Alt != w->Alt || r->Terminal != w->Terminal || r->Rate != w->Rate) return 0;
    if (r->Kept.HasFeedback != (w->Need.HasFeedback ? 1U : 0U)) return 0;
    if (!PipeFits(&r->Kept.Data, &w->Need.Data)) return 0;
    if (r->Kept.HasFeedback && !PipeFits(&r->Kept.Feedback, &w->Need.Feedback)) return 0;
    return 1;
}

int UacAltExpiry(unsigned int failOn)
{
    return failOn ? UAC_SEL_EXPIRED : UAC_SEL_REFUSED;
}

static void NoRetry(UAC_ALT_REC *r)
{
    r->RetryAt = 0;
    r->RetryMs = 0;
}

int UacAltStart(UAC_ALT_REC *r, const UAC_ALT_GATE *g, const UAC_ALT_WANT *w,
                UAC_ALT_TIME now, UAC_ALT_TIME *at)
{
    *at = now;
    if (!Admits(g) || r->State == UAC_ALT_FAILED) return UAC_ACT_REFUSE;
    if (r->State == UAC_ALT_ACTIVE) return UAC_ACT_RUNNING;
    if (r->Owner != UAC_OWN_START) return UAC_ACT_BUSY;
    if (r->State == UAC_ALT_LINGERING) {
        if (CanAdopt(r, g, w)) {
            r->SavedDeadline = r->Deadline;
            r->State = UAC_ALT_ACTIVE;
            r->Owner = UAC_OWN_STREAM;
            r->Adopted = 1;
            r->Snap = w->Snap;
            Bump(r);
            return UAC_ACT_ADOPT;
        }
        /* Ended first, after its own hold (29.4.4). */
        *at = Later(now, AddMs(r->AttemptAt, r->Snap.HoldMs));
        return UAC_ACT_ZERO;
    }
    /* Idle or Unknown: a fresh selection. */
    if (w->Snap.HoldDir == UAC_HOLD_BOTH) *at = Later(now, AddMs(r->AttemptAt, w->Snap.HoldMs));
    r->Epoch = g->Epoch;
    return UAC_ACT_SELECT;
}

unsigned int UacAltSelectDone(UAC_ALT_REC *r, const UAC_ALT_WANT *w, const UAC_ALT_GEOM *kept,
                              int outcome, UAC_ALT_TIME now)
{
    /* A late completion after the failure changes nothing (the gate stays
     * the authority either way). */
    if (r->State == UAC_ALT_FAILED) return r->State;
    switch (outcome) {
    case UAC_SEL_OK:
    case UAC_SEL_BAD_PIPES:
        r->AttemptAt = now;
        r->Alt = w->Alt; r->Terminal = w->Terminal; r->Rate = w->Rate;
        r->Kept = kept ? *kept : w->Need;
        r->Kept.HasFeedback = r->Kept.HasFeedback ? 1U : 0U;
        r->Snap = w->Snap;
        NoRetry(r);
        r->Adopted = 0;
        r->Adoptable = 0;
        if (outcome == UAC_SEL_OK) {
            r->State = UAC_ALT_ACTIVE;
            r->Owner = UAC_OWN_STREAM;
        } else {
            /* Selected, but its pipes are unusable: not adoptable, to
             * alternate 0 after the hold (29.4.6). */
            r->State = UAC_ALT_LINGERING;
            r->StopAt = now;
            r->Deadline = AddMs(r->AttemptAt, r->Snap.HoldMs);
        }
        break;
    case UAC_SEL_REFUSED:
        r->AttemptAt = now;
        r->State = UAC_ALT_UNKNOWN;
        r->Adoptable = 0;
        break;
    case UAC_SEL_EXPIRED:
        r->AttemptAt = now;
        r->State = UAC_ALT_FAILED;
        break;
    default:   /* UAC_SEL_UNSUBMITTED: no device interaction, nothing moves */
        return r->State;
    }
    Bump(r);
    return r->State;
}

void UacAltStartFailed(UAC_ALT_REC *r, UAC_ALT_TIME now)
{
    if (r->State != UAC_ALT_ACTIVE) return;
    r->State = UAC_ALT_LINGERING;
    r->Owner = UAC_OWN_NONE;
    if (r->Adopted) {
        r->Deadline = r->SavedDeadline;   /* no transfer was sent */
    } else {
        r->Adoptable = 0;
        r->StopAt = now;
        r->Deadline = AddMs(r->AttemptAt, r->Snap.HoldMs);
    }
    r->Adopted = 0;
    Bump(r);
}

int UacAltCommit(const UAC_ALT_REC *r, const UAC_ALT_GATE *g)
{
    return Admits(g) && r->State == UAC_ALT_ACTIVE && r->Epoch == g->Epoch;
}

int UacAltHandBack(UAC_ALT_REC *r, const UAC_ALT_GATE *g, int clean, UAC_ALT_TIME now,
                   UAC_ALT_TIME *at)
{
    *at = now;
    if (r->State != UAC_ALT_ACTIVE) return UAC_ACT_NONE;   /* repeated stops */
    r->Adopted = 0;
    if (g->Failed) {
        r->State = UAC_ALT_FAILED;
        r->Owner = UAC_OWN_NONE;
        Bump(r);
        return UAC_ACT_REFUSE;
    }
    if (g->Admit == UAC_ADMIT_CLOSED) {
        /* Removal: nothing sent; the alternate is left as it is. */
        r->State = UAC_ALT_UNKNOWN;
        r->Owner = UAC_OWN_NONE;
        Bump(r);
        return UAC_ACT_DISCARD;
    }
    r->Adoptable = clean ? 1U : 0U;
    NoRetry(r);
    r->StopAt = now;
    r->Deadline = AddMs(r->AttemptAt, r->Snap.HoldMs);
    if (r->Adoptable) r->Deadline = Later(r->Deadline, AddMs(now, r->Snap.LingerMs));
    r->State = UAC_ALT_LINGERING;
    if (g->Admit == UAC_ADMIT_OPEN && r->Deadline <= now) {
        r->Owner = UAC_OWN_STREAM;   /* the stopping caller switches now */
        Bump(r);
        return UAC_ACT_ZERO;
    }
    /* Lingering: the alt thread at the deadline, or the power or PnP
     * caller that closed the gate ends it. */
    r->Owner = UAC_OWN_NONE;
    *at = r->Deadline;
    Bump(r);
    return UAC_ACT_LINGER;
}

static int EndAdmits(const UAC_ALT_GATE *g, unsigned int owner)
{
    if (g->Failed) return 0;
    switch (owner) {
    case UAC_OWN_POWER:  return g->Admit == UAC_ADMIT_BELOW_D0;
    case UAC_OWN_PNP:    return g->Admit == UAC_ADMIT_QUIESCING;
    case UAC_OWN_STREAM:
    case UAC_OWN_THREAD:
    case UAC_OWN_START:  return g->Admit == UAC_ADMIT_OPEN;
    default:             return 0;
    }
}

int UacAltEnd(UAC_ALT_REC *r, const UAC_ALT_GATE *g, unsigned int owner, UAC_ALT_TIME now,
              UAC_ALT_TIME *at)
{
    *at = now;
    if (g->Failed || r->State == UAC_ALT_FAILED || !EndAdmits(g, owner)) return UAC_ACT_REFUSE;
    if (r->State != UAC_ALT_LINGERING) return UAC_ACT_NONE;
    if (r->Owner != owner) return UAC_ACT_BUSY;
    if (owner == UAC_OWN_THREAD && (r->Deadline > now || r->RetryAt > now)) return UAC_ACT_NONE;
    *at = Later(now, AddMs(r->AttemptAt, r->Snap.HoldMs));
    return UAC_ACT_ZERO;
}

unsigned int UacAltZeroDone(UAC_ALT_REC *r, int outcome, UAC_ALT_TIME now)
{
    if (r->State == UAC_ALT_FAILED) return r->State;   /* late completion */
    switch (outcome) {
    case UAC_SEL_OK:
        r->AttemptAt = now;
        r->State = UAC_ALT_IDLE;
        break;
    case UAC_SEL_REFUSED:
    case UAC_SEL_BAD_PIPES:
        r->AttemptAt = now;
        r->State = UAC_ALT_UNKNOWN;
        break;
    case UAC_SEL_EXPIRED:
        r->AttemptAt = now;
        r->State = UAC_ALT_FAILED;
        break;
    default:
        /* UAC_SEL_UNSUBMITTED: nothing reached the device, so the record
         * and AttemptAt stay; it is held back until RetryAt, 100 ms
         * doubling to 1 s, so a due record that keeps failing to submit
         * never turns the alt thread into a loop (29.4.6). */
        r->RetryMs = r->RetryMs ? r->RetryMs * 2 : UAC_ALT_RETRY_MIN_MS;
        if (r->RetryMs > UAC_ALT_RETRY_MAX_MS) r->RetryMs = UAC_ALT_RETRY_MAX_MS;
        r->RetryAt = AddMs(now, r->RetryMs);
        Bump(r);
        return r->State;
    }
    r->Adoptable = 0;
    r->Adopted = 0;
    r->Deadline = UAC_ALT_NEVER;
    NoRetry(r);
    Bump(r);
    return r->State;
}

void UacAltFail(UAC_ALT_REC *r)
{
    if (r->State == UAC_ALT_FAILED) return;
    r->State = UAC_ALT_FAILED;
    Bump(r);
}

int UacAltRemove(UAC_ALT_REC *r)
{
    r->State = UAC_ALT_UNKNOWN;
    r->Owner = UAC_OWN_NONE;
    r->Adoptable = 0;
    r->Adopted = 0;
    r->Deadline = UAC_ALT_NEVER;
    Bump(r);
    return UAC_ACT_DISCARD;
}

int UacAltRunnable(const UAC_ALT_REC *r, const UAC_ALT_GATE *g, UAC_ALT_TIME now,
                   UAC_ALT_TIME *next)
{
    /* Held back by the gate or not lingering: no time at all (29.5.4). */
    if (!Admits(g) || r->State != UAC_ALT_LINGERING || r->Epoch != g->Epoch) return 0;
    /* Owned: its release wakes the thread; no time either. */
    if (r->Owner != UAC_OWN_NONE) return 0;
    {
        UAC_ALT_TIME due = Later(r->Deadline, r->RetryAt);
        if (due <= now) return 1;
        if (next && due < *next) *next = due;
    }
    return 0;
}

int UacAltScan(const UAC_ALT_REC *r, unsigned int n, const UAC_ALT_GATE *g, UAC_ALT_TIME now,
               UAC_ALT_TIME *next)
{
    unsigned int i;
    int first = -1;
    UAC_ALT_TIME soonest = UAC_ALT_NEVER;
    for (i = 0; i < n; i++)
        if (UacAltRunnable(&r[i], g, now, &soonest) && first < 0) first = (int)i;
    if (next) *next = soonest;
    return first;
}

int UacAltClaim(UAC_ALT_REC *r, const UAC_ALT_GATE *g, unsigned int owner, UAC_ALT_TIME now)
{
    if (r->Owner != UAC_OWN_NONE || owner == UAC_OWN_NONE) return 0;
    /* The thread acts only on what is runnable when it takes it. */
    if (owner == UAC_OWN_THREAD && !UacAltRunnable(r, g, now, 0)) return 0;
    r->Owner = owner;
    Bump(r);
    return 1;
}

void UacAltRelease(UAC_ALT_REC *r)
{
    unsigned int owner = r->State == UAC_ALT_ACTIVE ? UAC_OWN_STREAM : UAC_OWN_NONE;
    if (r->Owner == owner) return;
    r->Owner = owner;
    Bump(r);
}

int UacAltConflict(const UAC_ALT_REC *r, unsigned int n, unsigned long mask)
{
    unsigned int i;
    if (!mask) return UAC_CLAIM_FREE;
    for (i = 0; i < UAC_ALT_MAX_RECORDS; i++) {
        if (!(mask & (1UL << i))) continue;
        if (i >= n || r[i].State != UAC_ALT_LINGERING) return UAC_CLAIM_ACTIVE;
    }
    return UAC_CLAIM_LINGERING;
}
