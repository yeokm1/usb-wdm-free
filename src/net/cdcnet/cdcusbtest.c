/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * cdcusbtest - host test of CDCNET's USB lower edge (cdcusb.c as built)
 * on the fake operating system of shim\ (the shared core's), with a fake
 * CDC NCM function below it that answers the driver's URBs and can fail a
 * chosen class request. Host-test code only.
 *
 * What it proves (ethernet.md section 4.8, the resume path):
 * - a resume whose bring-up fails once (GET_NTB_PARAMETERS refused while
 *   the function wakes) leaves the data path closed and the link down,
 *   and the worker then brings the function up again on its own, a
 *   second or so later, with no reset, sleep or replug: the cached
 *   packet filter restored, the data alternate selected, the receive and
 *   interrupt transfers submitted again and the link reported again;
 * - a bring-up that keeps failing is retried a bounded number of times
 *   (BROKEN_TRIES), then left to a reset, which still recovers it;
 * - no rule of the fake operating system is broken, and every IRP and
 *   pool block is returned at the stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fakeos.h"
#include "fakewin.h"
#include "usbdi.h"
#include "cdcusb.h"

static unsigned long assertions, failures;
static void Check(int ok, const char *expression, int line)
{
    assertions++;
    if (!ok) { failures++; printf("FAIL %d: %s\n", line, expression); }
}
#define CHECK(e) Check((e) ? 1 : 0, #e, __LINE__)

/* ---------------- the fake NCM function ---------------- */

#define PIPE_INT ((USBD_PIPE_HANDLE)0x1001)
#define PIPE_IN  ((USBD_PIPE_HANDLE)0x1002)
#define PIPE_OUT ((USBD_PIPE_HANDLE)0x1003)

static const UCHAR device[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0x25, 0x05, 0xa2, 0xa4, 0, 1, 1, 2, 0, 1 };
/* NCM at High Speed (cdctest's ncm[]): control 0 (interrupt 0x83), data 1
 * (alternate 1: bulk 0x81 in, 0x02 out); bmNetworkCapabilities 0x21. */
static UCHAR config[] = {
    9,2,0,0,2,1,0,0x80,50,
    9,4,0,0,1,2,0x0d,0,0,
    5,0x24,0,0x10,1,
    5,0x24,6,0,1,
    13,0x24,15,4,0,0,0,0,0xea,5,0,0,0,
    6,0x24,0x1a,0,1,0x21,
    7,5,0x83,3,16,0,9,
    9,4,1,0,0,10,0,1,0,
    9,4,1,1,2,10,0,1,0,
    7,5,0x81,2,0,2,0,
    7,5,0x02,2,0,2,0
};
static const char macText[] = "02AABBCC0001";

static void *lock;
static PIRP queue;                          /* bulk-IN and interrupt IRPs held */
static volatile long alt, held, ntbReads, filterSets, lastFilter, resets;
static volatile long failNtb;               /* GET_NTB_PARAMETERS refusals to give, -1 for all */

static void Hold(PIRP irp)
{
    FwLock(lock);
    irp->FakeNext = queue; queue = irp;
    held++;
    FwUnlock(lock);
}

/* Take one held IRP on the pipe (any if pipe is NULL); NULL if none. */
static PIRP Take(USBD_PIPE_HANDLE pipe)
{
    PIRP *p, irp = NULL;
    FwLock(lock);
    for (p = &queue; *p; p = &(*p)->FakeNext) {
        PURB urb = (PURB)(*p)->Next.Parameters.Others.Argument1;
        if (!pipe || urb->UrbBulkOrInterruptTransfer.PipeHandle == pipe) {
            irp = *p; *p = irp->FakeNext; irp->FakeNext = NULL; held--;
            break;
        }
    }
    FwUnlock(lock);
    return irp;
}

static void Answer(PIRP irp, PURB urb, const void *data, ULONG length)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST *u = &urb->UrbControlDescriptorRequest;
    if (length > u->TransferBufferLength) length = u->TransferBufferLength;
    if (length) memcpy(u->TransferBuffer, data, length);
    u->TransferBufferLength = length;
    FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
}

static void Pipes(PUSBD_INTERFACE_INFORMATION i, UCHAR alternate)
{
    i->AlternateSetting = alternate;
    i->InterfaceHandle = (USBD_INTERFACE_HANDLE)(ULONG_PTR)(0x3000 + i->InterfaceNumber);
    if (i->InterfaceNumber == 0) {
        i->NumberOfPipes = 1;
        i->Pipes[0].MaximumPacketSize = 16; i->Pipes[0].EndpointAddress = 0x83;
        i->Pipes[0].PipeType = UsbdPipeTypeInterrupt; i->Pipes[0].PipeHandle = PIPE_INT;
    } else if (alternate == 0) {
        i->NumberOfPipes = 0;
    } else {
        i->NumberOfPipes = 2;
        i->Pipes[0].MaximumPacketSize = 512; i->Pipes[0].EndpointAddress = 0x81;
        i->Pipes[0].PipeType = UsbdPipeTypeBulk; i->Pipes[0].PipeHandle = PIPE_IN;
        i->Pipes[1].MaximumPacketSize = 512; i->Pipes[1].EndpointAddress = 0x02;
        i->Pipes[1].PipeType = UsbdPipeTypeBulk; i->Pipes[1].PipeHandle = PIPE_OUT;
    }
}

static void Submit(PDEVICE_OBJECT unused, PIRP irp, PURB urb)
{
    (void)unused;
    switch (urb->UrbHeader.Function) {
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE: {
        struct _URB_CONTROL_DESCRIPTOR_REQUEST *u = &urb->UrbControlDescriptorRequest;
        if (u->DescriptorType == USB_DEVICE_DESCRIPTOR_TYPE) Answer(irp, urb, device, sizeof(device));
        else if (u->DescriptorType == USB_CONFIGURATION_DESCRIPTOR_TYPE) Answer(irp, urb, config, sizeof(config));
        else if (u->DescriptorType == USB_STRING_DESCRIPTOR_TYPE && u->Index == 0) {
            static const UCHAR langs[4] = { 4, 3, 0x09, 0x04 };
            Answer(irp, urb, langs, 4);
        } else if (u->DescriptorType == USB_STRING_DESCRIPTOR_TYPE && u->Index == 4) {
            UCHAR s[26]; int k;
            s[0] = 26; s[1] = 3;
            for (k = 0; k < 12; k++) { s[2 + 2 * k] = (UCHAR)macText[k]; s[3 + 2 * k] = 0; }
            Answer(irp, urb, s, 26);
        } else {
            FakeComplete(irp, STATUS_UNSUCCESSFUL, (USBD_STATUS)0xC0000004L);
        }
        return;
    }
    case URB_FUNCTION_SELECT_CONFIGURATION: {
        struct _URB_SELECT_CONFIGURATION *u = &urb->UrbSelectConfiguration;
        PUCHAR p = (PUCHAR)&u->Interface, end = (PUCHAR)urb + u->Hdr.Length;
        u->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)0x2000;
        while (p < end) {
            PUSBD_INTERFACE_INFORMATION i = (PUSBD_INTERFACE_INFORMATION)p;
            if (!i->Length) break;
            Pipes(i, 0);
            p += i->Length;
        }
        alt = 0;
        FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    }
    case URB_FUNCTION_SELECT_INTERFACE: {
        struct _URB_SELECT_INTERFACE *u = (struct _URB_SELECT_INTERFACE *)urb;
        CHECK(held == 0);                   /* the data path is drained before */
        Pipes(&u->Interface, u->Interface.AlternateSetting);
        alt = u->Interface.AlternateSetting;
        FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    }
    case URB_FUNCTION_CLASS_INTERFACE: {
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *u = &urb->UrbControlVendorClassRequest;
        if (u->Request == 0x80) {                                /* GET_NTB_PARAMETERS */
            UCHAR b[28];
            ntbReads++;
            if (failNtb) {
                if (failNtb > 0) failNtb--;
                FakeComplete(irp, STATUS_UNSUCCESSFUL, (USBD_STATUS)0xC0000004L);
                return;
            }
            memset(b, 0, sizeof(b));
            b[0] = 28; b[2] = 1;                                 /* NTB16 */
            b[5] = 0x40;                                         /* in 16384 */
            b[8] = 4; b[12] = 4;
            b[17] = 0x40;                                        /* out 16384 */
            b[20] = 4; b[24] = 4; b[26] = 16;
            Answer(irp, urb, b, 28);
            return;
        }
        if (u->Request == 0x43) { filterSets++; lastFilter = u->Value; }
        u->TransferBufferLength = (u->TransferFlags & USBD_TRANSFER_DIRECTION_IN) ? 0 : u->TransferBufferLength;
        FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    }
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
        Answer(irp, urb, "\0\0", 2);
        return;
    case URB_FUNCTION_RESET_PIPE:
        resets++;
        FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    case URB_FUNCTION_ABORT_PIPE: {
        PIRP h;
        while ((h = Take(urb->UrbPipeRequest.PipeHandle)) != NULL)
            FakeComplete(h, STATUS_CANCELLED, USBD_STATUS_CANCELED);
        FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
        return;
    }
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        if (urb->UrbBulkOrInterruptTransfer.PipeHandle == PIPE_OUT) {
            FakeComplete(irp, STATUS_SUCCESS, USBD_STATUS_SUCCESS);
            return;
        }
        Hold(irp);
        return;
    default:
        FakeViolation("unexpected URB function");
        FakeComplete(irp, STATUS_NOT_SUPPORTED, (USBD_STATUS)0xC0000004L);
    }
}

static BOOLEAN Cancel(PIRP irp)
{
    PIRP *p, found = NULL;
    FwLock(lock);
    for (p = &queue; *p; p = &(*p)->FakeNext)
        if (*p == irp) { found = irp; *p = irp->FakeNext; irp->FakeNext = NULL; held--; break; }
    FwUnlock(lock);
    if (found) FakeComplete(found, STATUS_CANCELLED, USBD_STATUS_CANCELED);
    return found != NULL;
}

/* ---------------- the upper edge ---------------- */

static volatile long linkCalls, linkState = -1, taskDone, taskStatus;

static void Receive(void *c, const cdc_u8 *f, cdc_u32 n) { (void)c; (void)f; (void)n; }
static cdc_u32 TxTake(void *c, void **p, cdc_u32 m) { (void)c; (void)p; (void)m; return 0; }
static cdc_u32 TxCopy(void *c, void *p, cdc_u8 *b, cdc_u32 n) { (void)c; (void)p; (void)b; (void)n; return 0; }
static void TxDone(void *c, void **p, cdc_u32 n, int ok) { (void)c; (void)p; (void)n; (void)ok; }
static void Link(void *c, int connected, cdc_u32 speed) { (void)c; (void)speed; linkCalls++; linkState = connected; }
static void TaskDone(void *c, int request, long status) { (void)c; taskStatus = status; taskDone = request; }

static int Until(volatile long *v, long want, unsigned long ms)
{
    unsigned long t0 = FwTicks();
    while (*v != want) {
        if (FwTicks() - t0 > ms) return 0;
        FwSleep(5);
    }
    return 1;
}

static int Request(void *dev, int request, cdc_u32 value)
{
    taskDone = 0;
    if (!CdcUsbRequest(dev, request, value, NULL, 0)) return 0;
    return Until(&taskDone, request, 5000);
}

static void Scenario(int persistent)
{
    static DEVICE_OBJECT lower = { 2, NULL };
    CDC_CALLBACKS cb = { NULL, Receive, TxTake, TxCopy, TxDone, Link, TaskDone };
    CDC_START start;
    void *dev = NULL;
    long reads, filters;
    unsigned long t0;

    memset(&start, 0, sizeof(start));
    start.AllowMask = CDC_ALLOW(CDC_PROTO_NCM);
    failNtb = 0;
    alt = held = ntbReads = filterSets = lastFilter = resets = 0;
    linkCalls = 0; linkState = -1;
    CHECK(CdcUsbStart(&lower, &cb, &start, &dev) == STATUS_SUCCESS && dev);
    if (!dev) { printf("start: stage %lu status %lx\n", (unsigned long)start.Failure, (unsigned long)start.Status); return; }
    CHECK(start.Protocol == CDC_PROTO_NCM && alt == 1);
    CHECK(Until(&held, 5, 2000));                   /* four receives and the interrupt */

    CHECK(Request(dev, CDC_REQ_FILTER, 0x0b));      /* directed, multicast, broadcast */
    CHECK(filterSets == 1 && lastFilter == 0x1c);

    CHECK(Request(dev, CDC_REQ_POWER, 3));          /* sleep */
    CHECK(linkState == 0 && held == 0);

    failNtb = persistent ? -1 : 1;
    reads = ntbReads; filters = filterSets;
    CHECK(Request(dev, CDC_REQ_POWER, 1));          /* wake: the bring-up fails */
    CHECK(taskStatus == STATUS_SUCCESS);
    CHECK(ntbReads == reads + 1 && alt == 0 && linkState == 0 && held == 0);

    if (!persistent) {
        /* No reset, sleep or replug: the worker's retry brings it up. */
        t0 = FwTicks();
        CHECK(Until(&linkState, 1, 5000));
    printf("one-shot: recovered %lu ms after the failed resume\n", FwTicks() - t0);
        CHECK(ntbReads == reads + 2 && alt == 1);
        CHECK(filterSets == filters + 1 && lastFilter == 0x1c);   /* the cached filter */
        CHECK(Until(&held, 5, 2000));
    } else {
        /* Retries at 1, 3, 7, 15, 23 and 31 quiet seconds, then none. */
        FwSleep(36000);
        CHECK(ntbReads == reads + 1 + 6);
        FwSleep(9000);
        CHECK(ntbReads == reads + 1 + 6);
        CHECK(linkState == 0 && held == 0 && alt == 0);
        /* A reset still recovers it, once the function answers again. */
        failNtb = 0;
        CHECK(Request(dev, CDC_REQ_RESET, 0));
        CHECK(taskStatus == STATUS_SUCCESS && alt == 1 && linkState == 1);
        CHECK(Until(&held, 5, 2000));
    }
    CdcUsbStop(dev);
    CHECK(held == 0);
}

int main(void)
{
    lock = FwLockNew();
    config[2] = (UCHAR)sizeof(config);      /* wTotalLength */
    config[3] = (UCHAR)(sizeof(config) >> 8);
    FakeSubmit = Submit;
    FakeCancel = Cancel;
    Fake.Wdm110 = 1;
    Scenario(0);
    Scenario(1);
    CHECK(Fake.Violations == 0);
    CHECK(Fake.Irps == 0 && Fake.Pool == 0 && Fake.Threads == 0);
    printf("%s: %lu cdcusb resume checks, %lu failed\n", failures ? "FAIL" : "PASS", assertions, failures);
    return failures ? 1 : 0;
}
