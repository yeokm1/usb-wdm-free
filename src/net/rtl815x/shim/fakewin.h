/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * The few Win32 services the fake operating system needs (fakewin.c is
 * the only file that includes windows.h, so the shim headers never meet
 * the Win32 ones). Host-test code only. Identical copy in both packages.
 */
#ifndef FAKEWIN_H
#define FAKEWIN_H

#define FW_INFINITE 0xffffffffUL

void *FwEvent(int manual, int signaled);
void FwSet(void *event);
void FwReset(void *event);
int FwWait(void *object, unsigned long ms);    /* 0 signaled, 1 timeout */
void FwClose(void *object);
void *FwDup(void *object);
void *FwThread(void (*start)(void *), void *context);
void FwExitThread(void);
void FwSleep(unsigned long ms);
void *FwLockNew(void);
void FwLockFree(void *lock);
void FwLock(void *lock);
void FwUnlock(void *lock);
unsigned long FwThreadId(void);
unsigned long FwTicks(void);

#endif
