/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original NT2000+ ROOT-only synthetic RUN experiment. No USB transport.
 * Reuse this repository's original M0 metadata without changing M0; the
 * renamed M0 dispatch/entry functions are unreferenced and link-discarded.
 */
#include <strmini.h>
#include <ksmedia.h>
#define DriverEntry MetadataDriverEntry
#define Read MetadataRead
#define Control MetadataControl
#define Receive MetadataReceive
#define Cancel MetadataCancel
#define Timeout MetadataTimeout
#include "uvcmini.c"
#undef DriverEntry
#undef Read
#undef Control
#undef Receive
#undef Cancel
#undef Timeout
#include "uvcwork.h"
#include "uvctrace.h"

typedef struct M1_DEVICE {
    UVWORK_ANCHOR Work;
    ULONG RegisteredDescriptorBytes;
} M1_DEVICE;
static M1_DEVICE *Device(PHW_STREAM_REQUEST_BLOCK s)
{ return (M1_DEVICE *)s->HwDeviceExtension; }
static UVWORK_ANCHOR *Anchor(PHW_STREAM_REQUEST_BLOCK s)
{ return &Device(s)->Work; }

static void Done(PHW_STREAM_REQUEST_BLOCK s, NTSTATUS status)
{
    s->Status=status;
    UvPortNtSerialNotify(s,s->StreamObject,0,NULL); /* LAST class access */
}
static void Accepted(PHW_STREAM_REQUEST_BLOCK s,int result)
{
    /* DUPLICATE still belongs to the engine: never complete it twice. */
    if(result==UVFLOW_OK || result==UVFLOW_DUPLICATE) return;
    Done(s,result==UVFLOW_FULL?STATUS_DEVICE_BUSY:
        (result==UVFLOW_CLOSED || result==UVFLOW_NOT_READY)?STATUS_DEVICE_NOT_READY:
        STATUS_INVALID_PARAMETER);
}
static VOID STREAMAPI Read(PHW_STREAM_REQUEST_BLOCK s)
{
    UVHEX("READ_SRB ",s->Command);
    s->ActualBytesTransferred=0;
    if(s->Command!=SRB_READ_DATA) { Done(s,STATUS_NOT_SUPPORTED); return; }
    Accepted(s,UvwRead(Anchor(s),s));
}
static VOID STREAMAPI Control(PHW_STREAM_REQUEST_BLOCK s)
{
    unsigned int command=0,state=0;
    UVHEX("CONTROL_SRB ",s->Command);
    s->ActualBytesTransferred=0;
    switch(s->Command) {
    case SRB_GET_STREAM_STATE: command=UVFLOW_GET_STATE; break;
    case SRB_SET_STREAM_STATE:
        command=UVFLOW_SET_STATE; state=(unsigned int)s->CommandData.StreamState; break;
    case SRB_BEGIN_FLUSH: command=UVFLOW_FLUSH; break;
    case SRB_END_FLUSH: command=UVFLOW_END_FLUSH; break;
    case SRB_PROPOSE_DATA_FORMAT:
    case SRB_SET_DATA_FORMAT:
#ifdef UVWORK_USB
        {   /* M2: any advertised format may be proposed; no live switch, so
             * SET_DATA_FORMAT succeeds only for the format already open. */
            int i=UvwFormats(Anchor(s)) ? UvfMatch(UvwFormats(Anchor(s)),s->CommandData.OpenFormat) : -1;
            Done(s,i<0 ? STATUS_NO_MATCH :
                (s->Command==SRB_SET_DATA_FORMAT && i!=UvwSelected(Anchor(s))) ? STATUS_NOT_SUPPORTED :
                STATUS_SUCCESS);
            return;
        }
#else
        Done(s,Valid(s->CommandData.OpenFormat)?STATUS_SUCCESS:STATUS_NO_MATCH); return;
#endif
    case SRB_INDICATE_MASTER_CLOCK:
    case SRB_CLOSE_MASTER_CLOCK:
        /* No presentation timestamps or clock calls in this fixed source. */
        Done(s,STATUS_SUCCESS); return;
    default: Done(s,STATUS_NOT_IMPLEMENTED); return;
    }
    Accepted(s,UvwControl(Anchor(s),s,command,state));
}
static VOID STREAMAPI Receive(PHW_STREAM_REQUEST_BLOCK s)
{
    unsigned int command=0;
    NTSTATUS status=STATUS_SUCCESS;
    UVHEX("DEVICE_SRB ",s->Command);
    if(s->Command==SRB_INITIALIZE_DEVICE && UvwOwnsInitialize(Anchor(s),s)) return;
#ifdef UVWORK_VM_TRACE
    if(s->Command==SRB_GET_STREAM_INFO)
        UVHEX("STREAM_INFO_INCOMING_UNION ",s->NumberOfBytesToTransfer);
#endif
    s->ActualBytesTransferred=0;
    switch(s->Command) {
    case SRB_INITIALIZE_DEVICE:
        UVTRACE("M1_INIT_ENTRY\n");
        if(Anchor(s)->Engine) { Done(s,STATUS_INVALID_PARAMETER); return; }
        if(!s->CommandData.ConfigInfo || !s->CommandData.ConfigInfo->ClassDeviceObject) {
            Done(s,STATUS_INVALID_PARAMETER); return;
        }
#ifdef UVWORK_USB
        /* M2: USB requests go to the attached device object (strmini.h). */
        if(!s->CommandData.ConfigInfo->PhysicalDeviceObject) { Done(s,STATUS_INVALID_PARAMETER); return; }
        Anchor(s)->Lower=s->CommandData.ConfigInfo->PhysicalDeviceObject;
#endif
        Device(s)->RegisteredDescriptorBytes=sizeof(HW_STREAM_DESCRIPTOR);
        s->CommandData.ConfigInfo->StreamDescriptorSize=Device(s)->RegisteredDescriptorBytes;
        status=UvwInitialize(Anchor(s),s,s->CommandData.ConfigInfo->ClassDeviceObject);
        if(status!=STATUS_PENDING) {
            Device(s)->RegisteredDescriptorBytes=0; Done(s,status);
        }
        return;
    case SRB_GET_STREAM_INFO:
        UVHEX("STREAM_INFO_UNION_AFTER_ZERO ",s->NumberOfBytesToTransfer);
        UVHEX("STREAM_INFO_REQUIRED_BYTES ",sizeof(HW_STREAM_DESCRIPTOR));
        UVHEX("STREAM_INFO_BUFFER_PRESENT ",s->CommandData.StreamBuffer!=NULL);
        UVHEX("STREAM_INFO_REGISTERED_BYTES ",Device(s)->RegisteredDescriptorBytes);
        /* GET_STREAM_INFO's class allocation is the budget registered in
         * INIT.ConfigInfo.StreamDescriptorSize. NumberOfBytesToTransfer is
         * only documented for READ/WRITE_DATA and aliases the output field
         * ActualBytesTransferred, which was reset above. The old M1 guard
         * therefore always saw its own zero, not a class allocation size.
         * Trust the class's documented allocation, retain NULL/budget guards. */
        if(!s->CommandData.StreamBuffer ||
           Device(s)->RegisteredDescriptorBytes<sizeof(HW_STREAM_DESCRIPTOR))
            status=STATUS_BUFFER_TOO_SMALL;
        else {
            PHW_STREAM_DESCRIPTOR d=s->CommandData.StreamBuffer;
            RtlZeroMemory(d,sizeof(*d));
            d->StreamHeader.NumberOfStreams=1;
            d->StreamHeader.SizeOfHwStreamInformation=sizeof(HW_STREAM_INFORMATION);
            d->StreamHeader.Topology=&topology;
            d->StreamInfo.NumberOfPossibleInstances=1;
            d->StreamInfo.DataFlow=KSPIN_DATAFLOW_OUT;
            d->StreamInfo.DataAccessible=TRUE;
#ifdef UVWORK_USB
            {   /* M2: the camera's own formats, built at bootstrap. */
                const UVFMT *t=UvwFormats(Anchor(s));
                d->StreamInfo.NumberOfFormatArrayEntries=t ? t->Count : 0;
                d->StreamInfo.StreamFormatsArray=t ? (PKSDATAFORMAT *)t->Ranges : NULL;
            }
#else
            d->StreamInfo.NumberOfFormatArrayEntries=1;
            d->StreamInfo.StreamFormatsArray=formats;
#endif
            d->StreamInfo.Category=&capturePin; d->StreamInfo.Name=&capturePin;
            s->ActualBytesTransferred=sizeof(*d);
        }
        Done(s,status); return;
#ifdef UVWORK_USB
    case SRB_GET_DATA_INTERSECTION:
        Done(s,UvwFormats(Anchor(s)) ? UvfIntersect(UvwFormats(Anchor(s)),s) : STATUS_NO_MATCH); return;
    case SRB_OPEN_STREAM:
        if(!s->StreamObject || s->StreamObject->StreamNumber || !UvwFormats(Anchor(s)) ||
           !UvwSelectFormat(Anchor(s),UvfMatch(UvwFormats(Anchor(s)),s->CommandData.OpenFormat))) {
            Done(s,STATUS_NO_MATCH); return;
        }
#else
    case SRB_GET_DATA_INTERSECTION: Done(s,Intersect(s)); return;
    case SRB_OPEN_STREAM:
        if(!s->StreamObject || s->StreamObject->StreamNumber || !Valid(s->CommandData.OpenFormat)) {
            Done(s,STATUS_NO_MATCH); return;
        }
#endif
        s->StreamObject->ReceiveDataPacket=Read;
        s->StreamObject->ReceiveControlPacket=Control;
        s->StreamObject->Dma=FALSE; s->StreamObject->Pio=TRUE;
        s->StreamObject->Allocator=FALSE;
        s->StreamObject->StreamHeaderMediaSpecific=0;
        s->StreamObject->StreamHeaderWorkspace=0;
        command=UVFLOW_OPEN; break;
    case SRB_CLOSE_STREAM: command=UVFLOW_CLOSE; break;
    case SRB_CHANGE_POWER_STATE:
        command=s->CommandData.DeviceState==PowerDeviceD0?UVFLOW_POWER_UP:UVFLOW_POWER_DOWN;
        break;
    case SRB_SURPRISE_REMOVAL: command=UVFLOW_POWER_DOWN; break;
    case SRB_UNINITIALIZE_DEVICE:
        if(!Anchor(s)->Engine) { Done(s,STATUS_SUCCESS); return; }
        command=UVFLOW_UNINITIALIZE; break;
    case SRB_OPEN_DEVICE_INSTANCE:
    case SRB_CLOSE_DEVICE_INSTANCE:
    case SRB_INITIALIZATION_COMPLETE:
        Done(s,STATUS_SUCCESS); return;
    default: Done(s,STATUS_NOT_IMPLEMENTED); return;
    }
    Accepted(s,UvwControl(Anchor(s),s,command,0));
}
static VOID STREAMAPI Cancel(PHW_STREAM_REQUEST_BLOCK s)
{ UvwCancel(Anchor(s),s); }
static VOID STREAMAPI Timeout(PHW_STREAM_REQUEST_BLOCK s)
{ UvwCancel(Anchor(s),s); }

NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path)
{
    HW_INITIALIZATION_DATA init;
    UVTRACE("M1_DRIVER_ENTRY\n");
    Formats(); RtlZeroMemory(&init,sizeof(init));
    init.HwInitializationDataSize=sizeof(init);
    init.HwReceivePacket=Receive; init.HwCancelPacket=Cancel;
    init.HwRequestTimeoutHandler=Timeout;
    init.DeviceExtensionSize=sizeof(M1_DEVICE);
    /* Default synchronization ON, no ISR. Timer/dispatch/cancel share gate.
     * No callbacks wait or map; class queues reopen only with completion. */
    return StreamClassRegisterAdapter(driver,path,&init);
}
