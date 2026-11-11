/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The USB Audio 1.0 function (uac1desc.c) as the shared model the driver
 * runs on (src/audio/common/uacdesc.h, built with UAC_V1). Original code
 * from ADC-1 3.7.2.3 (audio channel clusters) and Tables 4-3 to 4-15;
 * docs/design/uac2.md section 26. No Windows types or allocation.
 */
#include "uac1desc.h"
#include "uacdesc.h"
#include "uacconv.h"

static void Clear(void *p, unsigned int n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

/* Entity visits one layout lookup may make: a Selector Unit fans out, so
 * cycles and shared inputs are bounded by work, not only by depth. */
#define CLUSTER_BUDGET 256

static unsigned int Cluster(const UAC1_DESC *d, unsigned int id, unsigned int depth,
                            unsigned int *budget);

/* A Selector Unit's output layout: its inputs' when they all agree;
 * otherwise it depends on the input selected (ADC-1 4.3.2.4 allows equal
 * channel counts in different positions), and none is named. */
static unsigned int SelectorCluster(const UAC1_DESC *d, const unsigned char *p, unsigned int depth,
                                    unsigned int *budget)
{
    unsigned int i, pins = p[4], first, c;
    if (p[0] < 6U + pins || !pins) return 0;
    first = Cluster(d, p[5], depth + 1, budget);
    for (i = 1; i < pins && *budget; i++) {
        c = Cluster(d, p[5 + i], depth + 1, budget);
        if (c != first) return 0;
    }
    return *budget ? first : 0;
}

/* The wChannelConfig of the cluster entity `id` outputs (ADC-1 3.7.2.3):
 * an Input Terminal, Mixer, Processing or Extension Unit states it; a
 * Feature Unit passes its source's, a Selector Unit its inputs' when they
 * agree, an Output Terminal its source's. 0 when there is none, the inputs
 * differ, the chain is malformed or the work budget runs out. */
static unsigned int Cluster(const UAC1_DESC *d, unsigned int id, unsigned int depth,
                            unsigned int *budget)
{
    if (depth > 8) return 0;
    for (;;) {
        const unsigned char *p = Uac1Entity(d, id);
        unsigned int n, pins;
        if (!p || !*budget) return 0;
        (*budget)--;
        n = p[0];
        switch (p[2]) {
        case UAC1_INPUT_TERMINAL:
            return n >= 12 ? (unsigned int)p[8] | ((unsigned int)p[9] << 8) : 0;
        case UAC1_MIXER_UNIT:
            pins = p[4];
            return n >= 8U + pins ? (unsigned int)p[6 + pins] | ((unsigned int)p[7 + pins] << 8) : 0;
        case UAC1_PROCESSING_UNIT:
        case UAC1_EXTENSION_UNIT:
            pins = p[6];
            return n >= 10U + pins ? (unsigned int)p[8 + pins] | ((unsigned int)p[9 + pins] << 8) : 0;
        case UAC1_FEATURE_UNIT: id = p[4]; break;
        case UAC1_SELECTOR_UNIT: return SelectorCluster(d, p, depth, budget);
        case UAC1_OUTPUT_TERMINAL: id = n >= 9 ? p[7] : 0; break;
        default: return 0;
        }
    }
}

int Uac1Model(const UAC1_DESC *s, int highSpeed, struct UAC_DESC *d)
{
    unsigned int i, k, budget;
    if (!s || !d) return UAC1_BAD_DESCRIPTOR;
    Clear(d, sizeof(*d));
    d->Bytes = s->Bytes;
    d->Length = s->Length;
    d->Control = s->Control;
    d->First = s->Control;
    d->Count = (UAC_BYTE)(1 + s->CollectionCount);
    for (i = 0; i < 256; i++) d->Entity[i] = s->Entity[i];
    d->Skipped = s->Skipped;
    d->Speed = highSpeed ? 2 : 1;
    for (i = 0; i < s->AltCount; i++) {
        const UAC_FMT_ALT *a = &s->Alt[i];
        UAC_ALT *o;
        /* PCM, PCM8, IEEE_FLOAT, ALAW and MULAW (the last four converted
         * to and from 16-bit PCM, section 26.12); MaxPacketsOnly both ways. */
        if (!UacConvKnown(a->FormatTag, a->Subframe, a->Bits) || d->AltCount == UAC_MAX_ALTS) {
            d->Skipped++;
            continue;
        }
        o = &d->Alt[d->AltCount++];
        o->Interface = a->Interface; o->Alternate = a->Alternate; o->Terminal = a->Terminal;
        o->Channels = a->Channels; o->Subslot = a->Subframe; o->Bits = a->Bits;
        o->DataEndpoint = a->DataEndpoint; o->FeedbackEndpoint = a->SyncEndpoint;
        o->Attributes = a->Attributes; o->Interval = a->Interval;
        o->FeedbackInterval = (UAC_BYTE)(a->SyncEndpoint ? a->SyncInterval : 0);
        o->MaxPacket = a->MaxPacket;
        o->FeedbackMaxPacket = (UAC_WORD)(a->SyncEndpoint ? a->SyncMaxPacket : 0);
        budget = CLUSTER_BUDGET;
        o->ChannelConfig = Cluster(s, a->Terminal, 0, &budget) & 0xfffU;
        o->RateMin = (unsigned int)a->RateMin; o->RateMax = (unsigned int)a->RateMax;
        o->RateCount = a->RateCount > UAC1_RATES ? UAC1_RATES : a->RateCount;
        for (k = 0; k < o->RateCount; k++) o->Rate[k] = (unsigned int)a->Rate[k];
        o->EpControls = a->EpControls;
        o->FormatTag = (UAC_BYTE)a->FormatTag;
    }
    return d->AltCount ? UAC1_OK : UAC1_NO_AUDIO;
}
