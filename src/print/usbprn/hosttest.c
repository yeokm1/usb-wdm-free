/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hosttest.c - host tests of prncore.c (usbprn's pure parts): alternate
 * choice, 1284 id parsing, port names, write chunking, status mapping.
 * Built and run by hosttest.ps1 with the package's own compiler.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <stdio.h>
#include <string.h>
#include "prncore.h"

static int g_Checks, g_Fails;

#define CHECK(c) do { g_Checks++; if (!(c)) { g_Fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- descriptor builder ---------------------------------------------------- */

static PC_U8 g_Cfg[512];
static PC_U32 g_Len;

static void CfgBegin(void)
{
    memset(g_Cfg, 0, sizeof(g_Cfg));
    g_Cfg[0] = 9; g_Cfg[1] = 2; g_Cfg[4] = 1; g_Cfg[5] = 1; g_Cfg[7] = 0xC0;
    g_Len = 9;
}

static void CfgIf(PC_U8 num, PC_U8 alt, PC_U8 neps, PC_U8 cls, PC_U8 sub, PC_U8 proto)
{
    PC_U8 *p = g_Cfg + g_Len;
    p[0] = 9; p[1] = 4; p[2] = num; p[3] = alt; p[4] = neps;
    p[5] = cls; p[6] = sub; p[7] = proto; p[8] = 0;
    g_Len += 9;
}

static void CfgEp(PC_U8 addr, PC_U8 attr, PC_U16 mps)
{
    PC_U8 *p = g_Cfg + g_Len;
    p[0] = 7; p[1] = 5; p[2] = addr; p[3] = attr;
    p[4] = (PC_U8)(mps & 0xFF); p[5] = (PC_U8)(mps >> 8); p[6] = 0;
    g_Len += 7;
}

static void CfgOther(PC_U8 type, PC_U8 len)
{
    PC_U8 *p = g_Cfg + g_Len;
    memset(p, 0, len);
    p[0] = len; p[1] = type;
    g_Len += len;
}

static void CfgEnd(void)
{
    g_Cfg[2] = (PC_U8)(g_Len & 0xFF);
    g_Cfg[3] = (PC_U8)(g_Len >> 8);
}

static void TestAlt(void)
{
    PC_ALT a;

    /* single bidirectional interface */
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1);
    CHECK(a.Found && a.Interface == 0 && a.Alternate == 0 && a.Protocol == 2);
    CHECK(a.OutEp == 0x01 && a.InEp == 0x82 && a.OutMax == 64 && a.InMax == 64);
    CHECK(a.IfOffset == 9 && a.NumEps == 2);

    /* the bidirectional quirk drops the IN endpoint */
    CHECK(PcSelectAlt(g_Cfg, g_Len, PC_Q_NO_BIDI, &a) == 1);
    CHECK(a.Protocol == 1 && a.OutEp == 0x01 && a.InEp == 0 && a.InMax == 0);

    /* 01 at alternate 0, 02 at alternate 1: 02 wins */
    CfgBegin();
    CfgIf(0, 0, 1, 7, 1, 1); CfgEp(0x02, 2, 64);
    CfgIf(0, 1, 2, 7, 1, 2); CfgEp(0x02, 2, 64); CfgEp(0x81, 2, 64);
    CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1);
    CHECK(a.Alternate == 1 && a.Protocol == 2 && a.OutEp == 0x02 && a.InEp == 0x81);
    CHECK(a.IfOffset == 9 + 9 + 7);

    /* 03 at alternate 0, 01 at alternate 1: 01 wins over 03 */
    CfgBegin();
    CfgIf(0, 0, 2, 7, 1, 3); CfgEp(0x01, 2, 512); CfgEp(0x81, 2, 512);
    CfgIf(0, 1, 1, 7, 1, 1); CfgEp(0x01, 2, 512);
    CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1);
    CHECK(a.Alternate == 1 && a.Protocol == 1 && a.OutMax == 512);

    /* 03 only: chosen, as 03 */
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 3); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1 && a.Protocol == 3);

    /* 02 with only an OUT endpoint is used as 01 */
    CfgBegin(); CfgIf(0, 0, 1, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1 && a.Protocol == 1 && a.InEp == 0);

    /* MFP: vendor scanner at 0, storage at 1, printer at 2 with a class
       descriptor between; interrupt endpoints ignored */
    CfgBegin();
    CfgIf(0, 0, 3, 0xFF, 0xFF, 0xFF); CfgEp(0x81, 2, 64); CfgEp(0x02, 2, 64); CfgEp(0x83, 3, 8);
    CfgIf(1, 0, 2, 8, 6, 0x50); CfgEp(0x04, 2, 64); CfgEp(0x85, 2, 64);
    CfgIf(2, 0, 3, 7, 1, 2); CfgOther(0x24, 5); CfgEp(0x87, 3, 8); CfgEp(0x06, 2, 64); CfgEp(0x88, 2, 64);
    CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1);
    CHECK(a.Interface == 2 && a.Protocol == 2 && a.OutEp == 0x06 && a.InEp == 0x88);

    /* two printer interfaces: the first is the one */
    CfgBegin();
    CfgIf(0, 0, 1, 7, 1, 1); CfgEp(0x01, 2, 64);
    CfgIf(1, 0, 2, 7, 1, 2); CfgEp(0x02, 2, 64); CfgEp(0x83, 2, 64);
    CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1 && a.Interface == 0 && a.Protocol == 1);

    /* no printer interface, or a printer with no OUT endpoint */
    CfgBegin(); CfgIf(0, 0, 2, 8, 6, 0x50); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 0 && !a.Found);
    CfgBegin(); CfgIf(0, 0, 1, 7, 1, 2); CfgEp(0x81, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 0);
    CfgBegin(); CfgIf(0, 0, 2, 7, 2, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 0);   /* subclass 2 is not a printer */

    /* malformed: a zero-length descriptor, one running past the end, a
       short read of the header, a wrong type */
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 2); CfgEp(0x01, 2, 64); g_Cfg[g_Len] = 0; g_Cfg[g_Len + 1] = 5;
    g_Len += 2; CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == -1);
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    g_Cfg[9 + 9] = 40;
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == -1);
    CHECK(PcSelectAlt(g_Cfg, 5, 0, &a) == -1);
    CHECK(PcSelectAlt(0, 64, 0, &a) == -1);
    CfgBegin(); g_Cfg[1] = 1; CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == -1);

    /* bNumEndpoints says 3, two endpoint descriptors follow: refused */
    CfgBegin(); CfgIf(0, 0, 3, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == -1);
    /* ... in another class's interface it does not matter */
    CfgBegin(); CfgIf(0, 0, 3, 0xFF, 0, 0); CfgEp(0x81, 2, 64);
    CfgIf(1, 0, 1, 7, 1, 1); CfgEp(0x02, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == 1 && a.Interface == 1 && a.NumEps == 1);

    /* wTotalLength shorter than the buffer: the tail is not read */
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    g_Cfg[2] = 18;
    CHECK(PcSelectAlt(g_Cfg, g_Len, 0, &a) == -1);  /* no endpoint within 18 bytes */
    /* a read shorter than wTotalLength stops at the read */
    CfgBegin(); CfgIf(0, 0, 2, 7, 1, 2); CfgEp(0x01, 2, 64); CfgEp(0x82, 2, 64); CfgEnd();
    CHECK(PcSelectAlt(g_Cfg, 9 + 9 + 7, 0, &a) == -1); /* one of two endpoints read */
}

static void TestQuirks(void)
{
    CHECK(PcQuirks(0x03f0, 0x0004) == PC_Q_NO_BIDI);
    CHECK(PcQuirks(0x04f9, 0x000d) == PC_Q_NO_BIDI);
    CHECK(PcQuirks(0x0482, 0x0010) == PC_Q_NO_BIDI);
    CHECK(PcQuirks(0x03f0, 0x0005) == 0);
    CHECK(PcQuirks(0x46f4, 0x0010) == 0);
}

static void TestId(void)
{
    PC_U8 b[1100];
    char v[64];
    const char *id = "MFG:Hewlett-Packard;CMD:PJL,PCL;MDL:HP LaserJet 4;CLS:PRINTER;DES:Hewlett-Packard HP LaserJet 4;";
    const char *lid = " manufacturer : ACME Inc ;Model:Model 9;command set:ESC/P;CLASS:PRINTER;COMPATIBLE ID:ACME_9;";

    b[0] = 0; b[1] = 10;
    CHECK(PcIdLength(b, 64) == 8);
    CHECK(PcIdLength(b, 6) == 4);           /* clamped to the bytes read */
    b[1] = 1;
    CHECK(PcIdLength(b, 64) == 0);          /* length under 2 */
    b[0] = 0xFF; b[1] = 0xFF;
    CHECK(PcIdLength(b, 1100) == PC_ID_MAX);
    CHECK(PcIdLength(b, 1) == 0);
    CHECK(PcIdLength(0, 10) == 0);

    CHECK(PcIdValue(id, (PC_U32)strlen(id), PC_KEY_MFG, v, sizeof(v)) == 15 && !strcmp(v, "Hewlett-Packard"));
    CHECK(PcIdValue(id, (PC_U32)strlen(id), PC_KEY_MDL, v, sizeof(v)) && !strcmp(v, "HP LaserJet 4"));
    CHECK(PcIdValue(id, (PC_U32)strlen(id), PC_KEY_CMD, v, sizeof(v)) && !strcmp(v, "PJL,PCL"));
    CHECK(PcIdValue(id, (PC_U32)strlen(id), PC_KEY_CID, v, sizeof(v)) == 0 && v[0] == 0);
    CHECK(PcIdValue(lid, (PC_U32)strlen(lid), PC_KEY_MFG, v, sizeof(v)) && !strcmp(v, "ACME Inc"));
    CHECK(PcIdValue(lid, (PC_U32)strlen(lid), PC_KEY_MDL, v, sizeof(v)) && !strcmp(v, "Model 9"));
    CHECK(PcIdValue(lid, (PC_U32)strlen(lid), PC_KEY_CMD, v, sizeof(v)) && !strcmp(v, "ESC/P"));
    CHECK(PcIdValue(lid, (PC_U32)strlen(lid), PC_KEY_CLS, v, sizeof(v)) && !strcmp(v, "PRINTER"));
    CHECK(PcIdValue(lid, (PC_U32)strlen(lid), PC_KEY_CID, v, sizeof(v)) && !strcmp(v, "ACME_9"));
    /* the length bounds the search: MDL lies past 20 bytes */
    CHECK(PcIdValue(id, 20, PC_KEY_MDL, v, sizeof(v)) == 0);
    /* a value cut by the length, and by the buffer */
    CHECK(PcIdValue(id, 19, PC_KEY_MFG, v, sizeof(v)) == 15);
    CHECK(PcIdValue(id, (PC_U32)strlen(id), PC_KEY_MFG, v, 6) == 5 && !strcmp(v, "Hewle"));
    CHECK(PcIdValue("MFG:A;MFGX:B;", 13, PC_KEY_MFG, v, sizeof(v)) == 1 && !strcmp(v, "A"));
    CHECK(PcIdValue("XMFG:A;MFG:B", 12, PC_KEY_MFG, v, sizeof(v)) == 1 && !strcmp(v, "B"));
    CHECK(PcIdValue("junk;MFG:B", 10, PC_KEY_MFG, v, sizeof(v)) == 1 && !strcmp(v, "B"));
    CHECK(PcIdValue("MFG:A", 5, 9, v, sizeof(v)) == 0);
    CHECK(PcIdValue("MFG:A\0MDL:B", 11, PC_KEY_MDL, v, sizeof(v)) == 0);  /* stops at NUL */

    CHECK(PcIdDescribe(id, (PC_U32)strlen(id), v, sizeof(v)) && !strcmp(v, "Hewlett-Packard HP LaserJet 4"));
    CHECK(PcIdDescribe(lid, (PC_U32)strlen(lid), v, sizeof(v)) && !strcmp(v, "ACME Inc Model 9"));
    CHECK(PcIdDescribe("MDL:X;", 6, v, sizeof(v)) && !strcmp(v, "X"));
    CHECK(PcIdDescribe("", 0, v, sizeof(v)) && !strcmp(v, "USB printer"));
    CHECK(PcIdDescribe(lid, (PC_U32)strlen(lid), v, 5) == 4 && !strcmp(v, "ACME"));
}

static void TestPorts(void)
{
    char s[16];
    PC_U8 used[PC_PORT_MAX / 8 + 1];

    CHECK(PcPortName(1, s) && !strcmp(s, "USB001"));
    CHECK(PcPortName(64, s) && !strcmp(s, "USB064"));
    CHECK(!PcPortName(0, s) && !PcPortName(65, s));
    CHECK(PcParsePortName("USB001", 6) == 1);
    CHECK(PcParsePortName("usb012", 6) == 12);
    CHECK(PcParsePortName("USB064:", 7) == 64);
    CHECK(PcParsePortName("USB065", 6) == 0);
    CHECK(PcParsePortName("USB000", 6) == 0);
    CHECK(PcParsePortName("USB1", 4) == 0);
    CHECK(PcParsePortName("USB0011", 7) == 0);
    CHECK(PcParsePortName("LPT1:", 5) == 0);
    CHECK(PcParsePortName("USBA01", 6) == 0);
    CHECK(PcParsePortName(0, 6) == 0);
    CHECK(PcDevicePath(1, s) && !strcmp(s, "\\\\.\\USBPRN1"));
    CHECK(PcDevicePath(12, s) && !strcmp(s, "\\\\.\\USBPRN12"));
    CHECK(PcDevicePath(64, s) && !strcmp(s, "\\\\.\\USBPRN64"));
    CHECK(!PcDevicePath(0, s) && !PcDevicePath(65, s));

    memset(used, 0, sizeof(used));
    CHECK(PcLowestFree(used) == 1);
    used[0] = 0x06;                         /* 1 and 2 */
    CHECK(PcLowestFree(used) == 3);
    used[0] = 0xFE; used[1] = 0x01;         /* 1 to 8 */
    CHECK(PcLowestFree(used) == 9);
    memset(used, 0xFF, sizeof(used));
    CHECK(PcLowestFree(used) == 0);
    used[PC_PORT_MAX >> 3] &= (PC_U8)~(1u << (PC_PORT_MAX & 7));
    CHECK(PcLowestFree(used) == PC_PORT_MAX);
}

static void TestChunkAndStatus(void)
{
    CHECK(PcChunk(100000, 16384) == 16384);
    CHECK(PcChunk(100, 16384) == 100);
    CHECK(PcChunk(10000, 0) == 4096);
    CHECK(PcChunk(0, 4096) == 0);

    CHECK(PcStatusError(1, 0x18) == PC_ERR_TIMEOUT);    /* benign */
    CHECK(PcStatusError(1, 0x38) == PC_ERR_PAPER);
    CHECK(PcStatusError(1, 0x28) == PC_ERR_PAPER);      /* paper out and offline */
    CHECK(PcStatusError(1, 0x10) == PC_ERR_FAULT);
    CHECK(PcStatusError(1, 0x00) == PC_ERR_FAULT);      /* error and offline */
    CHECK(PcStatusError(1, 0x08) == PC_ERR_OFFLINE);
    CHECK(PcStatusError(0, 0x38) == PC_ERR_TIMEOUT);    /* never read */
    CHECK(PcStatusError(1, 0xD8) == PC_ERR_TIMEOUT);    /* reserved bits ignored */
}

int main(void)
{
    TestAlt();
    TestQuirks();
    TestId();
    TestPorts();
    TestChunkAndStatus();
    printf("%d checks, %d failed\n", g_Checks, g_Fails);
    return g_Fails ? 1 : 0;
}
