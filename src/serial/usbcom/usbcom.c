/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * usbcom.c - USBCOM.SYS: the WDM driver of the Windows 98 SE / ME CDC-ACM
 * package (docs/design/serial.md 5.1.2 route B). It is the function driver
 * of the USB device (or of a function PDO that a composite-splitting bus
 * made), selects the configuration, finds the CDC-ACM functions with
 * acmcore.c, and acts as bus driver for one child PDO per function. The
 * child is installed by USBCOMP.INF as a Ports-class devnode whose loader
 * is VCOMM; USBCOMV.VXD opens the child's device object and uses the
 * direct-call table in ucio.c.
 *
 * The COM number is chosen here, before the child is reported, and written
 * into the child's hardware key as PortName (design 5.1.4).
 *
 * Original code, from the Windows 98 / 2000 DDK documentation and headers.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "usbcom.h"

PDRIVER_OBJECT UcDriver;

/* Ports in use by this driver, by COM number (bit n = COMn). */
static KSPIN_LOCK g_UsedLock;
static AC_U8      g_Used[32];
static LONG       g_TagCounter;

/* Device quirks (design 6.1 item 4). No entry is carried yet: none of the
   devices the design lists needs one per its section 6.1 notes. */
typedef struct UC_QUIRK { USHORT Vid, Pid; ULONG Quirks; } UC_QUIRK;
static const UC_QUIRK g_Quirks[] = { { 0, 0, 0 } };

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path);
static NTSTATUS UcAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo);
static NTSTATUS UcDispatch(PDEVICE_OBJECT dev, PIRP irp);
static NTSTATUS UcDispatchPnp(PDEVICE_OBJECT dev, PIRP irp);
static NTSTATUS UcDispatchPower(PDEVICE_OBJECT dev, PIRP irp);
static VOID UcUnload(PDRIVER_OBJECT drv);

/* ---- trace (test flavours only, usbcom.h) --------------------------------- */

#if defined(UC_TRACING)
#if defined(UC_TRACE_E9)
/* _outp is the compiler's intrinsic for the x86 OUT instruction. */
int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)
#endif

void UcTraceV(const char *fmt, const AC_U32 *v, int n)
{
    char line[200];
    AC_U32 len = AcFormat(line, sizeof(line) - 1, fmt, v, n), i;
    line[len++] = '\n';
    line[len] = 0;
#if defined(UC_TRACE_E9)
    for (i = 0; i < 4; i++)
        _outp(0xE9, "uc: "[i]);
    for (i = 0; i < len; i++)
        _outp(0xE9, line[i]);
#else
    (void)i;
#endif
#if DBG
    DbgPrint("usbcom: %s", line);
#endif
}
#endif

/* ---- small helpers -------------------------------------------------------- */

static NTSTATUS UcCompleteIrp(PIRP irp, NTSTATUS st, ULONG info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static ULONG UcWcsLen(const WCHAR *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

/* Appends `n` chars of s to buf (capacity cap chars, NUL kept). */
static BOOLEAN UcAppend(WCHAR *buf, ULONG cap, const WCHAR *s, ULONG n)
{
    ULONG len = UcWcsLen(buf), i;
    if (len + n + 1 > cap)
        return FALSE;
    for (i = 0; i < n; i++)
        buf[len + i] = s[i];
    buf[len + n] = 0;
    return TRUE;
}

static void UcAppendAscii(WCHAR *buf, ULONG cap, const char *s)
{
    WCHAR w[48];
    ULONG i;
    for (i = 0; s[i] && i < 47; i++)
        w[i] = (WCHAR)s[i];
    w[i] = 0;
    UcAppend(buf, cap, w, i);
}

static void UcAppendHex4(WCHAR *buf, ULONG cap, USHORT v)
{
    static const char hex[] = "0123456789ABCDEF";
    char s[5];
    int i;
    for (i = 3; i >= 0; i--) {
        s[i] = hex[v & 15];
        v >>= 4;
    }
    s[4] = 0;
    UcAppendAscii(buf, cap, s);
}

static void UcAppendDec(WCHAR *buf, ULONG cap, ULONG v)
{
    char s[12];
    int i = 11;
    s[i] = 0;
    do {
        s[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    UcAppendAscii(buf, cap, s + i);
}

static void UcPortNameW(ULONG n, WCHAR *out /* >= 8 */)
{
    char s[8];
    ULONG i, len = AcFormatPortName(n, s);
    for (i = 0; i <= len; i++)
        out[i] = (WCHAR)s[i];
}

static void UcMarkUsed(ULONG n, BOOLEAN used)
{
    KIRQL irql;
    if (n < 1 || n > AC_COM_MAX)
        return;
    KeAcquireSpinLock(&g_UsedLock, &irql);
    if (used)
        g_Used[n >> 3] |= (AC_U8)(1u << (n & 7));
    else
        g_Used[n >> 3] &= (AC_U8)~(1u << (n & 7));
    KeReleaseSpinLock(&g_UsedLock, irql);
}

/* ---- synchronous calls to the lower driver ------------------------------- */

static NTSTATUS UcSignal(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS UcCallLowerSync(UC_FDO *fdo, PIRP irp)
{
    KEVENT ev;
    NTSTATUS st;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, UcSignal, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(fdo->Lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    return st;
}

NTSTATUS UcSubmitUrbSync(UC_FDO *fdo, PURB urb)
{
    KEVENT ev;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION s;
    NTSTATUS st;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, fdo->Lower,
                                        NULL, 0, NULL, 0, TRUE, &ev, &iosb);
    if (irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    s = IoGetNextIrpStackLocation(irp);
    s->Parameters.Others.Argument1 = urb;
    st = IoCallDriver(fdo->Lower, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    return st;
}

/* ---- registry ---------------------------------------------------------------- */

static NTSTATUS UcOpenKey(const WCHAR *path, ACCESS_MASK access, HANDLE *h)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwOpenKey(h, access, &oa);
}

static NTSTATUS UcSetString(HANDLE h, const WCHAR *name, const WCHAR *value)
{
    UNICODE_STRING us;
    RtlInitUnicodeString(&us, name);
    return ZwSetValueKey(h, &us, 0, REG_SZ, (PVOID)value,
                         (UcWcsLen(value) + 1) * sizeof(WCHAR));
}

/* Reads a value into the caller's KEY_VALUE_PARTIAL_INFORMATION buffer. */
static PKEY_VALUE_PARTIAL_INFORMATION UcQuery(HANDLE h, const WCHAR *name,
                                              PUCHAR buf, ULONG size)
{
    UNICODE_STRING us;
    ULONG got = 0;
    RtlInitUnicodeString(&us, name);
    if (!NT_SUCCESS(ZwQueryValueKey(h, &us, KeyValuePartialInformation,
                                    buf, size, &got)))
        return NULL;
    return (PKEY_VALUE_PARTIAL_INFORMATION)buf;
}

/* COM number from a REG_SZ "COMn" value, 0 if none. */
static ULONG UcReadPortName(HANDLE h, PUCHAR buf, ULONG size)
{
    PKEY_VALUE_PARTIAL_INFORMATION v = UcQuery(h, L"PortName", buf, size);
    char s[8];
    ULONG i, n;
    const WCHAR *w;

    if (v == NULL || v->Type != REG_SZ || v->DataLength < 2 * sizeof(WCHAR))
        return 0;
    w = (const WCHAR *)v->Data;
    n = v->DataLength / sizeof(WCHAR);
    if (n > 7)
        return 0;
    for (i = 0; i < n && w[i]; i++) {
        if (w[i] > 0x7F)
            return 0;
        s[i] = (char)w[i];
    }
    return AcParsePortName(s, i);
}

/*
 * Marks every PortName found under HKLM\Enum\<bus>\<id>\<instance> in
 * `used`, except the instance `self` of USBCOM\COMPORT. Returns FALSE if
 * the Enum tree could not be read at all. Bounded to 4096 keys.
 */
static BOOLEAN UcScanEnum(AC_U8 used[32], const WCHAR *self)
{
    static const WCHAR root[] = L"\\Registry\\Machine\\Enum";
    PUCHAR info, val;
    WCHAR *path;
    HANDLE h0;
    ULONG i0, budget = 4096;
    const ULONG infoSize = 512, valSize = 64, cap = 384;

    if (!NT_SUCCESS(UcOpenKey(root, KEY_READ, &h0)))
        return FALSE;
    info = (PUCHAR)ExAllocatePool(PagedPool, 3 * infoSize + valSize + cap * sizeof(WCHAR));
    if (info == NULL) {
        ZwClose(h0);
        return FALSE;
    }
    val = info + 3 * infoSize;
    path = (WCHAR *)(val + valSize);

    for (i0 = 0; budget; i0++) {
        PKEY_BASIC_INFORMATION k0 = (PKEY_BASIC_INFORMATION)info;
        ULONG got, i1, len0;
        HANDLE h1;
        if (!NT_SUCCESS(ZwEnumerateKey(h0, i0, KeyBasicInformation, k0, infoSize, &got)))
            break;
        path[0] = 0;
        UcAppend(path, cap, root, UcWcsLen(root));
        UcAppendAscii(path, cap, "\\");
        if (!UcAppend(path, cap, k0->Name, k0->NameLength / sizeof(WCHAR)))
            continue;
        len0 = UcWcsLen(path);
        if (!NT_SUCCESS(UcOpenKey(path, KEY_READ, &h1)))
            continue;
        for (i1 = 0; budget; i1++) {
            PKEY_BASIC_INFORMATION k1 = (PKEY_BASIC_INFORMATION)(info + infoSize);
            ULONG i2, len1;
            HANDLE h2;
            if (!NT_SUCCESS(ZwEnumerateKey(h1, i1, KeyBasicInformation, k1, infoSize, &got)))
                break;
            path[len0] = 0;
            UcAppendAscii(path, cap, "\\");
            if (!UcAppend(path, cap, k1->Name, k1->NameLength / sizeof(WCHAR)))
                continue;
            len1 = UcWcsLen(path);
            if (!NT_SUCCESS(UcOpenKey(path, KEY_READ, &h2)))
                continue;
            for (i2 = 0; budget; i2++, budget--) {
                PKEY_BASIC_INFORMATION k2 = (PKEY_BASIC_INFORMATION)(info + 2 * infoSize);
                HANDLE h3;
                ULONG n;
                if (!NT_SUCCESS(ZwEnumerateKey(h2, i2, KeyBasicInformation, k2, infoSize, &got)))
                    break;
                path[len1] = 0;
                UcAppendAscii(path, cap, "\\");
                if (!UcAppend(path, cap, k2->Name, k2->NameLength / sizeof(WCHAR)))
                    continue;
                if (self) {
                    UNICODE_STRING a, b;
                    WCHAR mine[128];
                    mine[0] = 0;
                    UcAppend(mine, 128, root, UcWcsLen(root));
                    UcAppendAscii(mine, 128, "\\USBCOM\\COMPORT\\");
                    UcAppend(mine, 128, self, UcWcsLen(self));
                    RtlInitUnicodeString(&a, mine);
                    RtlInitUnicodeString(&b, path);
                    if (RtlEqualUnicodeString(&a, &b, TRUE))
                        continue;
                }
                if (!NT_SUCCESS(UcOpenKey(path, KEY_READ, &h3)))
                    continue;
                n = UcReadPortName(h3, val, valSize);
                if (n) {
                    used[n >> 3] |= (AC_U8)(1u << (n & 7));
                    UCT2("Enum scan: COM%lu at %w", n, path);
                }
                ZwClose(h3);
            }
            ZwClose(h2);
        }
        ZwClose(h1);
    }
    ExFreePool(info);
    ZwClose(h0);
    return TRUE;
}

/* The value name under the device's driver key that remembers port n's COM. */
static void UcRememberName(ULONG index, WCHAR *out /* >= 16 */)
{
    out[0] = 0;
    UcAppendAscii(out, 16, "UsbComPort");
    UcAppendDec(out, 16, index);
}

static ULONG UcReadRemembered(UC_FDO *fdo, ULONG index)
{
    HANDLE h;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
    PKEY_VALUE_PARTIAL_INFORMATION v;
    WCHAR name[16];
    ULONG n = 0;

    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(fdo->Pdo, PLUGPLAY_REGKEY_DRIVER,
                                            KEY_READ, &h)))
        return 0;
    UcRememberName(index, name);
    v = UcQuery(h, name, buf, sizeof(buf));
    if (v && v->Type == REG_DWORD && v->DataLength == sizeof(ULONG))
        n = *(ULONG UNALIGNED *)v->Data;
    ZwClose(h);
    return (n >= 1 && n <= AC_COM_MAX) ? n : 0;
}

static void UcWriteRemembered(UC_FDO *fdo, ULONG index, ULONG n)
{
    HANDLE h;
    UNICODE_STRING us;
    WCHAR name[16];

    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(fdo->Pdo, PLUGPLAY_REGKEY_DRIVER,
                                            KEY_WRITE, &h)))
        return;
    UcRememberName(index, name);
    RtlInitUnicodeString(&us, name);
    ZwSetValueKey(h, &us, 0, REG_DWORD, &n, sizeof(n));
    ZwClose(h);
}

/*
 * Writes PortName and FriendlyName into the child's hardware key (design
 * 5.1.4). Tried on every PnP IRP of the PDO until the devnode exists.
 */
static void UcWriteNames(UC_PORT *p)
{
    HANDLE h;
    WCHAR com[8], friendly[48];

    if (p->NamesWritten || p->Pdo == NULL)
        return;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(p->Pdo, PLUGPLAY_REGKEY_DEVICE,
                                            KEY_WRITE, &h)))
        return;
    UcPortNameW(p->PortNumber, com);
    friendly[0] = 0;
    UcAppendAscii(friendly, 48, "USB Serial Port (");
    UcAppend(friendly, 48, com, UcWcsLen(com));
    UcAppendAscii(friendly, 48, ")");
    if (NT_SUCCESS(UcSetString(h, L"PortName", com)) &&
        NT_SUCCESS(UcSetString(h, L"FriendlyName", friendly)))
        p->NamesWritten = TRUE;
    UCT2("PortName %w written to the child's hardware key: %lu", com, p->NamesWritten);
    ZwClose(h);
}

/* HKLM\Hardware\DeviceMap\SerialComm, which some programs enumerate. */
static void UcWriteSerialComm(UC_PORT *p)
{
    UNICODE_STRING us;
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    ULONG disp;
    WCHAR com[8], name[32];

    if (p->SerialCommWritten)
        return;
    RtlInitUnicodeString(&us, L"\\Registry\\Machine\\Hardware\\DeviceMap\\SerialComm");
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_WRITE, &oa, 0, NULL, REG_OPTION_VOLATILE, &disp)))
        return;
    UcPortNameW(p->PortNumber, com);
    name[0] = 0;
    UcAppendAscii(name, 32, "UsbCom");
    UcAppend(name, 32, com, UcWcsLen(com));
    if (NT_SUCCESS(UcSetString(h, name, com)))
        p->SerialCommWritten = TRUE;
    UCT2("SERIALCOMM %w: %lu", name, p->SerialCommWritten);
    ZwClose(h);
}

/* ---- configuration ------------------------------------------------------ */

static ULONG UcLookupQuirks(USHORT vid, USHORT pid)
{
    ULONG i;
    for (i = 0; g_Quirks[i].Vid; i++)
        if (g_Quirks[i].Vid == vid && g_Quirks[i].Pid == pid)
            return g_Quirks[i].Quirks;
    return 0;
}

static NTSTATUS UcGetDescriptor(UC_FDO *fdo, UCHAR type, PVOID buf, ULONG len)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
    NTSTATUS st;

    RtlZeroMemory(&urb, sizeof(urb));
    UsbBuildGetDescriptorRequest((PURB)&urb, sizeof(urb), type, 0, 0, buf, NULL, len, NULL);
    st = UcSubmitUrbSync(fdo, (PURB)&urb);
    if (NT_SUCCESS(st) && urb.TransferBufferLength < len && type == USB_DEVICE_DESCRIPTOR_TYPE)
        st = STATUS_DEVICE_DATA_ERROR;
    return st;
}

static void UcDeconfigure(UC_FDO *fdo)
{
    struct _URB_SELECT_CONFIGURATION urb;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb.ConfigurationDescriptor = NULL;
    (void)UcSubmitUrbSync(fdo, (PURB)&urb);
}

#define UC_IFACE_SIZE(n) (FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + \
                          (n) * sizeof(USBD_PIPE_INFORMATION))

/*
 * Reads the descriptors, finds the CDC-ACM functions and selects the
 * configuration with every interface at the alternate setting the
 * functions need (alternate 0 for the others). Fills `funcs`.
 */
static NTSTATUS UcConfigure(UC_FDO *fdo, AC_FUNC *funcs, int *nfuncs)
{
    USB_CONFIGURATION_DESCRIPTOR head;
    AC_IFACE *ifs = NULL;
    PURB urb = NULL;
    PUCHAR cursor;
    ULONG total, size, i, j;
    int nifs, nf;
    NTSTATUS st;
    UCHAR seen[256];

    st = UcGetDescriptor(fdo, USB_DEVICE_DESCRIPTOR_TYPE, &fdo->DevDesc, sizeof(fdo->DevDesc));
    if (!NT_SUCCESS(st))
        return st;
    fdo->Quirks = UcLookupQuirks(fdo->DevDesc.idVendor, fdo->DevDesc.idProduct);

    RtlZeroMemory(&head, sizeof(head));
    st = UcGetDescriptor(fdo, USB_CONFIGURATION_DESCRIPTOR_TYPE, &head, sizeof(head));
    if (!NT_SUCCESS(st))
        return st;
    total = head.wTotalLength;
    if (total < sizeof(head) || total > 4096)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (fdo->Config)
        ExFreePool(fdo->Config);
    fdo->Config = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePool(NonPagedPool, total);
    if (fdo->Config == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(fdo->Config, total);
    st = UcGetDescriptor(fdo, USB_CONFIGURATION_DESCRIPTOR_TYPE, fdo->Config, total);
    if (!NT_SUCCESS(st))
        return st;
    fdo->ConfigLength = total;

    nf = AcFindFunctions((const AC_U8 *)fdo->Config, total, fdo->Quirks, funcs, AC_MAX_FUNCS);
    UCT4("device %04x:%04x, configuration %lu bytes, %ld CDC-ACM functions",
         fdo->DevDesc.idVendor, fdo->DevDesc.idProduct, total, nf);
    if (nf > 0)
        UCT4("function 0: comm if %lu data if %lu, in max %lu out max %lu",
             funcs[0].CommIf, funcs[0].DataIf, funcs[0].InMax, funcs[0].OutMax);
    if (nf <= 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;   /* not CDC-ACM: Code 10 */

    ifs = (AC_IFACE *)ExAllocatePool(PagedPool, 32 * sizeof(AC_IFACE));
    if (ifs == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    nifs = AcListInterfaces((const AC_U8 *)fdo->Config, total, ifs, 32);
    if (nifs <= 0) {
        ExFreePool(ifs);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* One entry per interface number, at the wanted alternate setting. */
    RtlZeroMemory(seen, sizeof(seen));
    size = FIELD_OFFSET(struct _URB_SELECT_CONFIGURATION, Interface);
    for (i = 0; i < (ULONG)nifs; i++) {
        UCHAR want = 0;
        int k;
        for (k = 0; k < nf; k++) {
            if (funcs[k].DataIf == ifs[i].Number) want = funcs[k].DataAlt;
            else if (funcs[k].CommIf == ifs[i].Number) want = funcs[k].CommAlt;
        }
        if (ifs[i].Alt != want || seen[ifs[i].Number])
            continue;
        seen[ifs[i].Number] = 1;
        size += UC_IFACE_SIZE(ifs[i].NumEp);
    }

    urb = (PURB)ExAllocatePool(NonPagedPool, size);
    if (urb == NULL) {
        ExFreePool(ifs);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(urb, size);
    UsbBuildSelectConfigurationRequest(urb, (USHORT)size, fdo->Config);
    cursor = (PUCHAR)&urb->UrbSelectConfiguration.Interface;
    RtlZeroMemory(seen, sizeof(seen));
    for (i = 0; i < (ULONG)nifs; i++) {
        PUSBD_INTERFACE_INFORMATION info = (PUSBD_INTERFACE_INFORMATION)cursor;
        UCHAR want = 0;
        int k;
        for (k = 0; k < nf; k++) {
            if (funcs[k].DataIf == ifs[i].Number) want = funcs[k].DataAlt;
            else if (funcs[k].CommIf == ifs[i].Number) want = funcs[k].CommAlt;
        }
        if (ifs[i].Alt != want || seen[ifs[i].Number])
            continue;
        seen[ifs[i].Number] = 1;
        info->Length = (USHORT)UC_IFACE_SIZE(ifs[i].NumEp);
        info->InterfaceNumber = ifs[i].Number;
        info->AlternateSetting = ifs[i].Alt;
        info->NumberOfPipes = ifs[i].NumEp;
        for (j = 0; j < ifs[i].NumEp; j++)
            info->Pipes[j].MaximumTransferSize = UC_IN_SIZE;
        cursor += info->Length;
    }
    ExFreePool(ifs);

    st = UcSubmitUrbSync(fdo, urb);
    UCT2("select configuration (%lu bytes): %08lx", size, st);
    if (!NT_SUCCESS(st)) {
        ExFreePool(urb);
        return st;
    }
    if (fdo->Select)
        ExFreePool(fdo->Select);
    fdo->Select = urb;
    *nfuncs = nf;
    return STATUS_SUCCESS;
}

/* Looks up a pipe handle by endpoint address in the selected configuration. */
static USBD_PIPE_HANDLE UcFindPipe(UC_FDO *fdo, UCHAR ep)
{
    PUCHAR cursor = (PUCHAR)&fdo->Select->UrbSelectConfiguration.Interface;
    PUCHAR end = (PUCHAR)fdo->Select + fdo->Select->UrbHeader.Length;
    ULONG j;

    if (ep == 0)
        return NULL;
    while (cursor + FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) <= end) {
        PUSBD_INTERFACE_INFORMATION info = (PUSBD_INTERFACE_INFORMATION)cursor;
        if (info->Length == 0)
            break;
        for (j = 0; j < info->NumberOfPipes; j++)
            if (info->Pipes[j].EndpointAddress == ep)
                return info->Pipes[j].PipeHandle;
        cursor += info->Length;
    }
    return NULL;
}

/* Creates the child PDO of one port, named \Device\UsbComCOMn. */
static NTSTATUS UcCreatePdo(UC_FDO *fdo, UC_PORT *p)
{
    WCHAR name[40], com[8];
    UNICODE_STRING us;
    PDEVICE_OBJECT pdo;
    UC_PDOX *x;
    AC_U8 used[32];
    ULONG remembered, n, minimum = 3;
    KIRQL irql;
    NTSTATUS st;

    /* Instance id <VID>_<PID>_<driver key tag>_<n> (design 5.1.2 item 1). */
    p->InstanceId[0] = 0;
    UcAppendHex4(p->InstanceId, 64, fdo->DevDesc.idVendor);
    UcAppendAscii(p->InstanceId, 64, "_");
    UcAppendHex4(p->InstanceId, 64, fdo->DevDesc.idProduct);
    UcAppendAscii(p->InstanceId, 64, "_");
    UcAppend(p->InstanceId, 64, fdo->KeyTag, UcWcsLen(fdo->KeyTag));
    UcAppendAscii(p->InstanceId, 64, "_");
    UcAppendDec(p->InstanceId, 64, p->Index);

    RtlZeroMemory(used, sizeof(used));
    if (!UcScanEnum(used, p->InstanceId)) {
        minimum = 5;            /* Enum unreadable: keep clear of COM1-4 */
        UCT0("Enum scan failed: choosing from COM5");
    }
    UCT1("Enum scan: used map %08lx (COM0-31)", *(AC_U32 *)used);
    KeAcquireSpinLock(&g_UsedLock, &irql);
    for (n = 0; n < 32; n++)
        used[n] |= g_Used[n];
    KeReleaseSpinLock(&g_UsedLock, irql);
    remembered = UcReadRemembered(fdo, p->Index);
    n = AcChoosePort(used, remembered, minimum);
    if (n == 0)
        return STATUS_INSUFFICIENT_RESOURCES;
    UcMarkUsed(n, TRUE);
    p->PortNumber = n;
    p->Table.PortNumber = n;
    if (n != remembered)
        UcWriteRemembered(fdo, p->Index, n);

    name[0] = 0;
    UcAppendAscii(name, 40, UCIF_DEVICE_PREFIX);
    UcPortNameW(n, com);
    UcAppend(name, 40, com, UcWcsLen(com));
    RtlInitUnicodeString(&us, name);
    st = IoCreateDevice(UcDriver, sizeof(UC_PDOX), &us, FILE_DEVICE_SERIAL_PORT,
                        0, FALSE, &pdo);
    if (!NT_SUCCESS(st)) {
        UcMarkUsed(n, FALSE);
        return st;
    }
    x = (UC_PDOX *)pdo->DeviceExtension;
    x->Common.Kind = UcKindPdo;
    x->Port = p;
    p->Pdo = pdo;
    pdo->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    UCT3("port %lu is COM%lu, instance %w", p->Index, n, p->InstanceId);
    return STATUS_SUCCESS;
}

static NTSTATUS UcFdoStart(UC_FDO *fdo)
{
    AC_FUNC funcs[AC_MAX_FUNCS];
    int nf = 0, i;
    NTSTATUS st;
    BOOLEAN created = FALSE;

    st = UcConfigure(fdo, funcs, &nf);
    if (!NT_SUCCESS(st)) {
        UCT1("configure failed %08lx", st);
        return st;
    }
    /* Every function must have its bulk pipes before any port is made. */
    for (i = 0; i < nf; i++)
        if (UcFindPipe(fdo, funcs[i].BulkIn) == NULL || UcFindPipe(fdo, funcs[i].BulkOut) == NULL)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    if ((ULONG)nf < fdo->NumPorts)
        return STATUS_DEVICE_CONFIGURATION_ERROR;   /* fewer functions than before */

    for (i = 0; i < nf; i++) {
        UC_PORT *p;
        if ((ULONG)i < fdo->NumPorts) {
            p = fdo->Ports[i];
            p->Func = funcs[i];
        } else {
            st = UcPortCreate(fdo, (ULONG)i, &funcs[i], &p);
            if (!NT_SUCCESS(st))
                break;
            fdo->Ports[i] = p;
            fdo->NumPorts = (ULONG)i + 1;
            st = UcCreatePdo(fdo, p);
            if (!NT_SUCCESS(st)) {
                fdo->NumPorts = (ULONG)i;
                fdo->Ports[i] = NULL;
                UcPortFree(p);
                break;
            }
            created = TRUE;
        }
        p->InPipe = UcFindPipe(fdo, funcs[i].BulkIn);
        p->OutPipe = UcFindPipe(fdo, funcs[i].BulkOut);
        p->NotifyPipe = UcFindPipe(fdo, funcs[i].NotifyEp);
    }
    if (!NT_SUCCESS(st) || fdo->NumPorts == 0) {
        if (created)
            IoInvalidateDeviceRelations(fdo->Pdo, BusRelations);
        return NT_SUCCESS(st) ? STATUS_DEVICE_CONFIGURATION_ERROR : st;
    }
    fdo->Started = TRUE;
    for (i = 0; (ULONG)i < fdo->NumPorts; i++) {
        UcPortStart(fdo->Ports[i]);
        UcWriteSerialComm(fdo->Ports[i]);
    }
    if (created)
        IoInvalidateDeviceRelations(fdo->Pdo, BusRelations);
    return STATUS_SUCCESS;
}

static void UcFdoStopPorts(UC_FDO *fdo)
{
    ULONG i;
    for (i = 0; i < fdo->NumPorts; i++) {
        UC_PORT *p = fdo->Ports[i];
        UcPortStop(p);
        p->Started = FALSE;
    }
}

/* ---- IRP accounting on the FDO --------------------------------------------- */

static BOOLEAN UcAcquire(UC_FDO *fdo)
{
    InterlockedIncrement(&fdo->IoCount);
    if (fdo->Removed) {
        if (InterlockedDecrement(&fdo->IoCount) == 0)
            KeSetEvent(&fdo->RemoveEvent, IO_NO_INCREMENT, FALSE);
        return FALSE;
    }
    return TRUE;
}

static void UcRelease(UC_FDO *fdo)
{
    if (InterlockedDecrement(&fdo->IoCount) == 0)
        KeSetEvent(&fdo->RemoveEvent, IO_NO_INCREMENT, FALSE);
}

/* ---- DriverEntry / AddDevice -------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
    UNREFERENCED_PARAMETER(path);
    UCT0("DriverEntry");
    UcDriver = drv;
    KeInitializeSpinLock(&g_UsedLock);
    drv->MajorFunction[IRP_MJ_CREATE] = UcDispatch;
    drv->MajorFunction[IRP_MJ_CLOSE] = UcDispatch;
    drv->MajorFunction[IRP_MJ_CLEANUP] = UcDispatch;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = UcDispatch;
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = UcDispatch;
    drv->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = UcDispatch;
    drv->MajorFunction[IRP_MJ_PNP] = UcDispatchPnp;
    drv->MajorFunction[IRP_MJ_POWER] = UcDispatchPower;
    drv->DriverExtension->AddDevice = UcAddDevice;
    drv->DriverUnload = UcUnload;
    return STATUS_SUCCESS;
}

static VOID UcUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

/* A short tag from the USB devnode's driver key name, e.g. USB\0003 -> 0003. */
static void UcMakeKeyTag(UC_FDO *fdo)
{
    WCHAR buf[128];
    ULONG got = 0, i, start = 0, n = 0;

    fdo->KeyTag[0] = 0;
    if (NT_SUCCESS(IoGetDeviceProperty(fdo->Pdo, DevicePropertyDriverKeyName,
                                       sizeof(buf) - sizeof(WCHAR), buf, &got))) {
        buf[got / sizeof(WCHAR) < 127 ? got / sizeof(WCHAR) : 127] = 0;
        for (i = 0; buf[i]; i++)
            if (buf[i] == L'\\')
                start = i + 1;
        for (i = start; buf[i] && n < 16; i++) {
            WCHAR c = buf[i];
            if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                (c >= L'a' && c <= L'z'))
                fdo->KeyTag[n++] = c;
        }
        fdo->KeyTag[n] = 0;
    }
    if (n == 0) {
        UcAppendAscii(fdo->KeyTag, 24, "T");
        UcAppendDec(fdo->KeyTag, 24, (ULONG)InterlockedIncrement(&g_TagCounter));
    }
}

static NTSTATUS UcAddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT dev;
    UC_FDO *fdo;
    NTSTATUS st;

    st = IoCreateDevice(drv, sizeof(UC_FDO), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
    if (!NT_SUCCESS(st))
        return st;
    fdo = (UC_FDO *)dev->DeviceExtension;
    RtlZeroMemory(fdo, sizeof(*fdo));
    fdo->Common.Kind = UcKindFdo;
    fdo->Self = dev;
    fdo->Pdo = pdo;
    fdo->IoCount = 1;
    KeInitializeEvent(&fdo->RemoveEvent, NotificationEvent, FALSE);
    fdo->Lower = IoAttachDeviceToDeviceStack(dev, pdo);
    if (fdo->Lower == NULL) {
        IoDeleteDevice(dev);
        return STATUS_NO_SUCH_DEVICE;
    }
    UcMakeKeyTag(fdo);
    UCT2("AddDevice: PDO %08lx, key tag %w", pdo, fdo->KeyTag);
    dev->Flags |= DO_POWER_PAGABLE;
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* ---- create / close / ioctl ---------------------------------------------- */

static NTSTATUS UcDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    UC_COMMON *c = (UC_COMMON *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);

    if (c->Kind == UcKindFdo) {
        UC_FDO *fdo = (UC_FDO *)c;
        NTSTATUS st;
        if (s->MajorFunction != IRP_MJ_SYSTEM_CONTROL)
            return UcCompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (!UcAcquire(fdo))
            return UcCompleteIrp(irp, STATUS_DELETE_PENDING, 0);
        IoSkipCurrentIrpStackLocation(irp);
        st = IoCallDriver(fdo->Lower, irp);
        UcRelease(fdo);
        return st;
    }

    switch (s->MajorFunction) {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
    case IRP_MJ_CLEANUP:
        return UcCompleteIrp(irp, STATUS_SUCCESS, 0);
    case IRP_MJ_DEVICE_CONTROL:
        if (s->Parameters.DeviceIoControl.IoControlCode == IOCTL_USBCOM_GET_INTERFACE) {
            UC_PORT *p = ((UC_PDOX *)c)->Port;
            UCIF_REQUEST *req = (UCIF_REQUEST *)irp->AssociatedIrp.SystemBuffer;
            UCIF_REPLY *rep = (UCIF_REPLY *)irp->AssociatedIrp.SystemBuffer;
            if (s->Parameters.DeviceIoControl.InputBufferLength < sizeof(UCIF_REQUEST) ||
                s->Parameters.DeviceIoControl.OutputBufferLength < sizeof(UCIF_REPLY) ||
                req == NULL || req->Magic != UCIF_MAGIC || req->Version != UCIF_VERSION)
                return UcCompleteIrp(irp, STATUS_INVALID_PARAMETER, 0);
            rep->Magic = UCIF_MAGIC;
            rep->Table = &p->Table;
            UCT1("get interface: COM%lu", p->PortNumber);
            return UcCompleteIrp(irp, STATUS_SUCCESS, sizeof(UCIF_REPLY));
        }
        return UcCompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    case IRP_MJ_SYSTEM_CONTROL: {
        NTSTATUS st = irp->IoStatus.Status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }
    default:
        return UcCompleteIrp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* ---- PnP: child PDO -------------------------------------------------------- */

static NTSTATUS UcAllocString(PIRP irp, const WCHAR *s, ULONG chars /* incl. NULs */)
{
    PWCHAR b = (PWCHAR)ExAllocatePool(PagedPool, chars * sizeof(WCHAR));
    if (b == NULL)
        return UcCompleteIrp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    RtlCopyMemory(b, s, chars * sizeof(WCHAR));
    return UcCompleteIrp(irp, STATUS_SUCCESS, (ULONG)b);
}

static NTSTATUS UcPdoPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    UC_PDOX *x = (UC_PDOX *)dev->DeviceExtension;
    UC_PORT *p = x->Port;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = irp->IoStatus.Status;

    UcWriteNames(p);
    UCT2("child COM%lu PnP %02lx", p->PortNumber, s->MinorFunction);

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        /* Not sent on 98 SE: the child's loader is VCOMM, not NTKERN
           (guest leg, 2026-10-22). The port runs from the FDO's start. */
        p->PdoStarted = TRUE;
        if (p->Fdo->Started)
            UcPortStart(p);
        UcWriteSerialComm(p);
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE:
        /* 98 SE sends REMOVE to the child when its driver is (re)installed
           and then no START (guest leg, 2026-10-22), so these do not stop
           the port: it lives as long as the FDO is started, and the FDO's
           stop or removal tells an open port it is gone. The PDO itself is
           deleted when the parent goes (UcFdoRemove). */
        p->PdoStarted = FALSE;
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_ID:
        switch (s->Parameters.QueryId.IdType) {
        case BusQueryDeviceID:
            return UcAllocString(irp, L"USBCOM\\COMPORT", 15);
        case BusQueryHardwareIDs:
            return UcAllocString(irp, L"USBCOM\\COMPORT\0", 16);
        case BusQueryInstanceID:
            return UcAllocString(irp, p->InstanceId, UcWcsLen(p->InstanceId) + 1);
        default:
            break;
        }
        break;
    case IRP_MN_QUERY_DEVICE_TEXT:
        if (s->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
            return UcAllocString(irp, L"USB Serial Port", 16);
        if (s->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
            return UcAllocString(irp, L"USB CDC-ACM function", 21);
        break;
    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES caps = s->Parameters.DeviceCapabilities.Capabilities;
        ULONG i;
        if (caps->Version < 1 || caps->Size < sizeof(DEVICE_CAPABILITIES)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        caps->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++)
            caps->DeviceState[i] = PowerDeviceD3;
        caps->DeviceWake = PowerDeviceUnspecified;
        caps->SystemWake = PowerSystemUnspecified;
        caps->LockSupported = FALSE;
        caps->EjectSupported = FALSE;
        caps->Removable = FALSE;
        caps->DockDevice = FALSE;
        caps->UniqueID = TRUE;
        caps->SilentInstall = FALSE;
        caps->RawDeviceOK = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->Address = p->Index;
        caps->UINumber = p->PortNumber;
        st = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS r = (PDEVICE_RELATIONS)ExAllocatePool(PagedPool, sizeof(DEVICE_RELATIONS));
            if (r == NULL)
                return UcCompleteIrp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
            r->Count = 1;
            r->Objects[0] = dev;
            ObReferenceObject(dev);
            return UcCompleteIrp(irp, STATUS_SUCCESS, (ULONG)r);
        }
        break;
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
    case IRP_MN_QUERY_RESOURCES:
        /* No resources: the reason VCOMM assigns no PortName (5.1.4). */
        break;
    default:
        break;
    }
    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

/* ---- PnP: FDO ---------------------------------------------------------------- */

static NTSTATUS UcFdoRelations(UC_FDO *fdo, PIRP irp)
{
    PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)irp->IoStatus.Information, r;
    ULONG count = old ? old->Count : 0, i, n = 0, size;

    for (i = 0; i < fdo->NumPorts; i++)
        if (fdo->Ports[i]->Pdo) n++;
    size = sizeof(DEVICE_RELATIONS) + (count + n) * sizeof(PDEVICE_OBJECT);
    r = (PDEVICE_RELATIONS)ExAllocatePool(PagedPool, size);
    if (r == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    r->Count = 0;
    for (i = 0; i < count; i++)
        r->Objects[r->Count++] = old->Objects[i];
    for (i = 0; i < fdo->NumPorts; i++) {
        PDEVICE_OBJECT pdo = fdo->Ports[i]->Pdo;
        if (pdo) {
            ObReferenceObject(pdo);
            r->Objects[r->Count++] = pdo;
        }
    }
    if (old)
        ExFreePool(old);
    irp->IoStatus.Information = (ULONG)r;
    irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

static NTSTATUS UcFdoRemove(UC_FDO *fdo, PIRP irp)
{
    NTSTATUS st;
    ULONG i;

    fdo->Removed = TRUE;
    UcFdoStopPorts(fdo);
    /* Drop AddDevice's reference and our own, then wait for other IRPs. */
    UcRelease(fdo);
    UcRelease(fdo);
    KeWaitForSingleObject(&fdo->RemoveEvent, Executive, KernelMode, FALSE, NULL);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(irp);
    st = IoCallDriver(fdo->Lower, irp);

    for (i = 0; i < fdo->NumPorts; i++) {
        UC_PORT *p = fdo->Ports[i];
        UcMarkUsed(p->PortNumber, FALSE);
        if (p->Pdo)
            IoDeleteDevice(p->Pdo);
        UcPortFree(p);
        fdo->Ports[i] = NULL;
    }
    fdo->NumPorts = 0;
    if (fdo->Select) ExFreePool(fdo->Select);
    if (fdo->Config) ExFreePool(fdo->Config);
    IoDetachDevice(fdo->Lower);
    IoDeleteDevice(fdo->Self);
    return st;
}

static NTSTATUS UcFdoPnp(UC_FDO *fdo, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;

    if (!UcAcquire(fdo))
        return UcCompleteIrp(irp, STATUS_DELETE_PENDING, 0);
    UCT1("FDO PnP %02lx", s->MinorFunction);

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = UcCallLowerSync(fdo, irp);
        if (NT_SUCCESS(st))
            st = UcFdoStart(fdo);
        UCT2("FDO start: %08lx, %lu ports", st, fdo->NumPorts);
        UcRelease(fdo);
        return UcCompleteIrp(irp, st, 0);

    case IRP_MN_REMOVE_DEVICE:
        return UcFdoRemove(fdo, irp);

    case IRP_MN_SURPRISE_REMOVAL:
        UcFdoStopPorts(fdo);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_STOP_DEVICE:
        UcFdoStopPorts(fdo);
        if (fdo->Started)
            UcDeconfigure(fdo);
        fdo->Started = FALSE;
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (s->Parameters.QueryDeviceRelations.Type == BusRelations) {
            st = UcFdoRelations(fdo, irp);
            if (!NT_SUCCESS(st)) {
                UcRelease(fdo);
                return UcCompleteIrp(irp, st, 0);
            }
        }
        break;

    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(irp);
    st = IoCallDriver(fdo->Lower, irp);
    UcRelease(fdo);
    return st;
}

static NTSTATUS UcDispatchPnp(PDEVICE_OBJECT dev, PIRP irp)
{
    UC_COMMON *c = (UC_COMMON *)dev->DeviceExtension;
    if (c->Kind == UcKindFdo)
        return UcFdoPnp((UC_FDO *)c, irp);
    return UcPdoPnp(dev, irp);
}

/* ---- power (design 7.10) ---------------------------------------------------- */

static NTSTATUS UcPowerUpDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UC_FDO *fdo = (UC_FDO *)ctx;
    ULONG i;

    UNREFERENCED_PARAMETER(dev);
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status))
        for (i = 0; i < fdo->NumPorts; i++)
            UcPortResume(fdo->Ports[i]);
    PoStartNextPowerIrp(irp);
    UcRelease(fdo);
    return STATUS_SUCCESS;
}

/*
 * The FDO is the power policy owner: system power IRPs are mapped to
 * device power IRPs (S0 -> D0 after the S-IRP completes; a sleep state ->
 * D3 before the S-IRP goes down). The D-IRPs come back through
 * UcDispatchPower, which suspends and resumes the ports.
 */
static VOID UcDevIrpDone(PDEVICE_OBJECT dev, UCHAR minor, POWER_STATE state,
                         PVOID ctx, PIO_STATUS_BLOCK iosb)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(minor);
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(iosb);
}

static NTSTATUS UcSystemUpDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UC_FDO *fdo = (UC_FDO *)ctx;
    POWER_STATE ps;

    UNREFERENCED_PARAMETER(dev);
    if (irp->PendingReturned)
        IoMarkIrpPending(irp);
    ps.DeviceState = PowerDeviceD0;
    if (NT_SUCCESS(irp->IoStatus.Status) && !fdo->Removed)
        (void)PoRequestPowerIrp(fdo->Pdo, IRP_MN_SET_POWER, ps, UcDevIrpDone, NULL, NULL);
    PoStartNextPowerIrp(irp);
    UcRelease(fdo);
    return STATUS_SUCCESS;
}

static VOID UcSleepDevDone(PDEVICE_OBJECT dev, UCHAR minor, POWER_STATE state,
                           PVOID ctx, PIO_STATUS_BLOCK iosb)
{
    PIRP sirp = (PIRP)ctx;
    UC_FDO *fdo = (UC_FDO *)IoGetCurrentIrpStackLocation(sirp)->DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(minor);
    UNREFERENCED_PARAMETER(state);
    UNREFERENCED_PARAMETER(iosb);
    PoStartNextPowerIrp(sirp);
    IoSkipCurrentIrpStackLocation(sirp);
    PoCallDriver(fdo->Lower, sirp);
    UcRelease(fdo);
}

static NTSTATUS UcDispatchPower(PDEVICE_OBJECT dev, PIRP irp)
{
    UC_COMMON *c = (UC_COMMON *)dev->DeviceExtension;
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    UC_FDO *fdo;
    ULONG i;

    if (c->Kind == UcKindPdo) {
        NTSTATUS st = irp->IoStatus.Status;
        if (s->MinorFunction == IRP_MN_SET_POWER || s->MinorFunction == IRP_MN_QUERY_POWER)
            st = STATUS_SUCCESS;
        PoStartNextPowerIrp(irp);
        irp->IoStatus.Status = st;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    }

    fdo = (UC_FDO *)c;
    if (!UcAcquire(fdo)) {
        PoStartNextPowerIrp(irp);
        return UcCompleteIrp(irp, STATUS_DELETE_PENDING, 0);
    }
    if (s->MinorFunction == IRP_MN_SET_POWER)
        UCT2("FDO set power type %lu state %lu", s->Parameters.Power.Type,
             s->Parameters.Power.State.SystemState);
    if (s->MinorFunction == IRP_MN_SET_POWER &&
        s->Parameters.Power.Type == SystemPowerState) {
        if (s->Parameters.Power.State.SystemState == PowerSystemWorking) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, UcSystemUpDone, fdo, TRUE, TRUE, TRUE);
            return PoCallDriver(fdo->Lower, irp);   /* released in UcSystemUpDone */
        } else {
            POWER_STATE ps;
            ps.DeviceState = PowerDeviceD3;
            IoMarkIrpPending(irp);
            if (NT_SUCCESS(PoRequestPowerIrp(fdo->Pdo, IRP_MN_SET_POWER, ps,
                                             UcSleepDevDone, irp, NULL)))
                return STATUS_PENDING;              /* released in UcSleepDevDone */
            PoStartNextPowerIrp(irp);
            IoSkipCurrentIrpStackLocation(irp);
            PoCallDriver(fdo->Lower, irp);
            UcRelease(fdo);
            return STATUS_PENDING;
        }
    }
    if (s->MinorFunction == IRP_MN_SET_POWER &&
        s->Parameters.Power.Type == DevicePowerState) {
        if (s->Parameters.Power.State.DeviceState == PowerDeviceD0) {
            IoCopyCurrentIrpStackLocationToNext(irp);
            IoSetCompletionRoutine(irp, UcPowerUpDone, fdo, TRUE, TRUE, TRUE);
            return PoCallDriver(fdo->Lower, irp);   /* released in UcPowerUpDone */
        }
        for (i = 0; i < fdo->NumPorts; i++)
            UcPortSuspend(fdo->Ports[i]);
    }
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    {
        NTSTATUS st = PoCallDriver(fdo->Lower, irp);
        UcRelease(fdo);
        return st;
    }
}
