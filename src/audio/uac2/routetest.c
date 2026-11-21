/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test of uacroute.c (docs/design/uac2.md section 28.7, "The route
 * lock"; 28.16): a pure model of the locking protocol. Each actor (a
 * switch, a capture stream's open, create and close, an automatic level
 * write) is a list of steps, each step one hold of RouteLock or one write
 * to the model device; a switch's own level write is a reservation, the
 * write and the release, three steps, before its selector write and its
 * end, which confirms, fails (the selector did not move) or ends unknown.
 * The test's scheduler runs every interleaving of the actors' steps and
 * checks, after every step: no selector an open stream has locked moves;
 * the route a stream records is inside its locked graph; a level write is
 * made only by the control's marked writer and never on a control another
 * endpoint reads; a refused switch leaves no reservation; and at the end
 * of every schedule no writer mark and no reader is left. These are safety
 * properties of the table only (not progress, wakeups, rundown, power or
 * completion lifetime). The same endpoint's open during its own switch is
 * also run in exactly the order 28.16 names, and the open rule before
 * (which ignored its own endpoint's writer marks) is run separately to show
 * it fails; its schedules are counted apart. Built with small table sizes
 * (test.cmd); also the table built from the recorded X4's capture graph.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uacroute.h"

static unsigned int checks;
static void Check(int ok, const char *expression, unsigned int line)
{
    checks++;
    if (!ok) { printf("FAIL line %u: %s\n", line, expression); exit(1); }
}
#define CHECK(x) Check((x), #x, __LINE__)

/* The model: endpoint A (0) chooses between unit 1 and unit 2 with
 * chooser C; endpoint B (1) records after its own unit 9 through A's
 * chooser at its read position. */
enum { A = 0, B = 1 };
static int iC, iU1, iU2, iU9;

#define ACTORS 4
#define ACT_SWITCH 1
#define ACT_OPEN 2
#define ACT_LEVEL 3
#define END_CONFIRMED 0
#define END_FAILED 1                /* the selector write did not take; readback the old input */
#define END_UNKNOWN 2               /* the selector moved; readback unreadable (0) */
typedef struct ACTOR {
    int Type, Ep, Pc, Done, Tries, Recording, Waited, Holds;
    int Target, Control, End;       /* switch: C's target and how it ends; level: the control */
    UAC_ROUTE_SET Locked;
    unsigned int Snap[UAC_ROUTE_CONTROLS];
} ACTOR;
typedef struct MODEL {
    UAC_ROUTE_TABLE T;
    unsigned int Dev[UAC_ROUTE_CONTROLS];   /* the device's selector positions */
    int Actors;
    ACTOR Act[ACTORS];
    int OldRule;
    int Violation;
} MODEL;

static void Path(UAC_ROUTE_TABLE *t, unsigned int e, int a, int b, int c, int pin)
{
    UAC_ROUTE_PATH *p = &t->Ep[e].Route[t->Ep[e].RouteCount++];
    p->Count = 0; p->Needs = 0;
    if (a >= 0) p->Control[p->Count++] = (UAC_BYTE)a;
    if (b >= 0) p->Control[p->Count++] = (UAC_BYTE)b;
    if (c >= 0) p->Control[p->Count++] = (UAC_BYTE)c;
    p->Sel[0] = (UAC_BYTE)iC; p->Pin[0] = (UAC_BYTE)pin; p->Needs = 1;
}

static void Init(MODEL *m)
{
    UAC_CAP_CONTROL c;
    memset(m, 0, sizeof(*m));
    UacRouteInit(&m->T);
    c.Entity = 26; c.Selector = 1; c.Channel = 0; iC = UacRouteControl(&m->T, &c, 1);
    c.Entity = 1; c.Selector = 2; iU1 = UacRouteControl(&m->T, &c, 0);
    c.Entity = 2; iU2 = UacRouteControl(&m->T, &c, 0);
    c.Entity = 9; iU9 = UacRouteControl(&m->T, &c, 0);
    m->T.Endpoints = 2;
    Path(&m->T, A, iC, iU1, -1, 1); Path(&m->T, A, iC, iU2, -1, 2);
    Path(&m->T, B, iU9, iC, iU1, 1); Path(&m->T, B, iU9, iC, iU2, 2);
    UacRoutePublish(&m->T, (unsigned int)iC, 1);
    m->Dev[iC] = 1;
}

static int AddActor(MODEL *m, int type, int ep, int target)
{
    ACTOR *a = &m->Act[m->Actors];
    memset(a, 0, sizeof(*a));
    a->Type = type; a->Ep = ep;
    if (type == ACT_SWITCH) a->Target = target; else a->Control = target;
    return m->Actors++;
}

/* The open rule as it stood before: wait only for another endpoint's writer. */
static int OldOpen(UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *set)
{
    unsigned int k;
    UacRouteGraph(t, e, set);
    for (k = 0; k < set->Count; k++) {
        const UAC_ROUTE_ENTRY *x = &t->Entry[set->Index[k]];
        if (x->Writer >= 0 && (unsigned int)x->Writer != e) { set->Count = 0; return UAC_ROUTE_WAIT; }
    }
    UacRouteRegister(t, e, set);
    return UAC_ROUTE_OK;
}

static UAC_ROUTE_TABLE shadow;
static void Invariants(MODEL *m)
{
    int i;
    unsigned int k;
    for (i = 0; i < m->Actors; i++) {
        ACTOR *a = &m->Act[i];
        UAC_ROUTE_SET actual;
        if (a->Type != ACT_OPEN || !a->Recording) continue;
        /* No selector of the locked graph moves under the stream. */
        for (k = 0; k < a->Locked.Count; k++) {
            unsigned int x = a->Locked.Index[k];
            if (m->T.Entry[x].Selector && m->Dev[x] != a->Snap[x]) m->Violation = 1;
        }
        /* The route recorded (from the device's positions) is locked. */
        memcpy(&shadow, &m->T, sizeof(shadow));
        for (k = 0; k < shadow.Count; k++) if (shadow.Entry[k].Selector) shadow.Entry[k].Position = m->Dev[k];
        UacRouteGraph(&shadow, (unsigned int)a->Ep, &actual);
        for (k = 0; k < actual.Count; k++) {
            unsigned int j;
            for (j = 0; j < a->Locked.Count && a->Locked.Index[j] != actual.Index[k]; j++) ;
            if (j == a->Locked.Count) m->Violation = 1;
        }
    }
}

/* At the end of a schedule: no writer mark, no reader left. */
static int Clean(const MODEL *m)
{
    unsigned int k;
    for (k = 0; k < m->T.Count; k++)
        if (m->T.Entry[k].Writer >= 0 || m->T.Entry[k].Readers || m->T.Entry[k].ReaderMask) return 0;
    return 1;
}

/* A level write by actor a's endpoint on control c: only by its marked
 * writer, never on a control another endpoint reads. */
static void Write(MODEL *m, const ACTOR *a, int c, unsigned int kind)
{
    const UAC_ROUTE_ENTRY *x = &m->T.Entry[c];
    if (x->Writer != a->Ep || x->Kind != kind || (x->ReaderMask & ~(1u << a->Ep))) m->Violation = 1;
}

static void NoSwitchMarks(MODEL *m, int ep)
{
    unsigned int k;
    for (k = 0; k < m->T.Count; k++)
        if (m->T.Entry[k].Writer == ep && m->T.Entry[k].Kind == UAC_WRITER_SWITCH) m->Violation = 1;
}

/* One step of actor i; returns 0 when the actor cannot step now. */
static int Step(MODEL *m, int i)
{
    ACTOR *a = &m->Act[i];
    UAC_BYTE sel = (UAC_BYTE)iC, target = (UAC_BYTE)a->Target, back;
    unsigned int k;
    int r;
    if (a->Done) return 0;
    switch (a->Type) {
    case ACT_SWITCH:
        switch (a->Pc) {
        case 0:
            r = UacRouteSwitchBegin(&m->T, (unsigned int)a->Ep, 1, &sel, &target);
            if (r != UAC_ROUTE_OK) {
                NoSwitchMarks(m, a->Ep);    /* refused: no partial reservation */
                a->Done = 1; a->Pc = 9;
                return 1;
            }
            a->Pc = 1;
            return 1;
        case 1:     /* step 2 of "What a switch does": the new source's level, reserved */
            a->Holds = UacRouteAutoBegin(&m->T, (unsigned int)a->Ep, (unsigned int)iU2, UAC_WRITER_SWITCH) == UAC_ROUTE_OK;
            a->Pc = 2;
            return 1;
        case 2:     /* written */
            if (a->Holds) Write(m, a, iU2, UAC_WRITER_SWITCH);
            a->Pc = 3;
            return 1;
        case 3:     /* released */
            if (a->Holds) UacRouteAutoEnd(&m->T, (unsigned int)a->Ep, (unsigned int)iU2);
            a->Holds = 0;
            a->Pc = 4;
            return 1;
        case 4:     /* the selector write */
            if (a->End != END_FAILED) m->Dev[iC] = (unsigned int)a->Target;
            a->Pc = 5;
            return 1;
        default:    /* readback, published, marks cleared */
            back = (UAC_BYTE)(a->End == END_UNKNOWN ? 0 : m->Dev[iC]);
            UacRouteSwitchEnd(&m->T, (unsigned int)a->Ep, 1, &sel, &back);
            NoSwitchMarks(m, a->Ep);        /* completion leaves no orphan */
            a->Done = 1;
            return 1;
        }
    case ACT_OPEN:
        if (a->Pc == 0) {
            r = m->OldRule ? OldOpen(&m->T, (unsigned int)a->Ep, &a->Locked)
                           : UacRouteOpen(&m->T, (unsigned int)a->Ep, &a->Locked);
            if (r == UAC_ROUTE_WAIT) {
                a->Waited = 1;
                if (++a->Tries >= 3) { a->Done = 1; a->Pc = 9; }           /* busy after the time-out */
                return 1;
            }
            for (k = 0; k < UAC_ROUTE_CONTROLS; k++) a->Snap[k] = m->Dev[k];
            a->Pc = 1;
        } else if (a->Pc == 1) {
            a->Recording = 1;                                               /* UacStream::Create */
            a->Pc = 2;
        } else {
            a->Recording = 0;
            UacRouteClose(&m->T, (unsigned int)a->Ep, &a->Locked);
            a->Done = 1;
        }
        return 1;
    case ACT_LEVEL:
        if (a->Pc == 0) {
            r = UacRouteAutoBegin(&m->T, (unsigned int)a->Ep, (unsigned int)a->Control, UAC_WRITER_LEVEL);
            if (r != UAC_ROUTE_OK) { a->Done = 1; a->Pc = 9; return 1; }    /* skipped or busy */
            a->Pc = 1;
        } else if (a->Pc == 1) {
            Write(m, a, a->Control, UAC_WRITER_LEVEL);
            a->Pc = 2;
        } else {
            UacRouteAutoEnd(&m->T, (unsigned int)a->Ep, (unsigned int)a->Control);
            a->Done = 1;
        }
        return 1;
    }
    return 0;
}

/* Every interleaving, depth first; a waiting open is retried only after
 * another actor has stepped (it waits on the writer's clearing). */
#define DEPTH 32
static MODEL stack[DEPTH + 1];
static unsigned long runs, bad;
static void Explore(int depth, int last)
{
    MODEL *m = &stack[depth];
    int i, any = 0;
    if (depth >= DEPTH) { printf("FAIL: model deeper than %d\n", DEPTH); exit(1); }
    for (i = 0; i < m->Actors; i++) {
        MODEL *n = &stack[depth + 1];
        if (m->Act[i].Done) continue;
        if (m->Act[i].Type == ACT_OPEN && m->Act[i].Pc == 0 && m->Act[i].Waited && last == i) continue;
        memcpy(n, m, sizeof(*n));
        if (!Step(n, i)) continue;
        any = 1;
        Invariants(n);
        if (n->Violation) { bad++; runs++; continue; }
        Explore(depth + 1, i);
    }
    if (!any) {
        /* Everything ended, or only a waiting open retried by itself is
         * left: one complete schedule. */
        MODEL *n = &stack[depth + 1];
        memcpy(n, m, sizeof(*n));
        for (i = 0; i < n->Actors; i++)
            while (!n->Act[i].Done) { Step(n, i); Invariants(n); }
        if (n->Violation || !Clean(n)) bad++;
        runs++;
    }
}

typedef struct SPEC { int Type, Ep, X, End; } SPEC;
static void All(int oldRule, const SPEC *s, int count, unsigned long *outRuns, unsigned long *outBad)
{
    MODEL *m = &stack[0];
    int i, k;
    Init(m);
    m->OldRule = oldRule;
    for (i = 0; i < count; i++) {
        k = AddActor(m, s[i].Type, s[i].Ep, s[i].X);
        m->Act[k].End = s[i].End;
    }
    runs = bad = 0;
    Explore(0, -1);
    *outRuns = runs; *outBad = bad;
}

static unsigned long production, oldRuns, oldBad;
static void Exhaustive(void)
{
    /* X of a level actor: 1 unit 1, 2 unit 2, 9 unit 9 (mapped below). */
    static const SPEC own[] = { { ACT_SWITCH, A, 2, END_CONFIRMED }, { ACT_OPEN, A, 0, 0 } };
    static const SPEC ownFail[] = { { ACT_SWITCH, A, 2, END_FAILED }, { ACT_OPEN, A, 0, 0 } };
    static const SPEC ownUnknown[] = { { ACT_SWITCH, A, 2, END_UNKNOWN }, { ACT_OPEN, A, 0, 0 } };
    static const SPEC cross[] = { { ACT_SWITCH, A, 2, END_CONFIRMED }, { ACT_OPEN, B, 0, 0 } };
    static const SPEC crossUnknown[] = { { ACT_SWITCH, A, 2, END_UNKNOWN }, { ACT_OPEN, B, 0, 0 },
                                         { ACT_LEVEL, B, 9, 0 } };
    static const SPEC three[] = { { ACT_SWITCH, A, 2, END_CONFIRMED }, { ACT_OPEN, A, 0, 0 }, { ACT_OPEN, B, 0, 0 } };
    static const SPEC lvOther[] = { { ACT_LEVEL, A, 1, 0 }, { ACT_OPEN, B, 0, 0 }, { ACT_SWITCH, A, 2, END_CONFIRMED } };
    static const SPEC lvOpens[] = { { ACT_LEVEL, A, 1, 0 }, { ACT_OPEN, A, 0, 0 }, { ACT_OPEN, B, 0, 0 } };
    /* Level and Switch contending for the same level control (unit 2's
     * volume): another endpoint's automatic write against A's switch,
     * and A's own, with each switch ending. */
    static const SPEC sameB[] = { { ACT_LEVEL, B, 2, 0 }, { ACT_SWITCH, A, 2, END_CONFIRMED }, { ACT_OPEN, A, 0, 0 } };
    static const SPEC sameBFail[] = { { ACT_LEVEL, B, 2, 0 }, { ACT_SWITCH, A, 2, END_FAILED }, { ACT_OPEN, B, 0, 0 } };
    static const SPEC sameA[] = { { ACT_LEVEL, A, 2, 0 }, { ACT_SWITCH, A, 2, END_UNKNOWN }, { ACT_OPEN, B, 0, 0 } };
    static const SPEC sameAOpen[] = { { ACT_LEVEL, A, 2, 0 }, { ACT_SWITCH, A, 2, END_CONFIRMED }, { ACT_OPEN, A, 0, 0 } };
    static const struct { const SPEC *s; int n; } cases[] = {
        { own, 2 }, { ownFail, 2 }, { ownUnknown, 2 }, { cross, 2 }, { crossUnknown, 3 }, { three, 3 },
        { lvOther, 3 }, { lvOpens, 3 }, { sameB, 3 }, { sameBFail, 3 }, { sameA, 3 }, { sameAOpen, 3 }
    };
    unsigned long r, b;
    unsigned int k;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        SPEC s[3];
        int j;
        for (j = 0; j < cases[k].n; j++) {
            s[j] = cases[k].s[j];
            if (s[j].Type == ACT_LEVEL) s[j].X = s[j].X == 9 ? iU9 : s[j].X == 1 ? iU1 : iU2;
        }
        All(0, s, cases[k].n, &r, &b);
        if (!(r > 10 && b == 0)) printf("case %u: %lu schedules, %lu unsafe\n", k, r, b);
        CHECK(r > 10 && b == 0);
        production += r;
    }
    /* The rule before: some order records the new route under the old lock. */
    All(1, own, 2, &r, &b);
    CHECK(r > 10 && b > 0);
    oldRuns += r; oldBad += b;
}

/* Scripted orders. */
static MODEL s;
static void Run(const int *order, int n)
{
    int k;
    for (k = 0; k < n; k++) { CHECK(Step(&s, order[k])); Invariants(&s); CHECK(!s.Violation); }
}

static void Scripted(void)
{
    UAC_ROUTE_SET g;
    int sw, op, lv, opb;
    /* 28.16's order: A's switch reserves, A's open runs its hold (waits),
     * the switch writes its level, moves the selector and ends, the open
     * registers on the route the switch left. */
    Init(&s);
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    op = AddActor(&s, ACT_OPEN, A, 0);
    {
        int order[2]; order[0] = sw; order[1] = op;
        Run(order, 2);
    }
    CHECK(s.Act[op].Pc == 0 && s.Act[op].Waited && s.T.Entry[iC].Readers == 0);
    {
        int order[6]; order[0] = sw; order[1] = sw; order[2] = sw; order[3] = sw; order[4] = sw; order[5] = op;
        Run(order, 6);
    }
    CHECK(s.Act[sw].Done && s.T.Entry[iC].Position == 2 && s.T.Entry[iC].Writer < 0);
    UacRouteGraph(&s.T, A, &g);
    CHECK(s.Act[op].Pc == 1 && s.Act[op].Locked.Count == g.Count && g.Count == 2 &&
          s.Act[op].Locked.Index[0] == g.Index[0] && s.Act[op].Locked.Index[1] == g.Index[1]);
    CHECK(s.Act[op].Locked.Version == s.T.Version && s.T.Entry[iU2].Readers == 1 && s.T.Entry[iU1].Readers == 0);
    /* The same order under the rule before: the open locks the old route
     * and the switch moves the selector under it. */
    Init(&s);
    s.OldRule = 1;
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    op = AddActor(&s, ACT_OPEN, A, 0);
    CHECK(Step(&s, sw) && Step(&s, op) && Step(&s, op));
    CHECK(s.T.Entry[iU1].Readers == 1 && s.Act[op].Recording);
    CHECK(Step(&s, sw) && Step(&s, sw) && Step(&s, sw) && Step(&s, sw));
    Invariants(&s);
    CHECK(s.Violation);
    /* The reverse order: the open registers first, A's switch is refused
     * with nothing reserved. */
    Init(&s);
    op = AddActor(&s, ACT_OPEN, A, 0);
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    CHECK(Step(&s, op) && Step(&s, sw));
    CHECK(s.Act[sw].Done && s.Act[sw].Pc == 9 && s.T.Entry[iC].Writer < 0 && s.T.Entry[iU2].Writer < 0 &&
          s.Dev[iC] == 1 && !s.Violation);
    /* A's own level write in progress: A's open does not wait for it; B's,
     * whose route holds that control, does. */
    Init(&s);
    lv = AddActor(&s, ACT_LEVEL, A, iU1);
    op = AddActor(&s, ACT_OPEN, A, 0);
    opb = AddActor(&s, ACT_OPEN, B, 0);
    CHECK(Step(&s, lv) && s.T.Entry[iU1].Writer == A && s.T.Entry[iU1].Kind == UAC_WRITER_LEVEL);
    CHECK(Step(&s, op) && s.Act[op].Pc == 1 && !s.Act[op].Waited);
    CHECK(Step(&s, opb) && s.Act[opb].Pc == 0 && s.Act[opb].Waited);
    CHECK(Step(&s, lv) && Step(&s, lv) && Step(&s, opb) && s.Act[opb].Pc == 1);
    /* An automatic write on a control another endpoint reads is skipped. */
    CHECK(UacRouteAutoBegin(&s.T, A, (unsigned int)iU1, UAC_WRITER_LEVEL) == UAC_ROUTE_SKIP);
    CHECK(UacRouteAutoBegin(&s.T, B, (unsigned int)iU9, UAC_WRITER_LEVEL) == UAC_ROUTE_OK);
    CHECK(UacRouteAutoBegin(&s.T, B, (unsigned int)iU9, UAC_WRITER_LEVEL) == UAC_ROUTE_BUSY);
    /* Same control: B's Level write holds unit 2; A's switch is admitted
     * (its selector is free) but its own level reservation on unit 2 is
     * refused busy, so it writes nothing there and leaves B's mark. */
    Init(&s);
    lv = AddActor(&s, ACT_LEVEL, B, iU2);
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    CHECK(Step(&s, lv) && Step(&s, sw) && Step(&s, sw) && !s.Act[sw].Holds);
    CHECK(s.T.Entry[iU2].Writer == B && s.T.Entry[iU2].Kind == UAC_WRITER_LEVEL);
    CHECK(Step(&s, sw) && Step(&s, sw) && !s.Violation && s.T.Entry[iU2].Writer == B);
    CHECK(Step(&s, sw) && Step(&s, sw) && s.Act[sw].Done && s.T.Entry[iU2].Writer == B);
    CHECK(Step(&s, lv) && Step(&s, lv) && Clean(&s));
    /* A switch to the confirmed source moves nothing. */
    {
        UAC_BYTE sel = (UAC_BYTE)iC, one = (UAC_BYTE)s.T.Entry[iC].Position;
        CHECK(UacRouteSwitchBegin(&s.T, A, 1, &sel, &one) == UAC_ROUTE_NOTHING);
    }
    /* A switch that ends unknown leaves the position 0: an open then
     * locks every route. */
    Init(&s);
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    s.Act[sw].End = END_UNKNOWN;
    while (!s.Act[sw].Done) CHECK(Step(&s, sw));
    CHECK(s.T.Entry[iC].Position == 0 && s.Dev[iC] == 2 && Clean(&s));
    op = AddActor(&s, ACT_OPEN, B, 0);
    CHECK(Step(&s, op) && s.Act[op].Locked.Count == 4);
    /* A close (or a failed create) gives every reader back. */
    CHECK(Step(&s, op) && Step(&s, op) && Clean(&s));
    /* A failed switch: the selector did not move, the old input stays
     * published and nothing is left marked. */
    Init(&s);
    sw = AddActor(&s, ACT_SWITCH, A, 2);
    s.Act[sw].End = END_FAILED;
    while (!s.Act[sw].Done) CHECK(Step(&s, sw));
    CHECK(s.T.Entry[iC].Position == 1 && s.Dev[iC] == 1 && Clean(&s) && !s.Violation);
}

static UAC_DESC desc;
static UAC_BYTE file[65536];
static UAC_CAP_GRAPH graph;
static UAC_ROUTE_TABLE table;
static void FromX4(const char *path)
{
    UAC_ROUTE_SET set;
    UAC_CAP_CONTROL c;
    unsigned int n, k;
    int sel, u2, u23;
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    n = (unsigned int)fread(file, 1, sizeof(file), f);
    fclose(f);
    CHECK(UacParse(file, n, 3, 3, 4, &desc) == UAC_OK);
    CHECK(UacCapBuild(&desc, 6, &graph) == 1 && graph.RouteCount <= UAC_ROUTE_ROUTES);
    UacRouteInit(&table);
    CHECK(UacRouteFromGraph(&table, 0, &desc, &graph) == 1 && table.Ep[0].RouteCount == graph.RouteCount);
    c.Entity = 26; c.Selector = 1; c.Channel = 0; sel = UacRouteControl(&table, &c, 1);
    c.Entity = 2; c.Selector = 2; u2 = UacRouteControl(&table, &c, 0);
    c.Entity = 23; u23 = UacRouteControl(&table, &c, 0);
    CHECK(sel >= 0 && table.Entry[sel].Selector && u2 >= 0 && u23 >= 0);
    UacRouteGraph(&table, 0, &set);
    for (k = 0; k < set.Count && set.Index[k] != (UAC_BYTE)u23; k++) ;
    CHECK(k < set.Count);                          /* unknown: every route */
    UacRoutePublish(&table, (unsigned int)sel, 2);
    UacRouteGraph(&table, 0, &set);
    for (k = 0; k < set.Count && set.Index[k] != (UAC_BYTE)u23; k++) ;
    CHECK(k < set.Count && set.Version == 1);
    for (k = 0; k < set.Count && set.Index[k] != (UAC_BYTE)u2; k++) ;
    CHECK(k == set.Count);                         /* the microphone's unit is not on input 2's route */
    /* Exhaustion is reported, never partial: more routes than the table
     * keeps leave the endpoint with none and return 0. */
    graph.RouteCount = UAC_ROUTE_ROUTES + 1;
    CHECK(UacRouteFromGraph(&table, 1, &desc, &graph) == 0 && table.Ep[1].RouteCount == 0);
    /* A full table: UacRouteControl answers -1. */
    UacRouteInit(&table);
    for (k = 0; k < UAC_ROUTE_CONTROLS; k++) {
        c.Entity = (UAC_BYTE)(k + 1); c.Selector = 2; c.Channel = 0;
        CHECK(UacRouteControl(&table, &c, 0) == (int)k);
    }
    c.Entity = 255; c.Selector = 1;
    CHECK(UacRouteControl(&table, &c, 1) == -1);
}

int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: routetest x4-config0.bin\n"); return 2; }
    Scripted();
    Exhaustive();
    FromX4(argv[1]);
    printf("routetest: %u checks passed; %lu production schedules, every one safe; "
           "%lu schedules of the former open rule, %lu of them unsafe as expected\n",
           checks, production, oldRuns, oldBad);
    return 0;
}
