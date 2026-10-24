/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original STREAM.SYS metadata/STOP-pin experiment from strmini.h,
 * ks.h/ksmedia.h and published Stream Class API/SRB documentation.
 * ROOT\UVCSIM98 only. No USB, real camera, AVStream or kit sample code.
 * RUN is deliberately unsupported until timer/close lifetime is proven.
 */
#include <strmini.h>
#include <ksmedia.h>

#define WIDTH 320
#define HEIGHT 240
#define FRAME_BYTES (WIDTH * HEIGHT * 2)
#define FRAME_INTERVAL 1000000 /* 10fps, 100ns units */
#define YUY2_FOURCC 0x32595559

typedef struct _SIM_DEVICE { PHW_STREAM_OBJECT OpenStream; } SIM_DEVICE;
typedef struct _SIM_STREAM { KSSTATE State; } SIM_STREAM;

static const GUID typeVideo = { STATIC_KSDATAFORMAT_TYPE_VIDEO };
static const GUID videoInfo = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO };
/* Registered FOURCC media-subtype GUID construction, YUY2 little-endian. */
static const GUID yuy2 = { YUY2_FOURCC, 0, 0x10,
    {0x80,0,0,0xaa,0,0x38,0x9b,0x71} };
static GUID capturePin = { STATIC_PINNAME_VIDEO_CAPTURE };
static const GUID categories[] = {
    { STATIC_KSCATEGORY_VIDEO }, { STATIC_KSCATEGORY_CAPTURE }
};
static KSTOPOLOGY topology = { 2, categories, 0, NULL, 0, NULL, NULL, 0 };
static KS_DATARANGE_VIDEO range;
static KS_DATAFORMAT_VIDEOINFOHEADER format;
static PKSDATAFORMAT formats[] = { (PKSDATAFORMAT)&range };

static int Same(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

static void Formats(void)
{
    RtlZeroMemory(&format, sizeof(format));
    format.DataFormat.FormatSize = sizeof(format);
    format.DataFormat.SampleSize = FRAME_BYTES;
    format.DataFormat.MajorFormat = typeVideo;
    format.DataFormat.SubFormat = yuy2;
    format.DataFormat.Specifier = videoInfo;
    format.VideoInfoHeader.rcSource.right = WIDTH;
    format.VideoInfoHeader.rcSource.bottom = HEIGHT;
    format.VideoInfoHeader.rcTarget = format.VideoInfoHeader.rcSource;
    format.VideoInfoHeader.dwBitRate = FRAME_BYTES * 8 * 10;
    format.VideoInfoHeader.AvgTimePerFrame = FRAME_INTERVAL;
    format.VideoInfoHeader.bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
    format.VideoInfoHeader.bmiHeader.biWidth = WIDTH;
    format.VideoInfoHeader.bmiHeader.biHeight = HEIGHT;
    format.VideoInfoHeader.bmiHeader.biPlanes = 1;
    format.VideoInfoHeader.bmiHeader.biBitCount = 16;
    format.VideoInfoHeader.bmiHeader.biCompression = YUY2_FOURCC;
    format.VideoInfoHeader.bmiHeader.biSizeImage = FRAME_BYTES;
    RtlZeroMemory(&range, sizeof(range));
    range.DataRange = format.DataFormat;
    range.DataRange.FormatSize = sizeof(range);
    range.bFixedSizeSamples = TRUE;
    range.ConfigCaps.guid = videoInfo;
    range.ConfigCaps.InputSize.cx = WIDTH;
    range.ConfigCaps.InputSize.cy = HEIGHT;
    range.ConfigCaps.MinCroppingSize = range.ConfigCaps.InputSize;
    range.ConfigCaps.MaxCroppingSize = range.ConfigCaps.InputSize;
    range.ConfigCaps.CropGranularityX = range.ConfigCaps.CropGranularityY = 1;
    range.ConfigCaps.CropAlignX = range.ConfigCaps.CropAlignY = 1;
    range.ConfigCaps.MinOutputSize = range.ConfigCaps.InputSize;
    range.ConfigCaps.MaxOutputSize = range.ConfigCaps.InputSize;
    range.ConfigCaps.OutputGranularityX = range.ConfigCaps.OutputGranularityY = 1;
    range.ConfigCaps.MinFrameInterval = range.ConfigCaps.MaxFrameInterval = FRAME_INTERVAL;
    range.ConfigCaps.MinBitsPerSecond = range.ConfigCaps.MaxBitsPerSecond = FRAME_BYTES * 8 * 10;
    range.VideoInfoHeader = format.VideoInfoHeader;
}

static int Rectangle(const RECT *rect)
{
    return !rect->left && !rect->top &&
        ((!rect->right && !rect->bottom) ||
         (rect->right == WIDTH && rect->bottom == HEIGHT));
}

static int Valid(PKSDATAFORMAT input)
{
    PKS_DATAFORMAT_VIDEOINFOHEADER f;
    if (!input || input->FormatSize < sizeof(format) ||
        !Same(&input->MajorFormat, &typeVideo) || !Same(&input->SubFormat, &yuy2) ||
        !Same(&input->Specifier, &videoInfo)) return 0;
    f = (PKS_DATAFORMAT_VIDEOINFOHEADER)input;
    return (!input->SampleSize || input->SampleSize == FRAME_BYTES) &&
        Rectangle(&f->VideoInfoHeader.rcSource) && Rectangle(&f->VideoInfoHeader.rcTarget) &&
        f->VideoInfoHeader.bmiHeader.biSize == sizeof(KS_BITMAPINFOHEADER) &&
        f->VideoInfoHeader.bmiHeader.biWidth == WIDTH &&
        f->VideoInfoHeader.bmiHeader.biHeight == HEIGHT &&
        f->VideoInfoHeader.bmiHeader.biPlanes == 1 &&
        f->VideoInfoHeader.bmiHeader.biBitCount == 16 &&
        f->VideoInfoHeader.bmiHeader.biCompression == YUY2_FOURCC &&
        f->VideoInfoHeader.bmiHeader.biSizeImage == FRAME_BYTES &&
        f->VideoInfoHeader.AvgTimePerFrame == FRAME_INTERVAL;
}

static void DeviceDone(PHW_STREAM_REQUEST_BLOCK srb, NTSTATUS status)
{
    PVOID device = srb->HwDeviceExtension;
    srb->Status = status;
    StreamClassDeviceNotification(DeviceRequestComplete, device, srb);
    StreamClassDeviceNotification(ReadyForNextDeviceRequest, device);
}

static void StreamDone(PHW_STREAM_REQUEST_BLOCK srb, NTSTATUS status, int data)
{
    PHW_STREAM_OBJECT stream = srb->StreamObject;
    srb->Status = status;
    StreamClassStreamNotification(StreamRequestComplete, stream, srb);
    StreamClassStreamNotification(data ? ReadyForNextStreamDataRequest :
        ReadyForNextStreamControlRequest, stream);
}

static VOID STREAMAPI Read(PHW_STREAM_REQUEST_BLOCK srb)
{
    /* M0 publishes no frames. Never report synthetic completion as USB. */
    srb->ActualBytesTransferred = 0;
    StreamDone(srb, STATUS_NOT_SUPPORTED, 1);
}

static VOID STREAMAPI Control(PHW_STREAM_REQUEST_BLOCK srb)
{
    SIM_STREAM *stream = (SIM_STREAM *)srb->StreamObject->HwStreamExtension;
    NTSTATUS status = STATUS_SUCCESS;
    srb->ActualBytesTransferred = 0;
    switch (srb->Command) {
    case SRB_GET_STREAM_STATE:
        srb->CommandData.StreamState = stream->State;
        break;
    case SRB_SET_STREAM_STATE:
        if (srb->CommandData.StreamState == KSSTATE_RUN) status = STATUS_NOT_SUPPORTED;
        else if (srb->CommandData.StreamState > KSSTATE_PAUSE) status = STATUS_INVALID_PARAMETER;
        else stream->State = srb->CommandData.StreamState;
        break;
    case SRB_PROPOSE_DATA_FORMAT:
    case SRB_SET_DATA_FORMAT:
        status = Valid(srb->CommandData.OpenFormat) ? STATUS_SUCCESS : STATUS_NO_MATCH;
        break;
    case SRB_INDICATE_MASTER_CLOCK:
    case SRB_CLOSE_MASTER_CLOCK:
    case SRB_BEGIN_FLUSH:
    case SRB_END_FLUSH:
        break;
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }
    StreamDone(srb, status, 0);
}

static NTSTATUS Intersect(PHW_STREAM_REQUEST_BLOCK srb)
{
    PSTREAM_DATA_INTERSECT_INFO info = srb->CommandData.IntersectInfo;
    PKSDATARANGE r;
    if (!info || info->StreamNumber || !(r = info->DataRange) ||
        r->FormatSize < sizeof(KSDATARANGE)) return STATUS_INVALID_PARAMETER;
    if (!Same(&r->MajorFormat, &typeVideo) || !Same(&r->SubFormat, &yuy2) ||
        !Same(&r->Specifier, &videoInfo)) return STATUS_NO_MATCH;
    if (r->FormatSize >= sizeof(KS_DATARANGE_VIDEO)) {
        PKS_DATARANGE_VIDEO video = (PKS_DATARANGE_VIDEO)r;
        if (video->VideoInfoHeader.bmiHeader.biWidth != WIDTH ||
            video->VideoInfoHeader.bmiHeader.biHeight != HEIGHT ||
            video->VideoInfoHeader.bmiHeader.biCompression != YUY2_FOURCC ||
            video->VideoInfoHeader.AvgTimePerFrame != FRAME_INTERVAL)
            return STATUS_NO_MATCH;
    }
    if (info->SizeOfDataFormatBuffer == sizeof(ULONG) && info->DataFormatBuffer) {
        *(PULONG)info->DataFormatBuffer = sizeof(format);
        srb->ActualBytesTransferred = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    srb->ActualBytesTransferred = sizeof(format);
    if (info->SizeOfDataFormatBuffer < sizeof(format)) return STATUS_BUFFER_TOO_SMALL;
    if (!info->DataFormatBuffer) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(info->DataFormatBuffer, &format, sizeof(format));
    return STATUS_SUCCESS;
}

static VOID STREAMAPI Receive(PHW_STREAM_REQUEST_BLOCK srb)
{
    SIM_DEVICE *device = (SIM_DEVICE *)srb->HwDeviceExtension;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG available = srb->NumberOfBytesToTransfer;
    srb->ActualBytesTransferred = 0;
    switch (srb->Command) {
    case SRB_INITIALIZE_DEVICE:
        device->OpenStream = NULL;
        srb->CommandData.ConfigInfo->StreamDescriptorSize = sizeof(HW_STREAM_DESCRIPTOR);
        break;
    case SRB_GET_STREAM_INFO:
        if (!srb->CommandData.StreamBuffer || available < sizeof(HW_STREAM_DESCRIPTOR)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            PHW_STREAM_DESCRIPTOR d = srb->CommandData.StreamBuffer;
            RtlZeroMemory(d, sizeof(*d));
            d->StreamHeader.NumberOfStreams = 1;
            d->StreamHeader.SizeOfHwStreamInformation = sizeof(HW_STREAM_INFORMATION);
            d->StreamHeader.Topology = &topology;
            d->StreamInfo.NumberOfPossibleInstances = 1;
            d->StreamInfo.DataFlow = KSPIN_DATAFLOW_OUT;
            d->StreamInfo.DataAccessible = TRUE;
            d->StreamInfo.NumberOfFormatArrayEntries = 1;
            d->StreamInfo.StreamFormatsArray = formats;
            d->StreamInfo.Category = &capturePin;
            d->StreamInfo.Name = &capturePin;
            srb->ActualBytesTransferred = sizeof(*d);
        }
        break;
    case SRB_GET_DATA_INTERSECTION:
        status = Intersect(srb);
        break;
    case SRB_OPEN_STREAM:
        if (!srb->StreamObject || srb->StreamObject->StreamNumber ||
            !Valid(srb->CommandData.OpenFormat)) status = STATUS_NO_MATCH;
        else if (device->OpenStream) status = STATUS_DEVICE_BUSY;
        else {
            PHW_STREAM_OBJECT stream = srb->StreamObject;
            SIM_STREAM *extension = (SIM_STREAM *)stream->HwStreamExtension;
            extension->State = KSSTATE_STOP;
            stream->ReceiveDataPacket = Read;
            stream->ReceiveControlPacket = Control;
            stream->Dma = FALSE;
            stream->Pio = TRUE;
            stream->Allocator = FALSE;
            stream->StreamHeaderMediaSpecific = 0;
            stream->StreamHeaderWorkspace = 0;
            device->OpenStream = stream;
        }
        break;
    case SRB_CLOSE_STREAM:
        if (device->OpenStream == srb->StreamObject) device->OpenStream = NULL;
        else status = STATUS_INVALID_PARAMETER;
        break;
    case SRB_OPEN_DEVICE_INSTANCE:
    case SRB_CLOSE_DEVICE_INSTANCE:
    case SRB_INITIALIZATION_COMPLETE:
    case SRB_CHANGE_POWER_STATE:
    case SRB_UNINITIALIZE_DEVICE:
    case SRB_SURPRISE_REMOVAL:
        break;
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }
    DeviceDone(srb, status);
}

static VOID STREAMAPI Cancel(PHW_STREAM_REQUEST_BLOCK srb)
{
    /* M0 retains no requests. Required callback; never double-complete. */
    UNREFERENCED_PARAMETER(srb);
}

static VOID STREAMAPI Timeout(PHW_STREAM_REQUEST_BLOCK srb)
{
    UNREFERENCED_PARAMETER(srb);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path)
{
    HW_INITIALIZATION_DATA init;
    Formats();
    RtlZeroMemory(&init, sizeof(init));
    init.HwInitializationDataSize = sizeof(init);
    init.HwReceivePacket = Receive;
    init.HwCancelPacket = Cancel;
    init.HwRequestTimeoutHandler = Timeout;
    init.DeviceExtensionSize = sizeof(SIM_DEVICE);
    init.PerStreamExtensionSize = sizeof(SIM_STREAM);
    /* Synchronous metadata only; class serialization remains enabled. */
    return StreamClassRegisterAdapter(driver, path, &init);
}
