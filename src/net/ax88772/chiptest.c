/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests of the AX772F chip core (ax7chip.c, ethernet.md section 6.8)
 * against a model of the AX88772B's vendor requests, PHY and receive
 * stream written from the same Linux facts (asix_devices.c,
 * asix_common.c). The model is this file's, not the chip's firmware: a
 * misreading of Linux that the core and the model share passes here.
 * Host-test code only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ax7chip.h"

static unsigned long assertions, failures;
static void Check(int ok, const char *what, int line)
{
    assertions++;
    if (!ok) {
        failures++;
        printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x) != 0, #x, __LINE__)

/* ---- the model ---------------------------------------------------------- */

typedef struct MODEL {
    int Latched;                 /* BMSR's link bit reads clear once (a drop since the last read) */
    nc_u8 Code;                  /* STATMNGSTS: chip code | host enable */
    nc_u8 PhyAddr;
    int SwMii, HostEnableAfter, HostEnableNever;
    nc_u16 Eeprom[8];
    nc_u8 NodeId[6];
    nc_u16 RxCtl, Medium, Gpio, SwReset, PhySelect, Ipg0, Ipg2, QctValue, QctIndex;
    nc_u8 Multi[8];
    nc_u16 Phy[32];
    int ResetPolls;              /* reads of BMCR before RESET clears */
    int PhyResets;               /* BMCR resets written */
    nc_u16 Led, LedIndex;
    int Link;                    /* the cable */
    nc_u16 Lpa;
    unsigned long Requests, Sleeps, RefuseAfter;
    nc_u8 Log[4096];             /* bRequest of each request, in order */
    unsigned LogCount;
} MODEL;

static void ModelInit(MODEL *m)
{
    memset(m, 0, sizeof(*m));
    m->Code = AX_AX88772B_CHIPCODE;
    m->PhyAddr = 0x10;
    m->Eeprom[4] = 0x0e00; m->Eeprom[5] = 0x77c6; m->Eeprom[6] = 0x0302;   /* 00:0e:c6:77:02:03 */
    m->NodeId[0] = 0x00; m->NodeId[1] = 0x0e; m->NodeId[2] = 0xc6;
    m->NodeId[3] = 0x77; m->NodeId[4] = 0x09; m->NodeId[5] = 0x09;
    m->Phy[2] = 0x003b; m->Phy[3] = 0x1881;
    m->Phy[MII_BMCR] = BMCR_ANENABLE;
    m->Phy[MII_BMSR] = 0x7809;
    m->Lpa = ADVERTISE_100FULL | ADVERTISE_100HALF | ADVERTISE_10FULL | ADVERTISE_10HALF |
             ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP;
    m->Link = 1;
}

static nc_u16 PhyRead(MODEL *m, nc_u16 reg)
{
    if (reg == MII_BMCR && (m->Phy[MII_BMCR] & BMCR_RESET)) {
        if (m->ResetPolls-- <= 0) m->Phy[MII_BMCR] &= (nc_u16)~BMCR_RESET;
        return m->Phy[MII_BMCR];
    }
    if (reg == MII_BMSR) {
        nc_u16 v = (nc_u16)(m->Phy[MII_BMSR] & ~(BMSR_LSTATUS | BMSR_ANEGCOMPLETE));
        if (m->Link && !(m->Phy[MII_BMCR] & BMCR_PDOWN)) v = (nc_u16)(v | BMSR_LSTATUS | BMSR_ANEGCOMPLETE);
        if (m->Latched) { m->Latched = 0; v = (nc_u16)(v & ~BMSR_LSTATUS); }
        return v;
    }
    if (reg == MII_LPA) return (nc_u16)(m->Link ? m->Lpa : 0);
    return m->Phy[reg & 31];
}

static void PhyWrite(MODEL *m, nc_u16 reg, nc_u16 v)
{
    if (reg == MII_BMCR && (v & BMCR_RESET)) {
        m->PhyResets++;
        m->Phy[MII_BMCR] = (nc_u16)(BMCR_RESET | BMCR_ANENABLE);
        m->Phy[MII_ADVERTISE] = 0x01e1;
        return;
    }
    m->Phy[reg & 31] = (nc_u16)(v & ~BMCR_ANRESTART);
}

static long Control(void *context, int in, nc_u8 request, nc_u16 value, nc_u16 index,
                    void *data, nc_u16 length, nc_u16 *actual)
{
    MODEL *m = context;
    nc_u8 *b = data;
    *actual = 0;
    m->Requests++;
    if (m->LogCount < sizeof(m->Log)) m->Log[m->LogCount++] = request;
    if (m->RefuseAfter && m->Requests > m->RefuseAfter) return NC_E_IO;
    if (in) {
        switch (request) {
        case AX_CMD_STATMNGSTS_REG:
            if (length != 1) return NC_E_IO;
            b[0] = m->Code;
            if (m->SwMii && !m->HostEnableNever) {
                if (m->HostEnableAfter > 0) m->HostEnableAfter--;
                else b[0] |= AX_HOST_EN;
            }
            break;
        case AX_CMD_READ_PHY_ID:
            if (length != 2) return NC_E_IO;
            b[0] = 0xe0; b[1] = m->PhyAddr;
            break;
        case AX_CMD_READ_EEPROM:
            if (length != 2 || value >= 8) return NC_E_IO;
            b[0] = (nc_u8)m->Eeprom[value]; b[1] = (nc_u8)(m->Eeprom[value] >> 8);
            break;
        case AX_CMD_READ_NODE_ID:
            if (length != 6) return NC_E_IO;
            memcpy(b, m->NodeId, 6);
            break;
        case AX_CMD_READ_MII_REG: {
            nc_u16 v;
            if (length != 2 || !m->SwMii || value != m->PhyAddr || index > 31) return NC_E_IO;
            v = PhyRead(m, index);
            b[0] = (nc_u8)v; b[1] = (nc_u8)(v >> 8);
            break;
        }
        case AX_CMD_READ_RX_CTL:
            if (length != 2) return NC_E_IO;
            b[0] = (nc_u8)m->RxCtl; b[1] = (nc_u8)(m->RxCtl >> 8);
            break;
        case AX_CMD_READ_MEDIUM_STATUS:
            if (length != 2) return NC_E_IO;
            b[0] = (nc_u8)m->Medium; b[1] = (nc_u8)(m->Medium >> 8);
            break;
        default:
            return NC_E_IO;
        }
        *actual = length;
        return NC_OK;
    }
    switch (request) {
    case AX_CMD_SET_SW_MII: m->SwMii = 1; break;
    case AX_CMD_SET_HW_MII: m->SwMii = 0; break;
    case AX_CMD_WRITE_MII_REG:
        if (length != 2 || !m->SwMii || value != m->PhyAddr || index > 31) return NC_E_IO;
        PhyWrite(m, index, (nc_u16)(b[0] | (b[1] << 8)));
        break;
    case AX_CMD_WRITE_RX_CTL: m->RxCtl = value; break;
    case AX_CMD_WRITE_MEDIUM_MODE: m->Medium = value; break;
    case AX_CMD_WRITE_GPIOS: m->Gpio = value; break;
    case AX_CMD_SW_RESET: m->SwReset = value; break;
    case AX_CMD_SW_PHY_SELECT: m->PhySelect = value; break;
    case AX_CMD_WRITE_IPG0: m->Ipg0 = value; m->Ipg2 = index; break;
    case AX_QCTCTRL: m->QctValue = value; m->QctIndex = index; break;
    case S98_LEDS_REQ: m->Led = value; m->LedIndex = index; break;
    case AX_CMD_WRITE_NODE_ID:
        if (length != 6) return NC_E_IO;
        memcpy(m->NodeId, b, 6);
        break;
    case AX_CMD_WRITE_MULTI_FILTER:
        if (length != 8) return NC_E_IO;
        memcpy(m->Multi, b, 8);
        break;
    default:
        return NC_E_IO;
    }
    *actual = length;
    return NC_OK;
}

static void Sleep(void *context, nc_u32 ms)
{
    MODEL *m = context;
    (void)ms;
    m->Sleeps++;
}

static void IoInit(NET_IO *io, MODEL *m)
{
    io->Context = m;
    io->Control = Control;
    io->Sleep = Sleep;
    io->Status = NC_OK;
    io->Requests = 0;
}

static NET_PROBE Probe(nc_u16 vid, nc_u16 pid)
{
    NET_PROBE p;
    memset(&p, 0, sizeof(p));
    p.Vendor = vid; p.Product = pid; p.Speed = NC_SPEED_HIGH;
    p.In = 0x82; p.Out = 0x03; p.Interrupt = 0x81;
    p.InMax = p.OutMax = 512; p.IntMax = 8;
    return p;
}

static union { double Align; nc_u8 Bytes[NET_CHIP_STATE]; } State;

static long StartWith(MODEL *m, NET_IO *io, nc_u16 vid, nc_u16 pid, NET_PARAMS *params)
{
    NET_PROBE p = Probe(vid, pid);
    IoInit(io, m);
    memset(params, 0, sizeof(*params));
    return A7Start(State.Bytes, io, &p, params);
}

static int Find(const MODEL *m, nc_u8 request)
{
    unsigned i;
    for (i = 0; i < m->LogCount; i++) if (m->Log[i] == request) return (int)i;
    return -1;
}

/* ---- descriptors -------------------------------------------------------- */

static void TestProbe(void)
{
    static const nc_u8 device[18] = { 18, 1, 0x00, 0x02, 0xff, 0xff, 0, 64,
                                      0x95, 0x0b, 0x2b, 0x77, 0x02, 0x00, 1, 2, 3, 1 };
    nc_u8 config[39] = { 9, 2, 39, 0, 1, 1, 0, 0xa0, 0x32,
                         9, 4, 0, 0, 3, 0xff, 0xff, 0, 0,
                         7, 5, 0x81, 3, 8, 0, 11,
                         7, 5, 0x82, 2, 0x00, 0x02, 0,
                         7, 5, 0x03, 2, 0x00, 0x02, 0 };
    const nc_u8 *configs[1];
    nc_u32 sizes[1];
    NET_PROBE p;
    configs[0] = config; sizes[0] = sizeof(config);
    memset(&p, 0, sizeof(p));
    CHECK(A7Probe(device, 18, configs, sizes, 1, &p) == NC_OK);
    CHECK(p.Vendor == 0x0b95 && p.Product == 0x772b && p.Revision == 0x0002);
    CHECK(p.In == 0x82 && p.Out == 0x03 && p.Interrupt == 0x81 && p.Speed == NC_SPEED_HIGH);
    config[29] = 0x40; config[30] = 0; config[36] = 0x40; config[37] = 0;   /* Full Speed: 64 */
    CHECK(A7Probe(device, 18, configs, sizes, 1, &p) == NC_OK && p.Speed == NC_SPEED_FULL);
    config[29] = 0x00; config[30] = 0x04; config[36] = 0x00; config[37] = 0x04;   /* 1024 */
    CHECK(A7Probe(device, 18, configs, sizes, 1, &p) == NC_E_UNSUPPORTED);
    config[29] = 0x00; config[30] = 0x02; config[36] = 0x00; config[37] = 0x02;
    config[14] = 0x02;                                    /* not a vendor interface */
    CHECK(A7Probe(device, 18, configs, sizes, 1, &p) == NC_E_UNSUPPORTED);
    config[14] = 0xff;
    config[2] = 60;                                       /* wTotalLength beyond the data */
    CHECK(A7Probe(device, 18, configs, sizes, 1, &p) == NC_E_MALFORMED);
}

/* ---- bring-up and link -------------------------------------------------- */

static void TestStart(void)
{
    MODEL m;
    NET_IO io;
    NET_PARAMS params;
    A7_STATE *s = (A7_STATE *)State.Bytes;
    NET_LINK link;

    ModelInit(&m);
    m.ResetPolls = 3;
    m.HostEnableAfter = 2;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_OK);
    CHECK(params.Mac[0] == 0x00 && params.Mac[1] == 0x0e && params.Mac[2] == 0xc6 &&
          params.Mac[3] == 0x77 && params.Mac[4] == 0x02 && params.Mac[5] == 0x03);
    CHECK(memcmp(m.NodeId, params.Mac, 6) == 0);          /* WRITE_NODE_ID in the reset */
    CHECK(params.RxSize == 2048 && params.RxCount == 1 && params.TxHeader == 4 &&
          params.TxBatch == 1 && params.TxSize == 4 + 1514 + 4);
    CHECK(params.Version == (0x20 | (0x10 << 8)));
    CHECK(m.Gpio == AX_GPIO_RSE);
    CHECK(m.PhySelect == (1 | AX_PHYSEL_SSEN));
    CHECK(m.SwReset == AX_SWRESET_IPRL);
    CHECK(m.QctValue == 0x8000 && m.QctIndex == 0x8001);
    CHECK(m.Ipg0 == 0x001d && m.Ipg2 == 0x0012);
    CHECK(m.RxCtl == AX_DEFAULT_RX_CTL && params.RxConfig == 0x0088 && params.RxConfigRead == 0x0088);
    CHECK(m.Medium == AX88772_MEDIUM_DEFAULT && m.Medium == 0x0306);
    CHECK(m.Phy[MII_ADVERTISE] == 0x0de1);
    CHECK(m.Phy[MII_BMCR] == BMCR_ANENABLE);              /* restart bit is self-clearing here */
    CHECK(s->PhyId1 == 0x003b && s->PhyId2 == 0x1881);
    CHECK(!m.SwMii);                                      /* the MII is handed back */
    /* The order: MAC, PHY id, chip code before the reset; GPIO first in it. */
    CHECK(Find(&m, AX_CMD_READ_EEPROM) < Find(&m, AX_CMD_READ_PHY_ID));
    CHECK(Find(&m, AX_CMD_READ_PHY_ID) < Find(&m, AX_CMD_WRITE_GPIOS));
    CHECK(Find(&m, AX_CMD_WRITE_GPIOS) < Find(&m, AX_CMD_SW_PHY_SELECT));
    CHECK(Find(&m, AX_CMD_SW_PHY_SELECT) < Find(&m, AX_CMD_SW_RESET));
    CHECK(Find(&m, AX_QCTCTRL) < Find(&m, AX_CMD_WRITE_IPG0));
    CHECK(Find(&m, AX_CMD_WRITE_IPG0) < Find(&m, AX_CMD_WRITE_NODE_ID));
    CHECK(Find(&m, AX_CMD_WRITE_MEDIUM_MODE) < Find(&m, AX_CMD_READ_MII_REG));

    /* Link: 100 full with symmetric pause. */
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK);
    CHECK(link.Up && link.Mbps == 100 && link.FullDuplex && link.RxLength == 2048);
    CHECK(m.Medium == (AX_MEDIUM_AC | AX_MEDIUM_RE | AX_MEDIUM_FD | AX_MEDIUM_PS |
                       AX_MEDIUM_TFC | AX_MEDIUM_RFC));
    CHECK(A7Poll(State.Bytes, &io) == 0);
    /* Partner with ASM_DIR only: receive pause. */
    m.Lpa = ADVERTISE_100FULL | ADVERTISE_PAUSE_ASYM;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK);
    CHECK(m.Medium == (AX_MEDIUM_AC | AX_MEDIUM_RE | AX_MEDIUM_FD | AX_MEDIUM_PS | AX_MEDIUM_RFC));
    /* 10 half. */
    m.Lpa = ADVERTISE_10HALF | ADVERTISE_PAUSE_CAP;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK);
    CHECK(link.Up && link.Mbps == 10 && !link.FullDuplex);
    CHECK(m.Medium == (AX_MEDIUM_AC | AX_MEDIUM_RE));
    /* No common ability: BMCR's bits. */
    m.Lpa = 0;
    m.Phy[MII_BMCR] = BMCR_ANENABLE | 0x2000 | 0x0100;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK);
    CHECK(link.Up && link.Mbps == 100 && link.FullDuplex);
    m.Phy[MII_BMCR] = BMCR_ANENABLE;
    m.Lpa = ADVERTISE_100HALF;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Mbps == 100 && !link.FullDuplex);
    CHECK(m.Medium == (AX_MEDIUM_AC | AX_MEDIUM_RE | AX_MEDIUM_PS));
    /* Cable out: Poll sees it, Link writes the medium 0; the AX88772C's
     * PHY is not reset for it. */
    m.Link = 0;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && !link.Up && link.Mbps == 0);
    CHECK(m.Medium == 0 && m.PhyResets == 1);
    CHECK(A7Poll(State.Bytes, &io) == 0);
    m.Medium = 0x1234;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && !link.Up);
    CHECK(m.Medium == 0x1234);                            /* written only when it goes down */
    m.Link = 1;
    CHECK(A7Poll(State.Bytes, &io) == 1);

    /* A Link that fails part way leaves the link down in the chip's own
     * state, so Poll asks again even though the PHY still has a link. */
    m.Link = 1;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    CHECK(A7Poll(State.Bytes, &io) == 0);
    m.RefuseAfter = m.Requests + 3;                      /* fails inside the BMSR reads */
    CHECK(A7Link(State.Bytes, &io, &link) == NC_E_IO && !link.Up);
    m.RefuseAfter = 0;
    io.Status = NC_OK;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    CHECK(A7Poll(State.Bytes, &io) == 0);

    /* The link-down work failing part way (the AX88772A/B PHY's reset):
     * Poll asks again until it is done. */
    m.Phy[3] = 0x1861; s->PhyId2 = 0x1861;
    m.Link = 0;
    m.RefuseAfter = m.Requests + 16;                     /* inside the PHY reset */
    CHECK(A7Link(State.Bytes, &io, &link) < 0 && s->DownPending);
    m.RefuseAfter = 0; io.Status = NC_OK;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && !link.Up && !s->DownPending);
    CHECK(m.Phy[MII_ADVERTISE] == 0x0de1 && (m.Phy[MII_BMCR] & BMCR_ANENABLE));
    CHECK(A7Poll(State.Bytes, &io) == 0);
    m.Link = 1;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    m.Phy[3] = 0x1881; s->PhyId2 = 0x1881;

    /* A cable-down Link failing in its first PHY read: still pending, and
     * the next one does the link-down work. */
    m.Link = 0;
    m.RefuseAfter = m.Requests + 2;
    CHECK(A7Link(State.Bytes, &io, &link) < 0 && s->LinkPending && s->LinkUp);
    m.RefuseAfter = 0; io.Status = NC_OK;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    m.Medium = 0x0336;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && !link.Up && m.Medium == 0);
    CHECK(!s->LinkPending && !s->LinkUp && A7Poll(State.Bytes, &io) == 0);
    m.Link = 1;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);

    /* An interrupted PHY reset is finished even when the carrier is back
     * before the retry. */
    m.Phy[3] = 0x1861; s->PhyId2 = 0x1861;
    m.Link = 0;
    m.RefuseAfter = m.Requests + 8;                      /* after BMCR 0, before the reset */
    CHECK(A7Link(State.Bytes, &io, &link) < 0 && s->DownPending);
    CHECK(!(m.Phy[MII_BMCR] & BMCR_ANENABLE));            /* BMCR 0 written, reset not yet */
    m.RefuseAfter = 0; io.Status = NC_OK;
    m.Link = 1;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up && link.Mbps == 100);
    CHECK(!s->DownPending && (m.Phy[MII_BMCR] & BMCR_ANENABLE) && m.Phy[MII_ADVERTISE] == 0x0de1);
    m.Phy[3] = 0x1881; s->PhyId2 = 0x1881;

    /* A drop between two polls, over by the next read: the latched BMSR
     * shows it once, and the AX88772A/B PHY is still reset for it, from
     * Poll's read and from Link's. */
    m.Phy[3] = 0x1861; s->PhyId2 = 0x1861;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    {
        int resets = m.PhyResets;
        m.Latched = 1;
        CHECK(A7Poll(State.Bytes, &io) == 1 && s->DownPending);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && m.PhyResets == resets + 1 && !s->DownPending);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
        m.Latched = 1;                                    /* seen by Link's own first read */
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && m.PhyResets == resets + 2);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up && A7Poll(State.Bytes, &io) == 0);
    }
    /* The same drop when handing the MII back fails just after the read. */
    {
        int resets = m.PhyResets;
        m.Latched = 1;
        m.RefuseAfter = m.Requests + 3;                   /* SET_SW_MII, STATMNGSTS, READ: then refused */
        CHECK(A7Poll(State.Bytes, &io) == 1 && s->DownPending);
        m.RefuseAfter = 0; io.Status = NC_OK;
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && m.PhyResets == resets + 1);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
        m.Latched = 1;
        m.RefuseAfter = m.Requests + 3;
        CHECK(A7Link(State.Bytes, &io, &link) < 0 && s->DownPending);
        m.RefuseAfter = 0; io.Status = NC_OK;
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && m.PhyResets == resets + 2);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up && A7Poll(State.Bytes, &io) == 0);
    }
    m.Phy[3] = 0x1881; s->PhyId2 = 0x1881;

    /* An address write that failed is written again by Link. */
    {
        static const nc_u8 want[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
        m.RefuseAfter = m.Requests;
        CHECK(A7SetMac(State.Bytes, &io, want) < 0 && s->MacPending);
        m.RefuseAfter = 0; io.Status = NC_OK;
        CHECK(memcmp(m.NodeId, want, 6) != 0);
        CHECK(A7Poll(State.Bytes, &io) == 1);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
        CHECK(memcmp(m.NodeId, want, 6) == 0 && !s->MacPending && A7Poll(State.Bytes, &io) == 0);
    }

    /* A filter that failed: Poll asks for Link (and so the filter again)
     * while the link is up. */
    m.RefuseAfter = m.Requests;
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_ALL_MULTICAST, 0, 0) < 0 && s->FilterPending);
    m.RefuseAfter = 0; io.Status = NC_OK;
    CHECK(A7Poll(State.Bytes, &io) == 1);
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_ALL_MULTICAST, 0, 0) == NC_OK && !s->FilterPending);
    CHECK(A7Poll(State.Bytes, &io) == 0);

    /* Stop: medium 0, receive off, PHY powered down. */
    CHECK(A7Stop(State.Bytes, &io) == NC_OK);
    CHECK(m.Medium == 0 && m.RxCtl == 0 && (m.Phy[MII_BMCR] & BMCR_PDOWN));
    /* Suspend clears RE only. */
    m.Medium = 0x0336;
    CHECK(A7Suspend(State.Bytes, &io) == NC_OK && m.Medium == 0x0236);
    CHECK(io.Status == NC_OK);

    /* The AX88772A/B PHY (003b1861) is reset again when the link goes;
     * the AX88772C's (003b1881, above) is not. */
    {
        int resets;
        ModelInit(&m);
        m.Phy[3] = 0x1861;
        CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_OK && m.PhyResets == 1);
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
        m.Link = 0;
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && !link.Up && m.PhyResets == 2);
        CHECK(m.Phy[MII_ADVERTISE] == 0x0de1);
        resets = m.PhyResets;
        CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && m.PhyResets == resets);
    }
    /* No PHY answers its id: Start fails. */
    ModelInit(&m); m.Phy[2] = 0xffff;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_DEVICE);
    ModelInit(&m); m.Phy[2] = 0;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_DEVICE);

    /* D-Link 2001:1A02 reads NODE_ID, not the EEPROM. */
    ModelInit(&m);
    CHECK(StartWith(&m, &io, 0x2001, 0x1a02, &params) == NC_OK);
    CHECK(params.Mac[4] == 0x09 && params.Mac[5] == 0x09);
    CHECK(Find(&m, AX_CMD_READ_EEPROM) < 0);

    /* Refused: AX88772A, AX88772, an external PHY. */
    ModelInit(&m); m.Code = AX_AX88772A_CHIPCODE;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_UNSUPPORTED);
    CHECK(params.Version == 0x1010 && Find(&m, AX_CMD_WRITE_GPIOS) < 0);
    ModelInit(&m); m.Code = AX_AX88772_CHIPCODE;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_UNSUPPORTED);
    ModelInit(&m); m.PhyAddr = 0x03;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_UNSUPPORTED);
    ModelInit(&m); m.PhyAddr = 0x40;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_DEVICE);
    /* The MII never handed over: Start fails. */
    ModelInit(&m); m.HostEnableNever = 1;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_DEVICE);
    /* A PHY whose reset never clears. */
    ModelInit(&m); m.ResetPolls = 1000;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_DEVICE);
    /* A request failing part way. */
    ModelInit(&m); m.RefuseAfter = 7;
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_E_IO);
    CHECK(m.Requests == 8);                               /* nothing issued after the failure */
    /* Resume: three tries of the bring-up when it fails, one when the chip
     * is refused, and a pass. */
    {
        NET_PROBE p = Probe(0x0b95, 0x772b);
        unsigned long before;
        ModelInit(&m); m.HostEnableNever = 1;
        IoInit(&io, &m);
        CHECK(A7Resume(State.Bytes, &io, &p, &params) == NC_E_DEVICE);
        before = m.Requests;
        ModelInit(&m); m.Code = AX_AX88772A_CHIPCODE;
        IoInit(&io, &m);
        CHECK(A7Resume(State.Bytes, &io, &p, &params) == NC_E_UNSUPPORTED);
        CHECK(m.Requests == 5);                           /* one try: MAC, PHY id, code */
        CHECK(before > 3 * 5);
        ModelInit(&m);
        IoInit(&io, &m);
        io.Status = NC_E_IO;                              /* left over from before the sleep */
        CHECK(A7Resume(State.Bytes, &io, &p, &params) == NC_OK && io.Status == NC_OK);
    }
}

/* The SR9800: its bring-up, link medium, filter base and pad rule. */
static void TestSr9800(void)
{
    MODEL m;
    NET_IO io;
    NET_PARAMS params;
    NET_LINK link;
    NET_PROBE p;
    A7_STATE *s = (A7_STATE *)State.Bytes;
    nc_u8 buffer[4 + 1514 + 4];
    nc_u32 total;

    ModelInit(&m);
    m.Code = 0;                                           /* the SR9800 path never asks */
    m.HostEnableNever = 1;                                /* nor waits for host enable */
    CHECK(StartWith(&m, &io, 0x0fe6, 0x9800, &params) == NC_OK);
    CHECK(s->Kind == A7_KIND_SR9800 && Find(&m, AX_CMD_STATMNGSTS_REG) < 0);
    CHECK(m.Led == 0x4080 && m.LedIndex == 0x4001);
    CHECK(Find(&m, S98_LEDS_REQ) == 0 && Find(&m, AX_CMD_READ_NODE_ID) == 1);
    CHECK(Find(&m, AX_CMD_READ_EEPROM) < 0);
    CHECK(params.Mac[4] == 0x09 && params.Mac[5] == 0x09);
    CHECK(memcmp(m.NodeId, params.Mac, 6) == 0);
    CHECK(m.QctValue == 0x8100 && m.QctIndex == 0x8147);  /* High Speed: 4 KiB */
    CHECK(params.RxSize == 4096 && params.RxCount == 1);
    CHECK(m.Gpio == S98_GPIO_AT_RESET && m.SwReset == (AX_SWRESET_IPRL | S98_RESET_PHYLINE));
    CHECK(m.RxCtl == S98_RX_MODE && m.Medium == S98_MEDIUM_MODE);
    CHECK(m.Ipg0 == 0x001d && m.Ipg2 == 0x0012);
    CHECK(m.Phy[MII_ADVERTISE] == 0x01e1);
    CHECK(params.Version == (0x9800UL | (0x10UL << 16)));
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up && link.Mbps == 100 && link.FullDuplex);
    CHECK(m.Medium == S98_MEDIUM_MODE && link.RxLength == 4096);
    m.Lpa = ADVERTISE_10HALF;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Mbps == 10 && !link.FullDuplex);
    CHECK(m.Medium == (S98_MEDIUM_MODE & ~(AX_MEDIUM_PS | AX_MEDIUM_FD)));
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_PROMISCUOUS, 0, 0) == NC_OK);
    CHECK(m.RxCtl == (S98_RX_MODE | AX_RX_CTL_PRO));
    /* sr_tx_fixup: the pad word at a multiple of maxpacket - 1. */
    total = 4 + 507;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 515);
    total = 4 + 508;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 512);
    /* Full Speed: the 2 KiB burst. */
    ModelInit(&m);
    p = Probe(0x0fe6, 0x9800);
    p.Speed = NC_SPEED_FULL;
    IoInit(&io, &m);
    CHECK(A7Start(State.Bytes, &io, &p, &params) == NC_OK);
    CHECK(m.QctValue == 0x8000 && m.QctIndex == 0x8001 && params.RxSize == 2048);
    /* The AX88772B keeps its own pad rule. */
    ModelInit(&m);
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_OK && s->Kind == A7_KIND_AX772B);
    total = 4 + 508;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 516);
}

static void TestFilter(void)
{
    MODEL m;
    NET_IO io;
    NET_PARAMS params;
    nc_u8 mc[65 * 6];
    nc_u32 i, bit;
    ModelInit(&m);
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_OK);
    for (i = 0; i < 65; i++) {
        mc[6 * i] = 0x01; mc[6 * i + 1] = 0x00; mc[6 * i + 2] = 0x5e;
        mc[6 * i + 3] = 0x00; mc[6 * i + 4] = 0x00; mc[6 * i + 5] = (nc_u8)i;
    }
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_DIRECTED | NC_FILTER_BROADCAST, 0, 0) == NC_OK);
    CHECK(m.RxCtl == 0x0088);
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_PROMISCUOUS, 0, 0) == NC_OK);
    CHECK(m.RxCtl == (0x0088 | AX_RX_CTL_PRO));
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_ALL_MULTICAST, 0, 0) == NC_OK);
    CHECK(m.RxCtl == (0x0088 | AX_RX_CTL_AMALL));
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_MULTICAST, mc, 65) == NC_OK);
    CHECK(m.RxCtl == (0x0088 | AX_RX_CTL_AMALL));
    memset(m.Multi, 0, 8);
    CHECK(A7SetFilter(State.Bytes, &io, NC_FILTER_MULTICAST, mc, 1) == NC_OK);
    CHECK(m.RxCtl == (0x0088 | AX_RX_CTL_AM));
    bit = NcEtherCrc(mc, 6) >> 26;
    CHECK(m.Multi[bit >> 3] == (nc_u8)(1 << (bit & 7)));
    for (i = 0; i < 8; i++) if (i != (bit >> 3)) CHECK(m.Multi[i] == 0);
}

/* ---- transmit ----------------------------------------------------------- */

static void TestTx(void)
{
    nc_u8 buffer[4 + 1514 + 4];
    nc_u32 total;
    A7TxHeader(State.Bytes, buffer, 60, 512, 1);
    CHECK(buffer[0] == 60 && buffer[1] == 0 && buffer[2] == (nc_u8)~60 && buffer[3] == 0xff);
    A7TxHeader(State.Bytes, buffer, 1514, 512, 1);
    CHECK(buffer[0] == 0xea && buffer[1] == 0x05 && buffer[2] == 0x15 && buffer[3] == 0xfa);
    total = 4 + 1000;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 1004);
    total = 4 + 508;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 516);
    CHECK(buffer[512] == 0 && buffer[513] == 0 && buffer[514] == 0xff && buffer[515] == 0xff);
    total = 4 + 1020;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 512) == 0 && total == 1028);
    total = 4 + 60;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 64) == 0 && total == 68);
    total = 4 + 124;
    CHECK(A7TxEnd(State.Bytes, buffer, &total, sizeof(buffer), 64) == 0 && total == 132);
}

/* ---- receive ------------------------------------------------------------ */

#define MAXF 64
static struct { nc_u32 Count; nc_u32 Length[MAXF]; nc_u8 Data[MAXF][1514]; } Got;

static void Emit(void *context, const nc_u8 *frame, nc_u32 length)
{
    (void)context;
    if (Got.Count < MAXF && length <= 1514) {
        Got.Length[Got.Count] = length;
        memcpy(Got.Data[Got.Count], frame, length);
    }
    Got.Count++;
}

static nc_u32 Seed = 12345;
static nc_u32 Rand(void) { Seed = Seed * 1103515245UL + 12345UL; return (Seed >> 16) & 0x7fff; }

/* Append a record: header, frame, pad to even. */
static nc_u32 Record(nc_u8 *out, nc_u32 at, nc_u32 size, nc_u8 tag)
{
    nc_u32 word = ((size ^ 0xffffUL) << 16) + size, i;
    out[at] = (nc_u8)word; out[at + 1] = (nc_u8)(word >> 8);
    out[at + 2] = (nc_u8)(word >> 16); out[at + 3] = (nc_u8)(word >> 24);
    for (i = 0; i < size; i++) out[at + 4 + i] = (nc_u8)(tag + i * 7);
    at += 4 + size;
    if (size & 1) out[at++] = 0xee;
    return at;
}

static int Frame(nc_u32 index, nc_u32 size, nc_u8 tag)
{
    nc_u32 i;
    if (index >= MAXF || Got.Length[index] != size) return 0;
    for (i = 0; i < size; i++) if (Got.Data[index][i] != (nc_u8)(tag + i * 7)) return 0;
    return 1;
}

static void Reset(void)
{
    memset(State.Bytes, 0, sizeof(State.Bytes));
    memset(&Got, 0, sizeof(Got));
}

static long Parse(const nc_u8 *data, nc_u32 length, NET_RX_STATS *st)
{
    return A7RxParse(State.Bytes, data, length, Emit, 0, st);
}

static void TestRxBasic(void)
{
    static nc_u8 b[8192];
    NET_RX_STATS st;
    nc_u32 n;
    A7_STATE *s = (A7_STATE *)State.Bytes;

    Reset();
    n = Record(b, 0, 60, 1);
    n = Record(b, n, 1514, 2);
    n = Record(b, n, 61, 3);
    CHECK(Parse(b, n, &st) == 3 && st.Frames == 3 && st.Dropped == 0);
    CHECK(Got.Count == 3 && Frame(0, 60, 1) && Frame(1, 1514, 2) && Frame(2, 61, 3));

    /* Bad complement: malformed, nothing emitted, the stream reset. */
    Reset();
    n = Record(b, 0, 60, 1);
    n = Record(b, n, 70, 2);
    b[64 + 2] ^= 1;
    CHECK(Parse(b, n, &st) == NC_E_MALFORMED && Got.Count == 0 && st.Frames == 0);
    CHECK(s->Malformed == 1);

    /* Longer than 1518: malformed. */
    Reset();
    n = Record(b, 0, 1519, 1);
    CHECK(Parse(b, n, &st) == NC_E_MALFORMED && Got.Count == 0);
    /* 1515 to 1518 and 1 to 13: consumed, dropped, the stream in step. */
    Reset();
    n = Record(b, 0, 1518, 1);
    n = Record(b, n, 13, 2);
    n = Record(b, n, 1515, 3);
    n = Record(b, n, 14, 4);
    n = Record(b, n, 0, 5);
    n = Record(b, n, 100, 6);
    CHECK(Parse(b, n, &st) == 2 && st.Dropped == 4 && Got.Count == 2);
    CHECK(Frame(0, 14, 4) && Frame(1, 100, 6));
    /* An odd tail: malformed. */
    Reset();
    n = Record(b, 0, 60, 1);
    CHECK(Parse(b, n + 1, &st) == NC_E_MALFORMED && Got.Count == 0);
    CHECK(Parse(b, n + 3, &st) == NC_E_MALFORMED);
    /* An empty transfer and a transfer of only a pad word (length 0). */
    Reset();
    CHECK(Parse(b, 0, &st) == 0);
    n = Record(b, 0, 0, 0);
    CHECK(Parse(b, n, &st) == 0 && st.Dropped == 1 && s->Rx.Remaining == 0);
}

/* A stream of records cut into transfers at the given even offsets must
 * deliver exactly the records, whatever the cuts. */
static void Stream(const nc_u32 *sizes, nc_u32 count, const nc_u32 *cuts, nc_u32 ncuts)
{
    static nc_u8 b[16384];
    NET_RX_STATS st;
    nc_u32 n = 0, i, at = 0, kept = 0, k = 0;
    int ok = 1;
    Reset();
    for (i = 0; i < count; i++) n = Record(b, n, sizes[i], (nc_u8)(i * 13));
    for (i = 0; i <= ncuts; i++) {
        nc_u32 end = i < ncuts ? cuts[i] : n;
        if (end < at || end > n) { ok = 0; break; }
        if (Parse(b + at, end - at, &st) < 0) { ok = 0; break; }
        at = end;
    }
    CHECK(ok);
    for (i = 0; i < count; i++) {
        if (sizes[i] < 14 || sizes[i] > 1514) continue;
        if (!Frame(k, sizes[i], (nc_u8)(i * 13))) ok = 0;
        k++;
        kept++;
    }
    CHECK(ok && Got.Count == kept);
}

static void TestRxSpan(void)
{
    static const nc_u32 sizes[] = { 60, 1514, 61, 1515, 342, 13, 1000, 0, 99, 1517, 64 };
    nc_u32 total = 0, i, a, b, cuts[3];
    for (i = 0; i < 11; i++) total += 4 + sizes[i] + (sizes[i] & 1);
    /* One cut at every even offset: covers a header split 2 + 2, a cut
     * just before and after a header, and inside every kind of record. */
    for (a = 0; a <= total; a += 2) { cuts[0] = a; Stream(sizes, 11, cuts, 1); }
    /* Two cuts: a frame carried across three transfers. */
    for (a = 0; a <= total; a += 26)
        for (b = a; b <= total; b += 34) { cuts[0] = a; cuts[1] = b; Stream(sizes, 11, cuts, 2); }
    /* 2048-byte transfers, as the chip's bursts. */
    cuts[0] = 2048; cuts[1] = 4096; cuts[2] = 6144;
    Stream(sizes, 11, cuts, total > 6144 ? 3 : (total > 4096 ? 2 : 1));
    /* Random streams, random even cuts. */
    for (i = 0; i < 4000; i++) {
        nc_u32 sz[8], j, len = 0, c[3], m;
        for (j = 0; j < 8; j++) { sz[j] = Rand() % 1530; if (sz[j] > 1518) sz[j] = 1514; len += 4 + sz[j] + (sz[j] & 1); }
        for (j = 0; j < 3; j++) c[j] = (Rand() % (len / 2 + 1)) * 2;
        if (c[0] > c[1]) { m = c[0]; c[0] = c[1]; c[1] = m; }
        if (c[1] > c[2]) { m = c[1]; c[1] = c[2]; c[2] = m; }
        if (c[0] > c[1]) { m = c[0]; c[0] = c[1]; c[1] = m; }
        Stream(sz, 8, c, 3);
    }
}

static void TestRxSync(void)
{
    static nc_u8 b[8192];
    NET_RX_STATS st;
    nc_u32 n, cut;
    A7_STATE *s = (A7_STATE *)State.Bytes;

    /* A frame is carried; the next transfer does not continue it (data
     * lost on the way): the header where its end should be does not
     * agree, so the carried frame is dropped and the transfer parsed
     * from its first byte. */
    Reset();
    n = Record(b, 0, 1000, 1);
    cut = 500;
    CHECK(Parse(b, cut, &st) == 0 && s->Rx.Remaining == 1000 - (500 - 4));
    n = Record(b, 0, 60, 2);
    n = Record(b, n, 600, 3);
    CHECK(Parse(b, n, &st) == 2 && st.Dropped == 1 && Got.Count == 2);
    CHECK(Frame(0, 60, 2) && Frame(1, 600, 3) && s->Resyncs == 1);

    /* The rest of the frame comes, but too short to hold a header after
     * it: taken as the frame's end (Linux the same). */
    Reset();
    n = Record(b, 0, 100, 4);
    CHECK(Parse(b, 50, &st) == 0);
    CHECK(Parse(b + 50, n - 50, &st) == 1 && Frame(0, 100, 4));

    /* A malformed transfer clears a carried frame. */
    Reset();
    n = Record(b, 0, 1000, 5);
    CHECK(Parse(b, 500, &st) == 0 && s->Rx.Remaining);
    CHECK(Parse(b + 500, 501, &st) == NC_E_MALFORMED);  /* odd */
    CHECK(s->Rx.Remaining == 0 && !s->Rx.Split && Got.Count == 0);

    /* A trailing half header, then a transfer of exactly its other half. */
    Reset();
    n = Record(b, 0, 60, 6);
    n = Record(b, n, 70, 7);
    CHECK(Parse(b, 66, &st) == 1 && s->Rx.Split);
    CHECK(Parse(b + 66, 2, &st) == 0 && !s->Rx.Split && s->Rx.Remaining == 70);
    CHECK(Parse(b + 68, n - 68, &st) == 1 && Frame(1, 70, 7));

    /* RxReset drops what is carried. */
    Reset();
    n = Record(b, 0, 1000, 8);
    CHECK(Parse(b, 300, &st) == 0 && s->Rx.Remaining);
    A7RxReset(State.Bytes);
    CHECK(s->Rx.Remaining == 0 && s->Resyncs == 1);
    n = Record(b, 0, 80, 9);
    CHECK(Parse(b, n, &st) == 1 && Frame(0, 80, 9));

    /* A transfer that starts inside a record (no carry): malformed, and
     * the stream recovers at the next one that starts on a record. */
    Reset();
    n = Record(b, 0, 1000, 10);
    CHECK(Parse(b + 200, n - 200, &st) == NC_E_MALFORMED && Got.Count == 0);
    n = Record(b, 0, 90, 11);
    CHECK(Parse(b, n, &st) == 1 && Frame(0, 90, 11));
}

/* Transfers that keep starting inside records: after A7_REALIGN_AFTER
 * malformed ones Poll asks for Link, which turns receive off and on and
 * restarts the stream. */
static void TestRealign(void)
{
    static nc_u8 b[4096];
    MODEL m;
    NET_IO io;
    NET_PARAMS params;
    NET_LINK link;
    NET_RX_STATS st;
    A7_STATE *s = (A7_STATE *)State.Bytes;
    nc_u32 n, i;
    unsigned log;

    ModelInit(&m);
    CHECK(StartWith(&m, &io, 0x0b95, 0x772b, &params) == NC_OK);
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    memset(&Got, 0, sizeof(Got));
    n = Record(b, 0, 1000, 1);
    for (i = 0; i < A7_REALIGN_AFTER; i++) {
        CHECK(A7Poll(State.Bytes, &io) == 0);
        CHECK(Parse(b + 200, n - 200, &st) == NC_E_MALFORMED);
    }
    CHECK(s->BadRun == A7_REALIGN_AFTER);
    CHECK(A7Poll(State.Bytes, &io) == 1);
    log = m.LogCount;
    m.RxCtl = 0x1234;
    CHECK(A7Link(State.Bytes, &io, &link) == NC_OK && link.Up);
    CHECK(s->BadRun == 0 && s->Realigns == 1 && s->Rx.Remaining == 0);
    CHECK(m.RxCtl == AX_DEFAULT_RX_CTL);                  /* off, then on with the filter's value */
    for (i = log; i < m.LogCount && m.Log[i] != AX_CMD_WRITE_RX_CTL; i++) { }
    CHECK(i < m.LogCount && i + 1 < m.LogCount && m.Log[i + 1] == AX_CMD_WRITE_RX_CTL);
    CHECK(A7Poll(State.Bytes, &io) == 0);
    /* One good transfer ends a run. */
    CHECK(Parse(b + 200, n - 200, &st) == NC_E_MALFORMED && s->BadRun == 1);
    n = Record(b, 0, 80, 2);
    CHECK(Parse(b, n, &st) == 1 && s->BadRun == 0);
}

static void TestStatus(void)
{
    nc_u8 r[8] = { 0xa1, 0, 1, 0, 0, 0, 0, 0 };
    CHECK(A7Status(State.Bytes, r, 8) == 1);
    r[2] = 0;
    CHECK(A7Status(State.Bytes, r, 8) == 0);
    CHECK(A7Status(State.Bytes, r, 7) == -1);
}

int main(void)
{
    CHECK(sizeof(A7_STATE) <= NET_CHIP_STATE);
    CHECK(NetPackageChip.RxReset == A7RxReset);
    TestProbe();
    TestStart();
    TestFilter();
    TestSr9800();
    TestTx();
    TestRxBasic();
    TestRxSpan();
    TestRxSync();
    TestRealign();
    TestStatus();
    printf("ax772f chiptest: %lu checks, %lu failed\n", assertions, failures);
    return failures ? 1 : 0;
}
