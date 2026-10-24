/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * acmcore.c - operating-system-independent usbcom logic (see acmcore.h).
 *
 * Original code. The descriptor rules are USB 2.0 chapter 9 and CDC 1.2 /
 * PSTN 1.2 as summarised in docs/design/serial.md 6.1; the fallbacks
 * there (no Union, Union reversed, combined interface) are implemented
 * from that description, not from another implementation. The queue and
 * event semantics follow the Windows 98 DDK's VCOMM documentation.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include "acmcore.h"

#define AC_DT_CONFIG     0x02
#define AC_DT_INTERFACE  0x04
#define AC_DT_ENDPOINT   0x05
#define AC_DT_CS_IFACE   0x24

#define AC_CS_CALLMGMT   0x01
#define AC_CS_ACM        0x02
#define AC_CS_UNION      0x06

#define AC_MAX_ALTS      32  /* about 1.3 KiB of stack; kernel stacks are small */
#define AC_MAX_EPS       4

typedef struct AC_ALT {
    AC_IFACE If;
    AC_U32   Start, End;      /* byte range of this alternate setting */
    int      NumEps;
    AC_U8    EpAddr[AC_MAX_EPS];
    AC_U8    EpAttr[AC_MAX_EPS];
    AC_U16   EpMax[AC_MAX_EPS];
    AC_U8    EpInterval[AC_MAX_EPS];
} AC_ALT;

static AC_U16 AcLe16(const AC_U8 *p) { return (AC_U16)(p[0] | (p[1] << 8)); }

/* Fills alts[]; returns the count, or -1 if malformed. */
static int AcScan(const AC_U8 *cfg, AC_U32 len, AC_ALT *alts, int max)
{
    AC_U32 total, off;
    int n = 0;

    if (cfg == 0 || len < 9 || cfg[0] < 9 || cfg[1] != AC_DT_CONFIG)
        return -1;
    total = AcLe16(cfg + 2);
    if (total < 9 || total > len)
        return -1;
    off = cfg[0];
    while (off < total) {
        AC_U32 dlen;
        AC_U8 type;
        if (total - off < 2)
            return -1;
        dlen = cfg[off];
        type = cfg[off + 1];
        if (dlen < 2 || dlen > total - off)
            return -1;
        if (type == AC_DT_INTERFACE) {
            if (dlen < 9)
                return -1;
            if (n > 0)
                alts[n - 1].End = off;
            if (n >= max)
                return -1;
            alts[n].If.Number = cfg[off + 2];
            alts[n].If.Alt = cfg[off + 3];
            alts[n].If.NumEp = cfg[off + 4];
            alts[n].If.Class = cfg[off + 5];
            alts[n].If.SubClass = cfg[off + 6];
            alts[n].If.Protocol = cfg[off + 7];
            alts[n].Start = off;
            alts[n].End = total;
            alts[n].NumEps = 0;
            n++;
        } else if (type == AC_DT_ENDPOINT && n > 0) {
            AC_ALT *a = &alts[n - 1];
            if (dlen < 7)
                return -1;
            if (a->NumEps < AC_MAX_EPS) {
                a->EpAddr[a->NumEps] = cfg[off + 2];
                a->EpAttr[a->NumEps] = cfg[off + 3];
                a->EpMax[a->NumEps] = (AC_U16)(AcLe16(cfg + off + 4) & 0x07FF);
                a->EpInterval[a->NumEps] = cfg[off + 6];
                a->NumEps++;
            }
        }
        off += dlen;
    }
    return n;
}

int AcListInterfaces(const AC_U8 *cfg, AC_U32 len, AC_IFACE *out, int max)
{
    AC_ALT alts[AC_MAX_ALTS];
    int n, i;

    n = AcScan(cfg, len, alts, AC_MAX_ALTS);
    if (n < 0 || n > max)
        return -1;
    for (i = 0; i < n; i++)
        out[i] = alts[i].If;
    return n;
}

/* Index of the first endpoint of `a` with this type/direction, or -1. */
static int AcFindEp(const AC_ALT *a, AC_U8 xferType, int in)
{
    int i;
    for (i = 0; i < a->NumEps; i++) {
        if ((a->EpAttr[i] & 3) == xferType &&
            ((a->EpAddr[i] & 0x80) != 0) == (in != 0) &&
            a->EpMax[i] != 0)
            return i;
    }
    return -1;
}

static int AcHasBulkPair(const AC_ALT *a)
{
    return AcFindEp(a, 2, 1) >= 0 && AcFindEp(a, 2, 0) >= 0;
}

static int AcClaimed(const AC_FUNC *f, int n, AC_U8 number)
{
    int i;
    for (i = 0; i < n; i++)
        if (f[i].CommIf == number || f[i].DataIf == number)
            return 1;
    return 0;
}

int AcFindFunctions(const AC_U8 *cfg, AC_U32 len, AC_U32 quirks,
                    AC_FUNC *out, int max)
{
    AC_ALT alts[AC_MAX_ALTS];
    int n, i, found = 0;

    n = AcScan(cfg, len, alts, AC_MAX_ALTS);
    if (n < 0)
        return -1;
    if (quirks & AC_Q_IGNORE_DEVICE)
        return 0;

    for (i = 0; i < n && found < max; i++) {
        const AC_ALT *c = &alts[i];
        AC_FUNC f;
        AC_U32 off;
        int haveUnion = 0, haveCm = 0, haveData = 0, j, k;
        AC_U8 uCtl = 0, uSub = 0, cmData = 0, dataNum = 0;

        if (c->If.Class != 0x02 || c->If.SubClass != 0x02)
            continue;
        if (AcClaimed(out, found, c->If.Number))
            continue;
        /* Use the first alternate setting listed for the interface. */
        for (j = 0; j < i; j++)
            if (alts[j].If.Number == c->If.Number)
                break;
        if (j < i)
            continue;

        f.CommIf = c->If.Number;
        f.CommAlt = c->If.Alt;
        f.Caps = 0;
        f.Flags = 0;
        f.NotifyEp = 0;
        f.NotifyMax = 0;
        f.NotifyInterval = 0;

        for (off = c->Start + cfg[c->Start]; off < c->End; off += cfg[off]) {
            AC_U32 dlen = cfg[off];
            if (cfg[off + 1] != AC_DT_CS_IFACE || dlen < 3)
                continue;
            switch (cfg[off + 2]) {
            case AC_CS_CALLMGMT:
                if (dlen >= 5) { haveCm = 1; cmData = cfg[off + 4]; }
                break;
            case AC_CS_ACM:
                if (dlen >= 4) f.Caps = cfg[off + 3];
                break;
            case AC_CS_UNION:
                if (dlen >= 5 && !haveUnion) {
                    haveUnion = 1; uCtl = cfg[off + 3]; uSub = cfg[off + 4];
                }
                break;
            }
        }
        if (quirks & AC_Q_NO_CAP_LINE)
            f.Caps &= (AC_U8)~AC_CAP_LINE;

        k = AcFindEp(c, 3, 1);
        if (k >= 0) {
            f.NotifyEp = c->EpAddr[k];
            f.NotifyMax = c->EpMax[k];
            f.NotifyInterval = c->EpInterval[k];
        }

        if (haveUnion && !(quirks & AC_Q_NO_UNION_NORMAL)) {
            if (uCtl == c->If.Number) {
                dataNum = uSub; haveData = 1; f.Flags |= AC_F_UNION;
            } else if (uSub == c->If.Number) {
                dataNum = uCtl; haveData = 1;
                f.Flags |= AC_F_UNION | AC_F_SWAPPED;
            }
        }
        if (!haveData && haveCm && cmData != c->If.Number) {
            dataNum = cmData; haveData = 1; f.Flags |= AC_F_CALLMGMT;
        }
        if (!haveData && c->If.Number != 0xFF) {
            dataNum = (AC_U8)(c->If.Number + 1); haveData = 1;
            f.Flags |= AC_F_NEXT;
        }

        /* The data interface: the first alternate setting with a bulk pair. */
        k = -1;
        if (haveData && dataNum != c->If.Number &&
            !AcClaimed(out, found, dataNum)) {
            for (j = 0; j < n; j++) {
                if (alts[j].If.Number == dataNum && AcHasBulkPair(&alts[j])) {
                    k = j;
                    break;
                }
            }
        }
        if (k < 0) {
            if (!AcHasBulkPair(c))
                continue;           /* no usable data path: not a port */
            k = i;
            f.Flags = (AC_U8)((f.Flags & ~(AC_F_UNION | AC_F_SWAPPED |
                       AC_F_CALLMGMT | AC_F_NEXT)) | AC_F_COMBINED);
        }
        {
            const AC_ALT *d = &alts[k];
            int in = AcFindEp(d, 2, 1), outEp = AcFindEp(d, 2, 0);
            f.DataIf = d->If.Number;
            f.DataAlt = d->If.Alt;
            f.BulkIn = d->EpAddr[in];
            f.InMax = d->EpMax[in];
            f.BulkOut = d->EpAddr[outEp];
            f.OutMax = d->EpMax[outEp];
        }
        out[found++] = f;
    }
    return found;
}

/* ---- Line coding ----------------------------------------------------- */

AC_U32 AcMapBaud(AC_U32 b)
{
    static const AC_U32 cbr[24] = {
        110, 300, 600, 1200, 2400, 4800, 9600, 14400, 19200,
        0, 0, 38400, 0, 0, 0, 56000, 0, 0, 0, 128000, 0, 0, 0, 256000
    };
    if (b >= 0xFF10 && b <= 0xFF27)
        return cbr[b - 0xFF10];
    return b;
}

void AcEncodeLineCoding(AC_U32 rate, AC_U8 stopBits, AC_U8 parity,
                        AC_U8 dataBits, AC_U8 out[7])
{
    out[0] = (AC_U8)rate;
    out[1] = (AC_U8)(rate >> 8);
    out[2] = (AC_U8)(rate >> 16);
    out[3] = (AC_U8)(rate >> 24);
    out[4] = stopBits;
    out[5] = parity;
    out[6] = dataBits;
}

void AcDcbToLine(const AC_DCB *dcb, AC_LINE *line)
{
    line->Rate = AcMapBaud(dcb->BaudRate);
    line->StopBits = dcb->StopBits;
    line->Parity = dcb->Parity;
    line->DataBits = dcb->ByteSize;
    line->Dtr = (AC_U8)((dcb->BitMask & AC_fDTRDisable) ? 0 : 1);
    line->Rts = (AC_U8)((dcb->BitMask & AC_fRTSDisable) ? 0 : 1);
}

void AcDefaultDcb(AC_DCB *d, AC_U32 rxQueueSize)
{
    AC_U8 *p = (AC_U8 *)d;
    AC_U32 i;
    for (i = 0; i < sizeof(*d); i++)
        p[i] = 0;
    d->DCBLength = sizeof(*d);
    d->BaudRate = 9600;
    d->BitMask = AC_fBinary | AC_fDtrEnable | AC_fRTSEnable;
    d->XonLim = rxQueueSize / 4;
    d->XoffLim = rxQueueSize / 4;
    d->ByteSize = 8;
    d->XonChar = 0x11;
    d->XoffChar = 0x13;
}

long AcApplyDcb(AC_DCB *cur, const AC_DCB *req, AC_U32 mask)
{
    if (mask & AC_mBaudRate) {
        if (AcMapBaud(req->BaudRate) == 0)
            return AC_IE_BAUDRATE;
    }
    if (mask & AC_mByteSize) {
        if (!((req->ByteSize >= 5 && req->ByteSize <= 8) || req->ByteSize == 16))
            return AC_IE_BYTESIZE;
    }
    if ((mask & AC_mParity) && req->Parity > 4)
        return AC_IE_DEFAULT;
    if ((mask & AC_mStopBits) && req->StopBits > 2)
        return AC_IE_DEFAULT;

    if (mask & AC_mBaudRate)   cur->BaudRate = req->BaudRate;
    if (mask & AC_mBitMask)    cur->BitMask = req->BitMask;
    if (mask & AC_mXonLim)     cur->XonLim = req->XonLim;
    if (mask & AC_mXoffLim)    cur->XoffLim = req->XoffLim;
    if (mask & AC_mByteSize)   cur->ByteSize = req->ByteSize;
    if (mask & AC_mParity)     cur->Parity = req->Parity;
    if (mask & AC_mStopBits)   cur->StopBits = req->StopBits;
    if (mask & AC_mXonChar)    cur->XonChar = req->XonChar;
    if (mask & AC_mXoffChar)   cur->XoffChar = req->XoffChar;
    if (mask & AC_mErrorChar)  cur->ErrorChar = req->ErrorChar;
    if (mask & AC_mEofChar)    cur->EofChar = req->EofChar;
    if (mask & AC_mEvtChar1)   cur->EvtChar1 = req->EvtChar1;
    if (mask & AC_mEvtChar2)   cur->EvtChar2 = req->EvtChar2;
    if (mask & AC_mRlsTimeout) cur->RlsTimeout = req->RlsTimeout;
    if (mask & AC_mCtsTimeout) cur->CtsTimeout = req->CtsTimeout;
    if (mask & AC_mDsrTimeout) cur->DsrTimeout = req->DsrTimeout;
    if (mask & AC_mTxDelay)    cur->TxDelay = req->TxDelay;
    return 0;
}

/* ---- SERIAL_STATE ---------------------------------------------------- */

int AcNextNotify(const AC_U8 *buf, AC_U32 len, AC_U32 *off, AC_U8 commIf,
                 AC_U16 *state)
{
    while (*off <= len && len - *off >= 8) {
        const AC_U8 *h = buf + *off;
        AC_U32 wlen = AcLe16(h + 6);
        if (h[0] != 0xA1 || wlen > len - *off - 8) {
            *off = len;                  /* not a notification, or truncated */
            return 0;
        }
        *off += 8 + wlen;
        if (h[1] == 0x20 && AcLe16(h + 4) == commIf && wlen >= 2) {
            *state = AcLe16(h + 8);
            return 1;
        }
    }
    return 0;
}

int AcParseNotify(const AC_U8 *buf, AC_U32 len, AC_U8 commIf, AC_U16 *state)
{
    AC_U32 off = 0;
    int found = 0;

    while (AcNextNotify(buf, len, &off, commIf, state))
        found++;
    return found;
}

AC_U8 AcStateToMsr(AC_U16 prev, AC_U16 state, AC_U32 *events, AC_U32 *errors)
{
    AC_U8 msr = AC_MSR_CTS;
    AC_U16 changed = (AC_U16)(prev ^ state);

    if (state & AC_SS_DSR)  msr |= AC_MSR_DSR;
    if (state & AC_SS_RING) msr |= AC_MSR_RI;
    if (state & AC_SS_DCD)  msr |= AC_MSR_DCD;

    if (changed & AC_SS_DSR) { msr |= AC_MSR_DDSR; *events |= AC_EV_DSR; }
    if (changed & AC_SS_DCD) { msr |= AC_MSR_DDCD; *events |= AC_EV_RLSD; }
    if (changed & AC_SS_RING) {
        if (state & AC_SS_RING) {
            *events |= AC_EV_RING;
        } else {
            msr |= AC_MSR_TERI;
            *events |= AC_EV_RingTe;
        }
    }
    if (changed & (AC_SS_DSR | AC_SS_DCD)) {
        *events |= AC_EV_CTSS;
        if (state & AC_SS_DSR) *events |= AC_EV_DSRS;
        if (state & AC_SS_DCD) *events |= AC_EV_RLSDS;
    }
    /* Irregular signals are one-shot (PSTN 1.2 6.5.4): each report counts. */
    if (state & AC_SS_BREAK)   { *events |= AC_EV_BREAK; *errors |= AC_CE_BREAK; }
    if (state & AC_SS_FRAMING) { *events |= AC_EV_ERR; *errors |= AC_CE_FRAME; }
    if (state & AC_SS_PARITY)  { *events |= AC_EV_ERR; *errors |= AC_CE_RXPARITY; }
    if (state & AC_SS_OVERRUN) { *events |= AC_EV_ERR; *errors |= AC_CE_OVERRUN; }
    return msr;
}

AC_U32 AcMsrToModemStatus(AC_U8 msr)
{
    AC_U32 ms = 0;
    if (msr & AC_MSR_CTS) ms |= AC_MS_CTS_ON;
    if (msr & AC_MSR_DSR) ms |= AC_MS_DSR_ON;
    if (msr & AC_MSR_RI)  ms |= AC_MS_RING_ON;
    if (msr & AC_MSR_DCD) ms |= AC_MS_RLSD_ON;
    return ms;
}

/* ---- Queues ------------------------------------------------------------ */

static int AcQueueOk(const AC_QUEUE *q)
{
    return *q->Addr != 0 && *q->Size != 0 && *q->Count <= *q->Size &&
           *q->Get < *q->Size && *q->Put < *q->Size;
}

AC_U32 AcRxPut(const AC_QUEUE *q, const AC_DCB *dcb, AC_FLOW *flow,
               const AC_U8 *data, AC_U32 len, AC_U32 *events, AC_U32 *actions)
{
    AC_U8 *base;
    AC_U32 used = 0;

    if (!AcQueueOk(q))
        return 0;
    base = (AC_U8 *)*q->Addr;
    while (used < len) {
        AC_U8 b = data[used];
        if (dcb->BitMask & AC_fOutX) {
            if (b == (AC_U8)dcb->XoffChar) {
                flow->Status |= AC_fXoffHold;
                used++;
                continue;
            }
            if (b == (AC_U8)dcb->XonChar) {
                if (flow->Status & AC_fXoffHold)
                    *actions |= AC_RX_KICK_TX;
                flow->Status &= ~AC_fXoffHold;
                used++;
                continue;
            }
        }
        if ((dcb->BitMask & AC_fNullStrip) && b == 0) {
            used++;
            continue;
        }
        if (*q->Count >= *q->Size)
            break;                          /* full: the rest stays queued */
        base[*q->Put] = b;
        *q->Put = (*q->Put + 1 == *q->Size) ? 0 : *q->Put + 1;
        (*q->Count)++;
        *events |= AC_EV_RXCHAR;
        if (b == (AC_U8)dcb->EvtChar1)
            *events |= AC_EV_RXFLAG;
        if (!(dcb->BitMask & AC_fBinary) && b == (AC_U8)dcb->EofChar)
            flow->Status |= AC_fEof;
        used++;
    }
    if ((dcb->BitMask & AC_fInX) && !(flow->Status & AC_fXoffSent) &&
        *q->Size - *q->Count < dcb->XoffLim) {
        flow->FlowChar = (AC_U8)dcb->XoffChar;
        flow->FlowPending = 1;
        flow->Status |= AC_fXoffSent;
        *actions |= AC_RX_KICK_TX;
    }
    return used;
}

AC_U32 AcRxGet(const AC_QUEUE *q, const AC_DCB *dcb, AC_FLOW *flow,
               AC_U8 *out, AC_U32 len, AC_U32 *actions)
{
    const AC_U8 *base;
    AC_U32 n = 0;

    if (!AcQueueOk(q))
        return 0;
    base = (const AC_U8 *)*q->Addr;
    while (n < len && *q->Count > 0) {
        out[n++] = base[*q->Get];
        *q->Get = (*q->Get + 1 == *q->Size) ? 0 : *q->Get + 1;
        (*q->Count)--;
    }
    if ((flow->Status & AC_fXoffSent) && *q->Count < dcb->XonLim) {
        flow->FlowChar = (AC_U8)dcb->XonChar;
        flow->FlowPending = 1;
        flow->Status &= ~AC_fXoffSent;
        *actions |= AC_RX_KICK_TX;
    }
    return n;
}

AC_U32 AcTxPut(const AC_QUEUE *q, const AC_U8 *data, AC_U32 len)
{
    AC_U8 *base;
    AC_U32 n = 0;

    if (!AcQueueOk(q))
        return 0;
    base = (AC_U8 *)*q->Addr;
    while (n < len && *q->Count < *q->Size) {
        base[*q->Put] = data[n++];
        *q->Put = (*q->Put + 1 == *q->Size) ? 0 : *q->Put + 1;
        (*q->Count)++;
    }
    return n;
}

AC_U32 AcTxPull(const AC_QUEUE *q, AC_FLOW *flow, AC_U8 *out, AC_U32 max)
{
    AC_U32 n = 0;

    if (max == 0)
        return 0;
    if (flow->FlowPending) {
        out[n++] = flow->FlowChar;
        flow->FlowPending = 0;
    }
    if ((flow->Status & AC_fTximmed) && n < max) {
        out[n++] = flow->Immediate;
        flow->Status &= ~AC_fTximmed;
    }
    if (!(flow->Status & AC_fXoffHold) && AcQueueOk(q)) {
        const AC_U8 *base = (const AC_U8 *)*q->Addr;
        while (n < max && *q->Count > 0) {
            out[n++] = base[*q->Get];
            *q->Get = (*q->Get + 1 == *q->Size) ? 0 : *q->Get + 1;
            (*q->Count)--;
        }
    }
    return n;
}

void AcQueueClear(const AC_QUEUE *q)
{
    *q->Count = 0;
    *q->Get = 0;
    *q->Put = 0;
}

int AcQueuePeek(const AC_QUEUE *q, AC_U8 *ch)
{
    if (!AcQueueOk(q) || *q->Count == 0)
        return 0;
    *ch = ((const AC_U8 *)*q->Addr)[*q->Get];
    return 1;
}

/* ---- COM numbers ---------------------------------------------------------- */

AC_U32 AcParsePortName(const char *s, AC_U32 len)
{
    AC_U32 i, n = 0;

    if (len < 4 || len > 6)
        return 0;
    if ((s[0] | 0x20) != 'c' || (s[1] | 0x20) != 'o' || (s[2] | 0x20) != 'm')
        return 0;
    if (s[3] == '0')
        return 0;
    for (i = 3; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        n = n * 10 + (AC_U32)(s[i] - '0');
    }
    return (n >= 1 && n <= AC_COM_MAX) ? n : 0;
}

AC_U32 AcFormatPortName(AC_U32 n, char *out)
{
    AC_U32 len = 3;
    out[0] = 'C'; out[1] = 'O'; out[2] = 'M';
    if (n >= 100) out[len++] = (char)('0' + n / 100);
    if (n >= 10)  out[len++] = (char)('0' + (n / 10) % 10);
    out[len++] = (char)('0' + n % 10);
    out[len] = 0;
    return len;
}

AC_U32 AcChoosePort(const AC_U8 used[32], AC_U32 remembered, AC_U32 minimum)
{
    AC_U32 n;

    if (minimum < 1)
        minimum = 1;
    if (remembered >= 1 && remembered <= AC_COM_MAX &&
        !(used[remembered >> 3] & (1u << (remembered & 7))))
        return remembered;
    for (n = minimum; n <= AC_COM_MAX; n++)
        if (!(used[n >> 3] & (1u << (n & 7))))
            return n;
    return 0;
}

/* ---- Trace text -------------------------------------------------------- */

AC_U32 AcFormat(char *out, AC_U32 cap, const char *fmt, const AC_U32 *v, int n)
{
    AC_U32 len = 0;
    int arg = 0;

    if (cap == 0)
        return 0;
    while (*fmt && len + 1 < cap) {
        char pad = ' ', conv;
        int width = 0;
        AC_U32 x;

        if (*fmt != '%') {
            out[len++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        if (*fmt >= '1' && *fmt <= '9') { width = *fmt++ - '0'; }
        if (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') fmt++;
        conv = *fmt;
        if (conv == 0)
            break;
        fmt++;
        if (conv == '%') {
            out[len++] = '%';
            continue;
        }
        x = (arg < n && v) ? v[arg] : 0;
        arg++;
        if (conv == 's') {
            const char *s = (const char *)x;
            if (s == 0) s = "(null)";
            while (*s && len + 1 < cap) out[len++] = *s++;
        } else if (conv == 'w') {
            const AC_U16 *w = (const AC_U16 *)x;
            if (w == 0) {
                const char *s = "(null)";
                while (*s && len + 1 < cap) out[len++] = *s++;
            } else {
                while (*w && len + 1 < cap) {
                    out[len++] = (char)((*w >= 0x20 && *w < 0x7F) ? *w : '?');
                    w++;
                }
            }
        } else if (conv == 'c') {
            out[len++] = (char)x;
        } else if (conv == 'u' || conv == 'd' || conv == 'x' || conv == 'X') {
            char digits[12];
            int k = 0, neg = 0;
            AC_U32 base = (conv == 'x' || conv == 'X') ? 16 : 10;
            const char *set = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (conv == 'd' && (long)x < 0) { neg = 1; x = (AC_U32)(-(long)x); }
            do { digits[k++] = set[x % base]; x /= base; } while (x && k < 11);
            if (neg) {
                if (pad == '0' && len + 1 < cap) { out[len++] = '-'; neg = 0; }
                width--;
            }
            while (width > k && len + 1 < cap) { out[len++] = pad; width--; }
            if (neg && len + 1 < cap) out[len++] = '-';
            while (k && len + 1 < cap) out[len++] = digits[--k];
        } else {
            out[len++] = conv;
        }
    }
    out[len] = 0;
    return len;
}
