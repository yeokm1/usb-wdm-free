/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The KS filter graphs this driver offers (wave and topology filters), as
 * plain tables, and the rules they must meet. No Windows types: the driver
 * hands the link tables to PortCls as PCCONNECTION_DESCRIPTOR arrays, which
 * have the same layout (four ULONGs: FromNode, FromNodePin, ToNode,
 * ToNodePin; 2000 DDK portcls.h).
 */
#ifndef UACTOPO_H
#define UACTOPO_H

#ifdef __cplusplus
extern "C" {
#endif

#define UAC_FILTER_NODE 0xFFFFFFFFUL /* PCFILTER_NODE: a pin of the filter */
#define UAC_NODE_IN 1UL              /* KSNODEPIN_STANDARD_IN */
#define UAC_NODE_OUT 0UL             /* KSNODEPIN_STANDARD_OUT */

typedef struct UAC_LINK {
    unsigned long FromNode, FromPin, ToNode, ToPin;
} UAC_LINK;

/* Wave filter: pin 0 render sink, pin 1 bridge out; node 0 the DAC. */
#define UAC_WAVE_PINS 2
#define UAC_WAVE_NODES 1
#define UAC_WAVE_LINKS 2
extern const UAC_LINK UacWaveLinks[UAC_WAVE_LINKS];

/* Topology filter: pin 0 bridge in (from the wave filter), pin 1 the
 * speaker line out; node 0 a SUM node between them. */
#define UAC_TOPO_PINS 2
#define UAC_TOPO_NODES 1
#define UAC_TOPO_LINKS 2
extern const UAC_LINK UacTopoLinks[UAC_TOPO_LINKS];

/* The graph for a device's directions (render, capture or both). Pin
 * numbers are -1 where the direction is absent. Render only is the two
 * tables above. Capture runs the other way: wave filter capture bridge in
 * -> ADC -> capture pin out; topology filter microphone line in -> SUM ->
 * line out to the wave filter. Each filter keeps a node between any two
 * of its pins (the 98 SE rule below). */
typedef struct UAC_GRAPH {
    unsigned int WavePins, WaveNodes, WaveLinkCount;
    const UAC_LINK *WaveLinks;
    unsigned int TopoPins, TopoNodes, TopoLinkCount;
    const UAC_LINK *TopoLinks;
    int RenderPin, RenderBridge, CapturePin, CaptureBridge;  /* wave filter */
    int TopoRenderIn, TopoSpeaker, TopoMic, TopoCaptureOut;  /* topology filter */
    int Dac, Adc;                                            /* wave nodes */
} UAC_GRAPH;

/* NULL when neither direction is present. */
const UAC_GRAPH *UacGraphFor(int render, int capture);

#define UAC_TOPO_OK 0
#define UAC_TOPO_BAD_NODE 1    /* node index out of range */
#define UAC_TOPO_BAD_PIN 2     /* filter pin out of range, or node pin not 0/1 */
#define UAC_TOPO_PIN_TO_PIN 3  /* a filter pin wired straight to a filter pin */
#define UAC_TOPO_UNLINKED 4    /* a node without both an input and an output */

/* Checks one filter's links. Rules: every node and pin index in range;
 * node pins are only the standard in (1) and out (0); no link joins two
 * filter pins with no node between them (Windows 98 SE's SysAudio took a
 * page fault on such a topology filter, 2026-10-22,
 * docs/testing/2026-10-22-uac2-98se.md); every node has an input and an
 * output link. */
int UacLinksCheck(const UAC_LINK *links, unsigned int count,
                  unsigned int nodes, unsigned int pins);

#ifdef __cplusplus
}
#endif
#endif
