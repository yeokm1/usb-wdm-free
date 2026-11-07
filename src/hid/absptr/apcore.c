/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * absptr portable core (apcore.h; docs/design/absptr.md sections 5, 6).
 * Original code. The report descriptor rules are those of the Device
 * Class Definition for HID 1.11 (6.2.2 items, 8.4 report format), the
 * usages those of the HID Usage Tables 1.12; the PenPartner facts are
 * from Linux drivers/hid/wacom_wac.c and wacom_sys.c (design 5.2; facts
 * only). No division or multiplication wider than 32 bits, so nothing
 * from a C runtime is needed in the driver.
 */
#include "apcore.h"

/* Usages, as (page << 16) | id. */
#define U(page, id) (((AU32)(page) << 16) | (AU32)(id))
#define PG_DESKTOP 0x01
#define PG_BUTTON  0x09
#define PG_DIGI    0x0D

#define DT_DIGITIZER   U(PG_DIGI, 0x01)
#define DT_PEN         U(PG_DIGI, 0x02)
#define DT_TOUCHSCREEN U(PG_DIGI, 0x04)
#define DT_DEVCONFIG   U(PG_DIGI, 0x0E)
#define DT_STYLUS      U(PG_DIGI, 0x20)
#define DT_FINGER      U(PG_DIGI, 0x22)
#define DT_INRANGE     U(PG_DIGI, 0x32)
#define DT_TIP         U(PG_DIGI, 0x42)
#define DT_BARREL      U(PG_DIGI, 0x44)
#define DT_ERASER      U(PG_DIGI, 0x45)
#define DT_CONFIDENCE  U(PG_DIGI, 0x47)
#define DT_DEVMODE     U(PG_DIGI, 0x52)
#define DT_CONTACTID   U(PG_DIGI, 0x51)
#define DT_CONTACTCNT  U(PG_DIGI, 0x54)
#define DT_BARREL2     U(PG_DIGI, 0x5A)
#define GD_POINTER     U(PG_DESKTOP, 0x01)
#define GD_MOUSE       U(PG_DESKTOP, 0x02)
#define GD_X           U(PG_DESKTOP, 0x30)
#define GD_Y           U(PG_DESKTOP, 0x31)

#define TLC_OTHER  0
#define TLC_TOUCH  AP_KIND_TOUCH
#define TLC_PEN    AP_KIND_PEN
#define TLC_MOUSE  AP_KIND_MOUSE
#define TLC_CONFIG 9

typedef struct GLOBALS {
    AU32 Page;
    AS32 LogMin, LogMax;
    AU32 Size, Count;
    AU8 Id;
} GLOBALS;

typedef struct USAGE {
    AU32 Min, Max;      /* one usage: Min == Max */
} USAGE;

typedef struct WORK {
    GLOBALS G, Stack[AP_DEPTH_MAX];
    unsigned StackDepth;
    USAGE Usage[AP_USAGES_MAX];
    unsigned NumUsage;
    AU32 UsageMin;
    AU8 HaveUsageMin;
    unsigned Depth;
    AU16 Bits[3][256];          /* input, output, feature bits so far, per id */
    AU8 UsesIds;
    /* the top-level collection being read */
    unsigned Tlc;
    unsigned Contacts;
    AU8 InContact;
    unsigned ContactDepth;
    unsigned InSlot;            /* 1-based slot being read, 0 outside */
    unsigned SlotDepth;
    AU8 MouseRelative;          /* a relative X or Y in report MouseRelId */
    AU8 MouseRelId;
    AP_SOURCE Cand;
    /* the results */
    AP_SOURCE Digi, Mouse;
    AU8 HaveDigi, HaveMouse;
    AP_FIELD DevMode;
    AU8 HaveDevMode;
} WORK;

unsigned ApWorkSize(void)
{
    return sizeof(WORK);
}

static void Zero(void *p, unsigned n)
{
    AU8 *b = (AU8 *)p;
    while (n--)
        *b++ = 0;
}

static AU32 ItemU(const AU8 *d, unsigned size)
{
    AU32 v = 0;
    unsigned i;
    for (i = 0; i < size; i++)
        v |= (AU32)d[i] << (8 * i);
    return v;
}

static AS32 ItemS(const AU8 *d, unsigned size)
{
    AU32 v = ItemU(d, size);
    if (size && size < 4 && (v & ((AU32)1 << (8 * size - 1))))
        v |= ~(AU32)0 << (8 * size);
    return (AS32)v;
}

/* The usage of field i of a main item, from the local usage list; the
 * last usage repeats for the fields beyond it (HID 1.11 6.2.2.8). */
static AU32 UsageAt(const WORK *w, unsigned i)
{
    unsigned k;
    AU32 span;
    for (k = 0; k < w->NumUsage; k++) {
        span = w->Usage[k].Max - w->Usage[k].Min + 1;
        if (i < span)
            return w->Usage[k].Min + i;
        i -= span;
    }
    if (w->NumUsage)
        return w->Usage[w->NumUsage - 1].Max;
    return 0;
}

/* A usage given with one or two data bytes takes the current Usage Page;
 * a four-byte usage carries its own page (HID 1.11 6.2.2.8). */
static AU32 FullUsage(const WORK *w, AU32 v, unsigned size)
{
    if (size == 4)
        return v;
    return (w->G.Page << 16) | (v & 0xFFFF);
}

static void ClearLocals(WORK *w)
{
    w->NumUsage = 0;
    w->HaveUsageMin = 0;
}

static int AddUsage(WORK *w, AU32 min, AU32 max)
{
    if (w->NumUsage >= AP_USAGES_MAX || max < min)
        return AP_E_LIMIT;
    w->Usage[w->NumUsage].Min = min;
    w->Usage[w->NumUsage].Max = max;
    w->NumUsage++;
    return AP_OK;
}

static void Capture(AP_FIELD *f, const WORK *w, unsigned offset)
{
    if (f->Present)
        return;
    f->Present = 1;
    f->Size = (AU8)w->G.Size;
    f->BitOffset = (AU16)offset;
    f->ReportId = w->G.Id;
    f->LogMin = w->G.LogMin;
    f->LogMax = w->G.LogMax;
    f->Signed = (AU8)(w->G.LogMin < 0);
}

static void InputField(WORK *w, AU32 usage, unsigned offset, int relative)
{
    AP_SOURCE *c = &w->Cand;
    int allowed = w->Contacts == 0 || w->InContact;

    if (w->Tlc == TLC_TOUCH || w->Tlc == TLC_PEN) {
        if (w->Tlc == TLC_TOUCH && usage == DT_CONTACTCNT && !w->InSlot)
            Capture(&c->Count, w, offset);
        if (w->Tlc == TLC_TOUCH && w->InSlot) {
            AP_SLOT *s = &c->Slot[w->InSlot - 1];
            switch (usage) {
            case GD_X: if (!relative) Capture(&s->X, w, offset); break;
            case GD_Y: if (!relative) Capture(&s->Y, w, offset); break;
            case DT_TIP: Capture(&s->Tip, w, offset); break;
            case DT_CONTACTID: Capture(&s->Id, w, offset); break;
            case DT_CONFIDENCE: Capture(&s->Confidence, w, offset); break;
            default: break;
            }
        }
        if (!allowed)
            return;
        switch (usage) {
        case GD_X: if (!relative) Capture(&c->X, w, offset); break;
        case GD_Y: if (!relative) Capture(&c->Y, w, offset); break;
        case DT_TIP: Capture(&c->Tip, w, offset); break;
        case DT_INRANGE: Capture(&c->InRange, w, offset); break;
        case DT_BARREL: Capture(&c->Barrel, w, offset); break;
        case DT_BARREL2: Capture(&c->Barrel2, w, offset); break;
        case DT_ERASER: Capture(&c->Eraser, w, offset); break;
        case DT_CONFIDENCE: Capture(&c->Confidence, w, offset); break;
        default: break;
        }
    } else if (w->Tlc == TLC_MOUSE) {
        if (usage == GD_X || usage == GD_Y) {
            if (relative) {
                if (!w->MouseRelative) {
                    w->MouseRelative = 1;
                    w->MouseRelId = w->G.Id;
                }
            } else
                Capture(usage == GD_X ? &c->X : &c->Y, w, offset);
        } else if (usage >= U(PG_BUTTON, 1) && usage <= U(PG_BUTTON, 3)) {
            Capture(&c->Button[usage - U(PG_BUTTON, 1)], w, offset);
        }
    }
}

/* Field f is usable when it is absent or in the same report as X. */
static int SameReport(const AP_FIELD *f, AU8 id)
{
    return !f->Present || f->ReportId == id;
}

static void DropOther(AP_FIELD *f, AU8 id)
{
    if (!SameReport(f, id))
        Zero(f, sizeof(*f));
}

/* X and Y need a range to scale over (design 5.1). */
static int Ranged(const AP_FIELD *f)
{
    return f->Present && f->LogMax > f->LogMin;
}

/* The slots usable for multi-touch: contiguous from the first, each with
 * Tip, X and Y in the source's report (design 5.3). */
static void EndSlots(AP_SOURCE *c)
{
    unsigned i;
    c->NumSlots = 0;
    for (i = 0; i < AP_SLOTS; i++) {
        AP_SLOT *s = &c->Slot[i];
        DropOther(&s->Id, c->ReportId);
        DropOther(&s->Confidence, c->ReportId);
        if (!s->Tip.Present || !Ranged(&s->X) || !Ranged(&s->Y) || s->Tip.ReportId != c->ReportId ||
            s->X.ReportId != c->ReportId || s->Y.ReportId != c->ReportId)
            break;
        c->NumSlots = i + 1;
    }
    for (i = c->NumSlots; i < AP_SLOTS; i++)
        Zero(&c->Slot[i], sizeof(c->Slot[i]));
    DropOther(&c->Count, c->ReportId);
    if (c->NumSlots < 2 && !(c->NumSlots == 1 && c->Slot[0].Id.Present)) {
        /* One contact without an identifier: the single-touch path. */
        c->NumSlots = 0;
        Zero(&c->Slot[0], sizeof(c->Slot[0]));
        Zero(&c->Count, sizeof(c->Count));
    }
}

static void EndTlc(WORK *w)
{
    AP_SOURCE *c = &w->Cand;
    unsigned i;
    if (w->Tlc != TLC_TOUCH && w->Tlc != TLC_PEN && w->Tlc != TLC_MOUSE)
        return;
    if (!Ranged(&c->X) || !Ranged(&c->Y) || c->X.ReportId != c->Y.ReportId)
        return;
    c->Kind = w->Tlc;
    c->ReportId = c->X.ReportId;
    DropOther(&c->Tip, c->ReportId);
    DropOther(&c->InRange, c->ReportId);
    DropOther(&c->Barrel, c->ReportId);
    DropOther(&c->Barrel2, c->ReportId);
    DropOther(&c->Eraser, c->ReportId);
    DropOther(&c->Confidence, c->ReportId);
    for (i = 0; i < 3; i++)
        DropOther(&c->Button[i], c->ReportId);
    if (w->Tlc == TLC_TOUCH)
        EndSlots(c);
    if (w->Tlc == TLC_MOUSE) {
        /* A relative X or Y in the absolute pair's own report makes it a
         * relative mouse; one in another report does not (design 5.1). */
        if (!(w->MouseRelative && w->MouseRelId == c->ReportId) && !w->HaveMouse) {
            w->Mouse = *c;
            w->HaveMouse = 1;
        }
    } else if (c->Tip.Present || c->InRange.Present || c->Eraser.Present) {
        if (!w->HaveDigi) {
            w->Digi = *c;
            w->HaveDigi = 1;
        }
    }
}

static void StartTlc(WORK *w, AU32 usage)
{
    Zero(&w->Cand, sizeof(w->Cand));
    w->Contacts = 0;
    w->InContact = 0;
    w->ContactDepth = 0;
    w->InSlot = 0;
    w->SlotDepth = 0;
    w->MouseRelative = 0;
    switch (usage) {
    case DT_TOUCHSCREEN: w->Tlc = TLC_TOUCH; break;
    case DT_PEN: case DT_DIGITIZER: w->Tlc = TLC_PEN; break;
    case DT_DEVCONFIG: w->Tlc = TLC_CONFIG; break;
    case GD_MOUSE: case GD_POINTER: w->Tlc = TLC_MOUSE; break;
    default: w->Tlc = TLC_OTHER; break;
    }
}

static int MainItem(WORK *w, unsigned tag, AU32 data)
{
    unsigned type, i, offset;
    AU32 bits;
    switch (tag) {
    case 0x8:   /* Input */
    case 0x9:   /* Output */
    case 0xB:   /* Feature */
        type = tag == 0x8 ? 0 : tag == 0x9 ? 1 : 2;
        if (w->G.Size == 0 || w->G.Size > 32)
            return w->G.Count ? AP_E_LIMIT : AP_OK;
        bits = w->G.Size * w->G.Count;
        if (w->G.Count > AP_REPORT_MAX * 8 || (AU32)w->Bits[type][w->G.Id] + bits > (AP_REPORT_MAX - 1) * 8)
            return AP_E_LIMIT;
        offset = w->Bits[type][w->G.Id];
        if (w->Depth > 0 && !(data & 1) && (data & 2)) {
            /* Data, Variable: one usage per field. Arrays and constants
             * are skipped (design 5.4). */
            for (i = 0; i < w->G.Count; i++) {
                AU32 usage = UsageAt(w, i);
                unsigned at = offset + i * (unsigned)w->G.Size;
                if (type == 0)
                    InputField(w, usage, at, (int)(data & 4));
                else if (type == 2 && w->Tlc == TLC_CONFIG && usage == DT_DEVMODE && !w->HaveDevMode) {
                    Capture(&w->DevMode, w, at);
                    w->HaveDevMode = 1;
                }
            }
        }
        w->Bits[type][w->G.Id] = (AU16)(offset + bits);
        break;
    case 0xA:   /* Collection */
        if (w->Depth >= AP_DEPTH_MAX)
            return AP_E_NESTING;
        {
            AU32 usage = w->NumUsage ? w->Usage[0].Min : 0;
            if (w->Depth == 0)
                StartTlc(w, usage);
            else if ((usage == DT_FINGER || usage == DT_STYLUS) && (data == 0 || data == 2)) {
                w->Contacts++;
                if (w->Contacts == 1) {
                    w->InContact = 1;
                    w->ContactDepth = w->Depth + 1;
                }
                if (w->Contacts <= AP_SLOTS && !w->InSlot) {
                    w->InSlot = w->Contacts;
                    w->SlotDepth = w->Depth + 1;
                }
            }
        }
        w->Depth++;
        break;
    case 0xC:   /* End Collection */
        if (w->Depth == 0)
            return AP_E_BALANCE;
        if (w->InContact && w->Depth == w->ContactDepth)
            w->InContact = 0;
        if (w->InSlot && w->Depth == w->SlotDepth)
            w->InSlot = 0;
        w->Depth--;
        if (w->Depth == 0) {
            EndTlc(w);
            w->Tlc = TLC_OTHER;
        }
        break;
    default:
        break;
    }
    ClearLocals(w);
    return AP_OK;
}

static int GlobalItem(WORK *w, unsigned tag, const AU8 *d, unsigned size)
{
    AU32 u = ItemU(d, size);
    switch (tag) {
    case 0x0: w->G.Page = u & 0xFFFF; break;
    case 0x1: w->G.LogMin = ItemS(d, size); break;
    case 0x2:
        /* Signed only when the minimum is negative, as Linux hid-core's
         * parser reads it (a fact of its behaviour). */
        w->G.LogMax = w->G.LogMin < 0 ? ItemS(d, size) : (AS32)u;
        break;
    case 0x7: w->G.Size = u; break;
    case 0x8:
        if (u == 0 || u > 255)
            return AP_E_LIMIT;
        w->G.Id = (AU8)u;
        w->UsesIds = 1;
        break;
    case 0x9: w->G.Count = u; break;
    case 0xA:   /* Push */
        if (w->StackDepth >= AP_DEPTH_MAX)
            return AP_E_NESTING;
        w->Stack[w->StackDepth++] = w->G;
        break;
    case 0xB:   /* Pop */
        if (w->StackDepth == 0)
            return AP_E_BALANCE;
        w->G = w->Stack[--w->StackDepth];
        break;
    default:    /* physical extents, unit, exponent: not needed */
        break;
    }
    return AP_OK;
}

static int LocalItem(WORK *w, unsigned tag, const AU8 *d, unsigned size)
{
    AU32 v = FullUsage(w, ItemU(d, size), size);
    switch (tag) {
    case 0x0:
        return AddUsage(w, v, v);
    case 0x1:
        w->UsageMin = v;
        w->HaveUsageMin = 1;
        break;
    case 0x2:
        if (!w->HaveUsageMin)
            return AP_OK;
        w->HaveUsageMin = 0;
        return AddUsage(w, w->UsageMin, v);
    default:
        break;
    }
    return AP_OK;
}

static AU16 ReportBytes(const WORK *w, unsigned type, AU8 id)
{
    return (AU16)((w->Bits[type][id] + 7) / 8 + (w->UsesIds ? 1 : 0));
}

int ApParse(const AU8 *desc, unsigned len, void *work, AP_SOURCE *src)
{
    WORK *w = (WORK *)work;
    unsigned pos = 0;
    int r;

    Zero(w, sizeof(*w));
    Zero(src, sizeof(*src));
    if (len > AP_DESC_MAX)
        return AP_E_LIMIT;
    while (pos < len) {
        AU8 b = desc[pos];
        unsigned size, type, tag;
        if (b == 0xFE) {        /* long item: skipped */
            if (pos + 3 > len)
                return AP_E_SHORT;
            size = desc[pos + 1];
            if (pos + 3 + size > len)
                return AP_E_SHORT;
            pos += 3 + size;
            continue;
        }
        size = b & 3;
        if (size == 3)
            size = 4;
        type = (b >> 2) & 3;
        tag = b >> 4;
        if (pos + 1 + size > len)
            return AP_E_SHORT;
        if (type == 0)
            r = MainItem(w, tag, ItemU(desc + pos + 1, size));
        else if (type == 1)
            r = GlobalItem(w, tag, desc + pos + 1, size);
        else if (type == 2)
            r = LocalItem(w, tag, desc + pos + 1, size);
        else
            r = AP_OK;          /* reserved item type */
        if (r != AP_OK)
            return r;
        pos += 1 + size;
    }
    if (w->Depth != 0)
        return AP_E_BALANCE;
    if (w->HaveDigi)
        *src = w->Digi;
    else if (w->HaveMouse)
        *src = w->Mouse;
    else
        return AP_E_NOSOURCE;
    src->UsesIds = w->UsesIds;
    src->ReportBytes = ReportBytes(w, 0, src->ReportId);
    {
        unsigned id;
        for (id = 0; id < 256; id++)
            if (w->Bits[0][id]) {
                src->InBytes[id] = ReportBytes(w, 0, (AU8)id);
                if (src->InBytes[id] > src->MaxInBytes)
                    src->MaxInBytes = src->InBytes[id];
            }
    }
    if (w->HaveDevMode) {
        src->DevMode = w->DevMode;
        src->DevModeId = w->DevMode.ReportId;
        src->DevModeBytes = ReportBytes(w, 2, src->DevModeId);
    }
    return AP_OK;
}

void ApPenPartnerSource(AP_SOURCE *src)
{
    Zero(src, sizeof(*src));
    src->Kind = AP_KIND_PENPARTNER;
}

/* ---- translation ---- */

/* Bits off..off+size-1 of the report body (after the id), little-endian;
 * returns 0 and sets *ok to 0 when they run past the report. */
static AU32 GetBits(const AU8 *body, unsigned bytes, unsigned off, unsigned size, int *ok)
{
    AU32 v = 0;
    unsigned i;
    if (size == 0 || size > 32 || off + size > bytes * 8) {
        *ok = 0;
        return 0;
    }
    for (i = 0; i < size; i++) {
        unsigned bit = off + i;
        if (body[bit >> 3] & (1 << (bit & 7)))
            v |= (AU32)1 << i;
    }
    return v;
}

static AS32 FieldValue(const AP_FIELD *f, const AU8 *body, unsigned bytes, int *ok)
{
    AU32 v = GetBits(body, bytes, f->BitOffset, f->Size, ok);
    if (f->Signed && f->Size < 32 && (v & ((AU32)1 << (f->Size - 1))))
        v |= ~(AU32)0 << f->Size;
    return (AS32)v;
}

static int FieldOn(const AP_FIELD *f, const AU8 *body, unsigned bytes, int *ok)
{
    if (!f->Present)
        return 0;
    return FieldValue(f, body, bytes, ok) != 0;
}

/* v in f's logical range to 0..AP_SCALE_MAX, in 32-bit arithmetic. */
static AU16 Scale(const AP_FIELD *f, AS32 v)
{
    AU32 range, off;
    if (f->LogMax <= f->LogMin)
        return 0;
    if (v < f->LogMin)
        v = f->LogMin;
    if (v > f->LogMax)
        v = f->LogMax;
    range = (AU32)f->LogMax - (AU32)f->LogMin;
    off = (AU32)v - (AU32)f->LogMin;
    while (range > 65535) {
        range >>= 1;
        off >>= 1;
    }
    if (range == 0)
        return 0;
    return (AU16)(off * AP_SCALE_MAX / range);
}

static AU16 ScaleN(AU32 v, AU32 max)
{
    if (v > max)
        v = max;
    return (AU16)(v * AP_SCALE_MAX / max);
}

static int Changed(AP_STATE *st, AU16 x, AU16 y, AU8 buttons)
{
    if (st->X == x && st->Y == y && st->Buttons == buttons)
        return 0;
    st->X = x;
    st->Y = y;
    st->Buttons = buttons;
    return 1;
}

static int PenPartner(const AU8 *rep, unsigned len, AP_STATE *st)
{
    AU32 x, y;
    signed char press;
    AU8 status, buttons = 0;
    if (len < 7 || (rep[0] != 1 && rep[0] != 2))
        return 0;
    x = rep[1] | ((AU32)rep[2] << 8);
    y = rep[3] | ((AU32)rep[4] << 8);
    status = rep[5];
    press = (signed char)rep[6];
    if (rep[0] == 1) {
        /* Report 1: bit 80h of byte 5 is "in proximity"
         * (wacom_penpartner_irq, case 1). */
        if (!(status & 0x80))
            return Changed(st, st->X, st->Y, 0);
        if (press > -127)
            buttons |= AP_BTN_LEFT;
    } else {
        /* Report 2: the pen or eraser touches above -80 (case 2; the
         * eraser end acts as a tip, design 5.2). */
        if (press > -80)
            buttons |= AP_BTN_LEFT;
    }
    if (status & 0x40)
        buttons |= AP_BTN_RIGHT;
    return Changed(st, ScaleN(x, AP_PP_XMAX), ScaleN(y, AP_PP_YMAX), buttons);
}

/* Multi-touch (design 5.3): the pointer follows one contact, by its
 * Contact Identifier (by slot number without one), from its touch to its
 * release. Contact Count gives the valid slots of a report; a count of 0
 * continues a frame (hybrid mode) whose first report gave the total, or,
 * with no frame in progress, means no contact. A followed contact missing
 * from a whole frame is released at the next frame's start, in a report of
 * its own (the next report may then take another contact). The work is
 * done on a copy of the state, committed only when every field read was
 * inside the report. */
static int MultiTouch(const AP_SOURCE *src, const AU8 *body, unsigned bytes, AP_STATE *st)
{
    unsigned valid = src->NumSlots, i;
    int ok = 1, newFrame = 1;
    AP_STATE t = *st;

    if (src->Count.Present) {
        AU32 cc = (AU32)FieldValue(&src->Count, body, bytes, &ok);
        if (!ok)
            return 0;
        if (cc) {
            valid = cc < src->NumSlots ? (unsigned)cc : src->NumSlots;
            t.Remaining = (AU16)(cc > valid ? cc - valid : 0);
        } else if (t.Remaining) {
            newFrame = 0;
            valid = t.Remaining < src->NumSlots ? t.Remaining : src->NumSlots;
            t.Remaining = (AU16)(t.Remaining - valid);
        } else {
            if (!t.Tracking)
                return 0;
            t.Tracking = 0;
            t.Seen = 0;
            t.Buttons = 0;
            *st = t;
            return 1;
        }
    }
    if (newFrame) {
        if (t.Tracking && !t.Seen) {
            /* Released by omission in the frame before: report the
             * release alone, then read this frame (Seen stays for it). */
            t.Tracking = 0;
            t.Buttons = 0;
            t.Seen = 0;
            for (i = 0; i < valid; i++) {
                (void)FieldOn(&src->Slot[i].Tip, body, bytes, &ok);
                if (src->Slot[i].Id.Present)
                    (void)FieldValue(&src->Slot[i].Id, body, bytes, &ok);
            }
            if (!ok)
                return 0;
            *st = t;
            return 1;
        }
        t.Seen = 0;
    }
    for (i = 0; i < valid; i++) {
        const AP_SLOT *s = &src->Slot[i];
        AU32 id = s->Id.Present ? (AU32)FieldValue(&s->Id, body, bytes, &ok) : i;
        int tip = FieldOn(&s->Tip, body, bytes, &ok);
        if (s->Confidence.Present && !FieldOn(&s->Confidence, body, bytes, &ok))
            tip = 0;
        if (!ok)
            return 0;
        if (t.Tracking && id == t.TrackId) {
            t.Seen = 1;
            if (!tip) {
                t.Tracking = 0;
                t.Buttons = 0;
            } else {
                t.X = Scale(&s->X, FieldValue(&s->X, body, bytes, &ok));
                t.Y = Scale(&s->Y, FieldValue(&s->Y, body, bytes, &ok));
                t.Buttons = AP_BTN_LEFT;
            }
            break;
        }
        if (!t.Tracking && tip) {
            t.Tracking = 1;
            t.Seen = 1;
            t.TrackId = id;
            t.X = Scale(&s->X, FieldValue(&s->X, body, bytes, &ok));
            t.Y = Scale(&s->Y, FieldValue(&s->Y, body, bytes, &ok));
            t.Buttons = AP_BTN_LEFT;
            break;
        }
    }
    if (!ok)
        return 0;
    i = Changed(st, t.X, t.Y, t.Buttons);
    st->Tracking = t.Tracking;
    st->Seen = t.Seen;
    st->Remaining = t.Remaining;
    st->TrackId = t.TrackId;
    return (int)i;
}

unsigned ApReportLength(const AP_SOURCE *src, const AU8 *rep, unsigned avail)
{
    unsigned n;
    if (avail == 0)
        return 0;
    if (src->Kind == AP_KIND_PENPARTNER || src->MaxInBytes == 0)
        return avail;
    n = src->InBytes[src->UsesIds ? rep[0] : 0];
    return n;
}

int ApTranslate(const AP_SOURCE *src, const AU8 *rep, unsigned len, AP_STATE *st)
{
    const AU8 *body = rep;
    unsigned bytes = len;
    int ok = 1, tip, inr, move;
    AU8 buttons = 0;
    AU16 x, y;
    unsigned i;

    if (src->Kind == AP_KIND_PENPARTNER)
        return PenPartner(rep, len, st);
    if (src->Kind == AP_KIND_NONE || len == 0)
        return 0;
    if (src->UsesIds) {
        if (rep[0] != src->ReportId)
            return 0;
        body = rep + 1;
        bytes = len - 1;
    }
    if (src->Kind == AP_KIND_TOUCH && src->NumSlots)
        return MultiTouch(src, body, bytes, st);
    x = Scale(&src->X, FieldValue(&src->X, body, bytes, &ok));
    y = Scale(&src->Y, FieldValue(&src->Y, body, bytes, &ok));
    if (src->Kind == AP_KIND_MOUSE) {
        for (i = 0; i < 3; i++)
            if (FieldOn(&src->Button[i], body, bytes, &ok))
                buttons |= (AU8)(1 << i);
        if (!ok)
            return 0;
        return Changed(st, x, y, buttons);
    }
    tip = FieldOn(&src->Tip, body, bytes, &ok) || FieldOn(&src->Eraser, body, bytes, &ok);
    if (src->Confidence.Present && !FieldOn(&src->Confidence, body, bytes, &ok))
        tip = 0;
    inr = src->InRange.Present ? FieldOn(&src->InRange, body, bytes, &ok) : tip;
    if (!ok)
        return 0;
    move = src->Kind == AP_KIND_TOUCH ? tip : (inr || tip);
    if (!move)
        return Changed(st, st->X, st->Y, 0);
    if (tip)
        buttons |= AP_BTN_LEFT;
    if (src->Kind == AP_KIND_PEN) {
        if (FieldOn(&src->Barrel, body, bytes, &ok))
            buttons |= AP_BTN_RIGHT;
        if (FieldOn(&src->Barrel2, body, bytes, &ok))
            buttons |= AP_BTN_MIDDLE;
        if (!ok)
            return 0;
    }
    return Changed(st, x, y, buttons);
}

/* ---- the PenPartner mode switch and the Device Mode feature ---- */

unsigned ApPenPartnerModeData(AU8 *out, unsigned cap)
{
    if (cap < 2)
        return 0;
    out[0] = 0x02;      /* report id 2 */
    out[1] = 0x02;      /* the tablet mode */
    return 2;
}

int ApPenPartnerModeOk(const AU8 *answer, unsigned len)
{
    return len >= 2 && answer[1] == 0x02;
}

unsigned ApDevModeValue(const AP_SOURCE *src)
{
    const AP_FIELD *f = &src->DevMode;
    if (!f->Present || f->Size == 0 || f->Size > 32 || src->DevModeBytes == 0)
        return 0;
    if (f->LogMin <= 2 && f->LogMax >= 2)
        return 2;
    if (f->LogMin <= 1 && f->LogMax >= 1)
        return 1;
    return 0;
}

int ApDevModeApply(const AP_SOURCE *src, AU8 *buf, unsigned len)
{
    const AP_FIELD *f = &src->DevMode;
    unsigned base = src->UsesIds ? 1 : 0, i, value = ApDevModeValue(src);
    if (!value || len < base || (unsigned)f->BitOffset + f->Size > (len - base) * 8)
        return 0;
    if (src->UsesIds && buf[0] != src->DevModeId)
        return 0;
    for (i = 0; i < f->Size; i++) {
        unsigned bit = f->BitOffset + i;
        AU8 mask = (AU8)(1 << (bit & 7));
        if (i < 2 && (value & (1U << i)))
            buf[base + (bit >> 3)] |= mask;
        else
            buf[base + (bit >> 3)] &= (AU8)~mask;
    }
    return 1;
}

/* ---- what Windows sees (design 6) ---- */

static const AU8 Desc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
    0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00,
    0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F, 0x75, 0x10,
    0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xC0, 0xC0
};

unsigned ApReportDescriptor(AU8 *out, unsigned cap)
{
    unsigned i;
    if (cap < sizeof(Desc))
        return 0;
    for (i = 0; i < sizeof(Desc); i++)
        out[i] = Desc[i];
    return sizeof(Desc);
}

unsigned ApBuildReport(const AP_STATE *st, AU8 *out, unsigned cap)
{
    if (cap < AP_REPORT_LEN)
        return 0;
    out[0] = (AU8)(st->Buttons & 7);
    out[1] = (AU8)(st->X & 0xFF);
    out[2] = (AU8)(st->X >> 8);
    out[3] = (AU8)(st->Y & 0xFF);
    out[4] = (AU8)(st->Y >> 8);
    out[5] = 0;
    return AP_REPORT_LEN;
}

void ApStateReset(AP_STATE *st)
{
    Zero(st, sizeof(*st));
    st->X = AP_SCALE_MAX / 2;
    st->Y = AP_SCALE_MAX / 2;
}
