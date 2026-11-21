/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The capture graph, read only (uaccap.h). Original code from USB Audio
 * 2.0 section 4.7.2 (entity descriptors) and USB Audio 1.0 section 4.3.2,
 * and docs/design/uac2.md sections 28.3 to 28.5 and 28.12.
 */
#include "uaccap.h"

/* An entity's descriptor, bounds-checked against the configuration. */
static const UAC_BYTE *Entity(const UAC_DESC *d, unsigned int id)
{
    unsigned int off;
    if (!id || id > 255) return 0;
    off = d->Entity[id];
    if (!off || off + 4 > d->Length || off + d->Bytes[off] > d->Length || d->Bytes[off] < 4) return 0;
    return d->Bytes + off;
}

/* A Processing Unit's process-specific part, which starts at `tail` (just
 * after iProcessing). Every process type the specifications define has
 * one input pin; Up/Down-mix (1) and Dolby Prologic (2) append bNrModes
 * and that many modes, 4 bytes each in USB Audio 2.0 (ADC-2 4.7.2.11.1
 * and .2: 18 + 4 * m) and 2 bytes each in USB Audio 1.0 (ADC-1 4.3.2.6.1
 * and .2: 15 + n + 2 * m); the others append nothing (ADC-2 Stereo
 * Extender, 3: 17; ADC-1 3D-Stereo Extender, Reverberation, Chorus and
 * Dynamic Range Compressor, 3 to 6: 14 + n). A process type neither
 * table defines (PROCESS_UNDEFINED, 0, included) has no layout that can
 * be checked: malformed, as is a defined type with another pin count. */
static int ProcessTail(const UAC_BYTE *p, unsigned int tail)
{
    unsigned int n = p[0], type = (unsigned int)p[4] | ((unsigned int)p[5] << 8);
    if (p[6] != 1) return 0;
#ifdef UAC_V1
    if (type == 1 || type == 2) return n >= tail + 1 && n >= tail + 1 + 2 * (unsigned int)p[tail];
    return type >= 3 && type <= 6;
#else
    if (type == 1 || type == 2) return n >= tail + 1 && n >= tail + 1 + 4 * (unsigned int)p[tail];
    return type == 3;
#endif
}

typedef struct WALK {
    const UAC_DESC *Desc;
    UAC_CAP_GRAPH *Graph;
    UAC_CAP_ROUTE Stack;
    UAC_BYTE On[256];
    int Stop;
} WALK;

/* The number of logical channels in the cluster leaving entity `id`
 * (1 to 255); 0 when the descriptors define none; WIDTH_CUT when the
 * walk cuts a route at or above it anyway (a source naming no entity, an
 * entity 28.3's table does not list, one too short to read, a loop, a
 * chain deeper than a route), so the graph is incomplete whatever a
 * layout check here would say and the walk reports why. An Input
 * Terminal, a Mixer, a Processing or an Extension Unit states its own
 * (bNrChannels; none when 0); a Feature Unit, an SRC and an Effect Unit
 * pass their input's; a Selector Unit passes its inputs' when they all
 * agree (ADC-2 4.7.2.7 and ADC-1 4.3.2.4 require it) and defines none
 * otherwise. Each entity's answer is kept in the graph (Width), so a
 * shared input is read once. Only the bytes read are bounds-checked here:
 * the walk checks every entity it reaches in full. */
#define WIDTH_CUT 0x100u
#define WIDTH_NONE 0x200u
static unsigned int Width(WALK *w, unsigned int id, unsigned int depth)
{
    const UAC_BYTE *p = Entity(w->Desc, id);
    UAC_WORD *known;
    unsigned int n, k, i, c, v = WIDTH_CUT;
    if (!p) return WIDTH_CUT;
    known = &w->Graph->Width[id];
    if (*known) return *known == WIDTH_NONE ? 0 : *known;
    *known = WIDTH_CUT;                     /* met again before it is known: a loop */
    if (depth >= UAC_CAP_DEPTH) return WIDTH_CUT;
    n = p[0];
    switch (p[2]) {
#ifdef UAC_V1
    case 0x02: if (n > 7) v = p[7]; break;
    case 0x06: if (n > 4) v = Width(w, p[4], depth + 1); break;
    case 0x07: case 0x08:
#else
    case 0x02: if (n > 8) v = p[8]; break;
    case 0x06: case 0x0d: if (n > 4) v = Width(w, p[4], depth + 1); break;
    case 0x07: if (n > 6) v = Width(w, p[6], depth + 1); break;
    case 0x08: case 0x09:
#endif
        k = n > 6 ? p[6] : 0;
        if (k && n > 7 + k) v = p[7 + k];
        break;
    case 0x04:
        k = n > 4 ? p[4] : 0;
        if (k && n > 5 + k) v = p[5 + k];
        break;
    case 0x05:
        k = n > 4 ? p[4] : 0;
        if (!k || n < 5 + k) break;
        for (i = 0; i < k; i++) {
            c = Width(w, p[5 + i], depth + 1);
            if (c == WIDTH_CUT) { v = WIDTH_CUT; break; }
            if (!i) v = c;
            else if (c != v) v = 0;             /* inputs that differ: none */
        }
        break;
    }
    *known = (UAC_WORD)(v ? v : WIDTH_NONE);
    return v;
}

/* Whether a per-channel control array of `ch` logical channel entries
 * (beside the master's) matches the cluster entering from `source`: ch
 * must be that cluster's channel count, not merely fit bLength. A
 * Feature Unit (masterOnly) with no channel entries at all is accepted
 * whatever its cluster, as a tolerated nonconforming layout (both
 * specifications require every logical channel's entry): its master
 * bitmap sits whole where it sits for any cluster, and no channel's
 * controls are read from it (the recorded Logi headset and Brio 100
 * have such units on mono and stereo clusters). */
static int Matches(WALK *w, unsigned int source, unsigned int ch, int masterOnly)
{
    unsigned int c;
    if (masterOnly && !ch) return 1;
    c = Width(w, source, 0);
    return c == WIDTH_CUT || (c && c == ch);
}

/* A Mixer Unit's bmMixerControls length N (ADC-2 4.7.2.6, ADC-1 4.3.2.3):
 * one bit per (input channel, output channel), n the input channels of
 * every input pin together, m bNrChannels, rounded up to whole bytes.
 * -1 when an input's channel count is not defined, so N is not known; 0
 * when the walk cuts an input's route anyway. */
static int MixerBytes(WALK *w, const UAC_BYTE *p)
{
    unsigned int k = p[4], i, c, total = 0;
    for (i = 0; i < k; i++) {
        c = Width(w, p[5 + i], 0);
        if (!c) return -1;
        if (c == WIDTH_CUT) return 0;
        total += c;
    }
    return (int)((total * p[5 + k] + 7) / 8);
}

/* Class, source count and the offset of the first source id; 0 when the
 * entity's layout is not complete (malformed): every field its subtype
 * defines up to its last string index must lie inside bLength, and a
 * per-channel array must have one entry per channel of the cluster
 * entering it (Matches). USB Audio 2.0 (ADC-2 4.7.2): Input Terminal 17,
 * Output Terminal 12, Mixer 13 + p + N (N from MixerBytes), Selector
 * 7 + p, Feature 6 + (ch + 1) * 4, Effect 16 + ch * 4 (Tables 4-15 to
 * 4-19 state that bLength and iEffects at 15 + ch * 4, though their
 * fields laid out contiguously from bmaControls(1) at 11 imply
 * 12 + ch * 4: the tables contradict themselves, and the larger is
 * required as the conservative choice, so a device built to the shorter
 * reading is refused here; a compatibility limitation, not a finding
 * that it is invalid), each
 * with ch the entering cluster's channels, Processing 16 + p and its
 * process-specific part (ProcessTail), Extension 15 + p, Sampling Rate
 * Converter 8. USB Audio 1.0 (ADC-1 4.3.2): Input Terminal 12, Output
 * Terminal 9, Mixer 10 + p + N, Selector 6 + p, Feature 7 + (ch + 1) * n
 * (n = bControlSize >= 1), Processing 13 + p + n (n = bControlSize) and
 * its process-specific part, Extension 13 + p + n. */
static unsigned int Classify(WALK *w, const UAC_BYTE *p, unsigned int *count, unsigned int *first)
{
    unsigned int n = p[0], k;
    int mix;
    *count = 0; *first = 0;
    switch (p[2]) {
#ifdef UAC_V1
    case 0x02: return n >= 12 ? UAC_CAP_END : 0;
    case 0x04:
        if (n < 5) return 0;
        *first = 5; *count = k = p[4];
        if (!k || n < 10 + k || (mix = MixerBytes(w, p)) < 0 || n < 10 + k + (unsigned int)mix) return 0;
        return UAC_CAP_JOIN;
    case 0x05:
        if (n < 5) return 0;
        *first = 5; *count = k = p[4];
        if (!k || n < 6 + k) return 0;
        return k == 1 ? UAC_CAP_PASS : UAC_CAP_CHOICE;
    case 0x06:
        if (n < 8 || !p[5] || (n - 7) % p[5] || (n - 7) / p[5] < 1) return 0;
        if (!Matches(w, p[4], (n - 7) / p[5] - 1, 1)) return 0;
        *first = 4; *count = 1;
        return UAC_CAP_PASS;
    case 0x07: case 0x08:
        if (n < 7) return 0;
        *first = 7; *count = k = p[6];
        if (!k || n < 12 + k || n < 13 + k + p[11 + k]) return 0;
        if (p[2] == 0x07 && !ProcessTail(p, 13 + k + p[11 + k])) return 0;
        return k == 1 ? UAC_CAP_OPAQUE : UAC_CAP_OPAQUE_JOIN;
#else
    case 0x02: return n >= 17 ? UAC_CAP_END : 0;
    case 0x04:
        if (n < 5) return 0;
        *first = 5; *count = k = p[4];
        if (!k || n < 13 + k || (mix = MixerBytes(w, p)) < 0 || n < 13 + k + (unsigned int)mix) return 0;
        return UAC_CAP_JOIN;
    case 0x05:
        if (n < 5) return 0;
        *first = 5; *count = k = p[4];
        if (!k || n < 7 + k) return 0;
        return k == 1 ? UAC_CAP_PASS : UAC_CAP_CHOICE;
    case 0x06:
        if (n < 10 || (n - 6) % 4 || !Matches(w, p[4], (n - 6) / 4 - 1, 1)) return 0;
        *first = 4; *count = 1;
        return UAC_CAP_PASS;
    case 0x0d:
        if (n < 8) return 0;
        *first = 4; *count = 1;
        return UAC_CAP_PASS;
    case 0x07:
        if (n < 16 || (n - 16) % 4 || !Matches(w, p[6], (n - 16) / 4, 0)) return 0;
        *first = 6; *count = 1;
        return UAC_CAP_PASS;
    case 0x08: case 0x09:
        if (n < 7) return 0;
        *first = 7; *count = k = p[6];
        if (!k || n < (p[2] == 0x08 ? 16 + k : 15 + k)) return 0;
        if (p[2] == 0x08 && !ProcessTail(p, 16 + k)) return 0;
        return k == 1 ? UAC_CAP_OPAQUE : UAC_CAP_OPAQUE_JOIN;
#endif
    default:
        return 0xFF;    /* not in 28.3's table (a clock entity, an Output Terminal, ...) */
    }
}

static void Keep(WALK *w, unsigned int length, unsigned int cut)
{
    UAC_CAP_GRAPH *g = w->Graph;
    unsigned int i;
    if (cut) g->Flags |= cut;
    if (g->RouteCount >= UAC_CAP_ROUTES) {
        g->Flags |= UAC_CAP_MANY_ROUTES;
        w->Stop = 1;
        return;
    }
    g->Route[g->RouteCount].Length = length;
    g->Route[g->RouteCount].Cut = cut;
    for (i = 0; i < UAC_CAP_DEPTH; i++) {
        g->Route[g->RouteCount].Id[i] = (UAC_BYTE)(i < length ? w->Stack.Id[i] : 0);
        g->Route[g->RouteCount].Pin[i] = (UAC_BYTE)(i + 1 < length ? w->Stack.Pin[i] : 0);
        g->Route[g->RouteCount].Class[i] = (UAC_BYTE)(i < length ? w->Stack.Class[i] : 0);
    }
    g->RouteCount++;
}

/* Depth-first with a per-route stack: a unit reached by two routes is
 * entered on each (a diamond keeps both). */
static void Enter(WALK *w, unsigned int id, unsigned int depth)
{
    const UAC_BYTE *p;
    unsigned int cls, count, first, i;
    if (w->Stop) return;
    if (depth >= UAC_CAP_DEPTH) { Keep(w, depth, UAC_CAP_TOO_DEEP); return; }
    w->Stack.Id[depth] = (UAC_BYTE)id;
    w->Stack.Pin[depth] = 0;
    w->Stack.Class[depth] = 0;
    p = Entity(w->Desc, id);
    if (!p) { Keep(w, depth + 1, UAC_CAP_MALFORMED); return; }
    if (w->On[id]) { Keep(w, depth + 1, UAC_CAP_LOOP); return; }
    cls = Classify(w, p, &count, &first);
    if (!cls) { Keep(w, depth + 1, UAC_CAP_MALFORMED); return; }
    if (cls == 0xFF) { Keep(w, depth + 1, UAC_CAP_UNLISTED); return; }
    w->Stack.Class[depth] = (UAC_BYTE)cls;
    if (cls == UAC_CAP_END) { Keep(w, depth + 1, 0); return; }
    w->On[id] = 1;
    for (i = 0; i < count && !w->Stop; i++) {
        w->Stack.Pin[depth] = (UAC_BYTE)(i + 1);
        Enter(w, p[first + i], depth + 1);
    }
    w->On[id] = 0;
}

/* The (selector, input) pairs route r meets before any join. -1 when more
 * than UAC_CAP_SELECTORS. */
static int Key(const UAC_CAP_ROUTE *r, UAC_BYTE *sel, UAC_BYTE *in)
{
    unsigned int i, n = 0;
    for (i = 1; i < r->Length; i++) {
        if (r->Class[i] == UAC_CAP_JOIN || r->Class[i] == UAC_CAP_OPAQUE_JOIN) break;
        if (r->Class[i] != UAC_CAP_CHOICE) continue;
        if (n >= UAC_CAP_SELECTORS) return -1;
        sel[n] = r->Id[i]; in[n] = r->Pin[i]; n++;
    }
    return (int)n;
}

static unsigned int TerminalType(const UAC_DESC *d, unsigned int id)
{
    const UAC_BYTE *p = Entity(d, id);
    return p ? (unsigned int)p[4] | ((unsigned int)p[5] << 8) : 0;
}

int UacCapBuild(const UAC_DESC *desc, unsigned int output, UAC_CAP_GRAPH *graph)
{
    WALK w;
    const UAC_BYTE *p;
    unsigned int i, j, k, r;
    UAC_BYTE sel[UAC_CAP_SELECTORS], in[UAC_CAP_SELECTORS];
    int n;
    if (!graph) return 0;
    for (i = 0; i < sizeof(*graph); i++) ((unsigned char *)graph)[i] = 0;
    graph->Output = (UAC_BYTE)output;
    p = desc && desc->Bytes && output && output <= 255 ? Entity(desc, output) : 0;
#ifdef UAC_V1
    if (!p || p[2] != 0x03 || p[0] < 9 || TerminalType(desc, output) != 0x0101) {
#else
    if (!p || p[2] != 0x03 || p[0] < 12 || TerminalType(desc, output) != 0x0101) {
#endif
        graph->Flags = UAC_CAP_MALFORMED;
        return 0;
    }
    for (i = 0; i < sizeof(w); i++) ((unsigned char *)&w)[i] = 0;
    w.Desc = desc; w.Graph = graph;
    w.Stack.Id[0] = (UAC_BYTE)output; w.Stack.Class[0] = UAC_CAP_OUTPUT; w.Stack.Pin[0] = 1;
    w.On[output] = 1;
    Enter(&w, p[7], 1);

    /* Every Choice on any route, in route order. */
    for (r = 0; r < graph->RouteCount; r++)
        for (i = 1; i < graph->Route[r].Length; i++) {
            if (graph->Route[r].Class[i] != UAC_CAP_CHOICE) continue;
            for (k = 0; k < graph->SelectorCount && graph->Selector[k] != graph->Route[r].Id[i]; k++) ;
            if (k == graph->SelectorCount && k < UAC_CAP_SELECTORS) graph->Selector[graph->SelectorCount++] = graph->Route[r].Id[i];
        }
    if (graph->Flags & UAC_CAP_INCOMPLETE) return 0;

    /* The chooser: the first multi-input entity from the Output Terminal,
     * if it is a Choice (a Join first: no chooser). */
    if (graph->RouteCount) {
        const UAC_CAP_ROUTE *r0 = &graph->Route[0];
        for (i = 1; i < r0->Length && (r0->Class[i] == UAC_CAP_PASS || r0->Class[i] == UAC_CAP_OPAQUE); i++) ;
        if (i < r0->Length && r0->Class[i] == UAC_CAP_CHOICE) graph->Chooser = r0->Id[i];
    }
    if (!graph->Chooser) return 1;
    /* Its sources: the distinct pairs before any join, in route order. */
    for (r = 0; r < graph->RouteCount; r++) {
        n = Key(&graph->Route[r], sel, in);
        if (n < 0) { graph->Flags |= UAC_CAP_MANY_SOURCES; graph->SourceCount = 0; graph->Chooser = 0; return 1; }
        for (k = 0; k < graph->SourceCount; k++) {
            const UAC_CAP_SOURCE *s = &graph->Source[k];
            if (s->Count != (unsigned int)n) continue;
            for (j = 0; j < (unsigned int)n && s->Selector[j] == sel[j] && s->Input[j] == in[j]; j++) ;
            if (j == (unsigned int)n) break;
        }
        if (k < graph->SourceCount) continue;
        if (graph->SourceCount >= UAC_CAP_SOURCES) {
            graph->Flags |= UAC_CAP_MANY_SOURCES; graph->SourceCount = 0; graph->Chooser = 0;
            return 1;
        }
        graph->Source[k].Count = (unsigned int)n;
        for (j = 0; j < (unsigned int)n; j++) { graph->Source[k].Selector[j] = sel[j]; graph->Source[k].Input[j] = in[j]; }
        graph->Source[k].End = graph->Route[r].Id[graph->Route[r].Length - 1];
        graph->Source[k].Type = TerminalType(desc, graph->Source[k].End);
        graph->SourceCount++;
    }
    if (graph->SourceCount < 2) { graph->SourceCount = 0; graph->Chooser = 0; }
    return 1;
}

int UacCapInSource(const UAC_CAP_GRAPH *graph, unsigned int r, unsigned int s)
{
    UAC_BYTE sel[UAC_CAP_SELECTORS], in[UAC_CAP_SELECTORS];
    const UAC_CAP_SOURCE *src;
    unsigned int j;
    int n;
    if (!graph || r >= graph->RouteCount || graph->Route[r].Cut) return 0;
    if (!graph->SourceCount) return s == 1;
    if (!s || s > graph->SourceCount) return 0;
    src = &graph->Source[s - 1];
    n = Key(&graph->Route[r], sel, in);
    if (n < 0 || (unsigned int)n != src->Count) return 0;
    for (j = 0; j < src->Count; j++) if (src->Selector[j] != sel[j] || src->Input[j] != in[j]) return 0;
    return 1;
}

/* Claims. */
static int Same(const UAC_CAP_CONTROL *a, const UAC_CAP_CONTROL *b)
{
    return a->Entity == b->Entity && a->Selector == b->Selector && a->Channel == b->Channel;
}

void UacClaimInit(UAC_CLAIMS *t)
{
    t->Count = 0; t->Sealed = 0; t->Refused = 0;
}

int UacClaimHolder(const UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int *kind)
{
    unsigned int i;
    for (i = 0; i < t->Count; i++)
        if (Same(&t->Control[i], c)) { if (kind) *kind = t->Kind[i]; return t->Holder[i]; }
    return -1;
}

int UacClaimTake(UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int holder, unsigned int kind)
{
    if (UacClaimHolder(t, c, 0) >= 0) return 0;
    if (t->Sealed || t->Count >= UAC_CLAIM_MAX || holder > 255) { t->Refused++; return 0; }
    t->Control[t->Count] = *c;
    t->Holder[t->Count] = (UAC_BYTE)holder;
    t->Kind[t->Count] = (UAC_BYTE)kind;
    t->Count++;
    return 1;
}

int UacClaimMay(const UAC_CLAIMS *t, const UAC_CAP_CONTROL *c, unsigned int holder)
{
    int h = UacClaimHolder(t, c, 0);
    return h < 0 || (unsigned int)h == holder;
}

/* Feature Unit helpers (as uacvol.c reads them). */
static int UnitChannels(const UAC_BYTE *p)
{
#ifdef UAC_V1
    if (!p || p[2] != 0x06 || p[0] < 8 || !p[5] || p[5] > 4 || (p[0] - 7) % p[5] ||
        (p[0] - 7) / p[5] < 1) return -1;
    return (p[0] - 7) / p[5] - 1;
#else
    if (!p || p[2] != 0x06 || p[0] < 10 || (p[0] - 6) % 4) return -1;
    return (p[0] - 6) / 4 - 1;
#endif
}

static int Access(const UAC_BYTE *fu, unsigned int channel, unsigned int selector)
{
#ifdef UAC_V1
    unsigned int size = fu[5], bit = selector - 1;
    if (!selector || bit >= 8 * size) return 0;
    return (fu[6 + channel * size + bit / 8] >> (bit % 8)) & 1 ? 3 : 0;
#else
    return UacCtlAccess(fu + 5 + 4 * channel, 4, selector);
#endif
}

/* Whether the unit at index i of every route of source s is eligible,
 * as a master (perChannel 0) or per channel (1). */
static int Eligible(const UAC_CAP_GRAPH *g, unsigned int s, UAC_BYTE unit, int perChannel)
{
    unsigned int r, i, j, found;
    for (r = 0; r < g->RouteCount; r++) {
        const UAC_CAP_ROUTE *rt = &g->Route[r];
        if (!UacCapInSource(g, r, s)) continue;
        for (found = 0, j = 1; j < rt->Length; j++) if (rt->Id[j] == unit) { found = j; break; }
        if (!found) return 0;                       /* not on every route: a part only */
        for (i = 1; i < found; i++) {
            unsigned int cls = rt->Class[i];
            if (cls == UAC_CAP_OPAQUE_JOIN) return 0;
            /* Per channel: the unit's logical channel n must be the
             * Output Terminal's channel n. Only a one-input Pass entity
             * (Feature, SRC, Effect, a one-input Selector) is defined to
             * pass its input cluster unchanged; across anything else,
             * a multi-input Selector included (equal channel counts on
             * its inputs do not show the same channels in the same
             * order), the correspondence is not proved and per-channel
             * control is refused: software, or a master. */
            if (perChannel && cls != UAC_CAP_PASS) return 0;
        }
    }
    return 1;
}

static int Free(const UAC_CLAIMS *t, unsigned int holder, UAC_BYTE unit, unsigned int selector,
                unsigned int from, unsigned int to)
{
    UAC_CAP_CONTROL c;
    unsigned int ch;
    if (!t) return 1;
    c.Entity = unit; c.Selector = (UAC_BYTE)selector;
    for (ch = from; ch <= to; ch++) { c.Channel = (UAC_BYTE)ch; if (!UacClaimMay(t, &c, holder)) return 0; }
    return 1;
}

static void PickOne(const UAC_DESC *d, const UAC_CAP_GRAPH *g, unsigned int s, unsigned int channels,
                    const UAC_CLAIMS *t, unsigned int holder, unsigned int selector,
                    UAC_BYTE *unit, unsigned int *mode, unsigned int *unitChannels)
{
    const UAC_CAP_ROUTE *r0 = 0;
    unsigned int r, i, c, pass;
    *unit = 0; *mode = UAC_VOL_NONE; *unitChannels = 0;
    for (r = 0; r < g->RouteCount && !r0; r++) if (UacCapInSource(g, r, s)) r0 = &g->Route[r];
    if (!r0) return;
    for (pass = 0; pass < 2; pass++)
        for (i = 1; i + 1 < r0->Length; i++) {
            const UAC_BYTE *p = Entity(d, r0->Id[i]);
            int n = UnitChannels(p);
            if (n < 0) continue;
            if (pass == 0) {
                if (Access(p, 0, selector) != 3 || !Eligible(g, s, r0->Id[i], 0) ||
                    !Free(t, holder, r0->Id[i], selector, 0, 0)) continue;
                *unit = r0->Id[i]; *mode = UAC_VOL_MASTER;
                return;
            } else {
                int all = 1;
                if ((unsigned int)n < channels || n > UAC_VOL_CHANNELS) continue;
                for (c = 1; c <= (unsigned int)n; c++) if (Access(p, c, selector) != 3) all = 0;
                if (!all || !Eligible(g, s, r0->Id[i], 1) ||
                    !Free(t, holder, r0->Id[i], selector, 1, (unsigned int)n)) continue;
                *unit = r0->Id[i]; *mode = UAC_VOL_CHANNEL; *unitChannels = (unsigned int)n;
                return;
            }
        }
}

void UacCapPick(const UAC_DESC *desc, const UAC_CAP_GRAPH *graph, unsigned int s,
                unsigned int channels, const UAC_CLAIMS *t, unsigned int holder, UAC_CAP_LEVEL *out)
{
    out->VolumeUnit = out->MuteUnit = 0;
    out->VolumeMode = out->MuteMode = UAC_VOL_NONE;
    out->VolumeChannels = out->MuteChannels = 0;
    if (!desc || !graph || (graph->Flags & UAC_CAP_INCOMPLETE)) return;
    if (channels < 1) channels = 1;
    if (channels > UAC_VOL_CHANNELS) channels = UAC_VOL_CHANNELS;
    PickOne(desc, graph, s, channels, t, holder, 2, &out->VolumeUnit, &out->VolumeMode, &out->VolumeChannels);
    PickOne(desc, graph, s, channels, t, holder, 1, &out->MuteUnit, &out->MuteMode, &out->MuteChannels);
}

static void TakeLevel(UAC_CLAIMS *t, unsigned int holder, UAC_BYTE unit, unsigned int selector,
                      unsigned int mode, unsigned int channels)
{
    UAC_CAP_CONTROL c;
    unsigned int ch;
    if (mode == UAC_VOL_NONE) return;
    c.Entity = unit; c.Selector = (UAC_BYTE)selector;
    for (ch = mode == UAC_VOL_MASTER ? 0 : 1; ch <= (mode == UAC_VOL_MASTER ? 0 : channels); ch++) {
        c.Channel = (UAC_BYTE)ch;
        UacClaimTake(t, &c, holder, UAC_CLAIM_LEVEL);
    }
}

void UacCapClaims(const UAC_DESC *desc, const UAC_CAP_ENDPOINT *eps, unsigned int count, UAC_CLAIMS *t)
{
    unsigned int e, i, s, r, k, ch, sources;
    UAC_CAP_CONTROL c;
    UacClaimInit(t);
    for (e = 0; e < count; e++)                     /* 1. playback levels */
        if (!eps[e].Capture)
            for (i = 0; i < eps[e].LevelCount; i++) UacClaimTake(t, &eps[e].Level[i], e, UAC_CLAIM_LEVEL);
    for (e = 0; e < count; e++) {                   /* 2. capture selectors, then levels */
        const UAC_CAP_GRAPH *g = eps[e].Graph;
        if (!eps[e].Capture || !g || (g->Flags & UAC_CAP_INCOMPLETE)) continue;
        for (s = 0; s < g->SourceCount; s++)
            for (k = 0; k < g->Source[s].Count; k++) {
                c.Entity = g->Source[s].Selector[k]; c.Selector = 1; c.Channel = 0;
                UacClaimTake(t, &c, e, UAC_CLAIM_SELECTOR);
            }
        sources = g->SourceCount ? g->SourceCount : 1;
        for (s = 1; s <= sources; s++) {
            UAC_CAP_LEVEL lv;
            UacCapPick(desc, g, s, eps[e].Channels, t, e, &lv);
            TakeLevel(t, e, lv.VolumeUnit, 2, lv.VolumeMode, lv.VolumeChannels);
            TakeLevel(t, e, lv.MuteUnit, 1, lv.MuteMode, lv.MuteChannels);
        }
    }
    for (e = 0; e < count; e++)                     /* 3. playback normalisation */
        if (!eps[e].Capture)
            for (i = 0; i < eps[e].NormalCount; i++) UacClaimTake(t, &eps[e].Normal[i], e, UAC_CLAIM_NORMAL);
    for (e = 0; e < count; e++) {                   /* 4. pass reservations */
        const UAC_CAP_GRAPH *g = eps[e].Graph;
        if (!eps[e].Capture || !g || (g->Flags & UAC_CAP_INCOMPLETE)) continue;
        sources = g->SourceCount ? g->SourceCount : 1;
        for (s = 1; s <= sources; s++)
            for (r = 0; r < g->RouteCount; r++) {
                if (!UacCapInSource(g, r, s)) continue;
                for (i = 1; i < g->Route[r].Length; i++) {
                    const UAC_BYTE *p = Entity(desc, g->Route[r].Id[i]);
                    int n = UnitChannels(p);
                    unsigned int sel;
                    if (n < 0) continue;
                    for (sel = 1; sel <= 2; sel++)
                        for (ch = 0; ch <= (unsigned int)n && ch <= UAC_VOL_CHANNELS; ch++) {
                            if (Access(p, ch, sel) != 3) continue;
                            c.Entity = g->Route[r].Id[i]; c.Selector = (UAC_BYTE)sel; c.Channel = (UAC_BYTE)ch;
                            UacClaimTake(t, &c, e, UAC_CLAIM_RESERVED);
                        }
                }
            }
    }
    t->Sealed = 1;
}

int UacCapUnitChannels(const UAC_DESC *desc, unsigned int unit)
{
    return desc ? UnitChannels(Entity(desc, unit)) : -1;
}

int UacCapAccess(const UAC_DESC *desc, unsigned int unit, unsigned int channel, unsigned int selector)
{
    int n = UacCapUnitChannels(desc, unit);
    if (n < 0 || channel > (unsigned int)n) return 0;
    return Access(Entity(desc, unit), channel, selector);
}

unsigned int UacCapConfirmed(const UAC_CAP_GRAPH *graph, const UAC_BYTE *positions)
{
    unsigned int s, k;
    if (!graph || !positions) return 0;
    for (s = 0; s < graph->SourceCount; s++) {
        for (k = 0; k < graph->Source[s].Count; k++)
            if (positions[graph->Source[s].Selector[k]] != graph->Source[s].Input[k]) break;
        if (k == graph->Source[s].Count) return s + 1;
    }
    return 0;
}

static void Put(unsigned char *record, unsigned int offset, unsigned long v)
{
    record[offset] = (unsigned char)v; record[offset + 1] = (unsigned char)(v >> 8);
    record[offset + 2] = (unsigned char)(v >> 16); record[offset + 3] = (unsigned char)(v >> 24);
}

void UacCapDiag(const UAC_CAP_GRAPH *graph, const UAC_BYTE *positions, const long *status,
                unsigned char *record)
{
    unsigned int i, confirmed, state;
    if (!graph || !record) return;
    confirmed = UacCapConfirmed(graph, positions);
    if (graph->Flags & UAC_CAP_INCOMPLETE) state = 3;
    else if (graph->SourceCount && !confirmed) state = 1;
    else state = 0;
    Put(record, 232, graph->SourceCount);
    Put(record, 236, confirmed);
    Put(record, 244, state);
    Put(record, 252, graph->Flags);
    for (i = 0; i < UAC_CAP_SELECTORS; i++) {
        UAC_BYTE id = (UAC_BYTE)(i < graph->SelectorCount ? graph->Selector[i] : 0);
        Put(record, 256 + 12 * i, id);
        Put(record, 260 + 12 * i, id && positions ? positions[id] : 0);
        Put(record, 264 + 12 * i, id && status ? (unsigned long)status[id] : 0);
    }
}

/* Stage 1 (uaccap.h, design 28.18). */
int UacCapStart(const UAC_DESC *desc, unsigned int output, unsigned int channels,
                UAC_CAP_GRAPH *graph, unsigned char *record)
{
    unsigned int i;
    int complete;
    if (!record) return 0;
    for (i = 0; i < UAC_CAP_DIAG_SIZE; i++) record[i] = 0;
    Put(record, 0, channels >= 2 ? 2 : 1);
    Put(record, 4, output);
    for (i = 0; i < UAC_VOL_CHANNELS; i++) Put(record, 116 + 4 * i, UAC_VOL_UNITY);
    if (!graph) {
        Put(record, 244, 3);
        Put(record, 252, UAC_CAP_NOT_BUILT);
        return 0;
    }
    complete = UacCapBuild(desc, output, graph);
    UacCapDiag(graph, 0, 0, record);
    return complete;
}

void UacCapDiagName(unsigned int pair, unsigned short *name)
{
    static const char base[] = "Uac2VolMapC", hex[] = "0123456789ABCDEF";
    unsigned int i;
    for (i = 0; base[i]; i++) name[i] = (unsigned short)base[i];
    if (pair) {
        name[i++] = (unsigned short)hex[(pair >> 4) & 15];
        name[i++] = (unsigned short)hex[pair & 15];
    }
    name[i] = 0;
}
