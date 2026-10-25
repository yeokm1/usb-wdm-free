/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Direct tests of actual original M0 miniport callbacks with mock class.
 * Public DDK structures only; not a Windows kernel or USB test.
 */
#include <assert.h>
#include <stdarg.h>
#include "uvcmini.c"

static unsigned completedDevice, completedStream, readyDevice, readyControl, readyData;
static HW_INITIALIZATION_DATA registered;

VOID STREAMAPI StreamClassDeviceNotification(STREAM_MINIDRIVER_DEVICE_NOTIFICATION_TYPE type,
                                             PVOID device, ...)
{
    UNREFERENCED_PARAMETER(device);
    if (type == DeviceRequestComplete) completedDevice++;
    else if (type == ReadyForNextDeviceRequest) readyDevice++;
    else assert(0);
}

VOID STREAMAPI StreamClassStreamNotification(STREAM_MINIDRIVER_STREAM_NOTIFICATION_TYPE type,
                                             PHW_STREAM_OBJECT stream, ...)
{
    UNREFERENCED_PARAMETER(stream);
    if (type == StreamRequestComplete) completedStream++;
    else if (type == ReadyForNextStreamControlRequest) readyControl++;
    else if (type == ReadyForNextStreamDataRequest) readyData++;
    else assert(0);
}

NTSTATUS STREAMAPI StreamClassRegisterAdapter(PVOID driver, PVOID path,
                                              PHW_INITIALIZATION_DATA init)
{
    UNREFERENCED_PARAMETER(driver); UNREFERENCED_PARAMETER(path);
    registered = *init;
    return STATUS_SUCCESS;
}

int main(void)
{
    HW_STREAM_REQUEST_BLOCK request;
    HW_STREAM_DESCRIPTOR descriptor;
    PORT_CONFIGURATION_INFORMATION config;
    HW_STREAM_OBJECT stream, other;
    SIM_DEVICE device;
    SIM_STREAM extension;
    STREAM_DATA_INTERSECT_INFO intersect;
    KS_DATAFORMAT_VIDEOINFOHEADER output, bad;
    ULONG size;
    unsigned controlBefore;
    assert(NT_SUCCESS(DriverEntry(NULL, NULL)));
    assert(registered.HwInitializationDataSize == sizeof(registered));
    assert(registered.DeviceExtensionSize == sizeof(device));
    assert(registered.PerStreamExtensionSize == sizeof(extension));
    assert(!registered.BusMasterDMA && !registered.TurnOffSynchronization);
    RtlZeroMemory(&request, sizeof(request));
    RtlZeroMemory(&config, sizeof(config));
    request.HwDeviceExtension = &device;
    request.Command = SRB_INITIALIZE_DEVICE;
    request.CommandData.ConfigInfo = &config;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS);
    assert(config.StreamDescriptorSize == sizeof(descriptor));
    request.Command = SRB_GET_STREAM_INFO;
    request.CommandData.StreamBuffer = &descriptor;
    request.NumberOfBytesToTransfer = sizeof(descriptor) - 1;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_BUFFER_TOO_SMALL);
    request.NumberOfBytesToTransfer = sizeof(descriptor);
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS);
    assert(descriptor.StreamHeader.NumberOfStreams == 1);
    assert(descriptor.StreamInfo.DataFlow == KSPIN_DATAFLOW_OUT);
    assert(descriptor.StreamInfo.StreamFormatsArray[0]->SampleSize == FRAME_BYTES);
    assert(descriptor.StreamHeader.Topology->CategoriesCount == 2);

    RtlZeroMemory(&intersect, sizeof(intersect));
    intersect.DataRange = (PKSDATARANGE)&range;
    intersect.SizeOfDataFormatBuffer = sizeof(size);
    intersect.DataFormatBuffer = &size;
    request.Command = SRB_GET_DATA_INTERSECTION;
    request.CommandData.IntersectInfo = &intersect;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS && size == sizeof(format));
    intersect.DataFormatBuffer = &output;
    intersect.SizeOfDataFormatBuffer = sizeof(output) - 1;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_BUFFER_TOO_SMALL);
    intersect.SizeOfDataFormatBuffer = sizeof(output);
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS && Valid(&output.DataFormat));
    intersect.StreamNumber = 1;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_INVALID_PARAMETER);
    assert(!Valid(NULL));
    bad = format; bad.DataFormat.FormatSize--;
    assert(!Valid(&bad.DataFormat));
    bad = format; bad.VideoInfoHeader.bmiHeader.biWidth++;
    assert(!Valid(&bad.DataFormat));
    bad = format; bad.VideoInfoHeader.bmiHeader.biCompression = 0;
    assert(!Valid(&bad.DataFormat));
    bad = format; bad.VideoInfoHeader.AvgTimePerFrame++;
    assert(!Valid(&bad.DataFormat));
    bad = format; bad.VideoInfoHeader.rcSource.left = 1;
    assert(!Valid(&bad.DataFormat));
    bad = format; bad.DataFormat.SampleSize = 1;
    assert(!Valid(&bad.DataFormat));

    RtlZeroMemory(&stream, sizeof(stream));
    RtlZeroMemory(&other, sizeof(other));
    stream.HwStreamExtension = &extension;
    request.StreamObject = &stream;
    request.CommandData.OpenFormat = &format.DataFormat;
    request.Command = SRB_OPEN_STREAM;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS && extension.State == KSSTATE_STOP);
    assert(stream.ReceiveDataPacket && stream.ReceiveControlPacket && !stream.Dma && stream.Pio);
    request.StreamObject = &other;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_DEVICE_BUSY);
    request.StreamObject = &stream;
    request.Command = SRB_SET_STREAM_STATE;
    request.CommandData.StreamState = KSSTATE_PAUSE;
    stream.ReceiveControlPacket(&request);
    assert(request.Status == STATUS_SUCCESS && extension.State == KSSTATE_PAUSE);
    request.CommandData.StreamState = KSSTATE_RUN;
    stream.ReceiveControlPacket(&request);
    assert(request.Status == STATUS_NOT_SUPPORTED && extension.State == KSSTATE_PAUSE);
    request.Command = SRB_GET_STREAM_STATE;
    stream.ReceiveControlPacket(&request);
    assert(request.CommandData.StreamState == KSSTATE_PAUSE);
    request.Command = SRB_READ_DATA;
    request.NumberOfBytesToTransfer = FRAME_BYTES;
    stream.ReceiveDataPacket(&request);
    assert(request.Status == STATUS_NOT_SUPPORTED && !request.ActualBytesTransferred);
    request.Command = SRB_CLOSE_STREAM;
    registered.HwReceivePacket(&request);
    assert(request.Status == STATUS_SUCCESS && !device.OpenStream);
    controlBefore = completedStream;
    registered.HwCancelPacket(&request);
    registered.HwRequestTimeoutHandler(&request);
    assert(completedStream == controlBefore);
    assert(completedDevice == readyDevice);
    assert(completedStream == readyData + readyControl);
    puts("PASS actual M0 callbacks: bounds, intersection, format, one-pin ownership, STOP/PAUSE/RUN rejection, completion flow");
    return 0;
}
