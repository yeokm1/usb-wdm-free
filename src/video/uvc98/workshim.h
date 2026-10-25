/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Host-only deterministic scheduler. Never compiled into a kernel binary.
 * Actual public kit structures are retained; these are injected operations.
 */
#ifndef UVC_WORK_SHIM_H
#define UVC_WORK_SHIM_H
PVOID UvwFakeAllocate(POOL_TYPE,SIZE_T,ULONG);
VOID UvwFakeFree(PVOID);
PIO_WORKITEM UvwFakeItem(PDEVICE_OBJECT);
VOID UvwFakeFreeItem(PIO_WORKITEM);
VOID UvwFakeQueue(PIO_WORKITEM,PIO_WORKITEM_ROUTINE,WORK_QUEUE_TYPE,PVOID);
VOID UvwFakeReference(PVOID);
VOID UvwFakeDereference(PVOID);
VOID UvwFakeEvent(PKEVENT,EVENT_TYPE,BOOLEAN);
LONG UvwFakeSet(PKEVENT,KPRIORITY,BOOLEAN);
NTSTATUS UvwFakeWait(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
NTSTATUS UvwFakeCreate(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,HANDLE,PCLIENT_ID,PKSTART_ROUTINE,PVOID);
NTSTATUS UvwFakeTerminate(NTSTATUS);
NTSTATUS UvwFakeJoin(HANDLE,BOOLEAN,PLARGE_INTEGER);
NTSTATUS UvwFakeClose(HANDLE);
LONG UvwFakeExchange(PLONG,LONG);
LONG UvwFakeCompare(PLONG,LONG,LONG);
VOID UvwFakePriority(PHW_STREAM_OBJECT,PVOID,STREAM_PRIORITY,PHW_PRIORITY_ROUTINE,PVOID);
VOID UvwFakeTimer(PHW_STREAM_OBJECT,PVOID,ULONG,PHW_TIMER_ROUTINE,PVOID);
ULONGLONG UvwFakeTime(void);
LARGE_INTEGER UvwFakeDivide(LARGE_INTEGER,ULONG,PULONG);
KIRQL UvwFakeIrql(void);
int UvwTestRunOne(UVWORK_ANCHOR *);
#undef ExAllocatePoolWithTag
#undef ExFreePool
#undef ObReferenceObject
#undef ObDereferenceObject
#undef KeQueryInterruptTime
#undef KeGetCurrentIrql
#undef InterlockedExchange
#undef InterlockedCompareExchange
#define ExAllocatePoolWithTag UvwFakeAllocate
#define ExFreePool UvwFakeFree
#define IoAllocateWorkItem UvwFakeItem
#define IoFreeWorkItem UvwFakeFreeItem
#define IoQueueWorkItem UvwFakeQueue
#define ObReferenceObject UvwFakeReference
#define ObDereferenceObject UvwFakeDereference
#define KeInitializeEvent UvwFakeEvent
#define KeSetEvent UvwFakeSet
#define KeWaitForSingleObject UvwFakeWait
#define PsCreateSystemThread UvwFakeCreate
#define PsTerminateSystemThread UvwFakeTerminate
#define ZwWaitForSingleObject UvwFakeJoin
#define ZwClose UvwFakeClose
#define InterlockedExchange UvwFakeExchange
#define InterlockedCompareExchange UvwFakeCompare
#define StreamClassCallAtNewPriority UvwFakePriority
#define StreamClassScheduleTimer UvwFakeTimer
#define KeQueryInterruptTime UvwFakeTime
#define RtlExtendedLargeIntegerDivide UvwFakeDivide
#define KeGetCurrentIrql UvwFakeIrql
#endif
