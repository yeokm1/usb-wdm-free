/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded ownership bookkeeping, independent of Windows objects.
 * EVERY operation requires the caller's same external lock, including
 * lookup/snapshot/retire. The core never dereferences request addresses.
 * Output token/entry storage must be separate caller-owned memory and
 * must not alias any part of the table. Initialize only before first use
 * or after the engine has proved all external callbacks are drained.
 * This is not callback rundown or proof of a kernel driver's lifetime.
 */
#ifndef UVC_OWNERSHIP_H
#define UVC_OWNERSHIP_H
#define UVOWN_CAPACITY 32u

enum UVOWN_RESULT {
    UVOWN_OK = 0, UVOWN_INVALID, UVOWN_NOT_FOUND, UVOWN_DUPLICATE,
    UVOWN_CLOSED, UVOWN_FULL, UVOWN_CANCELLED, UVOWN_BAD_PHASE,
    UVOWN_EXHAUSTED
};
enum UVOWN_KIND { UVOWN_READ = 1, UVOWN_CONTROL = 2, UVOWN_ALL = 3 };
enum UVOWN_PHASE { UVOWN_ADMITTING = 1, UVOWN_QUEUED, UVOWN_WORKING };

typedef struct UVOWN_TOKEN { unsigned int Slot, Serial; } UVOWN_TOKEN;
typedef struct UVOWN_ENTRY {
    const void *Request;
    unsigned int Serial, Kind, Phase, Cancelled;
} UVOWN_ENTRY;
typedef struct UVOWN_TABLE {
    UVOWN_ENTRY Entry[UVOWN_CAPACITY];
    unsigned int Serial, ClosedKinds, Count, Reads, Controls;
} UVOWN_TABLE;

void UvOwnInitialize(UVOWN_TABLE *table);
/* Admission registers ownership BEFORE caller inspects/maps class storage.
 * A successful token owns the address until Retire, including ADMITTING.
 * Serial exhaustion fails closed; never wrap a generation onto an old token.
 */
int UvOwnAdmit(UVOWN_TABLE *table, const void *request, unsigned int kind,
               UVOWN_TOKEN *token);
/* CANCELLED leaves ADMITTING unchanged. After mapping/admission work ends,
 * that caller MUST retire and complete the cancelled request directly;
 * Next cannot select an ADMITTING entry, even with cancelledOnly set.
 * Never leave cancelled admission awaiting a future RUN transition.
 */
int UvOwnQueue(UVOWN_TABLE *table, UVOWN_TOKEN token);
int UvOwnBegin(UVOWN_TABLE *table, UVOWN_TOKEN token);
/* Cancel only searches driver-owned entries. Missing/retired addresses cause
 * no class-memory access. Caller must separately prevent stale callbacks
 * from applying to a newly reused address (callback rundown/ABA contract).
 */
int UvOwnCancel(UVOWN_TABLE *table, const void *request, UVOWN_TOKEN *token,
                unsigned int *kind, unsigned int *phase);
unsigned int UvOwnCancelKinds(UVOWN_TABLE *table, unsigned int kinds);
int UvOwnSnapshot(UVOWN_TABLE *table, UVOWN_TOKEN token, UVOWN_ENTRY *entry);
/* FIFO selection of registered QUEUED entries, without an external pointer
 * queue that could retain freed SRB extensions. cancelledOnly permits
 * paused/error cancellation completion without requiring a RUN state.
 */
int UvOwnNext(UVOWN_TABLE *table, unsigned int kinds, int cancelledOnly,
              UVOWN_TOKEN *token);
/* Copy the owned facts, REMOVE ownership under lock, release lock, and only
 * then notify the class. The token becomes invalid before notification.
 * Class-pointer/header/extension use after notification is forbidden.
 */
int UvOwnRetire(UVOWN_TABLE *table, UVOWN_TOKEN token, UVOWN_ENTRY *entry);
/* Read/control admission gates are independent. Closing does not drain or
 * retire existing entries. Reopening is an engine policy decision.
 */
int UvOwnGate(UVOWN_TABLE *table, unsigned int kinds, int closed);
#endif
