/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Win32 services for the fake operating system of the core tests.
 * Host-test code only. Identical copy in both packages.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "fakewin.h"

void *FwEvent(int manual, int signaled) { return CreateEvent(NULL, manual, signaled, NULL); }
void FwSet(void *e) { SetEvent(e); }
void FwReset(void *e) { ResetEvent(e); }
int FwWait(void *o, unsigned long ms) { return WaitForSingleObject(o, ms) == WAIT_OBJECT_0 ? 0 : 1; }
void FwClose(void *o) { CloseHandle(o); }

void *FwDup(void *o)
{
    HANDLE h = NULL;
    DuplicateHandle(GetCurrentProcess(), o, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}

typedef struct START { void (*Start)(void *); void *Context; } START;

static DWORD WINAPI Trampoline(LPVOID p)
{
    START s = *(START *)p;
    free(p);
    s.Start(s.Context);
    return 0;
}

void *FwThread(void (*start)(void *), void *context)
{
    DWORD id;
    START *s = malloc(sizeof(*s));
    s->Start = start; s->Context = context;
    return CreateThread(NULL, 0, Trampoline, s, 0, &id);
}

void FwExitThread(void) { ExitThread(0); }
void FwSleep(unsigned long ms) { Sleep(ms); }

void *FwLockNew(void)
{
    CRITICAL_SECTION *c = malloc(sizeof(*c));
    InitializeCriticalSection(c);
    return c;
}

void FwLockFree(void *l) { DeleteCriticalSection(l); free(l); }
void FwLock(void *l) { EnterCriticalSection(l); }
void FwUnlock(void *l) { LeaveCriticalSection(l); }
unsigned long FwThreadId(void) { return GetCurrentThreadId(); }
unsigned long FwTicks(void) { return GetTickCount(); }
