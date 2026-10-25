/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Research prototype: queue and off-thread unload joins tested only.
 * Unresolved: synchronous unload on this worker's last device dereference
 * would self-join. NOT safe for installation until that lifetime is solved.
 * Original code from published WDM contracts; not an Io*WorkItem substitute.
 */
#ifndef UM_CLEAN_RESEARCH
#error Unresolved self-unload lifetime: research compile only
#endif
#include "umclean.h"
struct UM_CLEAN_ITEM {
    LIST_ENTRY Link;
    PDEVICE_OBJECT Device;
    UM_CLEAN_ROUTINE Routine;
    PVOID Context;
    LONG Queued;
};
static struct {
    KSPIN_LOCK Lock;
    LIST_ENTRY Items;
    KEVENT Wake,Ready;
    PETHREAD Thread;
    PDRIVER_UNLOAD Previous;
    BOOLEAN Stop,Live;
} Clean;
static VOID Worker(PVOID unused)
{
    KIRQL irql;
    UNREFERENCED_PARAMETER(unused);
    Clean.Thread=(PETHREAD)KeGetCurrentThread();
    ObReferenceObject(Clean.Thread);
    KeSetEvent(&Clean.Ready,IO_NO_INCREMENT,FALSE);
    for(;;) {
        UM_CLEAN_ITEM *item=NULL;
        BOOLEAN stop;
        KeWaitForSingleObject(&Clean.Wake,Executive,KernelMode,FALSE,NULL);
        KeAcquireSpinLock(&Clean.Lock,&irql);
        if(!IsListEmpty(&Clean.Items)) {
            item=CONTAINING_RECORD(Clean.Items.Flink,UM_CLEAN_ITEM,Link);
            RemoveEntryList(&item->Link);
        }
        stop=Clean.Stop;
        if(IsListEmpty(&Clean.Items) && !stop) KeClearEvent(&Clean.Wake);
        KeReleaseSpinLock(&Clean.Lock,irql);
        if(item) {
            /* Copy all arguments before callback: it can free item/context. */
            UM_CLEAN_ROUTINE routine=item->Routine;
            PDEVICE_OBJECT device=item->Device;
            PVOID context=item->Context;
            routine(device,context);
        } else if(stop) break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static VOID Unload(PDRIVER_OBJECT driver)
{
    KIRQL irql;
    PDRIVER_UNLOAD previous=Clean.Previous;
    PAGED_CODE();
    KeAcquireSpinLock(&Clean.Lock,&irql);
    Clean.Stop=TRUE;
    KeSetEvent(&Clean.Wake,IO_NO_INCREMENT,FALSE);
    KeReleaseSpinLock(&Clean.Lock,irql);
    /* A pre-exit event is insufficient: its signaler could still execute this
     * image after unload. Wait on the actual referenced thread object. */
    KeWaitForSingleObject(Clean.Thread,Executive,KernelMode,FALSE,NULL);
    ObDereferenceObject(Clean.Thread);Clean.Thread=NULL;Clean.Live=FALSE;
    previous(driver);
}
NTSTATUS UmCleanInitialize(PDRIVER_OBJECT driver)
{
    HANDLE handle;
    NTSTATUS status;
    PAGED_CODE();
    if(Clean.Live || !driver || !driver->DriverUnload)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(&Clean,sizeof(Clean));
    KeInitializeSpinLock(&Clean.Lock);InitializeListHead(&Clean.Items);
    KeInitializeEvent(&Clean.Wake,NotificationEvent,FALSE);
    KeInitializeEvent(&Clean.Ready,NotificationEvent,FALSE);
    Clean.Previous=driver->DriverUnload;
    status=PsCreateSystemThread(&handle,THREAD_ALL_ACCESS,NULL,NULL,NULL,Worker,NULL);
    if(!NT_SUCCESS(status)) return status;
    KeWaitForSingleObject(&Clean.Ready,Executive,KernelMode,FALSE,NULL);
    ZwClose(handle);
    Clean.Live=TRUE;driver->DriverUnload=Unload;
    return STATUS_SUCCESS;
}
UM_CLEAN_ITEM *UmCleanAllocate(PDEVICE_OBJECT device)
{
    UM_CLEAN_ITEM *item;
    PAGED_CODE();
    if(!Clean.Live || Clean.Stop || !device) return NULL;
    item=(UM_CLEAN_ITEM*)ExAllocatePoolWithTag(NonPagedPool,sizeof(*item),'cImU');
    if(!item) return NULL;
    RtlZeroMemory(item,sizeof(*item));item->Device=device;
    ObReferenceObject(device);
    return item;
}
VOID UmCleanQueue(UM_CLEAN_ITEM *item,UM_CLEAN_ROUTINE routine,PVOID context)
{
    KIRQL irql;
    ASSERT(item && routine);
    /* Final release queues exactly once. Fail closed on a broken ownership
     * contract instead of corrupting an intrusive list with duplicate links. */
    if(!item || !routine || InterlockedExchange(&item->Queued,1)) {
        ASSERT(FALSE);return;
    }
    item->Routine=routine;item->Context=context;
    KeAcquireSpinLock(&Clean.Lock,&irql);
    ASSERT(Clean.Live && !Clean.Stop);
    InsertTailList(&Clean.Items,&item->Link);
    KeSetEvent(&Clean.Wake,IO_NO_INCREMENT,FALSE);
    KeReleaseSpinLock(&Clean.Lock,irql);
}
VOID UmCleanFree(UM_CLEAN_ITEM *item)
{
    PDEVICE_OBJECT device;
    PAGED_CODE();
    if(!item) return;
    device=item->Device;ExFreePool(item);ObDereferenceObject(device);
}
