/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Actual M1 dispatch routing, injected engine admission/notification.
 * These tests complement actual-engine tests; no fake kernel proof.
 */
#include "uvcm1.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
__declspec(dllimport) PVOID __stdcall VirtualAlloc(PVOID,SIZE_T,ULONG,ULONG);
__declspec(dllimport) int __stdcall VirtualProtect(PVOID,SIZE_T,ULONG,PULONG);
__declspec(dllimport) int __stdcall VirtualFree(PVOID,SIZE_T,ULONG);
static unsigned int checks,notifies,calls,lastCommand,lastState,protectDone;
static int admission;
static NTSTATUS initializeStatus,lastStatus;
static HW_INITIALIZATION_DATA registered;
static PHW_STREAM_REQUEST_BLOCK packet;
static PHW_STREAM_REQUEST_BLOCK alternate;
static const void *ownedInitialize;
static M1_DEVICE device;
#define anchor device.Work
static HW_STREAM_OBJECT stream;
static PORT_CONFIGURATION_INFORMATION config;
static void __cdecl Check(int ok,unsigned int line)
{ ++checks; if(!ok) { printf("FAIL line%u\n",line); exit(1); } }
#define CHECK(x) Check((x),__LINE__)
static void Protect(ULONG flags)
{ ULONG old; CHECK(VirtualProtect(packet,4096,flags,&old)); }
static void Reset(SRB_COMMAND command)
{
    Protect(4); memset(packet,0,sizeof(*packet));
    packet->HwDeviceExtension=&anchor; packet->StreamObject=&stream;
    packet->Command=command; admission=UVFLOW_OK; calls=notifies=0;
    device.RegisteredDescriptorBytes=0;
    anchor.Engine=NULL;ownedInitialize=NULL;alternate=NULL;
    protectDone=0; lastCommand=lastState=0;
}
NTSTATUS UvwInitialize(UVWORK_ANCHOR *a,PHW_STREAM_REQUEST_BLOCK s,PDEVICE_OBJECT fdo)
{
    CHECK(a==&anchor && s==packet && fdo==(PVOID)0x1234);++calls;
    if(initializeStatus==STATUS_PENDING){ownedInitialize=s;anchor.Engine=(UVWORK *)1;}
    return initializeStatus;
}
int UvwOwnsInitialize(UVWORK_ANCHOR *a,const void *s)
{ CHECK(a==&anchor);return ownedInitialize && s==ownedInitialize; }
int UvwRead(UVWORK_ANCHOR *a,PHW_STREAM_REQUEST_BLOCK s)
{ CHECK(a==&anchor && s==packet); ++calls; return admission; }
int UvwControl(UVWORK_ANCHOR *a,PHW_STREAM_REQUEST_BLOCK s,unsigned int command,unsigned int state)
{ CHECK(a==&anchor && s==packet); ++calls; lastCommand=command; lastState=state; return admission; }
void UvwCancel(UVWORK_ANCHOR *a,const void *s)
{ CHECK(a==&anchor && s==packet); ++calls; }
VOID UvPortNtSerialNotify(PHW_STREAM_REQUEST_BLOCK s,PHW_STREAM_OBJECT p,int device,PVOID context)
{
    (void)p;(void)device;(void)context;
    CHECK(s==packet || s==alternate); ++notifies; lastStatus=s->Status;
    if(protectDone) Protect(1); /* actual dispatch must return without SRB access */
}
NTSTATUS STREAMAPI StreamClassRegisterAdapter(PVOID a,PVOID b,PHW_INITIALIZATION_DATA i)
{ (void)a;(void)b; registered=*i; return STATUS_SUCCESS; }
VOID STREAMAPI StreamClassDeviceNotification(STREAM_MINIDRIVER_DEVICE_NOTIFICATION_TYPE n,PVOID d,...)
{ (void)n;(void)d; CHECK(0); }
VOID STREAMAPI StreamClassStreamNotification(STREAM_MINIDRIVER_STREAM_NOTIFICATION_TYPE n,PHW_STREAM_OBJECT s,...)
{ (void)n;(void)s; CHECK(0); }
static void Route(SRB_COMMAND cmd,unsigned int flow,unsigned int state)
{
    Reset(cmd); packet->CommandData.StreamState=(KSSTATE)state;
    Control(packet); CHECK(calls==1 && !notifies && lastCommand==flow && lastState==state);
}
int __cdecl main(void)
{
    unsigned int i;
    HW_STREAM_REQUEST_BLOCK otherInit;
    struct { HW_STREAM_DESCRIPTOR Descriptor; ULONG Canary; } metadata;
    packet=(PHW_STREAM_REQUEST_BLOCK)VirtualAlloc(NULL,4096,0x3000,4); CHECK(packet!=NULL);
    CHECK((PVOID)&packet->NumberOfBytesToTransfer==(PVOID)&packet->ActualBytesTransferred);
    CHECK(DriverEntry(NULL,NULL)==STATUS_SUCCESS);
    CHECK(!registered.TurnOffSynchronization && !registered.HwInterrupt &&
        registered.DeviceExtensionSize==sizeof(device));
    CHECK(registered.HwReceivePacket==Receive && registered.HwCancelPacket==Cancel &&
        registered.HwRequestTimeoutHandler==Timeout);
    Reset(SRB_INITIALIZE_DEVICE); config.ClassDeviceObject=(PVOID)0x1234;
    packet->CommandData.ConfigInfo=&config; initializeStatus=STATUS_PENDING;
    Receive(packet); CHECK(calls==1 && !notifies && config.StreamDescriptorSize==sizeof(HW_STREAM_DESCRIPTOR));
    CHECK(device.RegisteredDescriptorBytes==config.StreamDescriptorSize);
    /* Duplicate original INIT remains owned. Poison its input pointer and
     * byte counter: duplicate delivery must inspect/mutate neither. */
    packet->CommandData.ConfigInfo=(PVOID)1;packet->ActualBytesTransferred=0xaced;
    Receive(packet);CHECK(calls==1 && !notifies && packet->ActualBytesTransferred==0xaced);
    CHECK(device.RegisteredDescriptorBytes==sizeof(HW_STREAM_DESCRIPTOR));
    /* A distinct new INIT is rejected without notifying the original. */
    otherInit=*packet;otherInit.CommandData.ConfigInfo=(PVOID)1;alternate=&otherInit;
    Receive(&otherInit);CHECK(calls==1 && notifies==1 && lastStatus==STATUS_INVALID_PARAMETER);
    CHECK(ownedInitialize==packet && packet->ActualBytesTransferred==0xaced &&
        device.RegisteredDescriptorBytes==sizeof(HW_STREAM_DESCRIPTOR));
    Reset(SRB_INITIALIZE_DEVICE); protectDone=1; Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_INVALID_PARAMETER);
    Reset(SRB_INITIALIZE_DEVICE); packet->CommandData.ConfigInfo=&config;
    initializeStatus=STATUS_INSUFFICIENT_RESOURCES; protectDone=1; Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_INSUFFICIENT_RESOURCES);
    Reset(SRB_GET_STREAM_INFO); protectDone=1; Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_BUFFER_TOO_SMALL);
    /* Metadata allocation is the budget registered during INIT. Test both
     * zero input and a nonzero union input that output reset would erase. */
    Reset(SRB_GET_STREAM_INFO); memset(&metadata,0x77,sizeof(metadata));
    device.RegisteredDescriptorBytes=sizeof(metadata.Descriptor);
    packet->CommandData.StreamBuffer=&metadata.Descriptor;
    packet->NumberOfBytesToTransfer=0; Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_SUCCESS && packet->ActualBytesTransferred==sizeof(metadata.Descriptor));
    CHECK(metadata.Descriptor.StreamHeader.NumberOfStreams==1 && metadata.Canary==0x77777777);
    Reset(SRB_GET_STREAM_INFO);memset(&metadata,0x77,sizeof(metadata));
    device.RegisteredDescriptorBytes=sizeof(metadata.Descriptor);
    packet->CommandData.StreamBuffer=&metadata.Descriptor;
    packet->NumberOfBytesToTransfer=sizeof(metadata.Descriptor);Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_SUCCESS && packet->ActualBytesTransferred==sizeof(metadata.Descriptor));
    CHECK(metadata.Descriptor.StreamHeader.NumberOfStreams==1 && metadata.Canary==0x77777777);
    /* An unregistered or insufficient class budget fails without a write,
     * even if the irrelevant transfer field is large. */
    for(i=0;i<2;++i) {
        Reset(SRB_GET_STREAM_INFO); memset(&metadata,0x77,sizeof(metadata));
        packet->CommandData.StreamBuffer=&metadata.Descriptor;
        packet->NumberOfBytesToTransfer=~0UL;
        device.RegisteredDescriptorBytes=i?sizeof(metadata.Descriptor)-1:0;
        Receive(packet); CHECK(notifies==1 && lastStatus==STATUS_BUFFER_TOO_SMALL);
        CHECK(metadata.Descriptor.StreamHeader.NumberOfStreams==0x77777777 && metadata.Canary==0x77777777);
    }
    Reset(SRB_OPEN_STREAM); packet->CommandData.OpenFormat=&format.DataFormat;
    Receive(packet); CHECK(calls==1 && !notifies && lastCommand==UVFLOW_OPEN);
    CHECK(stream.ReceiveDataPacket==Read && stream.ReceiveControlPacket==Control && stream.Pio && !stream.Dma);
    Reset(SRB_OPEN_STREAM); protectDone=1; Receive(packet);
    CHECK(notifies==1 && lastStatus==STATUS_NO_MATCH);
    Reset(SRB_READ_DATA); Read(packet); CHECK(calls==1 && !notifies);
    for(i=UVFLOW_INVALID;i<=UVFLOW_EXHAUSTED;++i) {
        Reset(SRB_READ_DATA); admission=(int)i; protectDone=1; Read(packet);
        CHECK(calls==1 && notifies==(i==UVFLOW_DUPLICATE?0u:1u));
    }
    Route(SRB_GET_STREAM_STATE,UVFLOW_GET_STATE,0);
    for(i=0;i<=KSSTATE_RUN;++i) Route(SRB_SET_STREAM_STATE,UVFLOW_SET_STATE,i);
    Route(SRB_BEGIN_FLUSH,UVFLOW_FLUSH,0); Route(SRB_END_FLUSH,UVFLOW_END_FLUSH,0);
    Reset(SRB_CLOSE_STREAM); Receive(packet); CHECK(lastCommand==UVFLOW_CLOSE && calls==1 && !notifies);
    Reset(SRB_CHANGE_POWER_STATE); packet->CommandData.DeviceState=PowerDeviceD3;
    Receive(packet); CHECK(lastCommand==UVFLOW_POWER_DOWN && !notifies);
    Reset(SRB_CHANGE_POWER_STATE); packet->CommandData.DeviceState=PowerDeviceD0;
    Receive(packet); CHECK(lastCommand==UVFLOW_POWER_UP && !notifies);
    Reset(SRB_SURPRISE_REMOVAL); Receive(packet); CHECK(lastCommand==UVFLOW_POWER_DOWN && !notifies);
    Reset(SRB_UNINITIALIZE_DEVICE); anchor.Engine=(UVWORK *)1;
    Receive(packet); CHECK(lastCommand==UVFLOW_UNINITIALIZE && !notifies);
    Reset(SRB_UNINITIALIZE_DEVICE); anchor.Engine=NULL; protectDone=1;
    Receive(packet); CHECK(notifies==1 && lastStatus==STATUS_SUCCESS);
    Reset(SRB_READ_DATA); Cancel(packet); Timeout(packet); CHECK(calls==2 && !notifies);
    Reset(SRB_PROPOSE_DATA_FORMAT); packet->CommandData.OpenFormat=&format.DataFormat;
    protectDone=1; Control(packet); CHECK(notifies==1 && lastStatus==STATUS_SUCCESS);
    Protect(4); CHECK(VirtualFree(packet,0,0x8000));
    printf("PASS: %u actual M1 dispatch checks; synthetic-only routing and last-notify guard\n",checks);
    return 0;
}

