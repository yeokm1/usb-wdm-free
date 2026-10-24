/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * usbcomv.c - USBCOMV.VXD: the VCOMM port driver of the Windows 98 SE / ME
 * CDC-ACM package (docs/design/serial.md 5.1.2 route B, item 3).
 *
 * VCOMM loads this dynamic VxD when a program opens the PortName of a
 * devnode whose PortDriver is USBCOMV.VXD. On PortOpen the VxD opens the
 * matching USBCOM.SYS port device (\Device\UsbCom<PortName>) with
 * _NtKernCreateFile, fetches the direct-call table (ucif.h) with one
 * _NtKernDeviceIoControl, closes the handle, and from then on calls the
 * SYS directly. The receive and transmit queues are VCOMM's _PortData
 * queues; acmcore.c does the queue, flow-control and SERIAL_STATE work.
 *
 * Written from the Windows 98 DDK documentation (HELP_OTHER.CHM,
 * "Communications Port Drivers", comm_0t4m.htm to comm_12uo.htm, "VCOMM
 * Device Initialization" comm_0sqf.htm, "Building a Virtual Device"
 * vxds_6708.htm, "Event Callbacks" kernel_8tf4.htm) and its headers
 * (vmm.h, vcomm.h, ntkern.h). The DDK's serial port driver sample is not
 * read (design section 9). The DDB and the control procedure are in C
 * with inline assembly, so no separate assembler is needed.
 *
 * Interrupt-time rule: VCOMM may call PortRead, PortWrite, PortSetCommState,
 * PortEscapeFunction and the other "callable at interrupt time" functions
 * at interrupt time. They only touch this VxD's own state (with interrupts
 * disabled around shared queue updates) and schedule a global event; the
 * event callback, which is not at interrupt time, is the only place that
 * calls USBCOM.SYS (ucif.h calling rules).
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <basedef.h>
#include <vmm.h>
#include <vcomm.h>
#include <ntkern.h>
#include "acmcore.h"
#include "ucif.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG

/* ---- the DDB and the control procedure ------------------------------------ */

void UvControl(void);

struct VxD_Desc_Block USBCOMV_DDB = {
    0,                          /* DDB_Next */
    DDK_VERSION,                /* DDB_SDK_Version */
    UNDEFINED_DEVICE_ID,        /* port drivers: no services (comm_12nr) */
    1, 0,                       /* version 1.0 */
    0,                          /* DDB_Flags */
    { 'U', 'S', 'B', 'C', 'O', 'M', 'V', ' ' },
    UNDEFINED_INIT_ORDER,
    (ULONG)UvControl,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
    sizeof(struct VxD_Desc_Block),
    'Rsv1', 'Rsv2', 'Rsv3'
};

/* ---- trace (trace flavour only) ------------------------------------------- */
/*
 * The trace flavour (build98.ps1 -Flavour trace, UC_TRACE_E9) writes each
 * line to QEMU's debug console port 0xE9, interleaved with USBCOM.SYS's
 * "uc:" lines. Test-only, never released; the free build compiles it out.
 * Writing a port is safe at interrupt time.
 */
#if defined(UC_TRACE_E9)
/* _outp is the compiler's intrinsic for the x86 OUT instruction. */
int __cdecl _outp(unsigned short port, int value);
#pragma intrinsic(_outp)

static void UvTraceV(const char *fmt, const ULONG *v, int n)
{
    char line[160];
    ULONG len = AcFormat(line, sizeof(line) - 1, fmt, v, n), i;
    line[len++] = '\n';
    for (i = 0; i < 4; i++)
        _outp(0xE9, "uv: "[i]);
    for (i = 0; i < len; i++)
        _outp(0xE9, line[i]);
}
#define UVT0(f)             UvTraceV(f, 0, 0)
#define UVT1(f, a)          { ULONG v_[1]; v_[0] = (ULONG)(a); UvTraceV(f, v_, 1); }
#define UVT2(f, a, b)       { ULONG v_[2]; v_[0] = (ULONG)(a); v_[1] = (ULONG)(b); UvTraceV(f, v_, 2); }
#define UVT3(f, a, b, c)    { ULONG v_[3]; v_[0] = (ULONG)(a); v_[1] = (ULONG)(b); \
                              v_[2] = (ULONG)(c); UvTraceV(f, v_, 3); }
#define UVT4(f, a, b, c, d) { ULONG v_[4]; v_[0] = (ULONG)(a); v_[1] = (ULONG)(b); \
                              v_[2] = (ULONG)(c); v_[3] = (ULONG)(d); UvTraceV(f, v_, 4); }
#else
#define UVT0(f)
#define UVT1(f, a)
#define UVT2(f, a, b)
#define UVT3(f, a, b, c)
#define UVT4(f, a, b, c, d)
#endif

/* ---- local NT types for the NTKERN calls ----------------------------------- */

typedef struct UV_USTR { USHORT Length, MaximumLength; USHORT *Buffer; } UV_USTR;
typedef struct UV_OA {
    ULONG Length; ULONG RootDirectory; UV_USTR *ObjectName; ULONG Attributes;
    PVOID SecurityDescriptor; PVOID SecurityQualityOfService;
} UV_OA;
typedef struct UV_IOSB { LONG Status; ULONG Information; } UV_IOSB;

#define UV_OBJ_CASE_INSENSITIVE 0x00000040
#define UV_ACCESS  (0xC0000000 | 0x00100000)   /* GENERIC_READ|WRITE|SYNCHRONIZE */
#define UV_FILE_OPEN 1
#define UV_SYNC_IO   0x00000020                 /* FILE_SYNCHRONOUS_IO_NONALERT */

/* ---- VMM / VCOMM / NTKERN service calls ---------------------------------- */
/* Every wrapper saves and restores ESP, so C-convention (caller pops) and
   NTKERN's stdcall services (callee pops) are both safe. */

static ULONG UvHeapAlloc(ULONG bytes)
{
    ULONG r, keep;
    _asm mov keep, esp
    _asm push HEAPZEROINIT
    _asm push bytes
    VMMCall(_HeapAllocate)
    _asm mov esp, keep
    _asm mov r, eax
    return r;
}

static void UvHeapFree(ULONG p)
{
    ULONG keep;
    if (p == 0)
        return;
    _asm mov keep, esp
    _asm push 0
    _asm push p
    VMMCall(_HeapFree)
    _asm mov esp, keep
}

static ULONG UvSystemTime(void)
{
    ULONG r;
    VMMCall(Get_System_Time)
    _asm mov r, eax
    return r;
}

static ULONG UvScheduleEvent(void (*cb)(void), ULONG ref)
{
    ULONG h;
    _asm mov esi, cb
    _asm mov edx, ref
    VMMCall(Schedule_Global_Event)
    _asm mov h, esi
    return h;
}

static void UvCancelEvent(ULONG h)
{
    _asm mov esi, h
    VMMCall(Cancel_Global_Event)
}

static ULONG UvSetTimeout(void (*cb)(void), ULONG ms, ULONG ref)
{
    ULONG h;
    _asm mov eax, ms
    _asm mov edx, ref
    _asm mov esi, cb
    VMMCall(Set_Global_Time_Out)
    _asm mov h, esi
    return h;
}

static void UvCancelTimeout(ULONG h)
{
    _asm mov esi, h
    VMMCall(Cancel_Time_Out)
}

static ULONG UvRegisterPortDriver(void (*ctl)(void))
{
    ULONG r, keep;
    _asm mov keep, esp
    _asm push ctl
    VxDCall(_VCOMM_Register_Port_Driver)
    _asm mov esp, keep
    _asm mov r, eax
    return r;
}

static ULONG UvAddPort(ULONG ref, void *open, char *name)
{
    ULONG r, keep;
    _asm mov keep, esp
    _asm push name
    _asm push open
    _asm push ref
    VxDCall(_VCOMM_Add_Port)
    _asm mov esp, keep
    _asm mov r, eax
    return r;
}

static void UvMapWin32ToRing0(void *w32, void *r0)
{
    _asm mov eax, w32
    _asm mov edx, r0
    VxDCall(VCOMM_Map_Win32DCB_To_Ring0)
}

static void UvMapRing0ToWin32(void *r0, void *w32)
{
    _asm mov eax, r0
    _asm mov edx, w32
    VxDCall(VCOMM_Map_Ring0DCB_To_Win32)
}

static LONG UvNtCreate(ULONG *h, UV_OA *oa, UV_IOSB *iosb)
{
    LONG r;
    ULONG keep;
    _asm mov keep, esp
    _asm push 0                  /* EaLength */
    _asm push 0                  /* EaBuffer */
    _asm push UV_SYNC_IO         /* CreateOptions */
    _asm push UV_FILE_OPEN       /* CreateDisposition */
    _asm push 0                  /* ShareAccess */
    _asm push 0                  /* FileAttributes */
    _asm push 0                  /* AllocationSize */
    _asm push iosb
    _asm push oa
    _asm push UV_ACCESS
    _asm push h
    VxDCall(_NtKernCreateFile)
    _asm mov esp, keep
    _asm mov r, eax
    return r;
}

static LONG UvNtIoctl(ULONG h, UV_IOSB *iosb, ULONG code, void *inBuf, ULONG inLen,
                      void *outBuf, ULONG outLen)
{
    LONG r;
    ULONG keep;
    _asm mov keep, esp
    _asm push outLen
    _asm push outBuf
    _asm push inLen
    _asm push inBuf
    _asm push code
    _asm push iosb
    _asm push 0                  /* ApcContext */
    _asm push 0                  /* ApcRoutine */
    _asm push 0                  /* Event */
    _asm push h
    VxDCall(_NtKernDeviceIoControl)
    _asm mov esp, keep
    _asm mov r, eax
    return r;
}

static void UvNtClose(ULONG h)
{
    ULONG keep;
    _asm mov keep, esp
    _asm push h
    VxDCall(_NtKernClose)
    _asm mov esp, keep
}

/* Interrupts off/on around queue state that the SYS callbacks also touch. */
#define UV_LOCK(f)   { _asm pushfd _asm pop f _asm cli }
#define UV_UNLOCK(f) { if ((f) & 0x200) { _asm sti } }

static void UvCopy(void *d, const void *s, ULONG n)
{
    UCHAR *dd = (UCHAR *)d;
    const UCHAR *ss = (const UCHAR *)s;
    while (n--) *dd++ = *ss++;
}

static void UvZero(void *d, ULONG n)
{
    UCHAR *dd = (UCHAR *)d;
    while (n--) *dd++ = 0;
}

/* ---- the port ---------------------------------------------------------------- */

#define UV_RX_DEFAULT 16384
#define UV_TX_DEFAULT 65536
#define UV_TX_MIN     65536               /* see PortWrite */
#define UV_OPS        16
#define UV_MAGIC      0x56434F55          /* 'UOCV' */
#define UV_NAMES      16
#define UV_W32DCB     28                  /* sizeof the Win32 DCB */

typedef void (*UV_NOTIFY)(ULONG hPort, ULONG ref, ULONG event, ULONG sub);

typedef struct UV_PORT {
    PortData        Pd;                   /* must be first (comm_12ph) */
    ULONG           Magic;
    ULONG           NameSlot;
    UCIF_TABLE     *T;
    UCIF_CALLBACKS  Cb;
    AC_DCB          Dcb;
    AC_FLOW         Flow;
    AC_QUEUE        RxQ, TxQ;
    UCHAR           WireDtr, WireRts, Break, Pad;
    volatile ULONG  SysGone;
    volatile ULONG  Closing;
    ULONG           RxStalled;
    ULONG           IgnoreErrors;
    ULONG           EvMask;
    ULONG          *pEvents;
    UCHAR          *pMsr;
    ULONG          *pUpdateTime;
    USHORT          State;
    UV_NOTIFY       EvNotify;   ULONG EvRef;
    UV_NOTIFY       RxNotify;   ULONG RxRef;   ULONG RxTrigger;
    UV_NOTIFY       TxNotify;   ULONG TxRef;   ULONG TxTrigger;
    UCIF_OP         Ops[UV_OPS];
    ULONG           OpHead, OpCount;
    ULONG           Kick;
    ULONG           CheckNotify;
    ULONG           EventHandle;
    ULONG           TimeoutHandle;
    ULONG           OwnRx, OwnRxSize, OwnTx, OwnTxSize;
} UV_PORT;

/* Port names VCOMM gave us (DC_Initialize) and which are open. */
static char  g_Names[UV_NAMES][8];
static ULONG g_Open[UV_NAMES];

void UvEventThunk(void);
void UvTimeoutThunk(void);

static void UvSchedule(UV_PORT *p)
{
    ULONG f;
    UV_LOCK(f);
    if (!p->EventHandle && !p->SysGone && !p->Closing)
        p->EventHandle = UvScheduleEvent(UvEventThunk, (ULONG)p);
    UV_UNLOCK(f);
}

static void UvKick(UV_PORT *p, ULONG flags)
{
    ULONG f;
    UV_LOCK(f);
    p->Kick |= flags;
    UV_UNLOCK(f);
    UvSchedule(p);
}

static void UvQueueOp(UV_PORT *p, ULONG op, ULONG value, const UCHAR *data)
{
    ULONG f;
    UV_LOCK(f);
    if (p->OpCount < UV_OPS) {
        UCIF_OP *o = &p->Ops[(p->OpHead + p->OpCount) % UV_OPS];
        o->Op = op;
        o->Value = value;
        if (data)
            UvCopy(o->Data, data, 7);
        p->OpCount++;
    }
    UV_UNLOCK(f);
    UvSchedule(p);
}

static void UvQueueLine(UV_PORT *p)
{
    AC_LINE line;
    UCHAR lc[7];
    AcDcbToLine(&p->Dcb, &line);
    AcEncodeLineCoding(line.Rate, line.StopBits, line.Parity, line.DataBits, lc);
    UvQueueOp(p, UCOP_LINE_CODING, 0, lc);
}

static void UvQueueControl(UV_PORT *p)
{
    UvQueueOp(p, UCOP_CONTROL, (p->WireDtr ? 1 : 0) | (p->WireRts ? 2 : 0), 0);
}

/* Raises enabled events: the event variable, then the client callback. */
static void UvRaise(UV_PORT *p, ULONG ev)
{
    ULONG hit = ev & p->EvMask, f;
    if (!hit)
        return;
    UV_LOCK(f);
    *p->pEvents |= hit;
    UV_UNLOCK(f);
    if (p->EvNotify)
        p->EvNotify((ULONG)p, p->EvRef, CN_EVENT, hit);
}

static void UvNotifyRx(UV_PORT *p)
{
    if (p->RxNotify && p->RxTrigger != 0xFFFFFFFF && p->Pd.QInCount >= p->RxTrigger &&
        p->Pd.QInCount > 0)
        p->RxNotify((ULONG)p, p->RxRef, CN_RECEIVE, 0);
}

static void UvNotifyTx(UV_PORT *p)
{
    if (p->TxNotify && p->TxTrigger != 0xFFFFFFFF && p->Pd.QOutCount < p->TxTrigger) {
        UVT2("notify transmit: queue %lu, trigger %lu", p->Pd.QOutCount, p->TxTrigger);
        p->TxNotify((ULONG)p, p->TxRef, CN_TRANSMIT, 0);
    }
}

/* The only place that calls USBCOM.SYS after PortOpen (not interrupt time). */
static void UvEvent(UV_PORT *p)
{
    ULONG f, kick, check;
    UCIF_OP op;

    UV_LOCK(f);
    p->EventHandle = 0;
    kick = p->Kick;
    p->Kick = 0;
    check = p->CheckNotify;
    p->CheckNotify = 0;
    UV_UNLOCK(f);
    if (p->SysGone || p->Closing)
        return;

    for (;;) {
        UV_LOCK(f);
        if (p->OpCount == 0) {
            UV_UNLOCK(f);
            break;
        }
        op = p->Ops[p->OpHead];
        UV_UNLOCK(f);
        if (p->T->Control(p->T->Port, &op) != 0) {
            /* The SYS queue is full: try again shortly. */
            if (!p->TimeoutHandle)
                p->TimeoutHandle = UvSetTimeout(UvTimeoutThunk, 20, (ULONG)p);
            break;
        }
        UV_LOCK(f);
        p->OpHead = (p->OpHead + 1) % UV_OPS;
        p->OpCount--;
        UV_UNLOCK(f);
    }
    if (kick)
        p->T->Kick(p->T->Port, kick);
    if (check) {
        UvNotifyRx(p);
        UvNotifyTx(p);
    }
}

static void UvTimeout(UV_PORT *p)
{
    p->TimeoutHandle = 0;
    UvSchedule(p);
}

/* EDX = reference data (kernel_8tf4); callbacks may use EAX-EDI. */
void __declspec(naked) UvEventThunk(void)
{
    _asm push edx
    _asm call UvEvent
    _asm add esp, 4
    _asm cld
    _asm ret
}

void __declspec(naked) UvTimeoutThunk(void)
{
    _asm push edx
    _asm call UvTimeout
    _asm add esp, 4
    _asm cld
    _asm ret
}

/* ---- callbacks from USBCOM.SYS (DISPATCH_LEVEL, see ucif.h) -------------------- */

static unsigned long __stdcall UvRxData(void *ctx, const unsigned char *data,
                                        unsigned long len)
{
    UV_PORT *p = (UV_PORT *)ctx;
    ULONG ev = 0, act = 0, n, f;

    UV_LOCK(f);
    n = AcRxPut(&p->RxQ, &p->Dcb, &p->Flow, data, len, &ev, &act);
    if (n < len)
        p->RxStalled = 1;
    UV_UNLOCK(f);
    if (ev & AC_EV_RXCHAR) {
        ULONG now = UvSystemTime();
        p->Pd.dwLastReceiveTime = now;
        if (p->pUpdateTime)
            *p->pUpdateTime = now;
    }
    if (act & AC_RX_KICK_TX)
        UvKick(p, UCK_TX);
    UvRaise(p, ev);
    UvNotifyRx(p);
    return n;
}

static unsigned long __stdcall UvTxPull(void *ctx, unsigned char *buf,
                                        unsigned long max)
{
    UV_PORT *p = (UV_PORT *)ctx;
    ULONG n, f, before;

    UV_LOCK(f);
    before = p->Pd.QOutCount;
    n = AcTxPull(&p->TxQ, &p->Flow, buf, max);
    UV_UNLOCK(f);
    if (n && p->TxTrigger != 0xFFFFFFFF && before >= p->TxTrigger)
        UvNotifyTx(p);
    return n;
}

static void __stdcall UvTxDone(void *ctx, unsigned long sent, unsigned long idle)
{
    UV_PORT *p = (UV_PORT *)ctx;
    (void)sent;
    if (idle && p->Pd.QOutCount == 0 && !p->Flow.FlowPending &&
        !(p->Flow.Status & AC_fTximmed))
        UvRaise(p, AC_EV_TXEMPTY);
}

static void __stdcall UvSerialState(void *ctx, unsigned short state)
{
    UV_PORT *p = (UV_PORT *)ctx;
    ULONG ev = 0, err = 0, f;
    UCHAR msr = AcStateToMsr(p->State, state, &ev, &err);

    p->State = state;
    UV_LOCK(f);
    *p->pMsr = msr;
    p->Pd.dwCommError |= err;
    UV_UNLOCK(f);
    UvRaise(p, ev);
}

static void __stdcall UvGone(void *ctx)
{
    UV_PORT *p = (UV_PORT *)ctx;
    ULONG ev = 0, err = 0, f;
    UCHAR msr;

    p->SysGone = 1;                 /* never call the table again (ucif.h) */
    UVT0("Gone");
    msr = AcStateToMsr(p->State, 0, &ev, &err);
    p->State = 0;
    UV_LOCK(f);
    *p->pMsr = msr;
    if (p->EventHandle) {
        UvCancelEvent(p->EventHandle);
        p->EventHandle = 0;
    }
    UV_UNLOCK(f);
    if (p->TimeoutHandle) {
        UvCancelTimeout(p->TimeoutHandle);
        p->TimeoutHandle = 0;
    }
    UvRaise(p, ev | AC_EV_RLSD | AC_EV_DSR | AC_EV_ERR);
}

/* ---- port functions (comm_12nk ... comm_12uo) ------------------------------- */

#define UV_OK(p)          ((p)->Pd.dwLastError = 0, TRUE)
#define UV_FAIL(p, e)     ((p)->Pd.dwLastError = (ULONG)(e), FALSE)

static BOOL PortSetCommState(UV_PORT *p, _DCB *dcb, ULONG mask)
{
    AC_DCB nd;
    AC_LINE oldLine, newLine;
    long rc;
    ULONG f;

    if (dcb == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    nd = p->Dcb;
    rc = AcApplyDcb(&nd, (const AC_DCB *)dcb, mask);
    if (rc)
        return UV_FAIL(p, rc);
    AcDcbToLine(&p->Dcb, &oldLine);
    AcDcbToLine(&nd, &newLine);
    UV_LOCK(f);
    p->Dcb = nd;
    UV_UNLOCK(f);
    UVT4("SetCommState mask %08lx: %lu baud, %lu bits, parity %lu", mask, newLine.Rate,
         newLine.DataBits, newLine.Parity);
    if (oldLine.Rate != newLine.Rate || oldLine.DataBits != newLine.DataBits ||
        oldLine.Parity != newLine.Parity || oldLine.StopBits != newLine.StopBits)
        UvQueueLine(p);
    if ((mask & AC_mBitMask) &&
        (newLine.Dtr != p->WireDtr || newLine.Rts != p->WireRts)) {
        p->WireDtr = newLine.Dtr;
        p->WireRts = newLine.Rts;
        UvQueueControl(p);
    }
    if (mask & AC_mBitMask)
        UvKick(p, UCK_TX);           /* XON/XOFF settings may release TX */
    return UV_OK(p);
}

static BOOL PortGetCommState(UV_PORT *p, _DCB *dcb)
{
    if (dcb == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    UvCopy(dcb, &p->Dcb, sizeof(AC_DCB));
    return UV_OK(p);
}

static BOOL UvOwnQueue(ULONG *own, ULONG *ownSize, ULONG want)
{
    if (*own && *ownSize >= want)
        return TRUE;
    if (*own)
        UvHeapFree(*own);
    *own = UvHeapAlloc(want);
    *ownSize = *own ? want : 0;
    return *own != 0;
}

/*
 * PortSetup: the client's queues, or ours when it passes none. A larger
 * buffer of ours is allocated first and swapped in with interrupts off;
 * the old one is freed only after the swap, since a SYS callback may be
 * writing the live queue until then.
 */
static BOOL PortSetup(UV_PORT *p, UCHAR *rx, ULONG cbRx, UCHAR *tx, ULONG cbTx)
{
    ULONG f, rxAddr = (ULONG)rx, txAddr = (ULONG)tx;
    ULONG newRx = 0, newTx = 0, oldRx = 0, oldTx = 0;

    if (rxAddr == 0) {
        if (cbRx == 0) cbRx = p->OwnRxSize;
        if (cbRx > p->OwnRxSize) {
            newRx = UvHeapAlloc(cbRx);
            if (newRx == 0)
                return UV_FAIL(p, IE_MEMORY);
        }
    }
    if (txAddr == 0) {
        if (cbTx == 0) cbTx = p->OwnTxSize;
        if (cbTx < UV_TX_MIN) cbTx = UV_TX_MIN;
        if (cbTx > p->OwnTxSize) {
            newTx = UvHeapAlloc(cbTx);
            if (newTx == 0) {
                UvHeapFree(newRx);
                return UV_FAIL(p, IE_MEMORY);
            }
        }
    }
    if (cbRx == 0 || cbTx == 0) {
        UvHeapFree(newRx);
        UvHeapFree(newTx);
        return UV_FAIL(p, IE_INVALIDPARAM);
    }
    UV_LOCK(f);
    if (newRx) { oldRx = p->OwnRx; p->OwnRx = newRx; p->OwnRxSize = cbRx; }
    if (newTx) { oldTx = p->OwnTx; p->OwnTx = newTx; p->OwnTxSize = cbTx; }
    p->Pd.QInAddr = rxAddr ? rxAddr : p->OwnRx;
    p->Pd.QInSize = cbRx;
    p->Pd.QOutAddr = txAddr ? txAddr : p->OwnTx;
    p->Pd.QOutSize = cbTx;
    AcQueueClear(&p->RxQ);
    AcQueueClear(&p->TxQ);
    p->RxStalled = 0;
    UV_UNLOCK(f);
    UvHeapFree(oldRx);
    UvHeapFree(oldTx);
    UVT4("Setup rx %08lx/%lu tx %08lx/%lu", rxAddr, cbRx, txAddr, cbTx);
    UvKick(p, UCK_PURGE_RX | UCK_RX);
    return UV_OK(p);
}

static BOOL PortTransmitChar(UV_PORT *p, ULONG ch)
{
    ULONG f;
    if (p->SysGone)
        return UV_FAIL(p, IE_HARDWARE);
    UV_LOCK(f);
    if (p->Flow.Status & AC_fTximmed) {
        UV_UNLOCK(f);
        return UV_FAIL(p, IE_TRANSMITCHARFAILED);
    }
    p->Flow.Immediate = (UCHAR)ch;
    p->Flow.Status |= AC_fTximmed;
    UV_UNLOCK(f);
    UvKick(p, UCK_TX);
    return UV_OK(p);
}

static void UvFree(UV_PORT *p)
{
    if (p->OwnRx) UvHeapFree(p->OwnRx);
    if (p->OwnTx) UvHeapFree(p->OwnTx);
    if (p->NameSlot < UV_NAMES)
        g_Open[p->NameSlot] = 0;
    p->Magic = 0;
    UvHeapFree((ULONG)p);
}

static BOOL PortClose(UV_PORT *p)
{
    ULONG f;
    UCIF_OP op;

    UVT1("Close (device gone: %lu)", p->SysGone);
    p->Closing = 1;
    UV_LOCK(f);
    if (p->EventHandle) {
        UvCancelEvent(p->EventHandle);
        p->EventHandle = 0;
    }
    UV_UNLOCK(f);
    if (p->TimeoutHandle) {
        UvCancelTimeout(p->TimeoutHandle);
        p->TimeoutHandle = 0;
    }
    if (!p->SysGone) {
        /* Flush queued operations, then drop DTR and RTS (design 7.6). */
        while (p->OpCount) {
            op = p->Ops[p->OpHead];
            if (p->T->Control(p->T->Port, &op) != 0)
                break;
            p->OpHead = (p->OpHead + 1) % UV_OPS;
            p->OpCount--;
        }
        op.Op = UCOP_CONTROL;
        op.Value = 0;
        (void)p->T->Control(p->T->Port, &op);
        p->T->Detach(p->T->Port);
    }
    UvFree(p);
    return TRUE;
}

static void UvComstat(UV_PORT *p, _COMSTAT *cs)
{
    cs->BitMask = p->Flow.Status & (AC_fXoffHold | AC_fXoffSent | AC_fEof | AC_fTximmed);
    cs->cbInque = p->Pd.QInCount;
    cs->cbOutque = p->Pd.QOutCount;
}

static BOOL PortGetQueueStatus(UV_PORT *p, _COMSTAT *cs)
{
    if (cs == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    UvComstat(p, cs);
    return UV_OK(p);
}

static BOOL PortClearError(UV_PORT *p, _COMSTAT *cs, ULONG *errs)
{
    ULONG f;
    UV_LOCK(f);
    if (errs)
        *errs = p->Pd.dwCommError;
    p->Pd.dwCommError = 0;
    UV_UNLOCK(f);
    if (cs)
        UvComstat(p, cs);
    return UV_OK(p);
}

static BOOL PortSetModemStatusShadow(UV_PORT *p, ULONG mask, UCHAR *msr)
{
    UVT2("SetModemStatusShadow mask %08lx msr %08lx", mask, msr);
    p->EvMask = mask;
    if (msr) {
        *msr = *p->pMsr;
        p->pMsr = msr;
    }
    return UV_OK(p);
}

static BOOL PortGetProperties(UV_PORT *p, _COMMPROP *cp)
{
    if (cp == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    UvZero(cp, sizeof(*cp));
    cp->wPacketLength = sizeof(*cp);
    cp->wPacketVersion = 2;
    cp->dwServiceMask = SP_SERIALCOMM;
    cp->dwMaxBaud = BAUD_USER;
    cp->dwProvSubType = PST_RS232;
    cp->dwProvCapabilities = PCF_DTRDSR | PCF_RTSCTS | PCF_RLSD | PCF_PARITY_CHECK |
                             PCF_XONXOFF | PCF_SETXCHAR | PCF_TOTALTIMEOUTS |
                             PCF_INTTIMEOUTS | PCF_SPECIALCHARS;
    cp->dwSettableParams = SP_PARITY | SP_BAUD | SP_DATABITS | SP_STOPBITS |
                           SP_HANDSHAKING | SP_PARITY_CHECK | SP_RLSD;
    cp->dwSettableBaud = 0x0001FFFF | BAUD_USER;
    cp->wSettableData = DATABITS_5 | DATABITS_6 | DATABITS_7 | DATABITS_8 | DATABITS_16;
    cp->wSettableStopParity = STOPBITS_10 | STOPBITS_15 | STOPBITS_20 | PARITY_NONE |
                              PARITY_ODD | PARITY_EVEN | PARITY_MARK | PARITY_SPACE;
    cp->dwCurrentTxQueue = p->Pd.QOutSize;
    cp->dwCurrentRxQueue = p->Pd.QInSize;
    return UV_OK(p);
}

static BOOL PortEscapeFunction(UV_PORT *p, ULONG fn, ULONG in, ULONG *out)
{
    ULONG f;
    UCHAR ch;

    UVT2("EscapeFunction %lu in %08lx", fn, in);
    switch (fn) {
    case SETXOFF:
        UV_LOCK(f); p->Flow.Status |= AC_fXoffHold; UV_UNLOCK(f);
        return UV_OK(p);
    case SETXON:
        UV_LOCK(f); p->Flow.Status &= ~AC_fXoffHold; UV_UNLOCK(f);
        UvKick(p, UCK_TX);
        return UV_OK(p);
    case SETRTS: case CLRRTS: case SETDTR: case CLRDTR:
        /* One SET_CONTROL_LINE_STATE per call, in order (design 7.2). */
        if (fn == SETRTS) p->WireRts = 1;
        if (fn == CLRRTS) p->WireRts = 0;
        if (fn == SETDTR) p->WireDtr = 1;
        if (fn == CLRDTR) p->WireDtr = 0;
        UvQueueControl(p);
        return UV_OK(p);
    case SETBREAK:
    case CLEARBREAK:
        p->Break = (UCHAR)(fn == SETBREAK);
        UvQueueOp(p, UCOP_BREAK, p->Break ? 0xFFFF : 0, 0);
        return UV_OK(p);
    case GETCOMBASEIRQ:
    case GETCOMBASEIRQ1:
        if (out) *out = 0;               /* no I/O base or IRQ */
        return UV_OK(p);
    case SETUPDATETIMEADDR:
        p->pUpdateTime = (ULONG *)in;
        return UV_OK(p);
    case IGNOREERRORONREADS:
        p->IgnoreErrors = 1;
        return UV_OK(p);
    case CLRTIMERLOGIC:
    case ENABLETIMERLOGIC:
        return UV_OK(p);                 /* data is pushed as it arrives */
    case PEEKCHAR:
        UV_LOCK(f);
        if (AcQueuePeek(&p->RxQ, &ch)) {
            UV_UNLOCK(f);
            if (out) *out = ch;
            return UV_OK(p);
        }
        UV_UNLOCK(f);
        if (out) *out = 0;
        return UV_FAIL(p, IE_DEFAULT);
    default:
        return UV_FAIL(p, IE_EXTINVALID);
    }
}

static BOOL PortPurge(UV_PORT *p, ULONG which)
{
    ULONG f;
    UVT1("Purge %lu", which);
    UV_LOCK(f);
    if (which == 0) {
        AcQueueClear(&p->TxQ);
        p->Flow.Status &= ~AC_fTximmed;
    } else {
        AcQueueClear(&p->RxQ);
        p->RxStalled = 0;
    }
    UV_UNLOCK(f);
    UvKick(p, which == 0 ? UCK_PURGE_TX : (UCK_PURGE_RX | UCK_RX));
    return UV_OK(p);
}

static BOOL PortSetEventMask(UV_PORT *p, ULONG mask, ULONG *events)
{
    UVT2("SetEventMask %08lx at %08lx", mask, events);
    p->EvMask = mask;
    p->Pd.dwClientEventMask = mask;
    if (events)
        p->pEvents = events;
    return UV_OK(p);
}

static BOOL PortGetEventMask(UV_PORT *p, ULONG mask, ULONG *events)
{
    ULONG f, ev;
    UV_LOCK(f);
    ev = *p->pEvents & mask;
    *p->pEvents &= ~mask;
    UV_UNLOCK(f);
    if (events)
        *events = ev;
    return UV_OK(p);
}

static BOOL PortWrite(UV_PORT *p, UCHAR *buf, ULONG n, ULONG *written)
{
    ULONG f, w;

    if (written) *written = 0;
    if (p->SysGone)
        return UV_FAIL(p, IE_HARDWARE);
    if (buf == NULL && n)
        return UV_FAIL(p, IE_INVALIDPARAM);
    UV_LOCK(f);
    /* VCOMM's Win32 WriteFile calls PortWrite once and then only waits for
       the queue to drain: whatever is not taken here is reported to the
       program as a failed write (98 SE guest leg, 2026-10-22; Microsoft's
       serial.vxd took a 16384-byte write whole). Our own transmit queue is
       therefore at least UV_TX_MIN. CE_TXFULL changes nothing in VCOMM and
       would block reads until ClearCommError, so it is not set. */
    w = AcTxPut(&p->TxQ, buf, n);
    UV_UNLOCK(f);
    UVT4("Write %lu: took %lu, queue %lu/%lu", n, w, p->Pd.QOutCount, p->Pd.QOutSize);
    if (written) *written = w;
    if (w)
        UvKick(p, UCK_TX);
    return UV_OK(p);
}

static BOOL PortRead(UV_PORT *p, UCHAR *buf, ULONG n, ULONG *got)
{
    ULONG f, r, act = 0, resume;

    if (got) *got = 0;
    if (buf == NULL && n)
        return UV_FAIL(p, IE_INVALIDPARAM);
    if (p->Pd.dwCommError && !p->IgnoreErrors)
        return UV_OK(p);                 /* _VCOMM_ReadComm: TRUE, nothing read */
    UV_LOCK(f);
    r = AcRxGet(&p->RxQ, &p->Dcb, &p->Flow, buf, n, &act);
    resume = r && p->RxStalled;
    if (resume)
        p->RxStalled = 0;
    UV_UNLOCK(f);
    if (got) *got = r;
    if (resume || (act & AC_RX_KICK_TX))
        UvKick(p, (resume ? UCK_RX : 0) | ((act & AC_RX_KICK_TX) ? UCK_TX : 0));
    return UV_OK(p);
}

static BOOL PortEnableNotification(UV_PORT *p, UV_NOTIFY fn, ULONG ref)
{
    UVT2("EnableNotification %08lx ref %08lx", fn, ref);
    p->EvNotify = fn;
    p->EvRef = ref;
    p->Pd.lpClientEventNotify = (ULONG)fn;
    p->Pd.dwClientRefData = ref;
    return UV_OK(p);
}

static BOOL PortSetReadCallBack(UV_PORT *p, ULONG trigger, UV_NOTIFY fn, ULONG ref)
{
    UVT2("SetReadCallBack trigger %ld fn %08lx", trigger, fn);
    p->RxTrigger = trigger;
    p->RxNotify = fn;
    p->RxRef = ref;
    p->Pd.lpClientReadNotify = (ULONG)fn;
    p->CheckNotify = 1;                  /* data may already be waiting */
    UvSchedule(p);
    return UV_OK(p);
}

static BOOL PortSetWriteCallBack(UV_PORT *p, ULONG trigger, UV_NOTIFY fn, ULONG ref)
{
    UVT2("SetWriteCallBack trigger %ld fn %08lx", trigger, fn);
    p->TxTrigger = trigger;
    p->TxNotify = fn;
    p->TxRef = ref;
    p->Pd.lpClientWriteNotify = (ULONG)fn;
    p->CheckNotify = 1;
    UvSchedule(p);
    return UV_OK(p);
}

static BOOL PortGetModemStatus(UV_PORT *p, ULONG *ms)
{
    if (ms == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    *ms = AcMsrToModemStatus(*p->pMsr);
    return UV_OK(p);
}

static BOOL PortGetCommConfig(UV_PORT *p, UCHAR *buf, ULONG *size)
{
    if (size == NULL || buf == NULL)
        return UV_FAIL(p, IE_INVALIDPARAM);
    if (*size < UV_W32DCB) {
        *size = UV_W32DCB;
        return UV_FAIL(p, IE_INVALIDPARAM);
    }
    UvMapRing0ToWin32(&p->Dcb, buf);
    *size = UV_W32DCB;
    return UV_OK(p);
}

static BOOL PortSetCommConfig(UV_PORT *p, UCHAR *buf, ULONG size)
{
    _DCB r0;
    if (buf == NULL || size < UV_W32DCB)
        return UV_FAIL(p, IE_INVALIDPARAM);
    UvCopy(&r0, &p->Dcb, sizeof(r0));
    UvMapWin32ToRing0(buf, &r0);
    return PortSetCommState(p, &r0, 0x0001FFFF);
}

static BOOL PortGetError(UV_PORT *p, ULONG *err)
{
    LONG e = (LONG)p->Pd.dwLastError;
    if (err == NULL)
        return FALSE;
    if (e == 0)
        *err = 0;
    else if (e == IE_EXTINVALID || e == IE_INVALIDSERVICE)
        *err = 1;                        /* ERROR_INVALID_FUNCTION */
    else if (e == IE_BAUDRATE || e == IE_BYTESIZE || e == IE_DEFAULT || e == IE_INVALIDPARAM)
        *err = 87;                       /* ERROR_INVALID_PARAMETER */
    else
        *err = 31;                       /* ERROR_GEN_FAILURE */
    return TRUE;
}

static BOOL PortDeviceIOCtl(UV_PORT *p)
{
    return UV_FAIL(p, IE_INVALIDSERVICE);
}

static PortFunctions g_Funcs = {
    PortSetCommState, PortGetCommState, PortSetup, PortTransmitChar, PortClose,
    PortGetQueueStatus, PortClearError, PortSetModemStatusShadow, PortGetProperties,
    PortEscapeFunction, PortPurge, PortSetEventMask, PortGetEventMask, PortWrite,
    PortRead, PortEnableNotification, PortSetReadCallBack, PortSetWriteCallBack,
    PortGetModemStatus, PortGetCommConfig, PortSetCommConfig, PortGetError,
    PortDeviceIOCtl
};

/* ---- open ------------------------------------------------------------------------ */

static ULONG UvStrLen(const char *s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

static int UvNameEq(const char *a, const char *b)
{
    while (*a && *b) {
        if ((*a | 0x20) != (*b | 0x20)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Fetches the SYS table for `name` (\Device\UsbCom<name>). */
static UCIF_TABLE *UvGetTable(const char *name)
{
    USHORT path[48];
    const char *prefix = UCIF_DEVICE_PREFIX;
    UV_USTR us;
    UV_OA oa;
    UV_IOSB iosb;
    UCIF_REQUEST req;
    UCIF_REPLY rep;
    ULONG h = 0, n = 0, i;
    LONG st;

    for (i = 0; prefix[i] && n < 40; i++) path[n++] = (USHORT)(UCHAR)prefix[i];
    for (i = 0; name[i] && n < 47; i++) path[n++] = (USHORT)(UCHAR)name[i];
    path[n] = 0;
    us.Length = (USHORT)(n * 2);
    us.MaximumLength = (USHORT)(n * 2 + 2);
    us.Buffer = path;
    UvZero(&oa, sizeof(oa));
    oa.Length = sizeof(oa);
    oa.ObjectName = &us;
    oa.Attributes = UV_OBJ_CASE_INSENSITIVE;
    UvZero(&iosb, sizeof(iosb));
    st = UvNtCreate(&h, &oa, &iosb);
    UVT3("NtKernCreateFile %w: %08lx handle %08lx", path, st, h);
    if (st < 0 || h == 0)
        return 0;
    req.Magic = UCIF_MAGIC;
    req.Version = UCIF_VERSION;
    UvZero(&rep, sizeof(rep));
    st = UvNtIoctl(h, &iosb, IOCTL_USBCOM_GET_INTERFACE, &req, sizeof(req), &rep, sizeof(rep));
    UvNtClose(h);
    UVT3("get interface: %08lx magic %08lx table %08lx", st, rep.Magic, rep.Table);
    if (st < 0 || rep.Magic != UCIF_MAGIC || rep.Table == 0 ||
        rep.Table->Version != UCIF_VERSION || rep.Table->Size < sizeof(UCIF_TABLE))
        return 0;
    return rep.Table;
}

static UV_PORT *PortOpen(char *name, ULONG vm, ULONG *err)
{
    UV_PORT *p;
    UCIF_TABLE *t;
    ULONG slot, f;

    (void)vm;
    UVT2("PortOpen %s vm %08lx", name, vm);
    if (name == 0) {                     /* open by number: not supported */
        if (err) *err = (ULONG)IE_BADID;
        return 0;
    }
    for (slot = 0; slot < UV_NAMES; slot++)
        if (g_Names[slot][0] && UvNameEq(g_Names[slot], name))
            break;
    if (slot < UV_NAMES && g_Open[slot]) {
        if (err) *err = (ULONG)IE_OPEN;
        return 0;
    }
    t = UvGetTable(name);
    if (t == 0) {
        UVT1("PortOpen %s: no table", name);
        if (err) *err = (ULONG)IE_HARDWARE;
        return 0;
    }
    p = (UV_PORT *)UvHeapAlloc(sizeof(UV_PORT));
    if (p == 0) {
        UVT1("PortOpen %s: no memory for the port", name);
        if (err) *err = (ULONG)IE_MEMORY;
        return 0;
    }
    p->Magic = UV_MAGIC;
    p->NameSlot = slot;
    p->T = t;
    p->Pd.PDLength = sizeof(PortData);
    p->Pd.PDVersion = 0x0100;            /* no documented value; checked in M1 */
    p->Pd.PDfunctions = &g_Funcs;
    p->Pd.PDNumFunctions = sizeof(g_Funcs) / sizeof(void *);
    p->Pd.bMSRShadow = AC_MSR_CTS;
    p->pMsr = &p->Pd.bMSRShadow;
    p->pEvents = &p->Pd.dwDetectedEvents;
    p->RxTrigger = 0xFFFFFFFF;
    p->TxTrigger = 0xFFFFFFFF;
    p->RxQ.Addr = &p->Pd.QInAddr;   p->RxQ.Size = &p->Pd.QInSize;
    p->RxQ.Count = &p->Pd.QInCount; p->RxQ.Get = &p->Pd.QInGet; p->RxQ.Put = &p->Pd.QInPut;
    p->TxQ.Addr = &p->Pd.QOutAddr;  p->TxQ.Size = &p->Pd.QOutSize;
    p->TxQ.Count = &p->Pd.QOutCount; p->TxQ.Get = &p->Pd.QOutGet; p->TxQ.Put = &p->Pd.QOutPut;
    if (!UvOwnQueue(&p->OwnRx, &p->OwnRxSize, UV_RX_DEFAULT) ||
        !UvOwnQueue(&p->OwnTx, &p->OwnTxSize, UV_TX_DEFAULT)) {
        p->NameSlot = UV_NAMES;
        UvFree(p);
        if (err) *err = (ULONG)IE_MEMORY;
        return 0;
    }
    p->Pd.QInAddr = p->OwnRx;  p->Pd.QInSize = p->OwnRxSize;
    p->Pd.QOutAddr = p->OwnTx; p->Pd.QOutSize = p->OwnTxSize;
    AcDefaultDcb(&p->Dcb, p->OwnRxSize);
    p->WireDtr = 1;                      /* DTR_CONTROL_ENABLE: the Uno resets (7.6) */
    p->WireRts = 1;

    p->Cb.Size = sizeof(p->Cb);
    p->Cb.Context = p;
    p->Cb.RxData = UvRxData;
    p->Cb.TxPull = UvTxPull;
    p->Cb.TxDone = UvTxDone;
    p->Cb.SerialState = UvSerialState;
    p->Cb.Gone = UvGone;
    if (t->Attach(t->Port, &p->Cb) != 0) {
        p->NameSlot = UV_NAMES;
        UvFree(p);
        if (err) *err = (ULONG)IE_OPEN;
        return 0;
    }
    UV_LOCK(f);
    if (slot < UV_NAMES)
        g_Open[slot] = 1;
    UV_UNLOCK(f);
    UvQueueLine(p);
    UvQueueControl(p);
    UVT2("PortOpen %s: port %08lx", name, p);
    return p;
}

/* ---- registration -------------------------------------------------------------- */

/*
 * DriverControl(DC_Initialize, hDevNode, RefData, ...). The 98 DDK gives two
 * orders for the remaining arguments: Name, AllocIOBase, AllocIrq
 * (comm_12pc.htm) and AllocBase, AllocIrq, PortName (vcomm.h comment). An
 * I/O base fits 16 bits and an IRQ 8 bits, so the name is whichever of the
 * first and third extra arguments is a pointer. Which order VCOMM uses is
 * read in M1.
 */
static void UvDriverControl(ULONG fn, ULONG devnode, ULONG ref, ULONG a, ULONG b, ULONG c)
{
    char *name;
    ULONG slot, free = UV_NAMES, len, added;

    (void)b;
    UVT3("DriverControl fn %lu devnode %08lx ref %08lx", fn, devnode, ref);
    UVT3("DriverControl args %08lx %08lx %08lx", a, b, c);
    /* hDevNode 0 is the non-Plug-and-Play call (comm_12pc). It comes when
       the VxD is loaded other than by a VCOMM open, e.g. by a Win32
       CreateFile of the file, and then carries no port (guest leg,
       2026-10-22): add nothing. */
    if (fn != DC_Initialize || devnode == 0)
        return;
    name = (char *)(a >= 0x10000 ? a : c);
    if ((ULONG)name < 0x10000)
        return;
    len = UvStrLen(name);
    if (len == 0 || len > 7)
        return;
    for (slot = 0; slot < UV_NAMES; slot++) {
        if (g_Names[slot][0] == 0 && free == UV_NAMES)
            free = slot;
        if (g_Names[slot][0] && UvNameEq(g_Names[slot], name))
            break;
    }
    if (slot == UV_NAMES) {
        if (free == UV_NAMES)
            return;
        slot = free;
        UvCopy(g_Names[slot], name, len + 1);
    }
    /* Called at every open, VxD resident or not: VCOMM drops the port at
       the last close and needs it added again (an add-once version failed
       every later open with error 2; 98 SE guest leg, 2026-10-22). */
    added = UvAddPort(ref, (void *)PortOpen, g_Names[slot]);
    UVT3("Add_Port %s (name in extra argument %s): %lu", g_Names[slot], (ULONG)name == a ? "1" : "3",
         added);
}

static ULONG UvDynamicInit(void)
{
    ULONG r;
    if (sizeof(AC_DCB) != sizeof(_DCB))
        return 0;
    UVT0("Sys_Dynamic_Device_Init: registering");
    r = UvRegisterPortDriver((void (*)(void))UvDriverControl);
    UVT1("Sys_Dynamic_Device_Init: Register_Port_Driver %lu", r);
    return r ? 1 : 0;
}

/*
 * Control procedure: EAX = message. Carry clear = success. All registers
 * but EAX and the flags are preserved.
 *
 * Sys_Dynamic_Device_Exit is refused (carry set; "the control procedure
 * has the option of rejecting either the load or the unload request",
 * kernel_9ipk): once loaded, the VxD stays. VCOMM unloads a port driver
 * when its last port closes, but it keeps the PortName pointer given to
 * _VCOMM_Add_Port and compares it at the next open; a reload at another
 * address left that pointer dangling and VCOMM faulted (98 SE guest leg,
 * 2026-10-22). Staying resident keeps every pointer VCOMM holds valid.
 */
#if defined(UC_TRACE_E9)
static void UvCtlTrace(ULONG msg)
{
    if (msg == SYS_DYNAMIC_DEVICE_INIT || msg == SYS_DYNAMIC_DEVICE_EXIT)
        UVT1("control message %lx", msg);
}
#endif

void __declspec(naked) UvControl(void)
{
#if defined(UC_TRACE_E9)
    _asm {
        pushad                          ; trace flavour: log every message
        push    eax
        call    UvCtlTrace
        add     esp, 4
        popad
    }
#endif
    _asm {
        cmp     eax, SYS_DYNAMIC_DEVICE_INIT
        je      do_init
        cmp     eax, SYS_DYNAMIC_DEVICE_EXIT
        je      no_exit
        cmp     eax, W32_DEVICEIOCONTROL ; a Win32 CreateFile("\\.\USBCOMV.VXD")
        jne     done                    ; (a load test): accept, do nothing
        xor     eax, eax
    done:
        clc
        ret
    no_exit:                            ; stay loaded (see above)
        stc
        ret
    do_init:
        pushad
        call    UvDynamicInit
        mov     [esp + 28], eax
        popad
        cmp     eax, 1                  ; CF = 1 if eax == 0
        ret
    }
}
