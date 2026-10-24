/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original adapter from published Stream Class/KS contracts and headers.
 * No kit sample code. Caller-owned lifetime/locking obligations in header.
 */
#include "uvcport.h"
static void Zero(PVOID p, unsigned int n)
{ unsigned char *b=(unsigned char *)p; while(n--) *b++=0; }
static UVPORT_BINDING *Find(UVPORT *p, const void *request)
{
    unsigned int i;
    for(i=0;i<UVOWN_CAPACITY;++i)
        if(p->Flow.Own.Entry[i].Request==request) return &p->Binding[i];
    return NULL;
}
void UvPortInitialize(UVPORT *p)
{ Zero(p,sizeof(*p)); UvFlowInitialize(&p->Flow); p->FrameBytes=UVCSIM_BYTES; }
int UvPortControl(UVPORT *p, PHW_STREAM_REQUEST_BLOCK s,
    unsigned int command, unsigned int state)
{
    UVPORT_BINDING *b; unsigned int slot; int r;
    if(!p || !s) return UVFLOW_INVALID;
    r=UvFlowControl(&p->Flow,s,s->StreamObject,command,state);
    if(r!=UVFLOW_OK) return r;
    b=Find(p,s); slot=(unsigned int)(b-p->Binding); Zero(b,sizeof(*b));
    b->Request=s; b->Stream=s->StreamObject;
    b->Token.Slot=slot; b->Token.Serial=p->Flow.Own.Entry[slot].Serial;
    s->TimeoutCounter=0; return UVFLOW_OK;
}
int UvPortRead(UVPORT *p, PHW_STREAM_REQUEST_BLOCK s, UVOWN_TOKEN *token)
{
    UVPORT_BINDING *b; int r;
    if(!p || !s || !token) return UVFLOW_INVALID;
    r=UvFlowRead(&p->Flow,s,s->StreamObject,token);
    if(r!=UVFLOW_OK) return r;
    b=&p->Binding[token->Slot]; Zero(b,sizeof(*b)); b->Token=*token;
    b->Request=s; b->Stream=s->StreamObject; b->Read=1;
    s->TimeoutCounter=0; return UVFLOW_OK;
}
void UvPortMap(PHW_STREAM_REQUEST_BLOCK s, UVOWN_TOKEN token,
    UVPORT_MAP mapper, PVOID context, UVPORT_MAPPING *m)
{
    UvPortMapBytes(s,token,UVCSIM_BYTES,mapper,context,m);
}
void UvPortMapBytes(PHW_STREAM_REQUEST_BLOCK s, UVOWN_TOKEN token,
    unsigned int frameBytes, UVPORT_MAP mapper, PVOID context, UVPORT_MAPPING *m)
{
    PMDL mdl; PKSSTREAM_HEADER h;
    Zero(m,sizeof(*m)); m->Token=token; m->Request=s;
    if(!s || !mapper) return;
    if(s->Irp) m->Cancelled=s->Irp->Cancel!=0;
    if(s->NumberOfBuffers!=1 || !s->Irp || !s->CommandData.DataBufferArray) return;
    mdl=s->Irp->MdlAddress; h=s->CommandData.DataBufferArray;
    if(!mdl || mdl->Next || h->Size<sizeof(*h) || h->OptionsFlags ||
        !frameBytes || h->FrameExtent<frameBytes || h->FrameExtent>MmGetMdlByteCount(mdl)) return;
    m->Header=h; m->Data=mapper(mdl,context);
    if(s->Irp->Cancel) m->Cancelled=1;
    m->Valid=m->Data!=NULL;
}
int UvPortReady(UVPORT *p, const UVPORT_MAPPING *m)
{
    UVOWN_ENTRY entry; UVPORT_BINDING *b; int r;
    if(!p || !m || (m->Valid && (!m->Header || !m->Data)) ||
        UvOwnSnapshot(&p->Flow.Own,m->Token,&entry)!=UVOWN_OK ||
        entry.Request!=m->Request || entry.Kind!=UVOWN_READ ||
        entry.Phase!=UVOWN_ADMITTING || p->Flow.Slot[m->Token.Slot].AdmissionDone) return UVFLOW_INVALID;
    b=&p->Binding[m->Token.Slot];
    if(m->Cancelled) UvFlowCancel(&p->Flow,m->Request);
    r=UvFlowReady(&p->Flow,m->Token,m->Valid!=0);
    if(r==UVFLOW_OK) { b->Header=m->Header; b->Data=m->Data; }
    return r;
}
int UvPortCancel(UVPORT *p, const void *request)
{ return UvFlowCancel(&p->Flow,request); }
int UvPortNext(UVPORT *p, unsigned int tick, UVPORT_WORK *w)
{
    int r;
    if(!p || !w) return UVFLOW_INVALID;
    Zero(w,sizeof(*w)); r=UvFlowNext(&p->Flow,tick,&w->Flow);
    if(r==UVFLOW_OK && w->Flow.Action!=UVFLOW_WAIT) {
        w->Binding=p->Binding[w->Flow.Token.Slot];
        if(w->Flow.Action!=UVFLOW_EXIT)
            w->Binding.Request->TimeoutCounter=w->Binding.Request->TimeoutOriginal;
    }
    return r;
}
int UvPortFinish(UVPORT *p, UVPORT_WORK w, unsigned int result,
    unsigned int bytes, UVPORT_COMPLETION *c)
{
    UVPORT_BINDING b; UVFLOW_COMPLETION done; int r;
    if(!p || !c || w.Flow.Token.Slot>=UVOWN_CAPACITY) return UVFLOW_INVALID;
    b=p->Binding[w.Flow.Token.Slot];
    if(b.Read && result==UVFLOW_OK && bytes!=p->FrameBytes) { result=UVFLOW_MAPPING; bytes=0; }
    r=UvFlowFinish(&p->Flow,w.Flow,result,bytes,&done);
    if(r!=UVFLOW_OK) return r;
    Zero(&p->Binding[w.Flow.Token.Slot],sizeof(b)); c->Flow=done; c->Binding=b;
    return UVFLOW_OK;
}
int UvPortReturned(UVPORT *p) { return UvFlowReturned(&p->Flow); }
int UvPortAfterJoin(UVPORT *p, int workerJoined, int callbacksDrained,
    UVPORT_COMPLETION *c)
{
    UVFLOW_COMPLETION done; UVPORT_BINDING b; int r;
    if(!p || !c || p->Flow.Uninitialize.Slot>=UVOWN_CAPACITY) return UVFLOW_INVALID;
    b=p->Binding[p->Flow.Uninitialize.Slot];
    r=UvFlowAfterJoin(&p->Flow,workerJoined,callbacksDrained,&done);
    if(r!=UVFLOW_OK) return r;
    Zero(&p->Binding[p->Flow.Uninitialize.Slot],sizeof(b)); c->Flow=done; c->Binding=b;
    return UVFLOW_OK;
}
static NTSTATUS Status(unsigned int r)
{
    switch(r) {
    case UVFLOW_OK: return STATUS_SUCCESS;
    case UVFLOW_CANCELLED: return STATUS_CANCELLED;
    case UVFLOW_MAPPING: return STATUS_INVALID_BUFFER_SIZE;
    case UVFLOW_FULL: return STATUS_DEVICE_BUSY;
    case UVFLOW_CLOSED: case UVFLOW_NOT_READY: return STATUS_DEVICE_NOT_READY;
    default: return STATUS_INVALID_PARAMETER;
    }
}
void UvPortDeliver(const UVPORT_COMPLETION *c, UVPORT_NOTIFY notify, PVOID context)
{
    PHW_STREAM_REQUEST_BLOCK s=c->Binding.Request;
    PHW_STREAM_OBJECT stream=c->Binding.Stream;
    PKSSTREAM_HEADER h=c->Binding.Header;
    unsigned int bytes=c->Flow.Result==UVFLOW_OK ? c->Flow.Bytes : 0;
    unsigned int command=c->Flow.Command;
    int device=command==UVFLOW_OPEN || command==UVFLOW_CLOSE ||
        command==UVFLOW_POWER_UP || command==UVFLOW_POWER_DOWN ||
        command==UVFLOW_UNINITIALIZE;
    s->ActualBytesTransferred=bytes; s->Status=Status(c->Flow.Result);
    if(command==UVFLOW_GET_STATE) s->CommandData.StreamState=(KSSTATE)c->Flow.State;
    if(c->Binding.Read && h) {
        h->DataUsed=bytes;
        if(bytes) {
            h->Duration=1000000; /* fixed 10fps synthetic mode, not USB timing */
            h->OptionsFlags=KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;
        }
    }
    /* LAST operation: reentry may release ALL class-owned storage. */
    notify(s,stream,device,context);
}
