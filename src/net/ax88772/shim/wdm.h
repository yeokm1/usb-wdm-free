/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host-test stand-in for the kernel declarations netusb.c uses, so that
 * netusb.c and netcore.c compile unchanged against a fake operating
 * system (fakeos.c) for the core tests (ethernet.md section 10.3). Written
 * from the names the driver uses; layouts are this test's own, not the
 * DDK's, and nothing here is built into a driver. Status values are the
 * documented NTSTATUS codes. Identical copy in both packages.
 */
#ifndef SHIM_WDM_H
#define SHIM_WDM_H

#include <stddef.h>
#include <string.h>

typedef unsigned char UCHAR, *PUCHAR;
typedef unsigned short USHORT, *PUSHORT;
typedef unsigned long ULONG, *PULONG;
typedef long LONG, *PLONG;
typedef char CHAR, *PCHAR;
typedef unsigned short WCHAR, *PWCHAR;
typedef unsigned char BOOLEAN, *PBOOLEAN;
typedef void VOID, *PVOID;
typedef long NTSTATUS;
typedef unsigned char KIRQL, *PKIRQL;
typedef unsigned __int64 ULONGLONG;
typedef __int64 LONGLONG;
typedef unsigned int UINT, *PUINT;
typedef unsigned long ULONG_PTR;
typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#define UNREFERENCED_PARAMETER(p) (p)
#define FIELD_OFFSET(t, f) ((LONG)offsetof(t, f))
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#define IO_NO_INCREMENT 0

#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_TIMEOUT                   ((NTSTATUS)0x00000102L)
#define STATUS_PENDING                   ((NTSTATUS)0x00000103L)
#define STATUS_MORE_PROCESSING_REQUIRED  ((NTSTATUS)0xC0000016L)
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_SUPPORTED             ((NTSTATUS)0xC00000BBL)
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#define STATUS_NO_SUCH_DEVICE            ((NTSTATUS)0xC000000EL)
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#define STATUS_DEVICE_DATA_ERROR         ((NTSTATUS)0xC000009CL)
#define STATUS_DEVICE_NOT_CONNECTED      ((NTSTATUS)0xC000009DL)
#define STATUS_CANCELLED                 ((NTSTATUS)0xC0000120L)
#define STATUS_IO_TIMEOUT                ((NTSTATUS)0xC00000B5L)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182L)
#define STATUS_DEVICE_REMOVED            ((NTSTATUS)0xC00002B6L)

typedef enum { NotificationEvent, SynchronizationEvent } EVENT_TYPE;
typedef enum { Executive } KWAIT_REASON;
typedef enum { KernelMode } KPROCESSOR_MODE;
typedef enum { NonPagedPool } POOL_TYPE;

/* Dispatcher objects: one header, so KeWaitForSingleObject takes both. */
typedef struct _KEVENT { ULONG Magic; void *Handle; } KEVENT, *PKEVENT;
typedef struct _KSPIN_LOCK_SHIM { ULONG Magic; void *Lock; ULONG Owner; } KSPIN_LOCK, *PKSPIN_LOCK;

struct _DEVICE_OBJECT;
struct _IRP;
typedef NTSTATUS (*PIO_COMPLETION_ROUTINE)(struct _DEVICE_OBJECT *, struct _IRP *, PVOID);

typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    union {
        struct { ULONG OutputBufferLength, InputBufferLength, IoControlCode; PVOID Type3InputBuffer; } DeviceIoControl;
        struct { PVOID Argument1, Argument2, Argument3, Argument4; } Others;
    } Parameters;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus;
    IO_STACK_LOCATION Next;
    struct _IRP *FakeNext;          /* the fake device's queue */
    struct _DEVICE_OBJECT *FakeDevice;
    ULONG FakeMagic;
} IRP, *PIRP;

typedef struct _DEVICE_OBJECT { char StackSize; void *Fake; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT { void *Fake; } DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWCHAR Buffer; } UNICODE_STRING, *PUNICODE_STRING;

#define IRP_MJ_INTERNAL_DEVICE_CONTROL 0x0f
#define IoGetNextIrpStackLocation(i) (&(i)->Next)
#define IoSetCompletionRoutine(i, r, c, s, e, k) ((i)->Next.CompletionRoutine = (r), (i)->Next.Context = (c))
#define IoCallDriver(d, i) IofCallDriver((d), (i))
#define ObDereferenceObject(o) ObfDereferenceObject(o)

PIRP IoAllocateIrp(char stackSize, BOOLEAN charge);
void IoFreeIrp(PIRP irp);
NTSTATUS IofCallDriver(PDEVICE_OBJECT device, PIRP irp);
BOOLEAN IoCancelIrp(PIRP irp);
BOOLEAN IoIsWdmVersionAvailable(UCHAR major, UCHAR minor);
void KeInitializeEvent(PKEVENT event, EVENT_TYPE type, BOOLEAN state);
LONG KeSetEvent(PKEVENT event, LONG increment, BOOLEAN wait);
void KeClearEvent(PKEVENT event);
NTSTATUS KeWaitForSingleObject(PVOID object, KWAIT_REASON reason, KPROCESSOR_MODE mode,
                               BOOLEAN alertable, PLARGE_INTEGER timeout);
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval);
void KeInitializeSpinLock(PKSPIN_LOCK lock);
void KeAcquireSpinLock(PKSPIN_LOCK lock, PKIRQL old);
void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL old);
PVOID ExAllocatePoolWithTag(POOL_TYPE type, ULONG size, ULONG tag);
void ExFreePool(PVOID p);
NTSTATUS PsCreateSystemThread(PHANDLE handle, ULONG access, PVOID attributes, HANDLE process,
                              PVOID client, void (*start)(PVOID), PVOID context);
NTSTATUS PsTerminateSystemThread(NTSTATUS status);
NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ULONG access, PVOID type, KPROCESSOR_MODE mode,
                                   PVOID *object, PVOID info);
void ObfDereferenceObject(PVOID object);
NTSTATUS ZwClose(HANDLE handle);
ULONG DbgPrint(const char *format, ...);

#endif
