/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
#include "uvcown.h"

typedef char UVOWN_UINT_MUST_BE_32_BITS[(sizeof(unsigned int) == 4) ? 1 : -1];

static int Kind(unsigned int kind)
{
    return kind == UVOWN_READ || kind == UVOWN_CONTROL;
}

static int Kinds(unsigned int kinds)
{
    return kinds && !(kinds & ~UVOWN_ALL);
}

static UVOWN_ENTRY *FindToken(UVOWN_TABLE *table, UVOWN_TOKEN token)
{
    UVOWN_ENTRY *entry;
    if (!table || token.Slot >= UVOWN_CAPACITY || !token.Serial) return 0;
    entry = &table->Entry[token.Slot];
    return entry->Request && entry->Serial == token.Serial ? entry : 0;
}

void UvOwnInitialize(UVOWN_TABLE *table)
{
    unsigned int i;
    if (!table) return;
    for (i = 0; i < UVOWN_CAPACITY; i++) {
        table->Entry[i].Request = 0;
        table->Entry[i].Serial = table->Entry[i].Kind = table->Entry[i].Phase = table->Entry[i].Cancelled = 0;
    }
    table->Serial = table->ClosedKinds = table->Count = table->Reads = table->Controls = 0;
}

int UvOwnAdmit(UVOWN_TABLE *table, const void *request, unsigned int kind,
               UVOWN_TOKEN *token)
{
    unsigned int i, freeSlot = UVOWN_CAPACITY;
    UVOWN_ENTRY *entry;
    if (!table || !request || !Kind(kind) || !token) return UVOWN_INVALID;
    token->Slot = UVOWN_CAPACITY; token->Serial = 0;
    for (i = 0; i < UVOWN_CAPACITY; i++) {
        if (table->Entry[i].Request == request) return UVOWN_DUPLICATE;
        if (!table->Entry[i].Request && freeSlot == UVOWN_CAPACITY) freeSlot = i;
    }
    if (table->ClosedKinds & kind) return UVOWN_CLOSED;
    if (freeSlot == UVOWN_CAPACITY) return UVOWN_FULL;
    if (table->Serial == ~0u) return UVOWN_EXHAUSTED;
    entry = &table->Entry[freeSlot];
    entry->Request = request;
    entry->Serial = ++table->Serial;
    entry->Kind = kind;
    entry->Phase = UVOWN_ADMITTING;
    entry->Cancelled = 0;
    table->Count++;
    if (kind == UVOWN_READ) table->Reads++; else table->Controls++;
    token->Slot = freeSlot; token->Serial = entry->Serial;
    return UVOWN_OK;
}

int UvOwnQueue(UVOWN_TABLE *table, UVOWN_TOKEN token)
{
    UVOWN_ENTRY *entry = FindToken(table, token);
    if (!entry) return UVOWN_NOT_FOUND;
    if (entry->Phase != UVOWN_ADMITTING) return UVOWN_BAD_PHASE;
    if (entry->Cancelled) return UVOWN_CANCELLED;
    entry->Phase = UVOWN_QUEUED;
    return UVOWN_OK;
}

int UvOwnBegin(UVOWN_TABLE *table, UVOWN_TOKEN token)
{
    UVOWN_ENTRY *entry = FindToken(table, token);
    if (!entry) return UVOWN_NOT_FOUND;
    if (entry->Phase != UVOWN_QUEUED) return UVOWN_BAD_PHASE;
    if (entry->Cancelled) return UVOWN_CANCELLED;
    entry->Phase = UVOWN_WORKING;
    return UVOWN_OK;
}

int UvOwnCancel(UVOWN_TABLE *table, const void *request, UVOWN_TOKEN *token,
                unsigned int *kind, unsigned int *phase)
{
    unsigned int i;
    if (!table || !request) return UVOWN_INVALID;
    if (token) { token->Slot = UVOWN_CAPACITY; token->Serial = 0; }
    if (kind) *kind = 0;
    if (phase) *phase = 0;
    for (i = 0; i < UVOWN_CAPACITY; i++) {
        UVOWN_ENTRY *entry = &table->Entry[i];
        if (entry->Request != request) continue;
        entry->Cancelled = 1;
        if (token) { token->Slot = i; token->Serial = entry->Serial; }
        if (kind) *kind = entry->Kind;
        if (phase) *phase = entry->Phase;
        return UVOWN_OK;
    }
    return UVOWN_NOT_FOUND;
}

unsigned int UvOwnCancelKinds(UVOWN_TABLE *table, unsigned int kinds)
{
    unsigned int i, changed = 0;
    if (!table || !Kinds(kinds)) return 0;
    for (i = 0; i < UVOWN_CAPACITY; i++) {
        UVOWN_ENTRY *entry = &table->Entry[i];
        if (entry->Request && (entry->Kind & kinds) && !entry->Cancelled) {
            entry->Cancelled = 1;
            changed++;
        }
    }
    return changed;
}

int UvOwnSnapshot(UVOWN_TABLE *table, UVOWN_TOKEN token, UVOWN_ENTRY *entry)
{
    UVOWN_ENTRY *found = FindToken(table, token);
    if (!entry) return UVOWN_INVALID;
    if (!found) return UVOWN_NOT_FOUND;
    *entry = *found;
    return UVOWN_OK;
}

int UvOwnNext(UVOWN_TABLE *table, unsigned int kinds, int cancelledOnly,
              UVOWN_TOKEN *token)
{
    unsigned int i, oldest = UVOWN_CAPACITY, serial = ~0u;
    if (!table || !Kinds(kinds) || !token) return UVOWN_INVALID;
    token->Slot = UVOWN_CAPACITY; token->Serial = 0;
    for (i = 0; i < UVOWN_CAPACITY; i++) {
        UVOWN_ENTRY *entry = &table->Entry[i];
        if (entry->Request && (entry->Kind & kinds) && entry->Phase == UVOWN_QUEUED &&
            (!cancelledOnly || entry->Cancelled) &&
            (oldest == UVOWN_CAPACITY || entry->Serial < serial)) {
            oldest = i;
            serial = entry->Serial;
        }
    }
    if (oldest == UVOWN_CAPACITY) return UVOWN_NOT_FOUND;
    token->Slot = oldest; token->Serial = serial;
    return UVOWN_OK;
}

int UvOwnRetire(UVOWN_TABLE *table, UVOWN_TOKEN token, UVOWN_ENTRY *entry)
{
    UVOWN_ENTRY *found = FindToken(table, token);
    if (!entry) return UVOWN_INVALID;
    if (!found) return UVOWN_NOT_FOUND;
    *entry = *found;
    table->Count--;
    if (found->Kind == UVOWN_READ) table->Reads--; else table->Controls--;
    found->Request = 0;
    found->Serial = found->Kind = found->Phase = found->Cancelled = 0;
    return UVOWN_OK;
}

int UvOwnGate(UVOWN_TABLE *table, unsigned int kinds, int closed)
{
    if (!table || !Kinds(kinds)) return UVOWN_INVALID;
    if (closed) table->ClosedKinds |= kinds;
    else table->ClosedKinds &= ~kinds;
    return UVOWN_OK;
}
