/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original Windows 98 cleanup dispatcher RESEARCH; no DDK sample sources.
 * Not integrated: synchronous worker-triggered unload may self-join.
 */
#ifndef UM_CLEAN_H
#define UM_CLEAN_H
#ifdef UM_CLEAN_TEST
#include "umclmock.h"
#else
#include <wdm.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct UM_CLEAN_ITEM UM_CLEAN_ITEM;
typedef VOID (*UM_CLEAN_ROUTINE)(PDEVICE_OBJECT device,PVOID context);
/* RESEARCH ONLY, not installable. DriverEntry/PASSIVE after PortCls initialization.
 * Requires a PortCls
 * unload routine. On success installs a wrapper which drains, then waits for
 * actual cleanup-thread termination BEFORE chaining the saved unload routine.
 * Failure leaves DriverUnload unchanged and no live thread. Not reentrant.
 */
NTSTATUS UmCleanInitialize(PDRIVER_OBJECT driver);
/* Allocate/free at PASSIVE. An allocated item owns one device reference.
 * Queue once, at <=DISPATCH; its callback runs PASSIVE and must free the item.
 * The callback may free its item/context before returning. The dispatcher
 * never touches an item after calling it. No arrivals once unload begins:
 * callers must retain their stream/miniport and FDO until final release.
 */
UM_CLEAN_ITEM *UmCleanAllocate(PDEVICE_OBJECT device);
VOID UmCleanQueue(UM_CLEAN_ITEM *item,UM_CLEAN_ROUTINE routine,PVOID context);
VOID UmCleanFree(UM_CLEAN_ITEM *item);
#ifdef __cplusplus
}
#endif
#endif
