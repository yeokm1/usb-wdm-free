/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host test: the shipped wave and topology link tables meet the rules of
 * uactopo.h, and each rule rejects the graph it names.
 */
#include <stdio.h>
#include "uactopo.h"

static int failures;

static void Expect(const char *name, int got, int want)
{
    printf("%s %s (got %d, want %d)\n", got == want ? "PASS" : "FAIL", name, got, want);
    if (got != want) failures++;
}

int main(void)
{
    /* The topology filter 98 SE's SysAudio faulted on: pin 0 -> pin 1. */
    static const UAC_LINK pinToPin[1] = { { UAC_FILTER_NODE, 0, UAC_FILTER_NODE, 1 } };
    static const UAC_LINK badNode[2] = {
        { UAC_FILTER_NODE, 0, 1, UAC_NODE_IN }, { 1, UAC_NODE_OUT, UAC_FILTER_NODE, 1 } };
    static const UAC_LINK badPin[2] = {
        { UAC_FILTER_NODE, 2, 0, UAC_NODE_IN }, { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 1 } };
    static const UAC_LINK badNodePin[2] = {
        { UAC_FILTER_NODE, 0, 0, UAC_NODE_OUT }, { 0, UAC_NODE_OUT, UAC_FILTER_NODE, 1 } };
    static const UAC_LINK deadEnd[1] = { { UAC_FILTER_NODE, 0, 0, UAC_NODE_IN } };

    Expect("wave links", UacLinksCheck(UacWaveLinks, UAC_WAVE_LINKS, UAC_WAVE_NODES, UAC_WAVE_PINS), UAC_TOPO_OK);
    Expect("topology links", UacLinksCheck(UacTopoLinks, UAC_TOPO_LINKS, UAC_TOPO_NODES, UAC_TOPO_PINS), UAC_TOPO_OK);
    Expect("pin to pin (98 SE SysAudio fault)", UacLinksCheck(pinToPin, 1, 0, 2), UAC_TOPO_PIN_TO_PIN);
    Expect("node out of range", UacLinksCheck(badNode, 2, 1, 2), UAC_TOPO_BAD_NODE);
    Expect("filter pin out of range", UacLinksCheck(badPin, 2, 1, 2), UAC_TOPO_BAD_PIN);
    Expect("node input on its output pin", UacLinksCheck(badNodePin, 2, 1, 2), UAC_TOPO_BAD_PIN);
    Expect("node without an output", UacLinksCheck(deadEnd, 1, 1, 2), UAC_TOPO_UNLINKED);
    Expect("no links, no nodes", UacLinksCheck(0, 0, 0, 2), UAC_TOPO_OK);
    {
        int r, c;
        for (r = 0; r < 2; r++) for (c = 0; c < 2; c++) {
            const UAC_GRAPH *g = UacGraphFor(r, c);
            char name[64];
            if (!r && !c) { Expect("no direction, no graph", g == 0, 1); continue; }
            sprintf(name, "graph render=%d capture=%d wave", r, c);
            Expect(name, UacLinksCheck(g->WaveLinks, g->WaveLinkCount, g->WaveNodes, g->WavePins), UAC_TOPO_OK);
            sprintf(name, "graph render=%d capture=%d topology", r, c);
            Expect(name, UacLinksCheck(g->TopoLinks, g->TopoLinkCount, g->TopoNodes, g->TopoPins), UAC_TOPO_OK);
            sprintf(name, "graph render=%d capture=%d pins", r, c);
            /* The topology filter always carries the render line (98 SE). */
            Expect(name, g->TopoRenderIn >= 0 && g->TopoSpeaker >= 0 &&
                         (r || c ? g->RenderPin >= 0 && g->RenderBridge >= 0 && g->Dac >= 0
                                 : g->RenderPin < 0 && g->Dac < 0) &&
                         (c ? g->CapturePin >= 0 && g->CaptureBridge >= 0 && g->TopoMic >= 0 && g->TopoCaptureOut >= 0 && g->Adc >= 0
                            : g->CapturePin < 0 && g->Adc < 0), 1);
            /* VOLUME then MUTE on the render line, between its SUM node and
             * the speaker pin; none on a capture-only device's stub line. */
            sprintf(name, "graph render=%d capture=%d volume and mute", r, c);
            if (!r) Expect(name, g->Volume < 0 && g->Mute < 0, 1);
            else {
                unsigned int i, path = 0;
                for (i = 0; i < g->TopoLinkCount; i++) {
                    const UAC_LINK *l = &g->TopoLinks[i];
                    if (l->FromNode == UAC_FILTER_NODE && l->FromPin == (unsigned long)g->TopoRenderIn && l->ToNode == 0) path |= 1;
                    if (l->FromNode == 0 && l->ToNode == (unsigned long)g->Volume) path |= 2;
                    if (l->FromNode == (unsigned long)g->Volume && l->ToNode == (unsigned long)g->Mute) path |= 4;
                    if (l->FromNode == (unsigned long)g->Mute && l->ToNode == UAC_FILTER_NODE &&
                        l->ToPin == (unsigned long)g->TopoSpeaker) path |= 8;
                }
                Expect(name, g->Volume > 0 && g->Mute > 0 && (unsigned int)g->Mute < g->TopoNodes && path == 15, 1);
            }
        }
    }
    printf("%s topotest\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
