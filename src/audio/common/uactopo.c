/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The KS filter graphs this driver offers, and their checks (uactopo.h).
 * Original code from the 2000 DDK's PortCls documentation of
 * PCCONNECTION_DESCRIPTOR and PCFILTER_NODE.
 */
#include "uactopo.h"

const UAC_LINK UacWaveLinks[UAC_WAVE_LINKS] = {
    { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN },  /* render pin -> DAC */
    { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 1 }  /* DAC -> bridge pin */
};

const UAC_LINK UacTopoLinks[UAC_TOPO_LINKS] = {
    { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN },  /* bridge in -> SUM */
    { 0, UAC_NODE_OUT, 1, UAC_NODE_IN },     /* SUM -> VOLUME */
    { 1, UAC_NODE_OUT, 2, UAC_NODE_IN },     /* VOLUME -> MUTE */
    { 2, UAC_NODE_OUT, UAC_FILTER_NODE, 1 }  /* MUTE -> speaker line */
};

/* Capture only: wave pin 0 capture (out), pin 1 bridge in; node 0 ADC. */
static const UAC_LINK CaptureWave[2] = {
    { UAC_FILTER_NODE, 1, 0, UAC_NODE_IN },  /* bridge in -> ADC */
    { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 0 }  /* ADC -> capture pin */
};
/* Both: wave pins 0 render, 1 render bridge, 2 capture, 3 capture bridge;
 * nodes 0 DAC, 1 ADC. Topology pins 0 render in, 1 speaker, 2 microphone
 * line in, 3 capture line out; nodes 0 and 1 SUM, 2 VOLUME, 3 MUTE (the
 * render line). A capture-only device keeps the render line without the
 * two controls (StubTopo). */
static const UAC_LINK BothWave[4] = {
    { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN }, { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 1 },
    { UAC_FILTER_NODE, 3, 1, UAC_NODE_IN }, { 1, UAC_NODE_OUT, UAC_FILTER_NODE, 2 }
};
static const UAC_LINK StubTopo[4] = {
    { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN }, { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 1 },
    { UAC_FILTER_NODE, 2, 1, UAC_NODE_IN }, { 1, UAC_NODE_OUT, UAC_FILTER_NODE, 3 }
};
static const UAC_LINK BothTopo[6] = {
    { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN }, { 0, UAC_NODE_OUT, 2, UAC_NODE_IN },
    { 2, UAC_NODE_OUT, 3, UAC_NODE_IN }, { 3, UAC_NODE_OUT, UAC_FILTER_NODE, 1 },
    { UAC_FILTER_NODE, 2, 1, UAC_NODE_IN }, { 1, UAC_NODE_OUT, UAC_FILTER_NODE, 3 }
};

static const UAC_GRAPH Graphs[3] = {
    { 2, 1, 2, UacWaveLinks, 2, 3, 4, UacTopoLinks,  0, 1, -1, -1,  0, 1, -1, -1,  0, -1,  1, 2 },
    /* Capture only: the full layout, its render pin a stub with no
     * instances (uac2wdm.cpp): 98 SE's WDMAud took a page fault
     * (wdmaud(07)+2F3C, 2026-10-23) on a wave filter with a capture pair
     * alone, with either topology. */
    { 4, 2, 4, BothWave,     4, 2, 4, StubTopo,      0, 1, 2, 3,   0, 1, 2, 3,    0, 1, -1, -1 },
    { 4, 2, 4, BothWave,     4, 4, 6, BothTopo,      0, 1, 2, 3,   0, 1, 2, 3,    0, 1,  2, 3 }
};

const UAC_GRAPH *UacGraphFor(int render, int capture)
{
    if (render && capture) return &Graphs[2];
    if (render) return &Graphs[0];
    if (capture) return &Graphs[1];
    return 0;
}

static int End(unsigned long node, unsigned long pin, unsigned int nodes, unsigned int pins)
{
    if (node == UAC_FILTER_NODE) return pin < pins ? UAC_TOPO_OK : UAC_TOPO_BAD_PIN;
    if (node >= nodes) return UAC_TOPO_BAD_NODE;
    return pin == UAC_NODE_IN || pin == UAC_NODE_OUT ? UAC_TOPO_OK : UAC_TOPO_BAD_PIN;
}

int UacLinksCheck(const UAC_LINK *links, unsigned int count,
                  unsigned int nodes, unsigned int pins)
{
    unsigned int i, n;
    int r;
    if (!links && count) return UAC_TOPO_BAD_NODE;
    for (i = 0; i < count; i++) {
        const UAC_LINK *l = &links[i];
        if ((r = End(l->FromNode, l->FromPin, nodes, pins)) != UAC_TOPO_OK) return r;
        if ((r = End(l->ToNode, l->ToPin, nodes, pins)) != UAC_TOPO_OK) return r;
        if (l->FromNode != UAC_FILTER_NODE && l->FromPin != UAC_NODE_OUT) return UAC_TOPO_BAD_PIN;
        if (l->ToNode != UAC_FILTER_NODE && l->ToPin != UAC_NODE_IN) return UAC_TOPO_BAD_PIN;
        if (l->FromNode == UAC_FILTER_NODE && l->ToNode == UAC_FILTER_NODE) return UAC_TOPO_PIN_TO_PIN;
    }
    for (n = 0; n < nodes; n++) {
        int in = 0, out = 0;
        for (i = 0; i < count; i++) {
            if (links[i].ToNode == n) in = 1;
            if (links[i].FromNode == n) out = 1;
        }
        if (!in || !out) return UAC_TOPO_UNLINKED;
    }
    return UAC_TOPO_OK;
}
