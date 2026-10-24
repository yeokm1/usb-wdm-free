/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
#include "uvcflow.h"
#include <string.h>
static int Same(UVOWN_TOKEN a, UVOWN_TOKEN b)
{ return a.Slot==b.Slot && a.Serial==b.Serial; }
static int Owned(UVFLOW *f,const void *request)
{
    unsigned int i;
    for(i=0;i<UVOWN_CAPACITY;i++) if(f->Own.Entry[i].Request==request) return 1;
    return 0;
}
static int Teardown(unsigned int command, unsigned int state)
{
    return command==UVFLOW_CLOSE || command==UVFLOW_POWER_DOWN ||
        command==UVFLOW_UNINITIALIZE || command==UVFLOW_FLUSH ||
        (command==UVFLOW_SET_STATE && state<UVFLOW_PAUSE);
}
static void Gate(UVFLOW *f)
{
    int closed=!f->Stream || f->State<UVFLOW_PAUSE || f->PowerBlocked ||
        f->Flushing || f->Closing || f->Removing || f->Faulted;
    UvOwnGate(&f->Own,UVOWN_READ,closed);
}
void UvFlowInitialize(UVFLOW *f)
{
    if(!f) return;
    memset(f,0,sizeof(*f)); UvOwnInitialize(&f->Own); Gate(f);
}
int UvFlowControl(UVFLOW *f,const void *request,const void *stream,
                  unsigned int command,unsigned int state)
{
    UVOWN_TOKEN token;
    unsigned int i,limit=UVFLOW_NORMAL_LIMIT;
    int result;
    if(!f || !request) return UVFLOW_INVALID;
    if(Owned(f,request)) return UVFLOW_DUPLICATE;
    if(command<UVFLOW_OPEN || command>UVFLOW_END_FLUSH ||
       (command==UVFLOW_SET_STATE && state>UVFLOW_RUN)) return UVFLOW_INVALID;
    if(f->Removing || f->FinalIssued) return UVFLOW_CLOSED;
    if(command==UVFLOW_CLOSE) limit=UVOWN_CAPACITY-1;
    else if(command==UVFLOW_UNINITIALIZE) limit=UVOWN_CAPACITY;
    else if(Teardown(command,state)) limit=UVOWN_CAPACITY-2;
    if(f->Own.Count>=limit) return UVFLOW_FULL;
    /* Reserve generation values as well as slots: final teardown must not
     * fail because ordinary work consumed the last representable serial. */
    if(f->Own.Serial>=~0u-(UVOWN_CAPACITY-limit)) return UVFLOW_EXHAUSTED;
    result=UvOwnAdmit(&f->Own,request,UVOWN_CONTROL,&token);
    if(result!=UVOWN_OK) return result==UVOWN_DUPLICATE?UVFLOW_DUPLICATE:UVFLOW_INVALID;
    memset(&f->Slot[token.Slot],0,sizeof(f->Slot[0]));
    f->Slot[token.Slot].Stream=stream;
    f->Slot[token.Slot].Command=command; f->Slot[token.Slot].State=state;
    UvOwnQueue(&f->Own,token);
    if(Teardown(command,state)) {
        f->Closing++;
        UvOwnCancelKinds(&f->Own,UVOWN_READ); Gate(f);
    }
    if(command==UVFLOW_UNINITIALIZE) {
        f->Removing=1; f->Uninitialize=token;
        UvOwnGate(&f->Own,UVOWN_ALL,1);
        for(i=0;i<UVOWN_CAPACITY;i++)
            if(f->Own.Entry[i].Request && i!=token.Slot)
                UvOwnCancel(&f->Own,f->Own.Entry[i].Request,0,0,0);
    }
    return UVFLOW_OK;
}
int UvFlowRead(UVFLOW *f,const void *request,const void *stream,UVOWN_TOKEN *token)
{
    int result;
    if(token) { token->Slot=UVOWN_CAPACITY; token->Serial=0; }
    if(!f || !token || !request) return UVFLOW_INVALID;
    if(Owned(f,request)) return UVFLOW_DUPLICATE;
    if(f->FinalIssued || stream!=f->Stream) return UVFLOW_CLOSED;
    if(f->Own.Count>=UVFLOW_NORMAL_LIMIT) return UVFLOW_FULL;
    if(f->Own.Serial>=~0u-(UVOWN_CAPACITY-UVFLOW_NORMAL_LIMIT)) return UVFLOW_EXHAUSTED;
    result=UvOwnAdmit(&f->Own,request,UVOWN_READ,token);
    if(result!=UVOWN_OK) return result==UVOWN_CLOSED?UVFLOW_CLOSED:
        (result==UVOWN_DUPLICATE?UVFLOW_DUPLICATE:UVFLOW_INVALID);
    memset(&f->Slot[token->Slot],0,sizeof(f->Slot[0]));
    f->Slot[token->Slot].Stream=stream;
    return UVFLOW_OK;
}
int UvFlowReady(UVFLOW *f,UVOWN_TOKEN token,int mapped)
{
    UVOWN_ENTRY entry;
    if(!f || UvOwnSnapshot(&f->Own,token,&entry)!=UVOWN_OK ||
       entry.Kind!=UVOWN_READ || entry.Phase!=UVOWN_ADMITTING ||
       f->Slot[token.Slot].AdmissionDone) return UVFLOW_INVALID;
    f->Slot[token.Slot].AdmissionDone=1;
    f->Slot[token.Slot].MappingFailed=!mapped;
    if(!mapped) UvOwnCancel(&f->Own,entry.Request,0,0,0);
    /* A cancelled admission remains ADMITTING. Next handles it explicitly
     * only after this mapping callback has published AdmissionDone. */
    UvOwnQueue(&f->Own,token);
    return UVFLOW_OK;
}
int UvFlowCancel(UVFLOW *f,const void *request)
{
    unsigned int i;
    if(!f || !request) return UVFLOW_INVALID;
    for(i=0;i<UVOWN_CAPACITY;i++)
        if(f->Own.Entry[i].Request==request &&
           f->Slot[i].Command==UVFLOW_UNINITIALIZE) return UVFLOW_BUSY;
    return UvOwnCancel(&f->Own,request,0,0,0)==UVOWN_OK?UVFLOW_OK:UVFLOW_CLOSED;
}
static int Work(UVFLOW *f,UVOWN_TOKEN token,unsigned int action,
                unsigned int now,UVFLOW_WORK *work)
{
    UVOWN_ENTRY entry;
    UVFLOW_SLOT *slot=&f->Slot[token.Slot];
    if(UvOwnSnapshot(&f->Own,token,&entry)!=UVOWN_OK) return UVFLOW_INVALID;
    if(entry.Phase==UVOWN_QUEUED && !entry.Cancelled)
        UvOwnBegin(&f->Own,token);
    work->Token=token; work->Request=entry.Request; work->Stream=slot->Stream;
    work->Action=action; work->Command=slot->Command; work->State=slot->State;
    work->Tick=now; work->Frame=f->Frame;
    if(action==UVFLOW_READ && !entry.Cancelled) {
        work->Frame=++f->Frame;
        f->Deadline=now+UVFLOW_INTERVAL_MS;
    }
    f->Active=token; f->Running=*work; f->Busy=1;
    return UVFLOW_OK;
}
int UvFlowNext(UVFLOW *f,unsigned int now,UVFLOW_WORK *work)
{
    unsigned int i;
    UVOWN_TOKEN token;
    if(!f || !work) return UVFLOW_INVALID;
    memset(work,0,sizeof(*work));
    if(f->FinalIssued || f->ExitIssued) return UVFLOW_CLOSED;
    if(f->Busy || f->Notifying) return UVFLOW_BUSY;
    /* Cancellation/mapping failures are serviced even in PAUSE/STOP. */
    for(i=0;i<UVOWN_CAPACITY;i++) {
        UVOWN_ENTRY *entry=&f->Own.Entry[i];
        if(entry->Request && entry->Kind==UVOWN_READ && entry->Cancelled &&
           f->Slot[i].AdmissionDone) {
            token.Slot=i; token.Serial=entry->Serial;
            return Work(f,token,UVFLOW_READ,now,work);
        }
    }
    if(UvOwnNext(&f->Own,UVOWN_CONTROL,0,&token)==UVOWN_OK) {
        UVFLOW_SLOT *slot=&f->Slot[token.Slot];
        if(Teardown(slot->Command,slot->State) && f->Own.Reads) return UVFLOW_OK;
        if(slot->Command==UVFLOW_UNINITIALIZE) {
            if(f->Own.Count!=1) return UVFLOW_OK;
            Work(f,token,UVFLOW_EXIT,now,work);
            f->ExitIssued=1;
            return UVFLOW_OK;
        }
        return Work(f,token,UVFLOW_CONTROL,now,work);
    }
    if(f->State==UVFLOW_RUN && f->Stream && !f->Closing && !f->Removing &&
       !f->PowerBlocked && !f->Flushing && !f->Faulted &&
       (unsigned int)(now-f->Deadline)<0x80000000u &&
       UvOwnNext(&f->Own,UVOWN_READ,0,&token)==UVOWN_OK)
        return Work(f,token,UVFLOW_READ,now,work);
    return UVFLOW_OK;
}
static unsigned int Apply(UVFLOW *f,UVFLOW_WORK work)
{
    unsigned int command=work.Command;
    if(command==UVFLOW_OPEN) {
        if(!work.Stream || f->Stream || f->PowerBlocked) return UVFLOW_NOT_READY;
        f->Stream=work.Stream; f->State=UVFLOW_STOP; f->Frame=0;
        f->Flushing=f->Faulted=0;
    } else if(command==UVFLOW_POWER_DOWN) {
        f->PowerBlocked=1; f->State=UVFLOW_STOP;
    } else if(command==UVFLOW_POWER_UP) f->PowerBlocked=0;
    else {
        if(!f->Stream || f->Stream!=work.Stream) return UVFLOW_NOT_READY;
        if(command==UVFLOW_CLOSE) {
            f->State=UVFLOW_STOP; f->Stream=0; f->Flushing=f->Faulted=0;
        }
        else if(command==UVFLOW_SET_STATE) {
            if(f->PowerBlocked) return UVFLOW_NOT_READY;
            if(work.State==UVFLOW_RUN && f->State!=UVFLOW_RUN)
                f->Deadline=work.Tick+UVFLOW_INTERVAL_MS;
            f->State=work.State;
        } else if(command==UVFLOW_FLUSH) f->Flushing=1;
        else if(command==UVFLOW_END_FLUSH) f->Flushing=0;
    }
    return UVFLOW_OK;
}
int UvFlowFinish(UVFLOW *f,UVFLOW_WORK work,unsigned int result,
                 unsigned int bytes,UVFLOW_COMPLETION *done)
{
    UVOWN_ENTRY entry;
    UVFLOW_SLOT *slot;
    if(done) memset(done,0,sizeof(*done));
    if(!f || !done || !f->Busy || f->Notifying || f->Running.Action==UVFLOW_EXIT ||
       !Same(f->Active,work.Token) ||
       UvOwnSnapshot(&f->Own,work.Token,&entry)!=UVOWN_OK)
        return UVFLOW_INVALID;
    /* Use only the token from the caller's work copy. Commands/stream/time
     * remain canonical owned facts, never reconstructed from class memory. */
    work=f->Running;
    slot=&f->Slot[work.Token.Slot];
    if(entry.Kind==UVOWN_READ) {
        if(slot->MappingFailed) result=UVFLOW_MAPPING;
        else if(entry.Cancelled) result=UVFLOW_CANCELLED;
    } else if(entry.Cancelled && !Teardown(work.Command,work.State)) result=UVFLOW_CANCELLED;
    else if(result==UVFLOW_OK) result=Apply(f,work);
    if(Teardown(work.Command,work.State) && result!=UVFLOW_OK) f->Faulted=1;
    if(Teardown(work.Command,work.State) && f->Closing) f->Closing--;
    done->Request=entry.Request; done->Stream=slot->Stream;
    done->Command=slot->Command; done->State=f->State; done->Result=result;
    done->Bytes=entry.Kind==UVOWN_READ && result==UVFLOW_OK?bytes:0;
    UvOwnRetire(&f->Own,work.Token,&entry);
    memset(slot,0,sizeof(*slot)); memset(&f->Running,0,sizeof(f->Running));
    f->Busy=0; f->Notifying=1;
    Gate(f);
    return UVFLOW_OK;
}
int UvFlowReturned(UVFLOW *f)
{
    if(!f || !f->Notifying || f->FinalIssued) return UVFLOW_INVALID;
    f->Notifying=0; return UVFLOW_OK;
}
int UvFlowAfterJoin(UVFLOW *f,int joined,int drained,UVFLOW_COMPLETION *done)
{
    UVOWN_ENTRY entry;
    if(done) memset(done,0,sizeof(*done));
    if(!f || !done || !f->ExitIssued || f->FinalIssued || f->Notifying ||
       !f->Busy || f->Own.Count!=1 || !Same(f->Active,f->Uninitialize))
        return UVFLOW_INVALID;
    if(!joined || !drained) return UVFLOW_BUSY;
    if(UvOwnRetire(&f->Own,f->Uninitialize,&entry)!=UVOWN_OK) return UVFLOW_INVALID;
    memset(done,0,sizeof(*done)); done->Request=entry.Request;
    done->Command=UVFLOW_UNINITIALIZE; done->Result=UVFLOW_OK; done->Final=1;
    f->Stream=0; f->State=UVFLOW_STOP; f->Busy=0; f->FinalIssued=1;
    memset(&f->Running,0,sizeof(f->Running));
    /* Final notification is outside this function. No return-to-worker or
     * post-notification access to this object is permitted. */
    return UVFLOW_OK;
}
