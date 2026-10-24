/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original PortCls adapter with one IMiniportMidi filter per MIDI port pair
 * (design 5), from the DDK headers and Microsoft's published PortCls, KS
 * and WDM documentation. No sample driver sources used.
 *
 * One x86 binary serves 98 SE to 8: the imports are the ones the Windows 98
 * DDK's import libraries name (build gate). Where NT 5 would use an
 * IoAllocateWorkItem, x86 queues an ExQueueWorkItem whose routine ends in
 * a tail jump to ObfDereferenceObject, so no instruction of this image runs
 * after its device reference is dropped (see Defer below).
 */
#define PC_NEW_NAMES
#include <initguid.h>
#include <portcls.h>
extern "C" {
#include "umcore.h"
#include "umusb.h"
}
inline void *operator new(size_t, void *p) { return p; }

#define UM_BUILD 0x00010006   /* diagnostic marker, bumped with the INF version */

/* ---- filter descriptors: both directions, render only, capture only ---- */
static KSDATARANGE_MUSIC music = {
    {sizeof(KSDATARANGE_MUSIC), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_MUSIC),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_MIDI), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)},
    STATICGUIDOF(KSMUSIC_TECHNOLOGY_PORT), 0, 0, 0xffff
};
static KSDATARANGE bus = {sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_MUSIC),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_MIDI_BUS), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)};
static PKSDATARANGE musicRange[] = {(PKSDATARANGE)&music}, busRange[] = {&bus};
#define PIN_STREAM(flow) {1,1,0,NULL,{0,NULL,0,NULL,1,musicRange,flow,KSPIN_COMMUNICATION_SINK,&KSCATEGORY_AUDIO,NULL,0}}
#define PIN_BRIDGE(flow) {0,0,0,NULL,{0,NULL,0,NULL,1,busRange,flow,KSPIN_COMMUNICATION_NONE,&KSCATEGORY_AUDIO,NULL,0}}
static PCPIN_DESCRIPTOR pinsBoth[] = {
    PIN_STREAM(KSPIN_DATAFLOW_IN), PIN_BRIDGE(KSPIN_DATAFLOW_OUT),
    PIN_BRIDGE(KSPIN_DATAFLOW_IN), PIN_STREAM(KSPIN_DATAFLOW_OUT)
};
static PCPIN_DESCRIPTOR pinsRender[] = {PIN_STREAM(KSPIN_DATAFLOW_IN), PIN_BRIDGE(KSPIN_DATAFLOW_OUT)};
static PCPIN_DESCRIPTOR pinsCapture[] = {PIN_BRIDGE(KSPIN_DATAFLOW_IN), PIN_STREAM(KSPIN_DATAFLOW_OUT)};
static PCCONNECTION_DESCRIPTOR connBoth[] = {{PCFILTER_NODE,0,PCFILTER_NODE,1},{PCFILTER_NODE,2,PCFILTER_NODE,3}};
static PCCONNECTION_DESCRIPTOR connOne[] = {{PCFILTER_NODE,0,PCFILTER_NODE,1}};
static GUID catBoth[] = {STATICGUIDOF(KSCATEGORY_AUDIO),STATICGUIDOF(KSCATEGORY_RENDER),STATICGUIDOF(KSCATEGORY_CAPTURE)};
static GUID catRender[] = {STATICGUIDOF(KSCATEGORY_AUDIO),STATICGUIDOF(KSCATEGORY_RENDER)};
static GUID catCapture[] = {STATICGUIDOF(KSCATEGORY_AUDIO),STATICGUIDOF(KSCATEGORY_CAPTURE)};
static PCFILTER_DESCRIPTOR filters[3] = {
    {0,NULL,sizeof(PCPIN_DESCRIPTOR),4,pinsBoth,sizeof(PCNODE_DESCRIPTOR),0,NULL,2,connBoth,3,catBoth},
    {0,NULL,sizeof(PCPIN_DESCRIPTOR),2,pinsRender,sizeof(PCNODE_DESCRIPTOR),0,NULL,1,connOne,2,catRender},
    {0,NULL,sizeof(PCPIN_DESCRIPTOR),2,pinsCapture,sizeof(PCNODE_DESCRIPTOR),0,NULL,1,connOne,2,catCapture}
};
static const WCHAR proxy[] = L"{17CCA71B-ECD7-11D0-B908-00A0C9223196}";

/* ---- optional trace to the QEMU debug console port (build with UMTRACE) */
#if defined(UMTRACE) && !defined(_WIN64)
static VOID TraceByte(UCHAR c) { __asm { mov al, c } __asm { mov dx, 0xe9 } __asm { out dx, al } }
static VOID Trace(const char *tag, ULONG a, ULONG b)
{
    static const char hex[] = "0123456789abcdef"; ULONG v[2]; int i, k;
    v[0] = a; v[1] = b;
    while (*tag) TraceByte((UCHAR)*tag++);
    for (k = 0; k < 2; k++) {
        TraceByte(' ');
        for (i = 28; i >= 0; i -= 4) TraceByte((UCHAR)hex[(v[k] >> i) & 15]);
    }
    TraceByte('\n');
}
#else
#define Trace(tag, a, b) ((void)0)
#endif

/* ---- diagnostics: own service key and driver key only ---- */
static WCHAR ServicePath[512];
static LONG AddCount, RemoveCount, DestroyCount, ElevatedCount;
static ULONG UnloadPresent, KernelHandle;
static VOID SetDword(HANDLE key, PCWSTR name, ULONG value)
{
    UNICODE_STRING n; RtlInitUnicodeString(&n, name);
    ZwSetValueKey(key, &n, 0, REG_DWORD, &value, sizeof(value));
}
static VOID Lifecycle()
{
    UNICODE_STRING path; OBJECT_ATTRIBUTES attributes; HANDLE key;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !ServicePath[0]) return;
    RtlInitUnicodeString(&path, ServicePath);
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | KernelHandle, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&key, KEY_SET_VALUE, &attributes))) {
        SetDword(key, L"MidiDiagBuild", UM_BUILD);
        SetDword(key, L"MidiAddCount", (ULONG)AddCount);
        SetDword(key, L"MidiRemoveCount", (ULONG)RemoveCount);
        SetDword(key, L"MidiDestroyCount", (ULONG)DestroyCount);
        SetDword(key, L"MidiElevatedCount", (ULONG)ElevatedCount);
        SetDword(key, L"MidiUnloadPresent", UnloadPresent);
        ZwClose(key);
    }
}
static VOID Record(PDEVICE_OBJECT pdo, ULONG stage, NTSTATUS status, UM_USB *usb)
{
    HANDLE key;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(pdo, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &key))) return;
    SetDword(key, L"MidiStage", stage);
    SetDword(key, L"MidiStatus", (ULONG)status);
    if (usb) {
        UM_USB_INFO info; UM_USB_STATS st;
        UmUsbGetInfo(usb, &info); UmUsbGetStats(usb, &st);
        SetDword(key, L"MidiDevice", ((ULONG)info.Vendor << 16) | info.Product);
        SetDword(key, L"MidiPorts", (info.Outputs << 16) | info.Inputs);
        SetDword(key, L"MidiFilters", info.Filters);
        SetDword(key, L"MidiInterface", ((ULONG)info.Interface << 8) | info.Alternate);
        SetDword(key, L"MidiMaxPacket", info.Speed);
        SetDword(key, L"TxBytes", st.TxBytes); SetDword(key, L"RxBytes", st.RxBytes);
        SetDword(key, L"TxUrbs", st.TxUrbs); SetDword(key, L"RxUrbs", st.RxUrbs);
        SetDword(key, L"BadEvents", st.BadEvents); SetDword(key, L"Overruns", st.Overruns);
        SetDword(key, L"Timeouts", st.Timeouts); SetDword(key, L"UsbLastStatus", (ULONG)st.LastStatus);
        SetDword(key, L"Stalls", st.Stalls); SetDword(key, L"UsbErrors", st.Errors);
    }
    ZwClose(key);
}

/* ---- passive destruction (design 5.2 lifetime) ----
 * Final Release at PASSIVE_LEVEL destroys inline. Above it, destruction is
 * queued to a system worker thread with a reference on the FDO, which keeps
 * the device object and hence this image (a pending-delete device's driver
 * is not unloaded) until the reference is dropped. x64 (XP x64 and later)
 * has IoQueueWorkItem for this. x86 must also load on 98 SE, which has no
 * IoAllocateWorkItem (Microsoft documents it as unavailable on Windows 98),
 * so the routine is a naked stub: it runs the destruction, then jumps to
 * ObfDereferenceObject with the worker's own return address on the stack,
 * so the final dereference returns straight to the system and no code of
 * this image executes after it. */
typedef VOID (*UM_RUN)(PVOID context);
typedef struct UM_DEFER {
#ifdef _WIN64
    PIO_WORKITEM Item;
#else
    WORK_QUEUE_ITEM Item;
#endif
    PDEVICE_OBJECT Device; UM_RUN Run; PVOID Context;
} UM_DEFER;
static NTSTATUS DeferInit(UM_DEFER *d, PDEVICE_OBJECT fdo, UM_RUN run, PVOID context)
{
#ifdef _WIN64
    d->Item = IoAllocateWorkItem(fdo);
    if (!d->Item) return STATUS_INSUFFICIENT_RESOURCES;
#endif
    ObReferenceObject(fdo);
    d->Device = fdo; d->Run = run; d->Context = context;
    return STATUS_SUCCESS;
}
#ifdef _WIN64
static VOID DeferRoutine(PDEVICE_OBJECT device, PVOID context)
{
    UM_DEFER *d = (UM_DEFER *)context; PIO_WORKITEM item = d->Item;
    PDEVICE_OBJECT held = d->Device; UM_RUN run = d->Run; PVOID object = d->Context;
    UNREFERENCED_PARAMETER(device);
    run(object);
    IoFreeWorkItem(item);
    ObDereferenceObject(held);   /* IoQueueWorkItem holds its own reference */
}
#else
static PVOID DerefTarget;   /* ObfDereferenceObject, set in DriverEntry */
extern "C" PDEVICE_OBJECT __stdcall DeferBody(PVOID context)
{
    UM_DEFER *d = (UM_DEFER *)context;
    PDEVICE_OBJECT held = d->Device; UM_RUN run = d->Run; PVOID object = d->Context;
    run(object);                 /* frees the object holding d */
    return held;
}
static __declspec(naked) VOID __stdcall DeferStub(PVOID context)
{
    __asm {
        push dword ptr [esp+4]
        call DeferBody           ; eax = device to dereference
        mov ecx, eax             ; fastcall argument
        pop edx                  ; worker's return address
        add esp, 4               ; drop our stdcall argument
        push edx
        jmp dword ptr [DerefTarget]
    }
}
#endif
static VOID DeferNow(UM_DEFER *d)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        PDEVICE_OBJECT held = d->Device; UM_RUN run = d->Run; PVOID object = d->Context;
#ifdef _WIN64
        PIO_WORKITEM item = d->Item;
#endif
        run(object);
#ifdef _WIN64
        IoFreeWorkItem(item);
#endif
        ObDereferenceObject(held);
        return;
    }
    InterlockedIncrement(&ElevatedCount);
#ifdef _WIN64
    IoQueueWorkItem(d->Item, DeferRoutine, DelayedWorkQueue, d);
#else
    ExInitializeWorkItem(&d->Item, DeferStub, d);
    ExQueueWorkItem(&d->Item, DelayedWorkQueue);
#endif
}

/* ---- the device: owns the USB transport; adapter power management ---- */
class Device : public IAdapterPowerManagement {
    LONG refs; UM_DEFER defer; BOOLEAN dead; KMUTEX lock;
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    static VOID Destroy(PVOID p) { ((Device *)p)->~Device(); ExFreePool(p); }
public:
    UM_USB *usb; PDEVICE_OBJECT fdo, pdo;
    Device(PDEVICE_OBJECT f, PDEVICE_OBJECT p) : refs(1), dead(FALSE), usb(NULL), fdo(f), pdo(p) {
        RtlZeroMemory(&defer, sizeof(defer)); KeInitializeMutex(&lock, 0);
    }
    ~Device() {
        if (usb) UmUsbDestroy(usb);
        InterlockedIncrement(&DestroyCount); Lifecycle();
    }
    NTSTATUS Prepare(PDEVICE_OBJECT lower) {
        NTSTATUS status = DeferInit(&defer, fdo, Destroy, this);
        if (!NT_SUCCESS(status)) return status;
        return UmUsbCreate(lower, &usb);
    }
    VOID Shutdown() {
        PAGED_CODE();
        KeWaitForSingleObject(&lock, Executive, KernelMode, FALSE, NULL);
        if (!dead && usb) { dead = TRUE; UmUsbStop(usb); Record(pdo, 20, STATUS_SUCCESS, usb); }
        KeReleaseMutex(&lock, FALSE);
    }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IAdapterPowerManagment))
            *out = (IAdapterPowerManagement *)this;
        else return STATUS_INVALID_PARAMETER;
        AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            if (defer.Device) DeferNow(&defer);
            else { this->~Device(); ExFreePool(this); }
        }
        return n;
    }
    STDMETHODIMP_(void) PowerChangeState(POWER_STATE state) {
        PAGED_CODE();
        Trace(">power", state.DeviceState, dead);
        KeWaitForSingleObject(&lock, Executive, KernelMode, FALSE, NULL);
        if (!dead && usb) UmUsbPower(usb, (BOOLEAN)(state.DeviceState == PowerDeviceD0));
        KeReleaseMutex(&lock, FALSE);
        Trace("<power", state.DeviceState, 0);
    }
    STDMETHODIMP QueryPowerChangeState(POWER_STATE state) {
        UNREFERENCED_PARAMETER(state); return STATUS_SUCCESS;
    }
    STDMETHODIMP QueryDeviceCapabilities(PDEVICE_CAPABILITIES caps) {
        UNREFERENCED_PARAMETER(caps); return STATUS_SUCCESS;
    }
};

static NTSTATUS Format(PKSDATAFORMAT f)
{
    if (!f || f->FormatSize < sizeof(KSDATAFORMAT) ||
        !IsEqualGUID(f->MajorFormat, KSDATAFORMAT_TYPE_MUSIC) ||
        !IsEqualGUID(f->SubFormat, KSDATAFORMAT_SUBTYPE_MIDI) ||
        !IsEqualGUID(f->Specifier, KSDATAFORMAT_SPECIFIER_NONE)) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

class Mini;
class Stream : public IMiniportMidiStream {
    LONG refs; Mini *owner; BOOLEAN capture; ULONG port; UM_DEFER defer;
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    static VOID Destroy(PVOID p) { ((Stream *)p)->~Stream(); ExFreePool(p); }
public:
    Stream(Mini *o, BOOLEAN c, ULONG p);
    ~Stream();
    NTSTATUS Prepare(PDEVICE_OBJECT fdo) { return DeferInit(&defer, fdo, Destroy, this); }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (!IsEqualGUID(id, IID_IUnknown) && !IsEqualGUID(id, IID_IMiniportMidiStream)) return STATUS_INVALID_PARAMETER;
        *out = (PMINIPORTMIDISTREAM)this; AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            if (defer.Device) DeferNow(&defer);
            else { this->~Stream(); ExFreePool(this); }
        }
        return n;
    }
    STDMETHODIMP SetFormat(PKSDATAFORMAT f) { return Format(f); }
    STDMETHODIMP SetState(KSSTATE s);
    STDMETHODIMP Read(PVOID bytes, ULONG size, PULONG used);
    STDMETHODIMP Write(PVOID bytes, ULONG size, PULONG used);
};

/* One filter: output port i and/or input port i of the device. */
class Mini : public IMiniportMidi {
    LONG refs, active[2]; PPORTMIDI port; PSERVICEGROUP group; UM_DEFER defer;
    ULONG index; ULONG kind;    /* 0 both, 1 render only, 2 capture only */
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    static VOID Destroy(PVOID p) { ((Mini *)p)->~Mini(); ExFreePool(p); }
    static VOID Notify(PVOID context) {
        Mini *m = (Mini *)context;
        if (m->port && m->group) m->port->Notify(m->group);
    }
public:
    Device *device;
    Mini(Device *d, ULONG i) : refs(1), port(NULL), group(NULL), index(i), device(d) {
        active[0] = active[1] = 0; d->AddRef(); RtlZeroMemory(&defer, sizeof(defer));
        BOOLEAN out = UmUsbHasPort(d->usb, FALSE, i), in = UmUsbHasPort(d->usb, TRUE, i);
        kind = out && in ? 0 : out ? 1 : 2;
    }
    ~Mini() {
        UmUsbBind(device->usb, FALSE, index, NULL, NULL);
        UmUsbBind(device->usb, TRUE, index, NULL, NULL);
        if (group) group->Release();
        device->Release();
    }
    NTSTATUS Prepare() { return DeferInit(&defer, device->fdo, Destroy, this); }
    ULONG Index() { return index; }
    const GUID *Categories(ULONG *count) {
        *count = filters[kind].CategoryCount; return filters[kind].Categories;
    }
    ULONG RenderPin() { return kind == 2 ? ~0u : 0; }
    ULONG CapturePin() { return kind == 0 ? 3 : kind == 2 ? 1 : ~0u; }
    VOID Closed(BOOLEAN capture) { InterlockedExchange(&active[capture ? 1 : 0], 0); }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        *out = NULL;
        if (IsEqualGUID(id, IID_IUnknown) || IsEqualGUID(id, IID_IMiniport) || IsEqualGUID(id, IID_IMiniportMidi))
            *out = (PMINIPORTMIDI)this;
        else return STATUS_INVALID_PARAMETER;
        AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            if (defer.Device) DeferNow(&defer);
            else { this->~Mini(); ExFreePool(this); }
        }
        return n;
    }
    STDMETHODIMP GetDescription(PPCFILTER_DESCRIPTOR *out) {
        Trace("desc", index, kind); *out = &filters[kind]; return STATUS_SUCCESS;
    }
    STDMETHODIMP DataRangeIntersection(ULONG pin, PKSDATARANGE a, PKSDATARANGE b, ULONG size, PVOID result, PULONG needed) {
        NTSTATUS status = Intersect(pin, a, b, size, result, needed);
        Trace("dri", (index << 16) | pin, (ULONG)status);
        Trace("dri-a", a ? a->SubFormat.Data1 : 0, a ? a->FormatSize : 0);
        return status;
    }
    NTSTATUS Intersect(ULONG pin, PKSDATARANGE a, PKSDATARANGE b, ULONG size, PVOID result, PULONG needed) {
        if (!needed) return STATUS_INVALID_PARAMETER;
        *needed = 0;
        if (pin >= filters[kind].PinCount || !a || !b || a->FormatSize < sizeof(KSDATARANGE) ||
            b->FormatSize < sizeof(KSDATARANGE)) return STATUS_NO_MATCH;
        BOOLEAN stream = (BOOLEAN)(pin == RenderPin() || pin == CapturePin());
        PKSDATARANGE own = stream ? (PKSDATARANGE)&music : &bus;
        if ((!IsEqualGUID(a->MajorFormat, own->MajorFormat) && !IsEqualGUID(a->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) ||
            (!IsEqualGUID(a->SubFormat, own->SubFormat) && !IsEqualGUID(a->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) ||
            (!IsEqualGUID(a->Specifier, own->Specifier) && !IsEqualGUID(a->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD)) ||
            !IsEqualGUID(b->MajorFormat, own->MajorFormat) || !IsEqualGUID(b->SubFormat, own->SubFormat) ||
            !IsEqualGUID(b->Specifier, own->Specifier)) return STATUS_NO_MATCH;
        if (stream && a->FormatSize >= sizeof(KSDATARANGE_MUSIC)) {
            PKSDATARANGE_MUSIC client = (PKSDATARANGE_MUSIC)a;
            if (!IsEqualGUID(client->Technology, KSMUSIC_TECHNOLOGY_PORT) &&
                !IsEqualGUID(client->Technology, GUID_NULL)) return STATUS_NO_MATCH;
        }
        *needed = sizeof(KSDATAFORMAT);
        if (!size) return STATUS_BUFFER_OVERFLOW;
        if (size < *needed) return STATUS_BUFFER_TOO_SMALL;
        if (!result) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(result, own, sizeof(KSDATAFORMAT));
        ((PKSDATAFORMAT)result)->FormatSize = sizeof(KSDATAFORMAT);
        return STATUS_SUCCESS;
    }
    STDMETHODIMP Init(PUNKNOWN adapter, PRESOURCELIST resources, PPORTMIDI p, PSERVICEGROUP *out) {
        UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(resources);
        *out = NULL;
        Trace("init", index, kind);
        NTSTATUS status = PcNewServiceGroup(&group, NULL);
        if (!NT_SUCCESS(status)) return status;
        port = p;   /* borrowed: the port owns this miniport */
        port->RegisterServiceGroup(group);
        UmUsbBind(device->usb, FALSE, index, Notify, this);
        UmUsbBind(device->usb, TRUE, index, Notify, this);
        *out = group; group->AddRef();
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(void) Service() { /* rings are filled by the transport */ }
    STDMETHODIMP NewStream(PMINIPORTMIDISTREAM *out, PUNKNOWN outer, POOL_TYPE pool, ULONG pin,
                           BOOLEAN capture, PKSDATAFORMAT format, PSERVICEGROUP *service) {
        PAGED_CODE(); UNREFERENCED_PARAMETER(pool);
        *out = NULL; *service = NULL;
        NTSTATUS status = Format(format);
        Trace("newstream", (index << 16) | pin, capture);
        if (outer || pin != (capture ? CapturePin() : RenderPin()) || !NT_SUCCESS(status))
            return STATUS_INVALID_PARAMETER;
        if (InterlockedCompareExchange(&active[capture ? 1 : 0], 1, 0)) return STATUS_DEVICE_BUSY;
        PVOID memory = ExAllocatePoolWithTag(NonPagedPool, sizeof(Stream), 'sImU');
        if (!memory) { Closed(capture); return STATUS_INSUFFICIENT_RESOURCES; }
        Stream *s = new(memory) Stream(this, capture, index);
        status = s->Prepare(device->fdo);
        if (!NT_SUCCESS(status)) { s->Release(); return status; }
        *out = s; *service = group; group->AddRef();
        Record(device->pdo, 10, STATUS_SUCCESS, NULL);
        return STATUS_SUCCESS;
    }
};

Stream::Stream(Mini *o, BOOLEAN c, ULONG p) : refs(1), owner(o), capture(c), port(p)
{
    RtlZeroMemory(&defer, sizeof(defer)); owner->AddRef();
}
Stream::~Stream()
{
    UmUsbState(owner->device->usb, capture, port, KSSTATE_STOP);
    owner->Closed(capture); owner->Release();
}
STDMETHODIMP Stream::SetState(KSSTATE s)
{
    Trace("state", (port << 16) | capture, (ULONG)s);
    if ((ULONG)s > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    return UmUsbState(owner->device->usb, capture, port, s);
}
STDMETHODIMP Stream::Read(PVOID bytes, ULONG size, PULONG used)
{
    if (!used) return STATUS_INVALID_PARAMETER;
    *used = 0;
    if (!capture || (size && !bytes)) return STATUS_INVALID_DEVICE_REQUEST;
    return UmUsbRead(owner->device->usb, port, bytes, size, used);
}
STDMETHODIMP Stream::Write(PVOID bytes, ULONG size, PULONG used)
{
    if (!used) return STATUS_INVALID_PARAMETER;
    *used = 0;
    if (capture || (size && !bytes)) return STATUS_INVALID_DEVICE_REQUEST;
    return UmUsbWrite(owner->device->usb, port, bytes, size, used);
}

/* ---- PnP ---- */
typedef struct EXT { PDEVICE_OBJECT Pdo, Lower; Device *Adapter; } EXT;
static EXT *Extension(PDEVICE_OBJECT fdo)
{
    return (EXT *)((PUCHAR)fdo->DeviceExtension + PORT_CLASS_DEVICE_EXTENSION_SIZE);
}
static PDRIVER_DISPATCH PortPnp;

/* Pre-register the filter's interfaces to set its own FriendlyName and the
 * KS proxy CLSID (design 5.4); PcRegisterSubdevice registers the same
 * interfaces (same reference string) and enables them. */
static VOID NameFilter(PDEVICE_OBJECT pdo, PCWSTR reference, const GUID *cats, ULONG count, const WCHAR *name)
{
    UNICODE_STRING ref, value; ULONG i;
    RtlInitUnicodeString(&ref, reference);
    for (i = 0; i < count; i++) {
        UNICODE_STRING link; HANDLE key;
        RtlZeroMemory(&link, sizeof(link));
        if (!NT_SUCCESS(IoRegisterDeviceInterface(pdo, (LPGUID)&cats[i], &ref, &link))) continue;
        if (NT_SUCCESS(IoOpenDeviceInterfaceRegistryKey(&link, KEY_ALL_ACCESS, &key))) {
            ULONG n = 0;
            while (name[n] && n < 31) n++;
            RtlInitUnicodeString(&value, L"FriendlyName");
            ZwSetValueKey(key, &value, 0, REG_SZ, (PVOID)name, (n + 1) * sizeof(WCHAR));
            RtlInitUnicodeString(&value, L"CLSID");
            ZwSetValueKey(key, &value, 0, REG_SZ, (PVOID)proxy, sizeof(proxy));
            ZwClose(key);
        }
        RtlFreeUnicodeString(&link);
    }
}

/* PortCls keeps the subdevice name pointer (a create item name), so the
 * names are static; the reference string is also the interface's. */
static const PWCHAR SubdeviceName[UM_MAX_PORTS] = {
    L"Midi00",L"Midi01",L"Midi02",L"Midi03",L"Midi04",L"Midi05",L"Midi06",L"Midi07",
    L"Midi08",L"Midi09",L"Midi10",L"Midi11",L"Midi12",L"Midi13",L"Midi14",L"Midi15",
    L"Midi16",L"Midi17",L"Midi18",L"Midi19",L"Midi20",L"Midi21",L"Midi22",L"Midi23",
    L"Midi24",L"Midi25",L"Midi26",L"Midi27",L"Midi28",L"Midi29",L"Midi30",L"Midi31"
};

static NTSTATUS Start(PDEVICE_OBJECT fdo, PIRP irp, PRESOURCELIST resources)
{
    EXT *ext = Extension(fdo); NTSTATUS status; ULONG i; UM_USB_INFO info;
    WCHAR name[32];
    PAGED_CODE();
    if (ext->Adapter) return STATUS_INVALID_DEVICE_STATE;   /* no restart after STOP */
    PVOID memory = ExAllocatePoolWithTag(NonPagedPool, sizeof(Device), 'dImU');
    if (!memory) return STATUS_INSUFFICIENT_RESOURCES;
    Device *d = new(memory) Device(fdo, ext->Pdo);
    status = d->Prepare(ext->Lower);
    Record(ext->Pdo, 1, status, NULL);
    if (!NT_SUCCESS(status)) { d->Release(); return status; }
    UmUsbGetInfo(d->usb, &info);
    for (i = 0; i < info.Filters && NT_SUCCESS(status); i++) {
        PPORT port = NULL; ULONG count; const GUID *cats;
        memory = ExAllocatePoolWithTag(NonPagedPool, sizeof(Mini), 'mImU');
        if (!memory) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
        Mini *m = new(memory) Mini(d, i);
        status = m->Prepare();
        if (NT_SUCCESS(status)) status = PcNewPort(&port, CLSID_PortMidi);
        if (NT_SUCCESS(status)) status = port->Init(fdo, irp, (PMINIPORTMIDI)m, NULL, resources);
        if (NT_SUCCESS(status)) {
            UmUsbFilterName(d->usb, i, name);
            cats = m->Categories(&count);
            NameFilter(ext->Pdo, SubdeviceName[i], cats, count, name);
            Trace("subdevice", i, 0);
            status = PcRegisterSubdevice(fdo, SubdeviceName[i], port);
        }
        if (port) port->Release();
        m->Release();
    }
    Record(ext->Pdo, 2, status, d->usb);
    if (NT_SUCCESS(status)) status = PcRegisterAdapterPowerManagement((PUNKNOWN)(IAdapterPowerManagement *)d, fdo);
    if (NT_SUCCESS(status)) status = UmUsbStart(d->usb);
    Record(ext->Pdo, NT_SUCCESS(status) ? 5 : 3, status, d->usb);
    Trace("start", info.Filters, (ULONG)status);
    if (NT_SUCCESS(status)) ext->Adapter = d;
    else { d->Shutdown(); d->Release(); }
    return status;
}

static NTSTATUS Add(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    InterlockedIncrement(&AddCount); Lifecycle();
    NTSTATUS status = PcAddAdapterDevice(driver, pdo, Start, UM_MAX_PORTS, PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(EXT));
    if (!NT_SUCCESS(status)) return status;
    PDEVICE_OBJECT lower = pdo, fdo = pdo->AttachedDevice;
    while (fdo && fdo->DriverObject != driver) { lower = fdo; fdo = fdo->AttachedDevice; }
    if (!fdo) return STATUS_NO_SUCH_DEVICE;
    Extension(fdo)->Pdo = pdo; Extension(fdo)->Lower = lower; Extension(fdo)->Adapter = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS Pnp(PDEVICE_OBJECT fdo, PIRP irp)
{
    UCHAR minor = IoGetCurrentIrpStackLocation(irp)->MinorFunction;
    if (minor == IRP_MN_QUERY_STOP_DEVICE) {
        /* No resources to rebalance and no restart path: refuse the stop. */
        irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_UNSUCCESSFUL;
    }
    if (minor == IRP_MN_SURPRISE_REMOVAL) {
        /* The transport stops now. PortCls is not told: once it sees a
         * surprise removal it fails every later request on the device,
         * cleanup and close included (observed on Windows 2000), so a client
         * with open MIDI streams can never close them and the REMOVE that
         * follows the last close never comes. Its streams keep working
         * against the stopped transport (writes fail, reads return nothing,
         * leaving RUN succeeds) until REMOVE, which PortCls does see. */
        Device *d = Extension(fdo)->Adapter;
        if (d) d->Shutdown();
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        return IoCallDriver(Extension(fdo)->Lower, irp);
    }
    if (minor == IRP_MN_STOP_DEVICE || minor == IRP_MN_REMOVE_DEVICE) {
        Device *d = Extension(fdo)->Adapter;
        if (minor == IRP_MN_REMOVE_DEVICE) { InterlockedIncrement(&RemoveCount); Lifecycle(); }
        Trace("pnp-stop", minor, d != NULL);
        if (d) {
            d->Shutdown();
            Trace("pnp-shut", minor, 0);
            if (minor == IRP_MN_REMOVE_DEVICE) { Extension(fdo)->Adapter = NULL; ULONG left = d->Release(); Trace("pnp-rel", left, 0); UNREFERENCED_PARAMETER(left); }
        }
    }
    return PortPnp(fdo, irp);   /* no extension access after REMOVE */
}

#if defined(UMTRACE) && !defined(_WIN64)
static PDRIVER_UNLOAD SavedUnload;
static VOID TraceUnload(PDRIVER_OBJECT driver)
{
    Trace(">unload", 0, 0);
    SavedUnload(driver);
    Trace("<unload", 0, 0);
}
static PDRIVER_DISPATCH Saved[IRP_MJ_MAXIMUM_FUNCTION + 1];
static NTSTATUS TraceDispatch(PDEVICE_OBJECT fdo, PIRP irp)
{
    PIO_STACK_LOCATION st = IoGetCurrentIrpStackLocation(irp);
    UCHAR major = st->MajorFunction; NTSTATUS status;
    ULONG detail = major == IRP_MJ_DEVICE_CONTROL ? st->Parameters.DeviceIoControl.IoControlCode : st->MinorFunction;
    ULONG extra = 0;
    if (major == IRP_MJ_DEVICE_CONTROL && st->Parameters.DeviceIoControl.InputBufferLength >= 24 &&
        st->Parameters.DeviceIoControl.Type3InputBuffer) {
        PULONG in = (PULONG)st->Parameters.DeviceIoControl.Type3InputBuffer;
        extra = (in[0] & 0xffff0000) | ((in[4] & 0xff) << 8) | (in[5] & 0xff); /* set Data1 hi, Id, Flags */
    }
    Trace(">irp", ((ULONG)major << 24) | (detail & 0xffffff), extra);
    status = Saved[major](fdo, irp);
    Trace("<irp", ((ULONG)major << 24) | (detail & 0xffffff), (ULONG)status);
    return status;
}
#endif
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    if (path->Length < sizeof(ServicePath)) {
        RtlCopyMemory(ServicePath, path->Buffer, path->Length);
        ServicePath[path->Length / sizeof(WCHAR)] = 0;
    }
    /* OBJ_KERNEL_HANDLE is an NT 5 attribute (WDM 1.10, Windows 2000). */
    KernelHandle = IoIsWdmVersionAvailable(1, 0x10) ? OBJ_KERNEL_HANDLE : 0;
#ifndef _WIN64
    DerefTarget = (PVOID)ObfDereferenceObject;
#endif
    NTSTATUS status = PcInitializeAdapterDriver(driver, path, Add);
    Trace("entry", UM_BUILD, (ULONG)status);
    UnloadPresent = driver->DriverUnload != NULL;
    Lifecycle();
    if (NT_SUCCESS(status)) {
        PortPnp = driver->MajorFunction[IRP_MJ_PNP];
        driver->MajorFunction[IRP_MJ_PNP] = Pnp;
#if defined(UMTRACE) && !defined(_WIN64)
        {
            ULONG i;
            for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
                Saved[i] = driver->MajorFunction[i];
                driver->MajorFunction[i] = TraceDispatch;
            }
            SavedUnload = driver->DriverUnload;
            if (SavedUnload) driver->DriverUnload = TraceUnload;
        }
#endif
    }
    return status;
}
