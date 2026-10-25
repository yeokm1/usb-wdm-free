/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Experimental UAC 2.0 WaveCyclic/USB streaming: render and capture in
 * the formats and rates the device's descriptors and clocks offer.
 * Original code from DDK headers and published PortCls documentation.
 */
#define PC_NEW_NAMES
#include <initguid.h>
#include <portcls.h>
extern "C" {
#pragma warning(push)
#pragma warning(disable:4200) /* flexible arrays in the kit's USB headers */
#if defined(_AMD64_)
#include <usb.h>
#else
#include <usbdi.h>
#endif
#include <usbioctl.h>
#pragma warning(pop)
#include "uacdesc.h"
#include "uacusb.h"
#include "uacplan.h"
#include "uactopo.h"
}

static KSDATARANGE analog = {
    sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE bridgeRanges[] = { &analog };
/* uactopo.c holds the graphs (host-tested); UAC_LINK has
 * PCCONNECTION_DESCRIPTOR's layout. */
#define UAC_ASSERT(e, n) typedef char n[(e) ? 1 : -1]
UAC_ASSERT(sizeof(UAC_LINK) == sizeof(PCCONNECTION_DESCRIPTOR), uacLinkSize);
UAC_ASSERT(UAC_FILTER_NODE == PCFILTER_NODE, uacFilterNode);
/* The topology filter carries a SUM node between each pair of its pins:
 * Windows 98 SE's SysAudio took a page fault on the earlier pin-to-pin
 * topology (uactopo.h). A SUM node adds no mixer control (2000 DDK,
 * "WDMAud Topology Parsing"), so no control is offered that the driver
 * cannot carry out. */
#define UAC_RANGES 32
#define UAC_FORMATS UAC_PLAN_FORMATS

/* One direction's formats and the rates each runs at, from the device's
 * descriptors and its clock's RANGE answers (PnP start). */
struct UAC_DIR_FORMATS {
    ULONG Count;
    UAC_FORMAT Format[UAC_FORMATS];
    ULONG RateCount[UAC_FORMATS];
    ULONG Rate[UAC_FORMATS][UAC_USB_RATES];
    ULONG Ranges;
    KSDATARANGE_AUDIO Range[UAC_RANGES];
    PKSDATARANGE RangeList[UAC_RANGES];
    ULONG RangeFormat[UAC_RANGES];  /* index into Format[] */
};

/* Pool construction avoids the C++ runtime, exceptions and global ctors. */
inline void *operator new(size_t, void *memory) { return memory; }
static void Record(PDEVICE_OBJECT fdo, ULONG stage, NTSTATUS status);
static void RecordStats(PDEVICE_OBJECT fdo, UAC_USB_STATS *stats, BOOLEAN capture);
static void RecordStreams(PDEVICE_OBJECT fdo, ULONG attempts, ULONG created, ULONG closed, ULONG active);
#ifndef _WIN64
/* Windows 98 SE's NTKERN exports no IoAllocateWorkItem/IoQueueWorkItem/
 * IoFreeWorkItem (link probe against the 98 DDK wdm.lib, 2026-10-22). The
 * x86 build therefore runs the stream cleanup as an executive work item
 * (ExQueueWorkItem, in every WDM system) behind those names. The stream
 * keeps its own reference on the device object until DestroyPassive; unlike
 * IoQueueWorkItem, nothing else holds the image while Cleanup returns. */
struct UAC_WORK_ITEM {
    WORK_QUEUE_ITEM Item;
    PIO_WORKITEM_ROUTINE Routine;
    PVOID Context;
    PDEVICE_OBJECT Device;
};
static VOID UacExItem(PVOID p)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)p;
    j->Routine(j->Device, j->Context); /* frees j: no access after */
}
static PIO_WORKITEM UacAllocItem(PDEVICE_OBJECT d)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*j), 'w2aU');
    if (j) { RtlZeroMemory(j, sizeof(*j)); j->Device = d; }
    return (PIO_WORKITEM)j;
}
static VOID UacFreeItem(PIO_WORKITEM i) { ExFreePool(i); }
static VOID UacQueueItem(PIO_WORKITEM i, PIO_WORKITEM_ROUTINE r, WORK_QUEUE_TYPE q, PVOID c)
{
    UAC_WORK_ITEM *j = (UAC_WORK_ITEM *)i;
    j->Routine = r; j->Context = c;
    ExInitializeWorkItem(&j->Item, UacExItem, j);
    ExQueueWorkItem(&j->Item, q);
}
#define IoAllocateWorkItem UacAllocItem
#define IoFreeWorkItem UacFreeItem
#define IoQueueWorkItem UacQueueItem
#endif
#include "uacstream.h"

class Wave;
/* One direction of the wave filter: its formats, its one-stream gate and
 * the transport stream of the open pin instance. */
struct UAC_DIR {
    Wave *Owner;
    BOOLEAN Capture;
    LONG Active;
    ULONG Attempts, Created, Closed;
    UAC_USB *Usb;
    UAC_DIR_FORMATS Formats;
};

class Wave : public IMiniportWaveCyclic, public IAdapterPowerManagement {
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    LONG refs;
    PDEVICE_OBJECT device;
    PDEVICE_OBJECT lower;
    UAC_DESC *descriptor;
    UAC_DEV *dev;
    const UAC_GRAPH *graph;
    PPORTWAVECYCLIC port;
    KMUTEX operation;
    BOOLEAN removed, pnpBlocked, powerBlocked;
    UAC_DIR dir[2];             /* 0 render, 1 capture */
    PCPIN_DESCRIPTOR pins[4];
    PCNODE_DESCRIPTOR nodes[2];
    GUID categories[3];
    PCFILTER_DESCRIPTOR filter;

    static void Fill(PVOID context, PUCHAR bytes, ULONG count) {
        ((UacStream *)context)->FillBytes(bytes, count);
    }
    static void Captured(PVOID context, const UCHAR *bytes, ULONG count) {
        ((UacStream *)context)->WriteBytes(bytes, count);
    }
    static void Progress(PVOID context, ULONG bytes, ULONG errors) {
        ((UacStream *)context)->CompleteBytes(bytes, errors);
    }
    /* The device format (index) a stream format maps to, or -1. The
     * container must be the subslot; valid bits may differ (a 24-bit
     * device takes 32-bit samples in a 32-bit slot and ignores the low
     * byte, as its subslot says). */
    /* Exact valid bits first; otherwise playback takes any device format
     * of the container, capture one whose bits the stream declares room
     * for. Accept and Run use this one rule. Declared positions must be
     * the device format's (no remapping); a plain format, an unassigned
     * mask (0) or a device that names none takes the device's order. */
    static LONG Lookup(const UAC_DIR_FORMATS *f, BOOLEAN capture, ULONG channels, ULONG bits,
                       ULONG container, ULONG rate, ULONG mask) {
        for (ULONG pass = 0; pass < 2; pass++)
            for (ULONG i = 0; i < f->Count; i++) {
                const UAC_FORMAT *x = &f->Format[i];
                ULONG device = UacPlanChannelMask(x->ChannelConfig, x->Channels);
                if (x->Channels != channels || (ULONG)x->Subslot * 8 != container) continue;
                if (mask != UAC_PCM_NO_MASK && mask && device && mask != device) continue;
                if (!pass ? x->Bits != bits : (capture && x->Bits > bits)) continue;
                for (ULONG k = 0; k < f->RateCount[i]; k++)
                    if (f->Rate[i][k] == rate) return (LONG)i;
            }
        return -1;
    }
    /* Playback may declare fewer valid bits than the device takes (the
     * device ignores the low ones its subslot does not use); capture must
     * declare at least the device's bits, which are delivered unchanged. */
    static BOOLEAN Accept(PVOID context, BOOLEAN capture, ULONG channels,
                          ULONG bits, ULONG container, ULONG rate, ULONG mask) {
        UAC_DIR *d = (UAC_DIR *)context;
        if (d->Capture != capture) return FALSE;
        return Lookup(&d->Formats, capture, channels, bits, container, rate, mask) >= 0;
    }
    static NTSTATUS Run(PVOID context, UacStream *stream) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        NTSTATUS status = STATUS_DEVICE_NOT_READY;
        if (!w->removed && !w->pnpBlocked && !w->powerBlocked && w->dev) {
            const UAC_PCM *pcm = stream->Format();
            LONG i = Lookup(&d->Formats, d->Capture, pcm->Channels, pcm->Bits, pcm->Container,
                            pcm->Rate, pcm->Mask);
            status = STATUS_NOT_SUPPORTED;
            if (d->Usb) status = STATUS_SUCCESS;
            else if (i >= 0)
                status = UacUsbCreate(w->dev, d->Capture, d->Formats.Format[i].Terminal,
                    pcm->Channels, d->Formats.Format[i].Subslot, d->Formats.Format[i].Bits,
                    d->Formats.Format[i].ChannelConfig, pcm->Rate, Fill, Captured, Progress, stream, &d->Usb);
            if (NT_SUCCESS(status)) status = UacUsbStart(d->Usb);
            Record(w->device, 31, status);
        }
        KeReleaseMutex(&w->operation, FALSE);
        return status;
    }
    void Drain(UAC_DIR *d, BOOLEAN destroy) {
        if (d->Usb) {
            UacUsbStop(d->Usb);
            UAC_USB_STATS stats;
            UacUsbGetStats(d->Usb, &stats);
            if (!removed) RecordStats(device, &stats, d->Capture);
            if (destroy) { UacUsbDestroy(d->Usb); d->Usb = NULL; }
        }
    }
    static void Halt(PVOID context, UacStream *) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        w->Drain(d, FALSE);
        KeReleaseMutex(&w->operation, FALSE);
    }
    static void Close(PVOID context, UacStream *) {
        UAC_DIR *d = (UAC_DIR *)context;
        Wave *w = d->Owner;
        /* UacStream's device-associated cleanup guarantees PASSIVE even
         * if its final COM Release arrives at DISPATCH. Never free a
         * callback target before cancel-and-drain has returned. */
        PAGED_CODE();
        KeWaitForSingleObject(&w->operation, Executive, KernelMode, FALSE, NULL);
        w->Drain(d, TRUE);
        d->Closed++;
        if (!w->removed) RecordStreams(w->device, d->Attempts, d->Created, d->Closed, d->Active);
        KeReleaseMutex(&w->operation, FALSE);
    }
    static void Notify(PVOID context, PSERVICEGROUP group) {
        ((UAC_DIR *)context)->Owner->port->Notify(group);
    }
    /* One KSDATARANGE_AUDIO per (format, rate): a device clock offers
     * discrete rates, which a min/max range cannot say. */
    /* The order a format's rates are declared in. Playback: 48 kHz, then
     * 44.1 kHz, then the rest ascending; 98 SE's KMixer opens the device
     * at the first range it can (a 48 kHz client played at 44.1 kHz).
     * Capture stays ascending: with 48 kHz first, a capture-only mono
     * device faulted 98 SE's WDMAud at arrival (wdmaud(07)+2F3C,
     * 2026-10-24, 2 of 2), so capture may open at its lowest rate and
     * KMixer converts. */
    static ULONG RateRank(BOOLEAN capture, ULONG rate) {
        if (capture) return 0;
        return rate == 48000 ? 0 : rate == 44100 ? 1 : 2;
    }
    static void BuildRanges(UAC_DIR_FORMATS *f, BOOLEAN capture) {
        f->Ranges = 0;
        for (ULONG i = 0; i < f->Count; i++)
            for (ULONG pass = 0; pass < 3; pass++)
            for (ULONG k = 0; k < f->RateCount[i] && f->Ranges < UAC_RANGES; k++) {
                if (RateRank(capture, f->Rate[i][k]) != pass) continue;
                KSDATARANGE_AUDIO *r = &f->Range[f->Ranges];
                RtlZeroMemory(r, sizeof(*r));
                r->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
                r->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
                r->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                r->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
                r->MaximumChannels = f->Format[i].Channels;
                r->MinimumBitsPerSample = r->MaximumBitsPerSample = (ULONG)f->Format[i].Subslot * 8;
                r->MinimumSampleFrequency = r->MaximumSampleFrequency = f->Rate[i][k];
                f->RangeList[f->Ranges] = (PKSDATARANGE)r;
                f->RangeFormat[f->Ranges] = i;
                f->Ranges++;
            }
    }
    /* Formats and rates of one direction; rates a planned alternate
     * cannot carry are dropped. */
    void Learn(UAC_DIR *d) {
        UAC_DIR_FORMATS *f = &d->Formats;
        UAC_FORMAT all[UAC_FORMATS];
        ULONG n = UacPlanFormats(descriptor, d->Capture, UacUsbHighSpeed(dev), all, UAC_FORMATS);
        /* One wave pin per direction: the direction's first terminal (an
         * X4's analog output, not its S/PDIF one). */
        UAC_BYTE terminal = (UAC_BYTE)UacPlanTerminal(descriptor, d->Capture, UacUsbHighSpeed(dev));
        f->Count = 0;
        for (ULONG i = 0; i < n; i++) {
            ULONG rates[UAC_USB_RATES], count = 0, kept = 0;
            if (all[i].Subslot < 2) continue; /* 8-bit: see UacStream::Parse */
            if (all[i].Terminal != terminal) continue;
            if (!NT_SUCCESS(UacUsbRates(dev, all[i].Terminal, rates, UAC_USB_RATES, &count))) continue;
            for (ULONG k = 0; k < count; k++) {
                unsigned int index;
                if (UacPlanPick(descriptor, d->Capture, terminal, all[i].Channels, all[i].Subslot,
                                all[i].Bits, all[i].ChannelConfig, rates[k],
                                UacUsbHighSpeed(dev), &index) == UAC_OK)
                    f->Rate[f->Count][kept++] = rates[k];
            }
            if (!kept) continue;
            f->Format[f->Count] = all[i];
            f->RateCount[f->Count] = kept;
            f->Count++;
        }
        BuildRanges(f, d->Capture);
    }
    static void Pin(PCPIN_DESCRIPTOR *p, ULONG instances, ULONG ranges, PKSDATARANGE *list,
                    KSPIN_DATAFLOW flow, KSPIN_COMMUNICATION comm, const GUID *category) {
        RtlZeroMemory(p, sizeof(*p));
        p->MaxGlobalInstanceCount = p->MaxFilterInstanceCount = instances;
        p->KsPinDescriptor.DataRangesCount = ranges;
        p->KsPinDescriptor.DataRanges = list;
        p->KsPinDescriptor.DataFlow = flow;
        p->KsPinDescriptor.Communication = comm;
        p->KsPinDescriptor.Category = category;
    }
public:
    Wave(PDEVICE_OBJECT fdo, PDEVICE_OBJECT target, UAC_DESC *model)
        : refs(1), device(fdo), lower(target), descriptor(model), dev(NULL), graph(NULL),
          port(NULL), removed(FALSE), pnpBlocked(FALSE), powerBlocked(FALSE) {
        KeInitializeMutex(&operation, 0);
        RtlZeroMemory(dir, sizeof(dir));
        dir[0].Owner = dir[1].Owner = this; dir[1].Capture = TRUE;
    }
    ~Wave() {
        /* Close and PnP Shutdown own all USB destruction at PASSIVE. */
        ASSERT(dir[0].Usb == NULL && dir[1].Usb == NULL);
        if (dev) UacUsbClose(dev);
        if (descriptor) {
            ExFreePool((PVOID)descriptor->Bytes);
            ExFreePool(descriptor);
        }
    }
    /* PnP start: open the transport, learn both directions and build the
     * wave filter for what the device has. */
    NTSTATUS Prepare() {
        NTSTATUS s = UacUsbOpen(lower, descriptor, &dev);
        if (!NT_SUCCESS(s)) return s;
        Learn(&dir[0]); Learn(&dir[1]);
        graph = UacGraphFor(dir[0].Formats.Count != 0, dir[1].Formats.Count != 0);
        if (!graph) return STATUS_NOT_SUPPORTED;
        ULONG c = 0;
        categories[c++] = KSCATEGORY_AUDIO;
        BOOLEAN render = dir[0].Formats.Count != 0;
        if (render) categories[c++] = KSCATEGORY_RENDER;
        if (graph->CapturePin >= 0) categories[c++] = KSCATEGORY_CAPTURE;
        if (graph->RenderPin >= 0) {
            /* Capture-only devices keep a stub render pin: no instances,
             * no PCM range (uactopo.c, the 98 SE WDMAud fault). */
            if (render)
                Pin(&pins[graph->RenderPin], 1, dir[0].Formats.Ranges, dir[0].Formats.RangeList,
                    KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO);
            else
                Pin(&pins[graph->RenderPin], 0, 1, bridgeRanges,
                    KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO);
            Pin(&pins[graph->RenderBridge], 0, 1, bridgeRanges,
                KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO);
            RtlZeroMemory(&nodes[graph->Dac], sizeof(nodes[0]));
            nodes[graph->Dac].Type = &KSNODETYPE_DAC;
        }
        if (graph->CapturePin >= 0) {
            Pin(&pins[graph->CapturePin], 1, dir[1].Formats.Ranges, dir[1].Formats.RangeList,
                KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &PINNAME_CAPTURE);
            Pin(&pins[graph->CaptureBridge], 0, 1, bridgeRanges,
                KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO);
            RtlZeroMemory(&nodes[graph->Adc], sizeof(nodes[0]));
            nodes[graph->Adc].Type = &KSNODETYPE_ADC;
        }
        RtlZeroMemory(&filter, sizeof(filter));
        filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
        filter.PinCount = graph->WavePins; filter.Pins = pins;
        filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
        filter.NodeCount = graph->WaveNodes; filter.Nodes = nodes;
        filter.ConnectionCount = graph->WaveLinkCount;
        filter.Connections = (const PCCONNECTION_DESCRIPTOR *)graph->WaveLinks;
        filter.CategoryCount = c; filter.Categories = categories;
        return STATUS_SUCCESS;
    }
    const UAC_GRAPH *Graph() const { return graph; }
    void Shutdown(BOOLEAN permanent) {
        PAGED_CODE();
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        pnpBlocked = TRUE;
        Drain(&dir[0], TRUE); Drain(&dir[1], TRUE);
        if (permanent) removed = TRUE;
        KeReleaseMutex(&operation, FALSE);
    }
    STDMETHODIMP_(void) PowerChangeState(POWER_STATE state);
    STDMETHODIMP QueryPowerChangeState(POWER_STATE state);
    STDMETHODIMP QueryDeviceCapabilities(PDEVICE_CAPABILITIES caps);
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (IsEqualGUID(id, IID_IAdapterPowerManagment)) {
            *out = (IAdapterPowerManagement *)this; AddRef(); return STATUS_SUCCESS;
        }
        if (!IsEqualGUID(id, IID_IUnknown) && !IsEqualGUID(id, IID_IMiniport) &&
            !IsEqualGUID(id, IID_IMiniportWaveCyclic)) return STATUS_INVALID_PARAMETER;
        *out = (PMINIPORTWAVECYCLIC)this; AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) { this->~Wave(); ExFreePool(this); }
        return n;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *out) {
        *out = &filter; return STATUS_SUCCESS;
    }
    /* PortCls's default handler makes only mono/stereo WAVEFORMATEX
     * (DDK, "Default Data Intersection Handlers"): this one answers every
     * declared range (one format at one rate), with WAVEFORMATEXTENSIBLE
     * beyond two channels or 16-bit containers. The client range must
     * admit it: channels, container bits and rate inside its bounds, or a
     * plain/wildcard range. */
    STDMETHODIMP DataRangeIntersection(ULONG pin, PKSDATARANGE range,
        PKSDATARANGE match, ULONG size, PVOID result, PULONG needed) {
        UAC_DIR *d = NULL;
        ULONG k, index = UAC_RANGES;
        *needed = 0;
        if (!graph || !range || !match) return STATUS_NO_MATCH;
        if ((LONG)pin == graph->RenderPin) d = &dir[0];
        else if ((LONG)pin == graph->CapturePin) d = &dir[1];
        else return STATUS_NOT_IMPLEMENTED;
        for (k = 0; k < d->Formats.Ranges; k++)
            if (d->Formats.RangeList[k] == match) { index = k; break; }
        if (index == UAC_RANGES) return STATUS_NO_MATCH;
        const KSDATARANGE_AUDIO *mine = &d->Formats.Range[index];
        const UAC_FORMAT *f = &d->Formats.Format[d->Formats.RangeFormat[index]];
        ULONG channels = f->Channels, container = (ULONG)f->Subslot * 8, rate = mine->MaximumSampleFrequency;
        if (!IsEqualGUID(range->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) &&
            !IsEqualGUID(range->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) return STATUS_NO_MATCH;
        if (!IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
            !IsEqualGUID(range->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) return STATUS_NO_MATCH;
        if (IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
            range->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
            const KSDATARANGE_AUDIO *c = (const KSDATARANGE_AUDIO *)range;
            if (c->MaximumChannels != (ULONG)-1 && c->MaximumChannels < channels) return STATUS_NO_MATCH;
            if (container < c->MinimumBitsPerSample || container > c->MaximumBitsPerSample) return STATUS_NO_MATCH;
            if (rate < c->MinimumSampleFrequency || rate > c->MaximumSampleFrequency) return STATUS_NO_MATCH;
        } else if (!IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD) &&
                   !IsEqualGUID(range->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
            return STATUS_NO_MATCH;
        }
        /* The device's own positions (bmChannelConfig); unassigned (0)
         * when it names none. KMixer mixes into an unassigned layout as
         * silence (98 SE, 8 channels, 2026-10-23). Plain PCM only where it
         * says the same: mono or front pair, 16 valid bits in 16. */
        ULONG positions = UacPlanChannelMask(f->ChannelConfig, channels);
        BOOLEAN extensible = channels > 2 || container != 16 || f->Bits != container ||
                             (positions && positions != (channels == 1 ? 0x4UL : 0x3UL));
        ULONG length = extensible ? sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)
                                  : sizeof(KSDATAFORMAT_WAVEFORMATEX);
        *needed = length;
        if (!size) return STATUS_BUFFER_OVERFLOW;
        if (size < length || !result) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(result, length);
        PKSDATAFORMAT out = (PKSDATAFORMAT)result;
        out->FormatSize = length;
        out->SampleSize = channels * (container / 8);
        out->MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        out->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        out->Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        PWAVEFORMATEX w = (PWAVEFORMATEX)(out + 1);
        w->wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
        w->nChannels = (USHORT)channels;
        w->nSamplesPerSec = rate;
        w->wBitsPerSample = (USHORT)container;
        w->nBlockAlign = (USHORT)(channels * (container / 8));
        w->nAvgBytesPerSec = rate * w->nBlockAlign;
        if (extensible) {
            PWAVEFORMATEXTENSIBLE e = (PWAVEFORMATEXTENSIBLE)w;
            w->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            e->Samples.wValidBitsPerSample = f->Bits;
            e->dwChannelMask = positions;
            e->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        }
        return STATUS_SUCCESS;
    }
    STDMETHODIMP Init(PUNKNOWN adapter, PRESOURCELIST resources, PPORTWAVECYCLIC port) {
        UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(resources);
        this->port = port; return STATUS_SUCCESS;
    }
    STDMETHODIMP NewStream(PMINIPORTWAVECYCLICSTREAM *stream, PUNKNOWN outer,
        POOL_TYPE pool, ULONG pin, BOOLEAN capture, PKSDATAFORMAT format,
        PDMACHANNEL *dma, PSERVICEGROUP *service) {
        UNREFERENCED_PARAMETER(pool);
        *stream = NULL; *dma = NULL; *service = NULL;
        if (outer || !graph) return STATUS_INVALID_PARAMETER;
        if (capture ? (LONG)pin != graph->CapturePin : (LONG)pin != graph->RenderPin)
            return STATUS_INVALID_PARAMETER;
        if (!dir[capture ? 1 : 0].Formats.Count) return STATUS_INVALID_PARAMETER;
        UAC_DIR *d = &dir[capture ? 1 : 0];
        KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
        UAC_STREAM_HOOKS hooks = { d, Run, Halt, Close, Notify, Accept };
        d->Attempts++;
        NTSTATUS status = removed || pnpBlocked || powerBlocked ? STATUS_DEVICE_NOT_READY :
            UacStream::Create((PMINIPORTWAVECYCLIC)this, &d->Active, format,
                stream, dma, service, &hooks, device, capture);
        if (NT_SUCCESS(status)) d->Created++;
        if (!removed) {
            Record(device, 30, status);
            RecordStreams(device, d->Attempts, d->Created, d->Closed, d->Active);
        }
        KeReleaseMutex(&operation, FALSE);
        return status;
    }
};

#pragma code_seg("PAGE")
STDMETHODIMP_(void) Wave::PowerChangeState(POWER_STATE state) {
    PAGED_CODE();
    KeWaitForSingleObject(&operation, Executive, KernelMode, FALSE, NULL);
    powerBlocked = state.DeviceState != PowerDeviceD0;
    if (powerBlocked) { Drain(&dir[0], FALSE); Drain(&dir[1], FALSE); }
    KeReleaseMutex(&operation, FALSE);
}
STDMETHODIMP Wave::QueryPowerChangeState(POWER_STATE state) {
    PAGED_CODE(); UNREFERENCED_PARAMETER(state); return STATUS_SUCCESS;
}
STDMETHODIMP Wave::QueryDeviceCapabilities(PDEVICE_CAPABILITIES caps) {
    PAGED_CODE(); UNREFERENCED_PARAMETER(caps); return STATUS_SUCCESS;
}
#pragma code_seg()

class Topology : public IMiniportTopology {
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    LONG refs;
    PCPIN_DESCRIPTOR pins[4];
    PCNODE_DESCRIPTOR nodes[2];
    GUID categories[2];
    PCFILTER_DESCRIPTOR filter;
    static void Bridge(PCPIN_DESCRIPTOR *p, KSPIN_DATAFLOW flow, const GUID *category) {
        RtlZeroMemory(p, sizeof(*p));
        p->KsPinDescriptor.DataRangesCount = 1;
        p->KsPinDescriptor.DataRanges = bridgeRanges;
        p->KsPinDescriptor.DataFlow = flow;
        p->KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
        p->KsPinDescriptor.Category = category;
    }
public:
    Topology(const UAC_GRAPH *g) : refs(1) {
        RtlZeroMemory(pins, sizeof(pins)); RtlZeroMemory(nodes, sizeof(nodes));
        if (g->TopoRenderIn >= 0) {
            Bridge(&pins[g->TopoRenderIn], KSPIN_DATAFLOW_IN, &KSCATEGORY_AUDIO);
            Bridge(&pins[g->TopoSpeaker], KSPIN_DATAFLOW_OUT, &KSNODETYPE_SPEAKER);
        }
        if (g->TopoMic >= 0) {
            Bridge(&pins[g->TopoMic], KSPIN_DATAFLOW_IN, &KSNODETYPE_MICROPHONE);
            Bridge(&pins[g->TopoCaptureOut], KSPIN_DATAFLOW_OUT, &KSCATEGORY_AUDIO);
        }
        for (ULONG i = 0; i < g->TopoNodes; i++) nodes[i].Type = &KSNODETYPE_SUM;
        categories[0] = KSCATEGORY_AUDIO; categories[1] = KSCATEGORY_TOPOLOGY;
        RtlZeroMemory(&filter, sizeof(filter));
        filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
        filter.PinCount = g->TopoPins; filter.Pins = pins;
        filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
        filter.NodeCount = g->TopoNodes; filter.Nodes = nodes;
        filter.ConnectionCount = g->TopoLinkCount;
        filter.Connections = (const PCCONNECTION_DESCRIPTOR *)g->TopoLinks;
        filter.CategoryCount = 2; filter.Categories = categories;
    }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (!IsEqualGUID(id, IID_IUnknown) && !IsEqualGUID(id, IID_IMiniport) &&
            !IsEqualGUID(id, IID_IMiniportTopology)) return STATUS_INVALID_PARAMETER;
        *out = (PMINIPORTTOPOLOGY)this; AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) { this->~Topology(); ExFreePool(this); }
        return n;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *out) {
        *out = &filter; return STATUS_SUCCESS;
    }
    STDMETHODIMP DataRangeIntersection(ULONG pin, PKSDATARANGE range,
        PKSDATARANGE match, ULONG size, PVOID result, PULONG needed) {
        UNREFERENCED_PARAMETER(pin); UNREFERENCED_PARAMETER(range);
        UNREFERENCED_PARAMETER(match); UNREFERENCED_PARAMETER(size);
        UNREFERENCED_PARAMETER(result); *needed = 0; return STATUS_NOT_IMPLEMENTED;
    }
    STDMETHODIMP Init(PUNKNOWN adapter, PRESOURCELIST resources, PPORTTOPOLOGY port) {
        UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(resources);
        UNREFERENCED_PARAMETER(port); return STATUS_SUCCESS;
    }
};

struct EXT { PDEVICE_OBJECT Pdo, Lower; Wave *Miniport; };
static PDRIVER_DISPATCH PortClsPnp;
static EXT *Extension(PDEVICE_OBJECT fdo) {
    return (EXT *)((PUCHAR)fdo->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

/* Per-instance M0 diagnostics survive a failed START (Code 10), without
 * relying on a kernel debugger. Best effort: diagnostics never change the
 * result of the operation being tested. No device identifiers are recorded.
 */
static void Record(PDEVICE_OBJECT fdo, ULONG stage, NTSTATUS status)
{
    HANDLE key;
    UNICODE_STRING stageName = { 22, 24, L"Uac2M0Stage" };
    UNICODE_STRING statusName = { 24, 26, L"Uac2M0Status" };
    UNICODE_STRING buildName = { 18, 20, L"Uac2Build" };
    ULONG build = 16;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &stageName, 0, REG_DWORD, &stage, sizeof(stage));
        ZwSetValueKey(key, &statusName, 0, REG_DWORD, &status, sizeof(status));
        ZwSetValueKey(key, &buildName, 0, REG_DWORD, &build, sizeof(build));
        ZwClose(key);
    }
}

static void RecordDescriptors(PDEVICE_OBJECT fdo, PUCHAR bytes, ULONG length,
                              ULONG first, ULONG count, ULONG control, ULONG result)
{
    HANDLE key;
    ULONG parse[4] = { first, count, control, result };
    UNICODE_STRING configName = { 24, 26, L"Uac2M0Config" };
    UNICODE_STRING parseName = { 22, 24, L"Uac2M0Parse" };
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &configName, 0, REG_BINARY, bytes, length);
        ZwSetValueKey(key, &parseName, 0, REG_BINARY, parse, sizeof(parse));
        ZwClose(key);
    }
}

static void RecordStats(PDEVICE_OBJECT fdo, UAC_USB_STATS *stats, BOOLEAN capture)
{
    HANDLE key;
    UNICODE_STRING render = { 24, 26, L"Uac2UsbStats" };
    UNICODE_STRING input = { 28, 30, L"Uac2UsbStatsIn" };
    UNICODE_STRING name = capture ? input : render;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &name, 0, REG_BINARY, stats, sizeof(*stats));
        ZwClose(key);
    }
}

static void RecordStreams(PDEVICE_OBJECT fdo, ULONG attempts, ULONG created, ULONG closed, ULONG active)
{
    HANDLE key;
    ULONG counts[4] = { attempts, created, closed, active };
    UNICODE_STRING name = { 22, 24, L"Uac2Streams" };
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Extension(fdo)->Pdo,
            PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) {
        ZwSetValueKey(key, &name, 0, REG_BINARY, counts, sizeof(counts));
        ZwClose(key);
    }
}

static NTSTATUS Descriptor(PDEVICE_OBJECT lower, PVOID buffer, USHORT size)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    KEVENT event;
    IO_STATUS_BLOCK ios;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBufferLength = size;
    urb.TransferBuffer = buffer;
    urb.DescriptorType = USB_CONFIGURATION_DESCRIPTOR_TYPE;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    PIRP irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
        lower, NULL, 0, NULL, 0, TRUE, &event, &ios);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = &urb;
    NTSTATUS s = IoCallDriver(lower, irp);
    if (s == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        s = ios.Status;
    }
    if (NT_SUCCESS(s) && (!USBD_SUCCESS(urb.Hdr.Status) || urb.TransferBufferLength != size))
        s = STATUS_DEVICE_DATA_ERROR;
    return s;
}

static int Hex(WCHAR c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

static NTSTATUS ParseDevice(PDEVICE_OBJECT fdo, UAC_DESC **model)
{
    *model = NULL;
    EXT *ext = Extension(fdo);
    WCHAR ids[256]; ULONG used;
    NTSTATUS s = IoGetDeviceProperty(ext->Pdo, DevicePropertyHardwareID,
                                     sizeof(ids), ids, &used);
    Record(fdo, 1, s);
    if (!NT_SUCCESS(s)) return s;
    int first = -1;
    for (ULONG i = 0; i + 6 < used / sizeof(WCHAR); i++) {
        if (ids[i] == L'&' && ids[i+1] == L'M' && ids[i+2] == L'I' && ids[i+3] == L'_' &&
            Hex(ids[i+4]) >= 0 && Hex(ids[i+5]) >= 0) {
            first = Hex(ids[i+4]) * 16 + Hex(ids[i+5]); break;
        }
    }
    /* No MI number: the whole device is bound (98 SE under NUSB's EHCI,
     * whose hub does not split an IAD device); the function is found in
     * the configuration below (UacWholeFirst). */
    USB_CONFIGURATION_DESCRIPTOR header;
    s = Descriptor(ext->Lower, &header, sizeof(header));
    Record(fdo, 2, s);
    if (!NT_SUCCESS(s)) return s;
    if (header.bLength != sizeof(header) || header.bDescriptorType != 2 ||
        header.wTotalLength < sizeof(header)) return STATUS_DEVICE_DATA_ERROR;
    PUCHAR bytes = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, header.wTotalLength, 'd2aU');
    UAC_DESC *parsed = (UAC_DESC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UAC_DESC), 'p2aU');
    if (!bytes || !parsed) { s = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    s = Descriptor(ext->Lower, bytes, header.wTotalLength);
    Record(fdo, 3, s);
    if (NT_SUCCESS(s)) {
        unsigned int count = 0, control = 256, whole = 256;
        int rc = UAC_OK;
        if (first < 0) {
            rc = UacWholeFirst(bytes, header.wTotalLength, &whole);
            first = (int)whole;
        }
        if (!rc) rc = UacFunction(bytes, header.wTotalLength, first, &control, &count);
        if (!rc) rc = UacParse(bytes, header.wTotalLength, control, first, count, parsed);
        else RtlZeroMemory(parsed, sizeof(*parsed));
        RecordDescriptors(fdo, bytes, header.wTotalLength, first, count, control, rc);
        DbgPrint("uac2wdm: descriptor result %d, PCM alternates %u, skipped %u\n",
                 rc, parsed->AltCount, parsed->Skipped);
        if (rc) s = STATUS_DEVICE_CONFIGURATION_ERROR;
        Record(fdo, 4, s);
    }
done:
    if (NT_SUCCESS(s)) { *model = parsed; parsed = NULL; bytes = NULL; }
    if (parsed) ExFreePool(parsed);
    if (bytes) ExFreePool(bytes);
    return s;
}

static NTSTATUS Start(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources)
{
    EXT *ext = Extension(fdo);
    if (ext->Miniport) {
        ext->Miniport->Shutdown(TRUE);
        ext->Miniport->Release(); ext->Miniport = NULL;
    }
    UAC_DESC *model = NULL;
    NTSTATUS s = ParseDevice(fdo, &model);
    if (!NT_SUCCESS(s)) return s;
    PPORT wave = NULL, topo = NULL;
    void *wm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Wave), 'w2aU');
    void *tm = ExAllocatePoolWithTag(NonPagedPool, sizeof(Topology), 't2aU');
    if (!wm || !tm) {
        if (wm) ExFreePool(wm); if (tm) ExFreePool(tm);
        ExFreePool((PVOID)model->Bytes); ExFreePool(model);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Wave *w = new(wm) Wave(fdo, ext->Lower, model);
    ext->Miniport = w; w->AddRef(); /* explicit adapter/PnP reference */
    Topology *t = NULL;
    s = w->Prepare();
    Record(fdo, 13, s);
    if (NT_SUCCESS(s)) t = new(tm) Topology(w->Graph());
    else ExFreePool(tm);
    const UAC_GRAPH *g = w->Graph();
    if (NT_SUCCESS(s)) { s = PcNewPort(&wave, CLSID_PortWaveCyclic); Record(fdo, 5, s); }
    if (NT_SUCCESS(s)) { s = wave->Init(fdo, irp, (PMINIPORTWAVECYCLIC)w, NULL, resources); Record(fdo, 6, s); }
    if (NT_SUCCESS(s)) { s = PcNewPort(&topo, CLSID_PortTopology); Record(fdo, 7, s); }
    if (NT_SUCCESS(s)) { s = topo->Init(fdo, irp, t, NULL, resources); Record(fdo, 8, s); }
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, L"Wave", wave); Record(fdo, 9, s); }
    if (NT_SUCCESS(s)) { s = PcRegisterSubdevice(fdo, L"Topology", topo); Record(fdo, 10, s); }
    if (NT_SUCCESS(s) && g->RenderBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, wave, g->RenderBridge, topo, g->TopoRenderIn); Record(fdo, 11, s);
    }
    if (NT_SUCCESS(s) && g->CaptureBridge >= 0) {
        s = PcRegisterPhysicalConnection(fdo, topo, g->TopoCaptureOut, wave, g->CaptureBridge); Record(fdo, 14, s);
    }
    if (NT_SUCCESS(s)) { s = PcRegisterAdapterPowerManagement((PMINIPORTWAVECYCLIC)w, fdo); Record(fdo, 12, s); }
    if (!NT_SUCCESS(s)) {
        w->Shutdown(TRUE);
        ext->Miniport = NULL; w->Release();
    }
    w->Release(); if (t) t->Release();
    if (wave) wave->Release(); if (topo) topo->Release();
    DbgPrint("uac2wdm: start %08X\n", s);
    return s;
}

static NTSTATUS Add(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    NTSTATUS s = PcAddAdapterDevice(driver, pdo, Start, 2,
                                    PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(EXT));
    if (!NT_SUCCESS(s)) return s;
    /* Old-target path from the design: no XP-only IoGetLowerDeviceObject.
     * AddDevice runs before PnP starts this stack. */
    PDEVICE_OBJECT lower = pdo, fdo = pdo->AttachedDevice;
    while (fdo && fdo->DriverObject != driver) { lower = fdo; fdo = fdo->AttachedDevice; }
    if (!fdo) return STATUS_NO_SUCH_DEVICE;
    Extension(fdo)->Pdo = pdo;
    Extension(fdo)->Lower = lower;
    Extension(fdo)->Miniport = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchPnp(PDEVICE_OBJECT fdo, PIRP irp)
{
    /* PnP removal/stop IRPs run at PASSIVE. The extension owns a strong
     * Wave reference until this wrapper clears it before PortCls REMOVE.
     * No extension or device access occurs after forwarding REMOVE. */
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    if (stack->MinorFunction == IRP_MN_STOP_DEVICE ||
        stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL ||
        stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        PAGED_CODE();
        EXT *ext = Extension(fdo);
        Wave *w = ext->Miniport;
        if (w) {
            w->Shutdown(stack->MinorFunction != IRP_MN_STOP_DEVICE);
            if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
                ext->Miniport = NULL;
                w->Release();
            }
        }
    }
    return PortClsPnp(fdo, irp);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    NTSTATUS status = PcInitializeAdapterDriver(driver, path, Add);
    if (NT_SUCCESS(status)) {
        PortClsPnp = driver->MajorFunction[IRP_MJ_PNP];
        driver->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    }
    return status;
}
