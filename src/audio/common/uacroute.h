/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * The adapter's route table (docs/design/uac2.md section 28.7, "The route
 * lock"): one entry per hardware control of every capture graph, with the
 * open capture streams that read it (Readers), the endpoint whose write of
 * it is in progress and of which kind (Writer: Switch or Level), and, for
 * a selector, its last confirmed Position and the table's Version. Every
 * function here is one hold of RouteLock: the caller holds it around each
 * call and never across a wait or a USB request (the table itself takes
 * no lock, so the host tests drive it with a scheduler of their own). No
 * driver file calls this yet. No Windows types, allocation or floating
 * point.
 *
 * Contracts for the driver that will call it (design 28.7, "Integration
 * contracts"):
 * - Exhaustion is never ignored: UacRouteControl returning -1 or
 *   UacRouteFromGraph returning 0 (the endpoint is then left with no
 *   route, and an open would register an empty set) disables every
 *   selector switch and every automatic capture write on the adapter;
 *   they are refused or skipped, counted, so no stream records unprotected.
 * - One registered stream per endpoint: ReaderMask has one bit per
 *   endpoint, so a close clears it whatever else that endpoint holds. The
 *   capture pin is one-instance today; a second simultaneous registration
 *   of one endpoint needs a per-endpoint count first.
 * - Storage: a UAC_ROUTE_TABLE is about 64 KiB at the default sizes; it is
 *   allocated once from nonpaged pool at PnP start, at PASSIVE, before any
 *   spin lock is taken, and never placed on a kernel stack. Everything
 *   RouteLock guards (the table, the locked sets) and every function here
 *   is nonpaged; uaccap's discovery and claims run at PASSIVE.
 * The sizes below may be set smaller for the host model (routetest).
 */
#ifndef UACROUTE_H
#define UACROUTE_H
#include "uaccap.h"
#ifdef __cplusplus
extern "C" {
#endif

#ifndef UAC_ROUTE_CONTROLS
#define UAC_ROUTE_CONTROLS 128      /* hardware controls of every capture graph */
#endif
#ifndef UAC_ROUTE_ENDPOINTS
#define UAC_ROUTE_ENDPOINTS 8       /* endpoints (and readers) the table knows */
#endif
#ifndef UAC_ROUTE_ROUTES
#define UAC_ROUTE_ROUTES UAC_CAP_ROUTES /* routes per endpoint */
#endif
#define UAC_ROUTE_PER 48            /* controls on one route */

#define UAC_WRITER_SWITCH 1         /* a selector switch's reservation, its own level writes included */
#define UAC_WRITER_LEVEL 2          /* an automatic level write of the endpoint's own */

#define UAC_ROUTE_OK 0
#define UAC_ROUTE_WAIT 1            /* open: a writer it must wait for; nothing registered */
#define UAC_ROUTE_BUSY 2            /* switch or automatic write refused; nothing marked */
#define UAC_ROUTE_SKIP 3            /* automatic write: another endpoint reads the control */
#define UAC_ROUTE_NOTHING 4         /* switch: every selector already at its target */

typedef struct UAC_ROUTE_ENTRY {
    UAC_CAP_CONTROL Control;
    unsigned int Selector;          /* 1: a selector (Position below) */
    unsigned int Readers;           /* open streams whose locked graph holds it */
    unsigned int ReaderMask;        /* bit e: endpoint e is one of them */
    int Writer;                     /* -1 none, else the endpoint */
    unsigned int Kind;              /* UAC_WRITER_* while Writer >= 0 */
    unsigned int Position;          /* a selector's last confirmed input, 0 unknown */
} UAC_ROUTE_ENTRY;

/* One route of an endpoint's graph: the table indexes of every control on
 * it, and the (selector index, input) pairs it needs. */
typedef struct UAC_ROUTE_PATH {
    unsigned int Count, Needs;
    UAC_BYTE Control[UAC_ROUTE_PER];
    UAC_BYTE Sel[UAC_CAP_DEPTH], Pin[UAC_CAP_DEPTH];
} UAC_ROUTE_PATH;

typedef struct UAC_ROUTE_EP {
    unsigned int RouteCount;
    UAC_ROUTE_PATH Route[UAC_ROUTE_ROUTES];
} UAC_ROUTE_EP;

typedef struct UAC_ROUTE_TABLE {
    unsigned int Count, Endpoints;
    unsigned long Version;          /* increased by every change of any Position */
    UAC_ROUTE_ENTRY Entry[UAC_ROUTE_CONTROLS];
    UAC_ROUTE_EP Ep[UAC_ROUTE_ENDPOINTS];
} UAC_ROUTE_TABLE;

/* A set of controls (table indexes, ascending) and the Version it was
 * computed at: a stream's locked graph. */
typedef struct UAC_ROUTE_SET {
    unsigned int Count;
    unsigned long Version;
    UAC_BYTE Index[UAC_ROUTE_CONTROLS];
} UAC_ROUTE_SET;

void UacRouteInit(UAC_ROUTE_TABLE *t);
/* The entry of a control, added if new: its index, or -1 when full. */
int UacRouteControl(UAC_ROUTE_TABLE *t, const UAC_CAP_CONTROL *c, int selector);
/* Endpoint e's routes from its capture graph (every route, cut ones too:
 * a reader is protected on what it may carry): each Feature Unit's mute
 * and volume present on the master and each channel, each Choice's
 * selector, and the input each route takes. 1 on success, 0 when the
 * table, the endpoint's routes or a route is full (e is then left with no
 * route: the caller must not ignore it, see the contracts above). */
int UacRouteFromGraph(UAC_ROUTE_TABLE *t, unsigned int e, const UAC_DESC *desc, const UAC_CAP_GRAPH *g);

/* Endpoint e's selected graph from the table's positions: every route
 * whose selectors read its inputs, a selector at an unknown position
 * (0) admitting every input. */
void UacRouteGraph(const UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *set);
/* 1 when an open of e must wait on set: a control with a Switch writer of
 * any endpoint, e's own included, or a Level writer of another endpoint. */
int UacRouteBlocked(const UAC_ROUTE_TABLE *t, unsigned int e, const UAC_ROUTE_SET *set);
void UacRouteRegister(UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *set);
/* A capture stream's open, one hold: the graph computed from the table,
 * checked and registered (UAC_ROUTE_OK, *locked its locked graph), or
 * nothing registered (UAC_ROUTE_WAIT: wait for a Writer to clear, then
 * call again, which computes the graph again). */
int UacRouteOpen(UAC_ROUTE_TABLE *t, unsigned int e, UAC_ROUTE_SET *locked);
/* A close, or a create that failed: the locked graph given back. */
void UacRouteClose(UAC_ROUTE_TABLE *t, unsigned int e, const UAC_ROUTE_SET *locked);

/* A switch of endpoint e: sel[i] (table indexes) to target[i]. Refused
 * (UAC_ROUTE_BUSY, nothing marked) when a selector it would move has a
 * reader (another endpoint's, or e's own stream open or opening) or any
 * writer; UAC_ROUTE_NOTHING when none would move; else each selector that
 * moves gets e as its Switch writer (UAC_ROUTE_OK). */
int UacRouteSwitchBegin(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int n, const UAC_BYTE *sel,
                        const UAC_BYTE *target);
/* The switch has ended, confirmed or not: the positions read back
 * (readback[i] for sel[i], 0 unknown) published, e's Switch marks
 * cleared. */
void UacRouteSwitchEnd(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int n, const UAC_BYTE *sel,
                       const UAC_BYTE *readback);
/* A read of a selector by any endpoint: its position published. */
void UacRoutePublish(UAC_ROUTE_TABLE *t, unsigned int sel, unsigned int position);
/* An automatic write by endpoint e of control c (a table index): skipped
 * when another endpoint reads it, refused when another writer is set,
 * else e becomes its writer of the given kind. */
int UacRouteAutoBegin(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int c, unsigned int kind);
void UacRouteAutoEnd(UAC_ROUTE_TABLE *t, unsigned int e, unsigned int c);
/* 1 when any entry has a writer (the event an open waits on is set when
 * this turns 0 for its set; the model polls it). */
int UacRouteWriting(const UAC_ROUTE_TABLE *t, const UAC_ROUTE_SET *set);

#ifdef __cplusplus
}
#endif
#endif
