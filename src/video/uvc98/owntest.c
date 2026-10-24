/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Actual ownership-core sequence tests. No class memory is supplied or
 * dereferenced, and no claim of kernel callback/concurrency proof is made.
 */
#include <assert.h>
#include <stdio.h>
#include "uvcown.h"

static int addresses[128];
static unsigned int randomState = 0x19a4b3c7;
static unsigned int Random(void)
{
    randomState = randomState * 1664525u + 1013904223u;
    return randomState;
}

static void Invariants(UVOWN_TABLE *table)
{
    unsigned int i,j,count=0,reads=0,controls=0;
    for(i=0;i<UVOWN_CAPACITY;i++) {
        UVOWN_ENTRY *entry=&table->Entry[i];
        if(!entry->Request) { assert(!entry->Serial && !entry->Kind && !entry->Phase && !entry->Cancelled); continue; }
        count++;
        assert(entry->Serial && entry->Serial<=table->Serial);
        assert(entry->Kind==UVOWN_READ || entry->Kind==UVOWN_CONTROL);
        assert(entry->Phase>=UVOWN_ADMITTING && entry->Phase<=UVOWN_WORKING);
        assert(entry->Cancelled<=1);
        if(entry->Kind==UVOWN_READ) reads++; else controls++;
        for(j=i+1;j<UVOWN_CAPACITY;j++) if(table->Entry[j].Request) {
            assert(table->Entry[j].Request!=entry->Request);
            assert(table->Entry[j].Serial!=entry->Serial);
        }
    }
    assert(count==table->Count && reads==table->Reads && controls==table->Controls);
    assert(count==reads+controls && count<=UVOWN_CAPACITY);
}

static void AdmissionCancel(void)
{
    unsigned int stage,kind,phase;
    for(stage=0;stage<4;stage++) {
        UVOWN_TABLE table;
        UVOWN_TOKEN token,found,invalid;
        UVOWN_ENTRY entry;
        UvOwnInitialize(&table);
        /* This address is intentionally not valid class storage. */
        assert(UvOwnCancel(&table,(const void *)1,&found,&kind,&phase)==UVOWN_NOT_FOUND);
        assert(UvOwnAdmit(&table,(const void *)1,UVOWN_READ,&token)==UVOWN_OK);
        assert(UvOwnBegin(&table,token)==UVOWN_BAD_PHASE);
        assert(UvOwnAdmit(&table,(const void *)1,UVOWN_CONTROL,&invalid)==UVOWN_DUPLICATE);
        if(stage==1) {
            assert(UvOwnCancel(&table,(const void *)1,&found,&kind,&phase)==UVOWN_OK);
            assert(kind==UVOWN_READ && phase==UVOWN_ADMITTING);
            assert(UvOwnQueue(&table,token)==UVOWN_CANCELLED);
            /* Admission return owns this completion even while PAUSED:
             * cancelled ADMITTING requests are not worker-selectable. */
            assert(UvOwnSnapshot(&table,token,&entry)==UVOWN_OK);
            assert(entry.Phase==UVOWN_ADMITTING && entry.Cancelled);
            assert(UvOwnNext(&table,UVOWN_READ,1,&invalid)==UVOWN_NOT_FOUND);
            /* Retire below is the direct admission-return handoff, before
             * any class notification, rather than a wait for worker/RUN. */
        } else {
            assert(UvOwnQueue(&table,token)==UVOWN_OK);
            assert(UvOwnQueue(&table,token)==UVOWN_BAD_PHASE);
            if(stage==2) {
                assert(UvOwnCancel(&table,(const void *)1,&found,&kind,&phase)==UVOWN_OK);
                assert(phase==UVOWN_QUEUED);
                assert(UvOwnBegin(&table,token)==UVOWN_CANCELLED);
            } else {
                assert(UvOwnBegin(&table,token)==UVOWN_OK);
                if(stage==3) {
                    assert(UvOwnCancel(&table,(const void *)1,&found,&kind,&phase)==UVOWN_OK);
                    assert(phase==UVOWN_WORKING);
                }
            }
        }
        assert(UvOwnRetire(&table,token,&entry)==UVOWN_OK);
        assert(entry.Request==(const void *)1 && entry.Cancelled==(unsigned int)(stage!=0));
        assert(!table.Count && !table.Reads && !table.Controls);
        /* Retire occurs BEFORE the hypothetical class notification. A late
         * cancel must only inspect the now-empty ownership table. */
        assert(UvOwnCancel(&table,(const void *)1,&found,&kind,&phase)==UVOWN_NOT_FOUND);
        assert(UvOwnRetire(&table,token,&entry)==UVOWN_NOT_FOUND);
        assert(UvOwnBegin(&table,token)==UVOWN_NOT_FOUND);
        Invariants(&table);
    }
}

static void CapacityGatesAndStaleTokens(void)
{
    UVOWN_TABLE table;
    UVOWN_TOKEN tokens[UVOWN_CAPACITY],newToken,found;
    UVOWN_ENTRY entry;
    unsigned int i;
    UvOwnInitialize(&table);
    for(i=0;i<UVOWN_CAPACITY;i++)
        assert(UvOwnAdmit(&table,&addresses[i],i&1 ? UVOWN_CONTROL:UVOWN_READ,&tokens[i])==UVOWN_OK);
    assert(table.Reads==16 && table.Controls==16);
    assert(UvOwnAdmit(&table,&addresses[64],UVOWN_READ,&newToken)==UVOWN_FULL);
    assert(!newToken.Serial && newToken.Slot==UVOWN_CAPACITY);
    assert(UvOwnAdmit(&table,&addresses[0],UVOWN_READ,&newToken)==UVOWN_DUPLICATE);
    assert(UvOwnGate(&table,UVOWN_READ,1)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[64],UVOWN_READ,&newToken)==UVOWN_CLOSED);
    for(i=0;i<UVOWN_CAPACITY;i+=2) assert(UvOwnRetire(&table,tokens[i],&entry)==UVOWN_OK);
    for(i=0;i<16;i++) {
        assert(UvOwnAdmit(&table,&addresses[64+i],UVOWN_CONTROL,&newToken)==UVOWN_OK);
        assert(UvOwnSnapshot(&table,tokens[i*2],&entry)==UVOWN_NOT_FOUND);
        assert(UvOwnRetire(&table,tokens[i*2],&entry)==UVOWN_NOT_FOUND);
    }
    assert(table.Count==32 && table.Controls==32 && !table.Reads);
    assert(UvOwnGate(&table,UVOWN_ALL,1)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[100],UVOWN_CONTROL,&newToken)==UVOWN_CLOSED);
    assert(UvOwnGate(&table,UVOWN_ALL,0)==UVOWN_OK);
    assert(UvOwnNext(&table,UVOWN_ALL,0,&found)==UVOWN_NOT_FOUND); /* all ADMITTING */
    Invariants(&table);
}

static void FifoAndPausedCancellation(void)
{
    UVOWN_TABLE table;
    UVOWN_TOKEN a,b,c,control,next;
    UVOWN_ENTRY entry;
    UvOwnInitialize(&table);
    assert(UvOwnAdmit(&table,&addresses[0],UVOWN_READ,&a)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[1],UVOWN_READ,&b)==UVOWN_OK);
    assert(UvOwnRetire(&table,a,&entry)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[0],UVOWN_READ,&c)==UVOWN_OK);
    assert(c.Slot==a.Slot && c.Serial!=a.Serial);
    assert(UvOwnQueue(&table,b)==UVOWN_OK && UvOwnQueue(&table,c)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[2],UVOWN_CONTROL,&control)==UVOWN_OK);
    assert(UvOwnQueue(&table,control)==UVOWN_OK);
    assert(UvOwnNext(&table,UVOWN_READ,0,&next)==UVOWN_OK && next.Serial==b.Serial);
    assert(UvOwnNext(&table,UVOWN_CONTROL,0,&next)==UVOWN_OK && next.Serial==control.Serial);
    assert(UvOwnCancelKinds(&table,UVOWN_READ)==2);
    assert(UvOwnCancelKinds(&table,UVOWN_READ)==0);
    /* No RUN state exists in this core: queued cancelled requests remain
     * selectable promptly while a caller's stream is paused or stopping. */
    assert(UvOwnNext(&table,UVOWN_READ,1,&next)==UVOWN_OK && next.Serial==b.Serial);
    assert(UvOwnBegin(&table,next)==UVOWN_CANCELLED);
    assert(UvOwnRetire(&table,next,&entry)==UVOWN_OK && entry.Cancelled);
    assert(UvOwnNext(&table,UVOWN_READ,1,&next)==UVOWN_OK && next.Serial==c.Serial);
    assert(UvOwnRetire(&table,next,&entry)==UVOWN_OK);
    assert(UvOwnNext(&table,UVOWN_READ,1,&next)==UVOWN_NOT_FOUND);
    assert(UvOwnBegin(&table,control)==UVOWN_OK);
    assert(UvOwnSnapshot(&table,control,&entry)==UVOWN_OK && !entry.Cancelled);
    assert(UvOwnRetire(&table,control,&entry)==UVOWN_OK);
    Invariants(&table);
}

static void ExhaustionAndInvalid(void)
{
    UVOWN_TABLE table;
    UVOWN_TOKEN token,next;
    UVOWN_ENTRY entry;
    UvOwnInitialize(&table);
    assert(UvOwnAdmit(&table,0,UVOWN_READ,&token)==UVOWN_INVALID);
    assert(UvOwnAdmit(&table,&addresses[0],UVOWN_ALL,&token)==UVOWN_INVALID);
    assert(UvOwnGate(&table,0,1)==UVOWN_INVALID);
    assert(UvOwnGate(&table,4,1)==UVOWN_INVALID);
    assert(UvOwnNext(&table,UVOWN_ALL,0,0)==UVOWN_INVALID);
    table.Serial=~0u-1;
    assert(UvOwnAdmit(&table,&addresses[0],UVOWN_READ,&token)==UVOWN_OK && token.Serial==~0u);
    assert(UvOwnQueue(&table,token)==UVOWN_OK);
    assert(UvOwnNext(&table,UVOWN_ALL,0,&next)==UVOWN_OK && next.Serial==token.Serial);
    assert(UvOwnRetire(&table,token,&entry)==UVOWN_OK);
    assert(UvOwnAdmit(&table,&addresses[1],UVOWN_CONTROL,&next)==UVOWN_EXHAUSTED);
    assert(!table.Count && table.Serial==~0u);
    Invariants(&table);
}

static void Stress(void)
{
    UVOWN_TABLE table;
    unsigned int iteration;
    UvOwnInitialize(&table);
    for(iteration=0;iteration<100000;iteration++) {
        UVOWN_TOKEN token,next;
        UVOWN_ENTRY entry;
        unsigned int action=Random()%7, index=Random()%UVOWN_CAPACITY;
        UVOWN_ENTRY *chosen=&table.Entry[index];
        token.Slot=index; token.Serial=chosen->Serial;
        if(action==0) UvOwnAdmit(&table,&addresses[Random()%128],Random()&1 ? UVOWN_READ:UVOWN_CONTROL,&next);
        else if(action==1) UvOwnQueue(&table,token);
        else if(action==2) UvOwnBegin(&table,token);
        else if(action==3 && chosen->Request) UvOwnCancel(&table,chosen->Request,0,0,0);
        else if(action==4) UvOwnRetire(&table,token,&entry);
        else if(action==5) UvOwnGate(&table,Random()&1 ? UVOWN_READ:UVOWN_CONTROL,Random()&1);
        else UvOwnCancelKinds(&table,UVOWN_ALL);
        Invariants(&table);
    }
}

int main(void)
{
    AdmissionCancel(); CapacityGatesAndStaleTokens(); FifoAndPausedCancellation();
    ExhaustionAndInvalid(); Stress();
    puts("PASS actual opaque ownership core: admission/cancel/retire, capacity/gates, FIFO/kinds, stale tokens, exhaustion,100000 sequence operations");
    return 0;
}
