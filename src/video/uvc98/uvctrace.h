/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original VM-only diagnostic. QEMU isa-debugcon must own port0xE9.
 * Never enabled by normal builds or intended for physical-machine use.
 */
#ifndef UVC_TRACE_H
#define UVC_TRACE_H
#ifdef UVWORK_VM_TRACE
static void UvtText(const char *text)
{
    while(*text) WRITE_PORT_UCHAR((PUCHAR)0xe9,(UCHAR)*text++);
}
static void UvtHex(const char *label,ULONG value)
{
    unsigned int n;
    UvtText(label);
    for(n=0;n<8;++n) {
        unsigned int digit=(value>>(28-n*4))&15;
        WRITE_PORT_UCHAR((PUCHAR)0xe9,(UCHAR)(digit<10?'0'+digit:'A'+digit-10));
    }
    UvtText("\n");
}
#define UVTRACE(text) UvtText(text)
#define UVSTATUS(status) UvtHex("THREAD_STATUS ",(ULONG)(status))
#define UVHEX(label,value) UvtHex(label,(ULONG)(value))
#else
#define UVTRACE(text) ((void)0)
#define UVSTATUS(status) ((void)0)
#define UVHEX(label,value) ((void)0)
#endif
#endif
