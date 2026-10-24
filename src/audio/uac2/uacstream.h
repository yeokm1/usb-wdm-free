/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original stopped-stream/IDmaChannel implementation from portcls.h and
 * published IMiniportWaveCyclic::NewStream / DMA Channel Objects contracts.
 * Positions advance only through explicit USB-completion hooks, never time.
 * One PCM format per stream (any channels, 16/24/32-bit containers, any
 * rate the owner accepts); render streams are filled from the cyclic
 * buffer, capture streams write into it.
 */
#ifndef UAC2_STREAM_H
#define UAC2_STREAM_H

class UacStream;
struct UAC_STREAM_HOOKS {
    PVOID Context;
    NTSTATUS (*Start)(PVOID context, UacStream *stream);
    void (*Stop)(PVOID context, UacStream *stream);
    void (*Close)(PVOID context, UacStream *stream);
    void (*Notify)(PVOID context, PSERVICEGROUP group);
    /* Optional: whether the device runs this format; NULL keeps the first
     * build's single format (2 channels, 16 bits, 48000 Hz). */
    BOOLEAN (*Accept)(PVOID context, BOOLEAN capture, ULONG channels,
                      ULONG bits, ULONG container, ULONG rate, ULONG mask);
};
/* UAC_PCM.Mask of a plain WAVEFORMATEX (no positions declared). */
#define UAC_PCM_NO_MASK 0xffffffffUL
/* The PCM format a stream carries (container = bits per sample slot). */
struct UAC_PCM {
    ULONG Channels, Bits, Container, Rate, BlockAlign, BytesPerSecond;
    ULONG Mask; /* dwChannelMask, or UAC_PCM_NO_MASK */
};

class UacStream : public IMiniportWaveCyclicStream, public IDmaChannel {
    enum { MinCapacity = 16384, MaxCapacity = 1048576 };
    ULONG Capacity, BlockAlign, BytesPerSecond;
    UAC_PCM pcm;
    BOOLEAN capture;
    LONG refs;
    PUNKNOWN owner;
    LONG *active;
    PSERVICEGROUP group;
    PUCHAR buffer;
    ULONG used;
    KSSTATE state;
    KSPIN_LOCK ringLock;
    ULONG filled, completed, packetErrors;
    ULONG notifyBytes, pendingNotify;
    UAC_STREAM_HOOKS hooks;
    PIO_WORKITEM cleanupWork;
    PDEVICE_OBJECT cleanupDevice;
    BOOLEAN published;

    UacStream(PUNKNOWN parent, LONG *gate, const UAC_PCM *format, BOOLEAN input)
        : Capacity(MinCapacity), BlockAlign(format->BlockAlign), BytesPerSecond(format->BytesPerSecond),
          pcm(*format), capture(input),
          refs(1), owner(parent), active(gate), group(NULL), buffer(NULL),
          used(MinCapacity), state(KSSTATE_STOP), filled(0), completed(0), packetErrors(0),
          notifyBytes(format->BytesPerSecond / 100), pendingNotify(0), cleanupWork(NULL), cleanupDevice(NULL), published(FALSE) {
        /* About 62 ms of the format, whole frames, 16 KiB to 1 MiB. */
        ULONG want = BytesPerSecond / 16;
        if (want < MinCapacity) want = MinCapacity;
        if (want > MaxCapacity) want = MaxCapacity;
        Capacity = want - want % BlockAlign;
        used = Capacity;
        RtlZeroMemory(&hooks, sizeof(hooks));
        KeInitializeSpinLock(&ringLock);
        owner->AddRef();
    }
    ~UacStream() {
        /* The owner must drain every transport callback in Close before
         * this context, its ring, or its service group can disappear. */
        if (hooks.Close) hooks.Close(hooks.Context, this);
        if (group) group->Release();
        if (buffer) ExFreePool(buffer);
        InterlockedExchange(active, 0);
        owner->Release();
    }
    BOOLEAN Contains(PVOID address, ULONG count) {
        ULONG_PTR begin = (ULONG_PTR)buffer;
        ULONG_PTR p = (ULONG_PTR)address;
        return buffer && p >= begin && p - begin <= used &&
               count <= used - (ULONG)(p - begin);
    }
    static void operator delete(PVOID memory) { ExFreePool(memory); }
    void DestroyPassive() {
        ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
        PIO_WORKITEM work = cleanupWork;
        PDEVICE_OBJECT device = cleanupDevice;
        this->~UacStream(); ExFreePool(this);
        if (work) IoFreeWorkItem(work);
        if (device) ObDereferenceObject(device);
    }
    static VOID Cleanup(PDEVICE_OBJECT device, PVOID context) {
        UNREFERENCED_PARAMETER(device);
        ((UacStream *)context)->DestroyPassive();
    }
    /* 64-by-32 division with 32-bit limbs: VC6 would otherwise call CRT
     * helpers (_aulldiv) the kernel image does not carry. */
    static void Divide(ULONG high, ULONG low, ULONG divisor, ULONG *qhigh, ULONG *qlow, ULONG *rest) {
        ULONG r = 0, qh = 0, ql = 0;
        for (ULONG bit = 0; bit < 64; ++bit) {
            ULONG carry = r >> 31;
            r = (r << 1) | (high >> 31);
            high = (high << 1) | (low >> 31); low <<= 1;
            qh = (qh << 1) | (ql >> 31); ql <<= 1;
            if (carry || r >= divisor) { r -= divisor; ql |= 1; }
        }
        *qhigh = qh; *qlow = ql; *rest = r;
    }
    /* bytes -> 100 ns units: bytes * 10^7 / BytesPerSecond. FALSE if the
     * result does not fit a LONGLONG. */
    static BOOLEAN Normalize(LARGE_INTEGER value, ULONG bytesPerSecond, LARGE_INTEGER *out) {
        ULONG qhigh, qlow, rest;
        if (value.HighPart < 0 || !bytesPerSecond) return FALSE;
        Divide((ULONG)value.HighPart, value.LowPart, bytesPerSecond, &qhigh, &qlow, &rest);
        /* quotient * 10^7 with add-and-shift of 32-bit limbs, checked. */
        ULONG multiplier = 10000000, resultHigh = 0, resultLow = 0;
        while (multiplier) {
            if (multiplier & 1) {
                ULONG oldLow = resultLow, oldHigh = resultHigh;
                resultLow += qlow;
                resultHigh += qhigh + (resultLow < oldLow ? 1 : 0);
                if (resultHigh < oldHigh || resultHigh > 0x7fffffff) return FALSE;
            }
            if (qhigh & 0x80000000) { if (multiplier >> 1) return FALSE; }
            qhigh = (qhigh << 1) | (qlow >> 31); qlow <<= 1;
            multiplier >>= 1;
        }
        /* rest < bytesPerSecond < 2^25, so rest * 10^7 < 2^49. */
        ULONGLONG part = UInt32x32To64(rest, 10000000);
        ULONG fh, fl, fr;
        Divide((ULONG)(part >> 32), (ULONG)part, bytesPerSecond, &fh, &fl, &fr);
        ULONG before = resultLow;
        resultLow += fl;
        resultHigh += fh + (resultLow < before ? 1 : 0);
        if (resultHigh > 0x7fffffff) return FALSE;
        out->HighPart = (LONG)resultHigh; out->LowPart = resultLow;
        return TRUE;
    }
public:
    /* Create owns the one-stream gate, including every failure path.
     * Success returns independent references for Stream, Dma and Service.
     * The parent outlives the gate because this object retains its owner.
     */
    static NTSTATUS Create(PUNKNOWN parent, LONG *gate, PKSDATAFORMAT format,
        PMINIPORTWAVECYCLICSTREAM *stream, PDMACHANNEL *dma,
        PSERVICEGROUP *service, const UAC_STREAM_HOOKS *callbacks = NULL,
        PDEVICE_OBJECT cleanupDevice = NULL, BOOLEAN input = FALSE) {
        UAC_PCM pcm;
        if (!stream || !dma || !service) return STATUS_INVALID_PARAMETER;
        *stream = NULL; *dma = NULL; *service = NULL;
        ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
        if (!parent || !gate || !Parse(format, &pcm) || !Accepted(callbacks, input, &pcm))
            return STATUS_INVALID_PARAMETER;
        /* A transport Close hook can wait. Its final release must always
         * have a preallocated, device-associated passive cleanup path. */
        if (!cleanupDevice) return STATUS_INVALID_PARAMETER;
        if (InterlockedCompareExchange(gate, 1, 0) != 0)
            return STATUS_DEVICE_BUSY;
        PVOID memory = ExAllocatePoolWithTag(NonPagedPool, sizeof(UacStream), 's2aU');
        if (!memory) {
            InterlockedExchange(gate, 0);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        UacStream *s = new(memory) UacStream(parent, gate, &pcm, input);
        if (cleanupDevice) {
            s->cleanupWork = IoAllocateWorkItem(cleanupDevice);
            if (!s->cleanupWork) { s->Release(); return STATUS_INSUFFICIENT_RESOURCES; }
            ObReferenceObject(cleanupDevice);
            s->cleanupDevice = cleanupDevice;
        }
        s->buffer = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, s->Capacity, 'b2aU');
        NTSTATUS status = s->buffer ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(s->buffer, s->Capacity);
            status = PcNewServiceGroup(&s->group, NULL);
        }
        if (!NT_SUCCESS(status)) { s->Release(); return status; }
        /* Creation failures have no transport to close. Publish callbacks
         * only now, avoiding recursive owner-lock acquisition in cleanup. */
        if (callbacks) s->hooks = *callbacks;
        s->published = TRUE;
        *stream = (PMINIPORTWAVECYCLICSTREAM)s; /* construction reference */
        s->AddRef();
        *dma = (PDMACHANNEL)s;
        s->group->AddRef();
        *service = s->group;
        return STATUS_SUCCESS;
    }
    /* Structure of a PCM format: WAVE_FORMAT_PCM or EXTENSIBLE/PCM, 1-32
     * channels, 16/24/32-bit containers (8-bit PCM is unsigned in WAVE and
     * signed in UAC 2.0: not carried), valid bits up to the container,
     * 8-384 kHz, and block align / byte rate consistent with them. */
    static BOOLEAN Parse(PKSDATAFORMAT format, UAC_PCM *pcm) {
        const ULONG waveOffset = FIELD_OFFSET(KSDATAFORMAT_WAVEFORMATEX, WaveFormatEx);
        if (!format || format->FormatSize < waveOffset + sizeof(WAVEFORMATEX) ||
            !IsEqualGUID(format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
            !IsEqualGUID(format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
            !IsEqualGUID(format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
            return FALSE;
        PWAVEFORMATEX w = &((PKSDATAFORMAT_WAVEFORMATEX)format)->WaveFormatEx;
        ULONG container = w->wBitsPerSample, bits = container, positions = UAC_PCM_NO_MASK;
        if (!w->nChannels || w->nChannels > 32 ||
            (container != 16 && container != 24 && container != 32) ||
            w->nSamplesPerSec < 8000 || w->nSamplesPerSec > 384000 ||
            w->nBlockAlign != w->nChannels * (container / 8) ||
            w->nAvgBytesPerSec != w->nSamplesPerSec * w->nBlockAlign ||
            w->cbSize > format->FormatSize - waveOffset - sizeof(WAVEFORMATEX))
            return FALSE;
        if (w->wFormatTag == WAVE_FORMAT_PCM) {
            if (w->cbSize != 0) return FALSE;
        } else {
            if (w->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
                w->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) ||
                format->FormatSize < waveOffset + sizeof(WAVEFORMATEXTENSIBLE))
                return FALSE;
            PWAVEFORMATEXTENSIBLE e = (PWAVEFORMATEXTENSIBLE)w;
            bits = e->Samples.wValidBitsPerSample;
            if (!bits || bits > container || !IsEqualGUID(e->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                return FALSE;
            ULONG mask = e->dwChannelMask, ones = 0;
            while (mask) { ones += mask & 1; mask >>= 1; }
            /* Fewer positions than channels: the rest are unassigned. */
            if (ones > w->nChannels) return FALSE;
            positions = e->dwChannelMask;
        }
        pcm->Channels = w->nChannels; pcm->Bits = bits; pcm->Container = container;
        pcm->Rate = w->nSamplesPerSec; pcm->BlockAlign = w->nBlockAlign;
        pcm->BytesPerSecond = w->nAvgBytesPerSec;
        pcm->Mask = positions;
        return TRUE;
    }
    static BOOLEAN Accepted(const UAC_STREAM_HOOKS *callbacks, BOOLEAN input, const UAC_PCM *pcm) {
        if (callbacks && callbacks->Accept)
            return callbacks->Accept(callbacks->Context, input, pcm->Channels, pcm->Bits,
                                     pcm->Container, pcm->Rate, pcm->Mask);
        return !input && pcm->Channels == 2 && pcm->Bits == 16 && pcm->Container == 16 &&
               pcm->Rate == 48000;
    }
    /* The first build's check, kept for its host test: the one format. */
    static BOOLEAN ValidFormat(PKSDATAFORMAT format) {
        UAC_PCM pcm;
        PWAVEFORMATEX w;
        if (!Parse(format, &pcm) || !Accepted(NULL, FALSE, &pcm)) return FALSE;
        w = &((PKSDATAFORMAT_WAVEFORMATEX)format)->WaveFormatEx;
        if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            PWAVEFORMATEXTENSIBLE e = (PWAVEFORMATEXTENSIBLE)w;
            if (e->dwChannelMask != 0 && e->dwChannelMask != 3) return FALSE;
        }
        return TRUE;
    }
    const UAC_PCM *Format() const { return &pcm; }
    BOOLEAN Capture() const { return capture; }
    STDMETHODIMP QueryInterface(REFIID id, PVOID *out) {
        if (!out) return STATUS_INVALID_PARAMETER;
        *out = NULL;
        if (IsEqualGUID(id, IID_IUnknown) ||
            IsEqualGUID(id, IID_IMiniportWaveCyclicStream))
            *out = (PMINIPORTWAVECYCLICSTREAM)this;
        else if (IsEqualGUID(id, IID_IDmaChannel)) *out = (PDMACHANNEL)this;
        else return STATUS_INVALID_PARAMETER;
        AddRef(); return STATUS_SUCCESS;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() {
        LONG n = InterlockedDecrement(&refs);
        if (!n) {
            /* Even PASSIVE final release can originate from a transport
             * notification. Never run Close/Stop/join inline on its caller.
             * Native IoWorkItem retains the device/image through return. */
            if (published) {
                ASSERT(cleanupWork != NULL);
                IoQueueWorkItem(cleanupWork, Cleanup, DelayedWorkQueue, this);
            } else {
                /* Only Create's unpublished failure paths reach this arm.
                 * No callbacks were exposed; release the gate synchronously. */
                ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
                DestroyPassive();
            }
        }
        return n;
    }
    STDMETHODIMP SetFormat(PKSDATAFORMAT format) {
        /* The transport is planned for one format: only that one. */
        UAC_PCM next;
        if (state == KSSTATE_RUN) return STATUS_INVALID_DEVICE_STATE;
        if (!Parse(format, &next)) return STATUS_INVALID_PARAMETER;
        return next.Channels == pcm.Channels && next.Bits == pcm.Bits &&
               next.Container == pcm.Container && next.Rate == pcm.Rate &&
               /* The same declaration: a plain format stays plain, an
                * explicit mask the same mask (the device layout the
                * transport was opened for is not re-resolved). */
               next.Mask == pcm.Mask ?
               STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) SetNotificationFreq(ULONG interval, PULONG frameSize) {
        /* Bounded fixed-format arithmetic; no timer is armed by this shim. */
        if (!interval) interval = 10;
        if (interval > 1000) interval = 1000;
        /* Whole frames: ceil(rate * interval / 1000) frames. */
        ULONG bytes = ((pcm.Rate * interval + 999) / 1000) * BlockAlign;
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        notifyBytes = bytes; pendingNotify = 0;
        KeReleaseSpinLock(&ringLock, oldIrql);
        if (frameSize) *frameSize = bytes;
        return interval;
    }
    STDMETHODIMP SetState(KSSTATE next) {
        if (next == KSSTATE_RUN) {
            if (!hooks.Start || !hooks.Stop || !hooks.Close || !hooks.Notify)
                return STATUS_NOT_SUPPORTED;
            NTSTATUS status = hooks.Start(hooks.Context, this);
            if (NT_SUCCESS(status)) state = next;
            return status;
        }
        if (next != KSSTATE_STOP && next != KSSTATE_ACQUIRE && next != KSSTATE_PAUSE)
            return STATUS_INVALID_PARAMETER;
        if (state == KSSTATE_RUN && hooks.Stop) {
            hooks.Stop(hooks.Context, this);
            KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
            filled = completed; /* queued but canceled data must be resubmitted */
            KeReleaseSpinLock(&ringLock, oldIrql);
        }
        state = next;
        if (next == KSSTATE_STOP) {
            KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
            filled = completed = packetErrors = pendingNotify = 0;
            if (buffer) RtlZeroMemory(buffer, Capacity);
            KeReleaseSpinLock(&ringLock, oldIrql);
        }
        return STATUS_SUCCESS;
    }
    STDMETHODIMP GetPosition(PULONG position) {
        if (!position) return STATUS_INVALID_PARAMETER;
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        *position = completed;
        KeReleaseSpinLock(&ringLock, oldIrql);
        return STATUS_SUCCESS;
    }
    STDMETHODIMP NormalizePhysicalPosition(PLONGLONG position) {
        if (!position) return STATUS_INVALID_PARAMETER;
        LARGE_INTEGER bytes, time; bytes.QuadPart = *position;
        if (!Normalize(bytes, BytesPerSecond, &time)) return STATUS_INVALID_PARAMETER;
        *position = time.QuadPart;
        return STATUS_SUCCESS;
    }
    STDMETHODIMP_(void) Silence(PVOID address, ULONG count) {
        if (address && count) RtlZeroMemory(address, count); /* signed 16-bit PCM */
    }
    STDMETHODIMP AllocateBuffer(ULONG size, PPHYSICAL_ADDRESS constraint) {
        UNREFERENCED_PARAMETER(constraint);
        /* Already allocated before publication; no physical DMA is used. */
        return buffer && size && size <= Capacity ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(void) FreeBuffer() {
        /* Keep the published address stable until the final COM release. */
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        if (buffer) RtlZeroMemory(buffer, Capacity);
        KeReleaseSpinLock(&ringLock, oldIrql);
    }
    STDMETHODIMP_(ULONG) TransferCount() { return 0; }
    STDMETHODIMP_(ULONG) MaximumBufferSize() { return Capacity; }
    STDMETHODIMP_(ULONG) AllocatedBufferSize() { return Capacity; }
    STDMETHODIMP_(ULONG) BufferSize() {
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        ULONG size = used; KeReleaseSpinLock(&ringLock, oldIrql); return size;
    }
    STDMETHODIMP_(void) SetBufferSize(ULONG size) {
        if (size > Capacity) size = Capacity;
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        used = size - size % BlockAlign;
        if (used) { filled %= used; completed %= used; }
        else filled = completed = 0;
        KeReleaseSpinLock(&ringLock, oldIrql);
    }
    STDMETHODIMP_(PVOID) SystemAddress() { return buffer; }
    STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress() {
        PHYSICAL_ADDRESS address; address.QuadPart = 0; return address;
    }
    STDMETHODIMP_(PADAPTER_OBJECT) GetAdapterObject() { return NULL; }
    STDMETHODIMP_(void) CopyTo(PVOID destination, PVOID source, ULONG count) {
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        if (source && count && Contains(destination, count))
            RtlCopyMemory(destination, source, count);
        KeReleaseSpinLock(&ringLock, oldIrql);
    }
    STDMETHODIMP_(void) CopyFrom(PVOID destination, PVOID source, ULONG count) {
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        if (destination && count && Contains(source, count))
            RtlCopyMemory(destination, source, count);
        KeReleaseSpinLock(&ringLock, oldIrql);
    }
    /* Fill is the submitted cursor, not the reported completed cursor.
     * Caller serialization/transport drain owns the lifetime of this object. */
    void FillBytes(PUCHAR destination, ULONG count) {
        if (!destination || !count) return;
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        if (!used) RtlZeroMemory(destination, count);
        else while (count) {
            ULONG part = used - filled;
            if (part > count) part = count;
            RtlCopyMemory(destination, buffer + filled, part);
            destination += part; count -= part; filled += part;
            if (filled == used) filled = 0;
        }
        KeReleaseSpinLock(&ringLock, oldIrql);
    }
    /* Capture: received whole frames go in at the write cursor, which is
     * also the reported position. */
    void WriteBytes(const UCHAR *source, ULONG count) {
        if (!source || !count) return;
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        BOOLEAN notify = FALSE;
        if (used) {
            ULONG total = count;
            while (count) {
                ULONG part = used - completed;
                if (part > count) part = count;
                RtlCopyMemory(buffer + completed, source, part);
                source += part; count -= part; completed += part;
                if (completed == used) completed = 0;
            }
            filled = completed;
            if (notifyBytes) {
                pendingNotify += total;
                if (pendingNotify >= notifyBytes) { pendingNotify %= notifyBytes; notify = TRUE; }
            }
        }
        KeReleaseSpinLock(&ringLock, oldIrql);
        if (notify && hooks.Notify) hooks.Notify(hooks.Context, group);
    }
    void CompleteBytes(ULONG count, ULONG errors) {
        KIRQL oldIrql; KeAcquireSpinLock(&ringLock, &oldIrql);
        if (used) completed = (completed + count % used) % used;
        if (errors > MAXULONG - packetErrors) packetErrors = MAXULONG;
        else packetErrors += errors;
        BOOLEAN notify = FALSE;
        if (notifyBytes) {
            notify = count >= notifyBytes;
            pendingNotify += count % notifyBytes;
            if (pendingNotify >= notifyBytes) {
                pendingNotify -= notifyBytes; notify = TRUE;
            }
        }
        KeReleaseSpinLock(&ringLock, oldIrql);
        /* Notify derives from completed USB packets, never a timer. */
        if (notify && hooks.Notify) hooks.Notify(hooks.Context, group);
    }
};
#endif
