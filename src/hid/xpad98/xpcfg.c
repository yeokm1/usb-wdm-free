/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * xpad98: walk a USB configuration descriptor (USB 2.0 section 9.6.3-9.6.6)
 * and find the interface(s) this driver drives. Every interface's
 * alternate setting 0 is recorded, because the driver selects them all
 * (design 5.1). The family rules are those of design section 1:
 * ff/5d/01 wired XUSB, ff/5d/81 receiver slot, ff/47/d0 GIP on interface
 * 0; each needs exactly one interrupt IN and one interrupt OUT endpoint,
 * the check Linux xpad_probe makes.
 */
#include "xpcore.h"

#define DT_CONFIG    2
#define DT_INTERFACE 4
#define DT_ENDPOINT  5

static unsigned FamilyOf(const XP_IFACE *f)
{
    if (f->Class != 0xFF)
        return XP_FAM_NONE;
    if (f->SubClass == 0x5D && f->Protocol == 0x01)
        return XP_FAM_X360;
    if (f->SubClass == 0x5D && f->Protocol == 0x81)
        return XP_FAM_X360W;
    if (f->SubClass == 0x47 && f->Protocol == 0xD0 && f->Number == 0)
        return XP_FAM_GIP;
    return XP_FAM_NONE;
}

static int PairOf(const XP_IFACE *f, XU8 *in, XU8 *out)
{
    unsigned i;
    *in = *out = 0;
    if (f->NumEndpoints != 2)
        return 0;
    for (i = 0; i < 2; i++) {
        if ((f->EpAttr[i] & 3) != 3)    /* interrupt */
            return 0;
        if (f->EpAddr[i] & 0x80)
            *in = f->EpAddr[i];
        else
            *out = f->EpAddr[i];
    }
    return *in && *out;
}

unsigned XpSelectSet(const XP_CONFIG *k, int drivenOnly, XU8 *idx, unsigned *endpoints)
{
    unsigned i, c, n = 0, eps = 0;
    for (i = 0; i < k->NumInterfaces; i++) {
        int driven = 0;
        for (c = 0; c < k->NumChannels; c++)
            if (k->ChanIface[c] == i)
                driven = 1;
        if (drivenOnly && !driven)
            continue;
        idx[n++] = (XU8)i;
        eps += k->Iface[i].NumEndpoints;
    }
    *endpoints = eps;
    return n;
}

int XpParseConfig(const XU8 *cfg, unsigned len, XP_CONFIG *out)
{
    unsigned pos, total, i;
    XU8 declared[XP_MAX_IF];
    XP_IFACE *cur = 0;
    unsigned curIndex = 0;
    int skipping = 0;       /* inside a non-zero alternate setting */
    unsigned fam = XP_FAM_NONE;

    out->NumInterfaces = 0;
    out->NumChannels = 0;
    out->Family = XP_FAM_NONE;
    out->AllVendor = 1;
    out->ConfigValue = 0;
    if (len < 9 || cfg[0] < 9 || cfg[1] != DT_CONFIG)
        return XP_CFG_EBAD;
    total = cfg[2] | ((unsigned)cfg[3] << 8);
    if (total < 9 || total > len)
        return XP_CFG_EBAD;
    out->ConfigValue = cfg[5];

    for (pos = cfg[0]; pos < total; pos += cfg[pos]) {
        const XU8 *d = cfg + pos;
        if (total - pos < 2 || d[0] < 2 || d[0] > total - pos)
            return XP_CFG_EBAD;
        if (d[1] == DT_INTERFACE) {
            if (d[0] < 9)
                return XP_CFG_EBAD;
            if (cur && cur->NumEndpoints != declared[curIndex])
                return XP_CFG_EBAD;
            cur = 0;
            if (d[3] != 0) {        /* another alternate setting: skipped */
                skipping = 1;
                continue;
            }
            skipping = 0;
            for (i = 0; i < out->NumInterfaces; i++)
                if (out->Iface[i].Number == d[2])
                    return XP_CFG_EBAD;     /* alternate 0 twice */
            if (out->NumInterfaces >= XP_MAX_IF || d[4] > XP_MAX_EP)
                return XP_CFG_ETOOMANY;
            curIndex = out->NumInterfaces++;
            cur = &out->Iface[curIndex];
            declared[curIndex] = d[4];
            cur->Number = d[2];
            cur->NumEndpoints = 0;
            cur->Class = d[5];
            cur->SubClass = d[6];
            cur->Protocol = d[7];
            if (cur->Class != 0xFF)
                out->AllVendor = 0;
        } else if (d[1] == DT_ENDPOINT) {
            if (d[0] < 7)
                return XP_CFG_EBAD;
            if (skipping)
                continue;
            if (!cur || cur->NumEndpoints >= declared[curIndex])
                return XP_CFG_EBAD;
            i = cur->NumEndpoints++;
            cur->EpAddr[i] = d[2];
            cur->EpAttr[i] = d[3];
            cur->EpMaxPacket[i] = (XU16)(d[4] | ((XU16)d[5] << 8));
        }
        /* class-specific and other descriptors are skipped */
    }
    if (pos != total || out->NumInterfaces == 0)
        return XP_CFG_EBAD;
    if (cur && cur->NumEndpoints != declared[curIndex])
        return XP_CFG_EBAD;

    /* The family: the first driven interface decides; the receiver
     * collects every slot, the others exactly one interface. */
    for (i = 0; i < out->NumInterfaces; i++) {
        unsigned f = FamilyOf(&out->Iface[i]);
        XU8 in, o;
        if (f == XP_FAM_NONE || !PairOf(&out->Iface[i], &in, &o))
            continue;
        if (fam == XP_FAM_NONE)
            fam = f;
        if (f != fam)
            continue;
        if (out->NumChannels >= XP_MAX_CHAN)
            break;
        out->ChanIface[out->NumChannels] = (XU8)i;
        out->ChanIn[out->NumChannels] = in;
        out->ChanOut[out->NumChannels] = o;
        out->NumChannels++;
        if (fam != XP_FAM_X360W)
            break;
    }
    if (fam == XP_FAM_NONE)
        return XP_CFG_ENOPAD;
    out->Family = fam;
    return 0;
}
