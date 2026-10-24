/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * User-mode contract test adapters, never linked into the driver.
 */
#ifndef UM_CL_MOCK_H
#define UM_CL_MOCK_H
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
typedef LONG NTSTATUS;
typedef UCHAR KIRQL;
#define KSPIN_LOCK CRITICAL_SECTION
typedef struct MOCK_OBJECT {ULONG Kind;HANDLE Handle;LONG Refs;} MOCK_OBJECT;
typedef MOCK_OBJECT KEVENT,*PETHREAD,*PDEVICE_OBJECT;
typedef struct MOCK_DRIVER *PDRIVER_OBJECT;
typedef VOID (*PDRIVER_UNLOAD)(PDRIVER_OBJECT driver);
typedef struct MOCK_DRIVER {PDRIVER_UNLOAD DriverUnload;} MOCK_DRIVER;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
#define NotificationEvent 0
#define Executive 0
#define KernelMode 0
#define IO_NO_INCREMENT 0
#define NonPagedPool 0
#define ASSERT(x) assert(x)
#define PAGED_CODE() ((void)0)
static __inline LONG MockCompare(LONG *p,LONG n,LONG old) {
    return (LONG)InterlockedCompareExchange((PVOID*)p,(PVOID)n,(PVOID)old);
}
#define InterlockedCompareExchange(p,n,o) MockCompare(p,n,o)
#define InitializeListHead(h) ((h)->Flink=(h)->Blink=(h))
#define IsListEmpty(h) ((h)->Flink==(h))
static __inline LIST_ENTRY *MockRemove(LIST_ENTRY *h) {
    LIST_ENTRY *p=h->Flink;h->Flink=p->Flink;p->Flink->Blink=h;return p;
}
static __inline void MockInsert(LIST_ENTRY *h,LIST_ENTRY *p) {
    p->Blink=h->Blink;p->Flink=h;h->Blink->Flink=p;h->Blink=p;
}
#define RemoveHeadList(h) MockRemove(h)
#define InsertTailList(h,p) MockInsert(h,p)
#define RemoveEntryList(p) ((p)->Blink->Flink=(p)->Flink,(p)->Flink->Blink=(p)->Blink)
#define KeInitializeSpinLock(p) InitializeCriticalSection(p)
#define KeAcquireSpinLock(p,i) (EnterCriticalSection(p),*(i)=0)
#define KeReleaseSpinLock(p,i) LeaveCriticalSection(p)
#define KeWaitForSingleObject(p,a,b,c,d) MockWait(p)
#define KeClearEvent(p) ResetEvent((p)->Handle)
#define KeSetEvent(p,a,b) SetEvent((p)->Handle)
#define KeInitializeEvent(p,a,b) MockEvent(p,b)
#define ExAllocatePoolWithTag(p,n,t) MockAllocate(n)
#define ExFreePool(p) MockFree(p)
void MockEvent(KEVENT *event,BOOLEAN signaled);
DWORD MockWait(void *object);
void *MockAllocate(SIZE_T size);
void MockFree(void *p);
void ObReferenceObject(void *p);
void ObDereferenceObject(void *p);
PETHREAD KeGetCurrentThread(void);
NTSTATUS PsCreateSystemThread(HANDLE *handle,ULONG access,void *attributes,
    void *process,void *client,VOID (*entry)(PVOID),PVOID context);
#define PsTerminateSystemThread(s) ExitThread(s)
#define ZwClose(h) ObDereferenceObject(h)
#endif
