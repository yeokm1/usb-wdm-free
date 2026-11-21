/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The adapter's route table (uacroute.h): docs/design/uac2.md section 28.7,
 * "The route lock".
 */
#include "uacroute.h"

void UacRouteInit(UAC_ROUTE_TABLE *t)
{
    unsigned int i;
    for (i = 0; i < sizeof(*t); i++) ((unsigned char *)t)[i] = 0;
}

int UacRouteControl(UAC_ROUTE_TABLE *t, const UAC_CAP_CONTROL *c, int selector)
{
    unsigned int i;
    for (i = 0; i < t->Count; i++)
        if (t->Entry[i].Control.Entity == c->Entity && t->Entry[i].Control.Selector == c->Selector &&
            t->Entry[i].Control.Channel == c->Channel) return (int)i;
    if (t->Count >= UAC_ROUTE_CONTROLS) return -1;
    t->Entry[i].Control = *c;
    t->Entry[i].Selector = selector ? 1 : 0;
    t->Entry[i].Readers = t->Entry[i].ReaderMask = 0;
    t->Entry[i].Writer = -1;
    t->Entry[i].Kind = 0;
    t->Entry[i].Position = 0;
    t->Count++;
    return (int)i;
}

static int AddTo(UAC_ROUTE_PATH *p, int index)
{
    unsigned int k;
    if (index < 0) return 0;
    for (k = 0; k < p->Count; k++) if (p->Control[k] == (UAC_BYTE)index) return 1;
    if (p->Count >= UAC_ROUTE_PER) return 0;
    p->Control[p->Count++] = (UAC_BYTE)index;
    return 1;
}

int UacRouteFromGraph(UAC_ROUTE_TABLE *t, unsigned int e, const UAC_DESC *desc, const UAC_CAP_GRAPH *g)
{
    UAC_ROUTE_EP *ep;
    unsigned int r, i, ch, sel;
    UAC_CAP_CONTROL c;
    if (e >= UAC_ROUTE_ENDPOINTS || !g) return 0;
    ep = &t->Ep[e];
    ep->RouteCount = 0;
    if (e >= t->Endpoints) t->Endpoints = e + 1;
    if (g->RouteCount > UAC_ROUTE_ROUTES) return 0;
    for (r = 0; r < g->RouteCount; r++) {
        const UAC_CAP_ROUTE *rt = &g->Route[r];
        UAC_ROUTE_PATH *p = &ep->Route[ep->RouteCount];
        p->Count = p->Needs = 0;
        for (i = 1; i < rt->Length; i++) {
            if (rt->Class[i] == UAC_CAP_CHOICE) {
                int x;
                c.Entity = rt->Id[i]; c.Selector = 1; c.Channel = 0;
                x = UacRouteControl(t, &c, 1);
                if (!AddTo(p, x)) { ep->RouteCount = 0; return 0; }
                p->Sel[p->Needs] = (UAC_BYTE)x; p->Pin[p->Needs] = rt->Pin[i]; p->Needs++;
                continue;
            }
            if (UacCapUnitChannels(desc, rt->Id[i]) < 0) continue;
            for (sel = 1; sel <= 2; sel++)
                for (ch = 0; ch <= (unsigned int)UacCapUnitChannels(desc, rt->Id[i]) && ch <= UAC_VOL_CHANNELS; ch++) {
                    if (!UacCapAccess(desc, rt->Id[i], ch, sel)) continue;
                    c.Entity = rt->Id[i]; c.Selector = (UAC_BYTE)sel; c.Channel = (UAC_BYTE)ch;
                    if (!AddTo(p, UacRouteControl(t, &c, 0))) { ep->RouteCount = 0; return 0; }
                }
        }
        ep->RouteCount++;
    }
    return 1;
}

static void SetAdd(UAC_ROUTE_SET *s, UAC_BYTE index)
{
    unsigned int k, m;
    for (k = 0; k < s->Count && s->Index[k] < index; k++) ;
    if (k < s->Count && s->Index[k] == index) return;
    for (m = s->Count; m > k; m--) s->Index[m] = s->Index[m - 1];
    s->Index[k] = index;
    s->Count++;
}

void UacRouteGraph(const UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *set)
{
    unsigned int r, k;
    set->Count = 0;
    set->Version = t->Version;
    if (e >= UAC_ROUTE_ENDPOINTS) return;
    for (r = 0; r < t->Ep[e].RouteCount; r++) {
        const UAC_ROUTE_PATH *p = &t->Ep[e].Route[r];
        for (k = 0; k < p->Needs; k++) {
            unsigned int pos = t->Entry[p->Sel[k]].Position;
            if (pos && pos != p->Pin[k]) break;
        }
        if (k < p->Needs) continue;
        for (k = 0; k < p->Count; k++) SetAdd(set, p->Control[k]);
    }
}

int UacRouteBlocked(const UAC_ROUTE_TABLE *t, unsigned int e, const UAC_ROUTE_SET *set)
{
    unsigned int k;
    for (k = 0; k < set->Count; k++) {
        const UAC_ROUTE_ENTRY *x = &t->Entry[set->Index[k]];
        if (x->Writer < 0) continue;
        if (x->Kind == UAC_WRITER_SWITCH || (unsigned int)x->Writer != e) return 1;
    }
    return 0;
}

void UacRouteRegister(UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *set)
{
    unsigned int k;
    for (k = 0; k < set->Count; k++) {
        UAC_ROUTE_ENTRY *x = &t->Entry[set->Index[k]];
        x->Readers++;
        x->ReaderMask |= 1u << e;
    }
    set->Version = t->Version;
}

int UacRouteOpen(UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *locked)
{
    UacRouteGraph(t, e, locked);
    if (UacRouteBlocked(t, e, locked)) { locked->Count = 0; return UAC_ROUTE_WAIT; }
    UacRouteRegister(t, e, locked);
    return UAC_ROUTE_OK;
}

void UacRouteClose(UAC_ROUTE_TABLE *t, unsigned int e, const UAC_ROUTE_SET *locked)
{
    unsigned int k;
    for (k = 0; k < locked->Count; k++) {
        UAC_ROUTE_ENTRY *x = &t->Entry[locked->Index[k]];
        if (x->Readers) x->Readers--;
        x->ReaderMask &= ~(1u << e);
    }
}

int UacRouteSwitchBegin(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int n, const UAC_BYTE *sel,
                        const UAC_BYTE *target)
{
    unsigned int i, moves = 0;
    for (i = 0; i < n; i++) {
        const UAC_ROUTE_ENTRY *x = &t->Entry[sel[i]];
        if (x->Position == target[i]) continue;
        moves++;
        /* Any reader (e's own stream open or opening included) or any
         * writer refuses the whole switch. */
        if (x->Readers || x->ReaderMask || x->Writer >= 0) return UAC_ROUTE_BUSY;
    }
    if (!moves) return UAC_ROUTE_NOTHING;
    for (i = 0; i < n; i++)
        if (t->Entry[sel[i]].Position != target[i]) {
            t->Entry[sel[i]].Writer = (int)e;
            t->Entry[sel[i]].Kind = UAC_WRITER_SWITCH;
        }
    return UAC_ROUTE_OK;
}

void UacRoutePublish(UAC_ROUTE_TABLE *t, unsigned int sel, unsigned int position)
{
    if (sel >= t->Count) return;
    if (t->Entry[sel].Position != position) { t->Entry[sel].Position = position; t->Version++; }
}

void UacRouteSwitchEnd(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int n, const UAC_BYTE *sel,
                       const UAC_BYTE *readback)
{
    unsigned int i;
    for (i = 0; i < n; i++) UacRoutePublish(t, sel[i], readback[i]);
    for (i = 0; i < t->Count; i++)
        if (t->Entry[i].Writer == (int)e && t->Entry[i].Kind == UAC_WRITER_SWITCH) {
            t->Entry[i].Writer = -1; t->Entry[i].Kind = 0;
        }
}

int UacRouteAutoBegin(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int c, unsigned int kind)
{
    UAC_ROUTE_ENTRY *x;
    if (c >= t->Count) return UAC_ROUTE_BUSY;
    x = &t->Entry[c];
    if (x->ReaderMask & ~(1u << e)) return UAC_ROUTE_SKIP;
    if (x->Writer >= 0) return UAC_ROUTE_BUSY;
    x->Writer = (int)e;
    x->Kind = kind;
    return UAC_ROUTE_OK;
}

void UacRouteAutoEnd(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int c)
{
    if (c < t->Count && t->Entry[c].Writer == (int)e) { t->Entry[c].Writer = -1; t->Entry[c].Kind = 0; }
}

int UacRouteWriting(const UAC_ROUTE_TABLE *t, const UAC_ROUTE_SET *set)
{
    unsigned int k;
    for (k = 0; k < set->Count; k++) if (t->Entry[set->Index[k]].Writer >= 0) return 1;
    return 0;
}
