/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * prncore.c - the pure parts of usbprn; see prncore.h.
 *
 * The alternate-setting order (02, then 01, then 03; the bidirectional
 * quirk drops the IN endpoint) and the 1284 id length clamp (2 to 1023)
 * are adapted from Linux drivers/usb/class/usblp.c, v6.12
 * (usblp_select_alts(), usblp_cache_device_id_string()),
 * SPDX-License-Identifier: GPL-2.0+; NOTICE.md has the row.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 * Copyright (c) 1999 Michael Gee <michael@linuxspecific.com>
 * Copyright (c) 1999 Pavel Machek <pavel@ucw.cz>
 * Copyright (c) 2000 Randy Dunlap <rdunlap@xenotime.net>
 * Copyright (c) 2000 Vojtech Pavlik <vojtech@suse.cz>
 * Copyright (c) 2001 Pete Zaitcev <zaitcev@redhat.com>
 * Copyright (c) 2001 David Paschal <paschal@rcsis.com>
 * Copyright (c) 2006 Oliver Neukum <oliver@neukum.name>
 */
#include "prncore.h"

/* ---- interface and alternate choice ------------------------------------ */

static int PcRank(int proto, int haveOut, int haveIn, PC_U32 flags)
{
    if (!haveOut)
        return 0;
    if (proto == PC_PROTO_BIDI && haveIn && !(flags & PC_Q_NO_BIDI))
        return 3;
    if (proto == PC_PROTO_UNI || proto == PC_PROTO_BIDI)
        return 2;
    if (proto == PC_PROTO_1284_4 && haveIn)
        return 1;
    return 0;
}

int PcSelectAlt(const PC_U8 *cfg, PC_U32 len, PC_U32 flags, PC_ALT *out)
{
    PC_U32 off, total, ifOff = 0;
    int inIf = 0, best = 0, haveIf = 0;
    PC_U8 ifNum = 0, alt = 0, cls = 0, sub = 0, proto = 0, chosenIf = 0;
    PC_U8 outEp = 0, inEp = 0, numEp = 0, seenEp = 0;
    PC_U16 outMax = 0, inMax = 0;

    out->Found = 0;
    out->Interface = out->Alternate = out->Protocol = 0;
    out->OutEp = out->InEp = 0;
    out->OutMax = out->InMax = 0;
    out->IfOffset = 0;
    out->NumEps = 0;
    if (cfg == 0 || len < 9 || cfg[0] < 9 || cfg[1] != 2)
        return -1;
    total = (PC_U32)cfg[2] | ((PC_U32)cfg[3] << 8);
    if (total < len)
        len = total;

    for (off = 0; off <= len; ) {
        int atEnd = off + 2 > len;
        PC_U8 dlen = (PC_U8)(atEnd ? 0 : cfg[off]);
        PC_U8 type = (PC_U8)(atEnd ? 0 : cfg[off + 1]);

        if (!atEnd && (dlen < 2 || off + dlen > len))
            return -1;
        /* An interface ends at the next interface descriptor or the end. */
        if (inIf && (atEnd || (type == 4 && dlen >= 9))) {
            if (cls == 7 && sub == 1 && (!haveIf || ifNum == chosenIf)) {
                int haveIn = inEp != 0;
                int r = PcRank(proto, outEp != 0, haveIn, flags);
                /* A printer alternate whose endpoint descriptors do not
                   match its bNumEndpoints is not passed on to the stack. */
                if (seenEp != numEp)
                    return -1;
                haveIf = 1;
                chosenIf = ifNum;
                if (r > best) {
                    best = r;
                    out->Found = 1;
                    out->Interface = ifNum;
                    out->Alternate = alt;
                    out->Protocol = (PC_U8)(r == 2 && proto == PC_PROTO_BIDI ? PC_PROTO_UNI : proto);
                    out->OutEp = outEp;
                    out->OutMax = outMax;
                    out->InEp = (PC_U8)((r == 2) ? 0 : inEp);
                    out->InMax = (PC_U16)((r == 2) ? 0 : inMax);
                    out->IfOffset = ifOff;
                    out->NumEps = numEp;
                }
            }
            inIf = 0;
        }
        if (atEnd)
            break;
        if (type == 4 && dlen >= 9) {
            inIf = 1;
            ifOff = off;
            ifNum = cfg[off + 2];
            alt = cfg[off + 3];
            cls = cfg[off + 5];
            sub = cfg[off + 6];
            proto = cfg[off + 7];
            numEp = cfg[off + 4];
            seenEp = 0;
            outEp = inEp = 0;
            outMax = inMax = 0;
        } else if (type == 5 && dlen >= 7 && inIf) {
            PC_U8 addr = cfg[off + 2];
            PC_U8 attr = cfg[off + 3];
            PC_U16 mps = (PC_U16)(cfg[off + 4] | (cfg[off + 5] << 8));
            if (seenEp < 255)
                seenEp++;
            if ((attr & 3) == 2) {
                if ((addr & 0x80) && inEp == 0) {
                    inEp = addr;
                    inMax = mps;
                } else if (!(addr & 0x80) && outEp == 0) {
                    outEp = addr;
                    outMax = mps;
                }
            }
        }
        off += dlen;
    }
    return out->Found ? 1 : 0;
}

/* The bidirectional-quirk printers of Linux usblp.c quirk_printers[]
   (USBLP_QUIRK_BIDIR): their IN endpoint is not used. */
static const struct { PC_U16 Vid, Pid; } g_NoBidi[] = {
    { 0x03f0, 0x0004 },     /* HP DeskJet 895C */
    { 0x03f0, 0x0104 },     /* HP DeskJet 880C */
    { 0x03f0, 0x0204 },     /* HP DeskJet 815C */
    { 0x03f0, 0x0304 },     /* HP DeskJet 810C/812C */
    { 0x03f0, 0x0404 },     /* HP DeskJet 830C */
    { 0x03f0, 0x0504 },     /* HP DeskJet 885C */
    { 0x03f0, 0x0604 },     /* HP DeskJet 840C */
    { 0x03f0, 0x0804 },     /* HP DeskJet 816C */
    { 0x03f0, 0x1104 },     /* HP Deskjet 959C */
    { 0x0409, 0xefbe },     /* NEC Picty900 (HP OEM) */
    { 0x0409, 0xbef4 },     /* NEC Picty760 (HP OEM) */
    { 0x0409, 0xf0be },     /* NEC Picty920 (HP OEM) */
    { 0x0409, 0xf1be },     /* NEC Picty800 (HP OEM) */
    { 0x0482, 0x0010 },     /* Kyocera Mita FS 820 */
    { 0x04f9, 0x000d },     /* Brother HL-1440 */
};

PC_U32 PcQuirks(PC_U16 vid, PC_U16 pid)
{
    PC_U32 i;

    for (i = 0; i < sizeof(g_NoBidi) / sizeof(g_NoBidi[0]); i++)
        if (g_NoBidi[i].Vid == vid && g_NoBidi[i].Pid == pid)
            return PC_Q_NO_BIDI;
    return 0;
}

/* ---- IEEE 1284 device id ------------------------------------------------ */

PC_U32 PcIdLength(const PC_U8 *buf, PC_U32 got)
{
    PC_U32 n;

    if (buf == 0 || got < 2)
        return 0;
    n = ((PC_U32)buf[0] << 8) | buf[1];
    if (n < 2)
        n = 2;
    if (n > PC_ID_MAX + 2)
        n = PC_ID_MAX + 2;
    if (n > got)
        n = got;
    return n - 2;
}

static char PcUpper(char c)
{
    return (char)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
}

static int PcIsBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Whether id[a..b) names the key (blanks around it ignored). */
static int PcKeyIs(const char *s, PC_U32 n, const char *name)
{
    PC_U32 i;

    while (n && PcIsBlank(s[0])) {
        s++;
        n--;
    }
    while (n && PcIsBlank(s[n - 1]))
        n--;
    for (i = 0; i < n; i++)
        if (name[i] == 0 || PcUpper(s[i]) != name[i])
            return 0;
    return name[n] == 0;
}

static const char *const g_Keys[6][2] = {
    { "MFG", "MANUFACTURER" },
    { "MDL", "MODEL" },
    { "CMD", "COMMAND SET" },
    { "CLS", "CLASS" },
    { "DES", "DESCRIPTION" },
    { "CID", "COMPATIBLE ID" },
};

PC_U32 PcIdValue(const char *id, PC_U32 len, int key, char *out, PC_U32 cap)
{
    PC_U32 i = 0, n = 0;

    if (cap == 0)
        return 0;
    out[0] = 0;
    if (id == 0 || key < 0 || key > 5)
        return 0;
    while (i < len) {
        PC_U32 k0 = i, k1, v0, v1;
        while (i < len && id[i] != ':' && id[i] != ';' && id[i] != 0)
            i++;
        if (i >= len || id[i] == 0)
            break;
        if (id[i] == ';') {     /* a field with no key: skip it */
            i++;
            continue;
        }
        k1 = i++;
        v0 = i;
        while (i < len && id[i] != ';' && id[i] != 0)
            i++;
        v1 = i;
        if (i < len && id[i] == ';')
            i++;
        if (PcKeyIs(id + k0, k1 - k0, g_Keys[key][0]) ||
            PcKeyIs(id + k0, k1 - k0, g_Keys[key][1])) {
            while (v0 < v1 && PcIsBlank(id[v0]))
                v0++;
            while (v1 > v0 && PcIsBlank(id[v1 - 1]))
                v1--;
            for (n = 0; v0 < v1 && n + 1 < cap; v0++)
                out[n++] = id[v0];
            out[n] = 0;
            return n;
        }
    }
    return 0;
}

static PC_U32 PcCat(char *out, PC_U32 n, PC_U32 cap, const char *s)
{
    while (*s && n + 1 < cap)
        out[n++] = *s++;
    out[n] = 0;
    return n;
}

PC_U32 PcIdDescribe(const char *id, PC_U32 len, char *out, PC_U32 cap)
{
    char mfg[64], mdl[64];
    PC_U32 n;

    if (cap == 0)
        return 0;
    out[0] = 0;
    n = PcIdValue(id, len, PC_KEY_DES, out, cap);
    if (n)
        return n;
    PcIdValue(id, len, PC_KEY_MFG, mfg, sizeof(mfg));
    PcIdValue(id, len, PC_KEY_MDL, mdl, sizeof(mdl));
    if (mfg[0] == 0 && mdl[0] == 0)
        return PcCat(out, 0, cap, "USB printer");
    n = PcCat(out, 0, cap, mfg);
    if (mfg[0] && mdl[0])
        n = PcCat(out, n, cap, " ");
    return PcCat(out, n, cap, mdl);
}

/* ---- port names --------------------------------------------------------- */

int PcPortName(PC_U32 n, char *out)
{
    if (n < 1 || n > PC_PORT_MAX)
        return 0;
    out[0] = 'U';
    out[1] = 'S';
    out[2] = 'B';
    out[3] = (char)('0' + n / 100);
    out[4] = (char)('0' + (n / 10) % 10);
    out[5] = (char)('0' + n % 10);
    out[6] = 0;
    return 1;
}

PC_U32 PcParsePortName(const char *s, PC_U32 len)
{
    PC_U32 n, i;

    if (s == 0)
        return 0;
    if (len == 7 && s[6] == ':')
        len = 6;
    if (len != 6 || PcUpper(s[0]) != 'U' || PcUpper(s[1]) != 'S' || PcUpper(s[2]) != 'B')
        return 0;
    for (n = 0, i = 3; i < 6; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        n = n * 10 + (PC_U32)(s[i] - '0');
    }
    return (n >= 1 && n <= PC_PORT_MAX) ? n : 0;
}

int PcDevicePath(PC_U32 n, char *out)
{
    static const char head[] = "\\\\.\\USBPRN";
    PC_U32 i;

    if (n < 1 || n > PC_PORT_MAX)
        return 0;
    for (i = 0; head[i]; i++)
        out[i] = head[i];
    if (n >= 10)
        out[i++] = (char)('0' + n / 10);
    out[i++] = (char)('0' + n % 10);
    out[i] = 0;
    return 1;
}

PC_U32 PcLowestFree(const PC_U8 *used)
{
    PC_U32 n;

    for (n = 1; n <= PC_PORT_MAX; n++)
        if (!(used[n >> 3] & (1u << (n & 7))))
            return n;
    return 0;
}

/* ---- writes and status -------------------------------------------------- */

PC_U32 PcChunk(PC_U32 left, PC_U32 max)
{
    if (max == 0)
        max = 4096;
    return left < max ? left : max;
}

int PcStatusError(int valid, PC_U8 status)
{
    if (!valid)
        return PC_ERR_TIMEOUT;
    if (status & PC_ST_PAPER_EMPTY)
        return PC_ERR_PAPER;
    if (!(status & PC_ST_NOT_ERROR))
        return PC_ERR_FAULT;
    if (!(status & PC_ST_SELECT))
        return PC_ERR_OFFLINE;
    return PC_ERR_TIMEOUT;
}
