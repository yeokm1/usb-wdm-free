/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Executes the actual flow/ownership cores with synchronous notification
 * reentry. This is policy testing, not a kernel callback lifetime proof.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uvcflow.h"
static int addresses[512], stream;
static unsigned int completions[512], checks;
static void Invariants(UVFLOW *f)
{
    unsigned int i,n=0,reads=0;
    for(i=0;i<UVOWN_CAPACITY;i++) if(f->Own.Entry[i].Request) {
        n++; if(f->Own.Entry[i].Kind==UVOWN_READ) reads++;
        assert(f->Own.Entry[i].Serial);
    }
    assert(n==f->Own.Count && reads==f->Own.Reads);
    assert(n-reads==f->Own.Controls && n<=UVOWN_CAPACITY);
    assert(!(f->Busy && f->Notifying));
    checks++;
}
static void Count(const UVFLOW_COMPLETION *done)
{
    unsigned int i;
    for(i=0;i<512;i++) if(done->Request==&addresses[i]) break;
    assert(i<512 && !completions[i]); completions[i]++;
}
static UVFLOW_COMPLETION Finish(UVFLOW *f,UVFLOW_WORK work,unsigned int result,unsigned int bytes)
{
    UVFLOW_COMPLETION done;
    UVFLOW_WORK nested;
    assert(UvFlowFinish(f,work,result,bytes,&done)==UVFLOW_OK);
    Count(&done);
    assert(UvFlowCancel(f,done.Request)==UVFLOW_CLOSED);
    assert(UvFlowNext(f,work.Tick,&nested)==UVFLOW_BUSY);
    Invariants(f);
    return done;
}
static UVFLOW_WORK Next(UVFLOW *f,unsigned int now,unsigned int action)
{
    UVFLOW_WORK work;
    assert(UvFlowNext(f,now,&work)==UVFLOW_OK && work.Action==action);
    Invariants(f); return work;
}
static UVFLOW_COMPLETION Control(UVFLOW *f,unsigned int id,unsigned int command,
                                  unsigned int state,unsigned int now)
{
    UVFLOW_WORK work;
    UVFLOW_COMPLETION done;
    assert(UvFlowControl(f,&addresses[id],&stream,command,state)==UVFLOW_OK);
    work=Next(f,now,UVFLOW_CONTROL);
    done=Finish(f,work,UVFLOW_OK,0);
    assert(UvFlowReturned(f)==UVFLOW_OK); return done;
}
static void Setup(UVFLOW *f,unsigned int now)
{
    memset(completions,0,sizeof(completions));
    UvFlowInitialize(f);
    assert(Control(f,0,UVFLOW_OPEN,0,now).Result==UVFLOW_OK);
    assert(Control(f,1,UVFLOW_SET_STATE,UVFLOW_PAUSE,now).State==UVFLOW_PAUSE);
}
static UVOWN_TOKEN Read(UVFLOW *f,unsigned int id,int ready)
{
    UVOWN_TOKEN token;
    assert(UvFlowRead(f,&addresses[id],&stream,&token)==UVFLOW_OK);
    if(ready) assert(UvFlowReady(f,token,1)==UVFLOW_OK);
    return token;
}
static void PausedAndAdmission(void)
{
    UVFLOW f; UVFLOW_WORK work; UVFLOW_COMPLETION done; UVOWN_TOKEN token;
    Setup(&f,0);
    token=Read(&f,2,0);
    assert(UvFlowCancel(&f,&addresses[2])==UVFLOW_OK);
    Next(&f,0,UVFLOW_WAIT); /* Mapper still owns its admission. */
    assert(UvFlowReady(&f,token,1)==UVFLOW_OK);
    assert(UvFlowReady(&f,token,1)==UVFLOW_INVALID);
    work=Next(&f,0,UVFLOW_READ);
    done=Finish(&f,work,UVFLOW_OK,153600);
    assert(done.Result==UVFLOW_CANCELLED && !done.Bytes && f.Frame==0);
    UvFlowReturned(&f);
    assert(UvFlowReady(&f,token,1)==UVFLOW_INVALID); /* Stale token. */
    token=Read(&f,3,0);
    assert(UvFlowControl(&f,&addresses[4],&stream,UVFLOW_CLOSE,0)==UVFLOW_OK);
    Next(&f,1000,UVFLOW_WAIT); /* Teardown cannot outrun delayed mapping. */
    assert(UvFlowReady(&f,token,0)==UVFLOW_OK);
    work=Next(&f,1000,UVFLOW_READ);
    done=Finish(&f,work,UVFLOW_OK,153600);
    assert(done.Result==UVFLOW_MAPPING && !done.Bytes);
    UvFlowReturned(&f);
    work=Next(&f,1000,UVFLOW_CONTROL);
    done=Finish(&f,work,UVFLOW_OK,0);
    assert(done.Command==UVFLOW_CLOSE && !f.Stream);
    UvFlowReturned(&f);
    assert(UvFlowRead(&f,&addresses[5],&stream,&token)==UVFLOW_CLOSED);
}
static void Reentry(unsigned int command)
{
    UVFLOW *f=(UVFLOW *)malloc(sizeof(*f));
    UVFLOW_WORK work,nested;
    UVFLOW_COMPLETION done,final;
    UVOWN_TOKEN rejected;
    assert(f); Setup(f,0);
    Control(f,2,UVFLOW_SET_STATE,UVFLOW_RUN,0);
    Read(f,3,1); Read(f,4,1);
    work=Next(f,100,UVFLOW_READ);
    done=Finish(f,work,UVFLOW_OK,153600);
    assert(done.Bytes==153600);
    /* This is the synchronous class-notification callback stack. It
     * submits teardown and returns; there is no drain wait or self-join. */
    assert(UvFlowControl(f,&addresses[5],&stream,command,UVFLOW_STOP)==UVFLOW_OK);
    assert(UvFlowNext(f,100,&nested)==UVFLOW_BUSY);
    assert(UvFlowRead(f,&addresses[6],&stream,&rejected)==UVFLOW_CLOSED);
    assert(UvFlowAfterJoin(f,1,1,&final)==UVFLOW_INVALID);
    assert(UvFlowReturned(f)==UVFLOW_OK);
    /* A queued sibling is cancelled without requiring a future RUN. */
    work=Next(f,100,UVFLOW_READ);
    done=Finish(f,work,UVFLOW_OK,153600);
    assert(done.Result==UVFLOW_CANCELLED && !done.Bytes);
    UvFlowReturned(f);
    if(command==UVFLOW_UNINITIALIZE) {
        work=Next(f,100,UVFLOW_EXIT);
        assert(work.Request==&addresses[5]);
        assert(UvFlowFinish(f,work,UVFLOW_OK,0,&done)==UVFLOW_INVALID);
        assert(UvFlowAfterJoin(f,0,0,&final)==UVFLOW_BUSY);
        assert(UvFlowAfterJoin(f,1,0,&final)==UVFLOW_BUSY);
        assert(UvFlowAfterJoin(f,0,1,&final)==UVFLOW_BUSY);
        assert(UvFlowAfterJoin(f,1,1,&final)==UVFLOW_OK);
        assert(final.Final && final.Request==&addresses[5]); Count(&final);
        assert(!f->Own.Count && UvFlowReturned(f)==UVFLOW_INVALID);
        /* A final class notification may free engine storage immediately.
         * No worker/Returned action follows this external handoff. */
        free(f); return;
    }
    work=Next(f,100,UVFLOW_CONTROL);
    done=Finish(f,work,UVFLOW_OK,0); UvFlowReturned(f);
    assert(done.Command==command && f->State==UVFLOW_STOP);
    if(command==UVFLOW_CLOSE) assert(!f->Stream);
    free(f);
}
static void CapacityAndShutdown(void)
{
    UVFLOW f; UVFLOW_WORK work; UVFLOW_COMPLETION done;
    UVOWN_TOKEN token;
    unsigned int i;
    Setup(&f,0);
    for(i=0;i<UVFLOW_NORMAL_LIMIT;i++) Read(&f,i+2,1);
    assert(UvFlowRead(&f,&addresses[30],&stream,&token)==UVFLOW_FULL);
    assert(UvFlowControl(&f,&addresses[31],&stream,UVFLOW_GET_STATE,0)==UVFLOW_FULL);
    /* Saturate ordinary and emergency-stop capacity, retaining CLOSE and
     * final UNINITIALIZE slots. No queue-full state can require a wait. */
    for(i=24;i<30;i++)
        assert(UvFlowControl(&f,&addresses[i+20],&stream,UVFLOW_SET_STATE,UVFLOW_STOP)==UVFLOW_OK);
    assert(UvFlowControl(&f,&addresses[60],&stream,UVFLOW_CLOSE,0)==UVFLOW_OK);
    assert(UvFlowControl(&f,&addresses[61],0,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
    assert(f.Own.Count==UVOWN_CAPACITY);
    assert(UvFlowControl(&f,&addresses[62],0,UVFLOW_UNINITIALIZE,0)==UVFLOW_CLOSED);
    assert(UvFlowCancel(&f,&addresses[61])==UVFLOW_BUSY);
    for(i=0;i<31;i++) {
        assert(UvFlowNext(&f,0,&work)==UVFLOW_OK &&
            (work.Action==UVFLOW_READ || work.Action==UVFLOW_CONTROL));
        done=Finish(&f,work,UVFLOW_OK,153600);
        assert(!done.Bytes); UvFlowReturned(&f);
    }
    Next(&f,0,UVFLOW_EXIT);
    assert(UvFlowAfterJoin(&f,1,1,&done)==UVFLOW_OK); Count(&done);
    assert(!f.Own.Count && f.FinalIssued);
}
static void DeadlineAndOrder(void)
{
    UVFLOW f; UVFLOW_WORK work; UVFLOW_COMPLETION done;
    unsigned int i;
    Setup(&f,0);
    Read(&f,2,1); Read(&f,3,1);
    Control(&f,4,UVFLOW_SET_STATE,UVFLOW_RUN,0);
    for(i=0;i<100;i++) Next(&f,i,UVFLOW_WAIT);
    work=Next(&f,100,UVFLOW_READ);
    assert(work.Frame==1); Finish(&f,work,UVFLOW_OK,153600); UvFlowReturned(&f);
    for(i=101;i<200;i++) Next(&f,i,UVFLOW_WAIT);
    /* A control wakeup does not create a second frame before deadline. */
    done=Control(&f,5,UVFLOW_GET_STATE,0,150); assert(done.State==UVFLOW_RUN);
    Next(&f,199,UVFLOW_WAIT);
    work=Next(&f,200,UVFLOW_READ);
    assert(work.Frame==2); Finish(&f,work,UVFLOW_OK,153600); UvFlowReturned(&f);
    Setup(&f,~0u-40u);
    Read(&f,2,1); Control(&f,3,UVFLOW_SET_STATE,UVFLOW_RUN,~0u-40u);
    Next(&f,10,UVFLOW_WAIT); Next(&f,58,UVFLOW_WAIT);
    work=Next(&f,59,UVFLOW_READ);
    Finish(&f,work,UVFLOW_OK,153600); UvFlowReturned(&f);
    assert(UvFlowControl(&f,&addresses[4],&stream,UVFLOW_SET_STATE,UVFLOW_PAUSE)==UVFLOW_OK);
    assert(UvFlowControl(&f,&addresses[5],&stream,UVFLOW_GET_STATE,0)==UVFLOW_OK);
    work=Next(&f,60,UVFLOW_CONTROL); Finish(&f,work,UVFLOW_OK,0); UvFlowReturned(&f);
    work=Next(&f,60,UVFLOW_CONTROL); done=Finish(&f,work,UVFLOW_OK,0); UvFlowReturned(&f);
    assert(done.State==UVFLOW_PAUSE);
}
static void PowerFlushFaultAndSerial(void)
{
    UVFLOW f; UVFLOW_WORK work; UVFLOW_COMPLETION done;
    UVOWN_TOKEN token;
    Setup(&f,0);
    Read(&f,2,1);
    assert(UvFlowRead(&f,&addresses[2],&stream,&token)==UVFLOW_DUPLICATE);
    assert(UvFlowControl(&f,&addresses[2],&stream,UVFLOW_CLOSE,0)==UVFLOW_DUPLICATE);
    assert(f.Own.Count==1 && !f.Closing);
    assert(UvFlowControl(&f,&addresses[3],0,UVFLOW_POWER_DOWN,0)==UVFLOW_OK);
    work=Next(&f,0,UVFLOW_READ); done=Finish(&f,work,UVFLOW_OK,999);
    assert(done.Result==UVFLOW_CANCELLED && !done.Bytes); UvFlowReturned(&f);
    work=Next(&f,0,UVFLOW_CONTROL); Finish(&f,work,UVFLOW_OK,0); UvFlowReturned(&f);
    assert(f.PowerBlocked && f.State==UVFLOW_STOP);
    assert(UvFlowRead(&f,&addresses[4],&stream,&token)==UVFLOW_CLOSED && !token.Serial);
    Control(&f,5,UVFLOW_POWER_UP,0,0);
    assert(!f.PowerBlocked && f.State==UVFLOW_STOP);
    Control(&f,6,UVFLOW_SET_STATE,UVFLOW_PAUSE,0);
    Control(&f,7,UVFLOW_FLUSH,0,0);
    assert(f.Flushing && UvFlowRead(&f,&addresses[8],&stream,&token)==UVFLOW_CLOSED);
    Control(&f,9,UVFLOW_CLOSE,0,0);
    assert(!f.Flushing);
    Control(&f,10,UVFLOW_OPEN,0,0); Control(&f,11,UVFLOW_SET_STATE,UVFLOW_RUN,0);
    assert(UvFlowControl(&f,&addresses[12],&stream,UVFLOW_SET_STATE,UVFLOW_STOP)==UVFLOW_OK);
    work=Next(&f,0,UVFLOW_CONTROL);
    done=Finish(&f,work,UVFLOW_NOT_READY,0); UvFlowReturned(&f);
    assert(done.Result==UVFLOW_NOT_READY && f.Faulted);
    assert(UvFlowRead(&f,&addresses[13],&stream,&token)==UVFLOW_CLOSED);
    Control(&f,14,UVFLOW_CLOSE,0,0); assert(!f.Faulted);
    Setup(&f,0);
    /* Reach the last ordinary generation value without spending years of
     * frame traffic. Reserved generations still allow CLOSE/UNINITIALIZE. */
    f.Own.Serial=~0u-8u;
    assert(UvFlowRead(&f,&addresses[2],&stream,&token)==UVFLOW_EXHAUSTED);
    assert(UvFlowControl(&f,&addresses[3],&stream,UVFLOW_GET_STATE,0)==UVFLOW_EXHAUSTED);
    f.Own.Serial=~0u-2u;
    assert(UvFlowControl(&f,&addresses[4],&stream,UVFLOW_SET_STATE,UVFLOW_STOP)==UVFLOW_EXHAUSTED);
    assert(UvFlowControl(&f,&addresses[5],&stream,UVFLOW_CLOSE,0)==UVFLOW_OK);
    assert(UvFlowControl(&f,&addresses[6],0,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
    assert(f.Own.Serial==~0u);
    work=Next(&f,0,UVFLOW_CONTROL); Finish(&f,work,UVFLOW_OK,0); UvFlowReturned(&f);
    Next(&f,0,UVFLOW_EXIT);
    assert(UvFlowAfterJoin(&f,1,1,&done)==UVFLOW_OK && done.Final); Count(&done);
}
static void ControlReentryAndNoCatchup(void)
{
    UVFLOW f; UVFLOW_WORK work,nested; UVFLOW_COMPLETION done;
    Setup(&f,0);
    assert(UvFlowControl(&f,&addresses[2],&stream,UVFLOW_GET_STATE,0)==UVFLOW_OK);
    work=Next(&f,0,UVFLOW_CONTROL); Finish(&f,work,UVFLOW_OK,0);
    assert(UvFlowControl(&f,&addresses[3],0,UVFLOW_UNINITIALIZE,0)==UVFLOW_OK);
    assert(UvFlowNext(&f,0,&nested)==UVFLOW_BUSY);
    UvFlowReturned(&f); Next(&f,0,UVFLOW_EXIT);
    assert(UvFlowAfterJoin(&f,1,1,&done)==UVFLOW_OK); Count(&done);
    Setup(&f,0); Control(&f,2,UVFLOW_SET_STATE,UVFLOW_RUN,0);
    Read(&f,3,1); Read(&f,4,1);
    work=Next(&f,10000,UVFLOW_READ); assert(work.Frame==1);
    Finish(&f,work,UVFLOW_OK,153600); UvFlowReturned(&f);
    Next(&f,10000,UVFLOW_WAIT); Next(&f,10099,UVFLOW_WAIT);
    assert(UvFlowControl(&f,&addresses[5],&stream,UVFLOW_SET_STATE,UVFLOW_ACQUIRE)==UVFLOW_OK);
    work=Next(&f,10099,UVFLOW_READ); done=Finish(&f,work,UVFLOW_OK,153600);
    assert(done.Result==UVFLOW_CANCELLED && f.Frame==1); UvFlowReturned(&f);
    work=Next(&f,10099,UVFLOW_CONTROL); done=Finish(&f,work,UVFLOW_OK,0);
    assert(done.State==UVFLOW_ACQUIRE); UvFlowReturned(&f);
}
int main(void)
{
    PausedAndAdmission(); Reentry(UVFLOW_SET_STATE); Reentry(UVFLOW_CLOSE);
    Reentry(UVFLOW_UNINITIALIZE); CapacityAndShutdown(); DeadlineAndOrder();
    PowerFlushFaultAndSerial();
    ControlReentryAndNoCatchup();
    printf("PASS: %u flow checkpoints, synchronous teardown reentry, delayed admission, reserved shutdown and wrap deadlines\n",checks);
    return 0;
}
