/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cfgtest.c - host test of sys/cfgwalk.c, USBMTP.SYS's walks of the device
 * and configuration descriptors (docs/design/mtp.md 3.3): good layouts are
 * accepted with the right pick; each malformed variant is refused, and the
 * rule it is refused by is asserted; every truncation is refused; every
 * byte value at every offset of a good layout is tried, the walk ends, and
 * what it accepts is self-consistent. cfgwalk.c is compiled into this file
 * with its byte accessor replaced by one that fails the test on any index
 * at or past the length it was given, so a read past the end is caught;
 * each input is also placed at the very end of an allocation of its exact
 * size. Run by cfgtest.ps1 from build.ps1.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_Fail, g_Checks, g_OutOfBounds;

/* The walk's every byte read goes through here (cfgwalk.c's CFG_AT). */
static unsigned char Acc(const unsigned char *p, unsigned long len, unsigned long i)
{
    if (i >= len) {
        g_OutOfBounds++;
        return 0;
    }
    return p[i];
}
#define CFG_AT(p, len, i) Acc((p), (len), (unsigned long)(i))
#include "sys/cfgwalk.c"

static void Check(int cond, const char *what)
{
    g_Checks++;
    if (!cond) {
        g_Fail++;
        printf("FAIL: %s\n", what);
    }
}

/* Builders. */
static unsigned char g_B[8192];
static unsigned long g_N;

static void Cfg(unsigned char nif)
{
    unsigned char c[9] = { 9, 2, 0, 0, 0, 1, 0, 0x80, 50 };
    c[4] = nif;
    memcpy(g_B, c, 9);
    g_N = 9;
}
static void Iface(unsigned char num, unsigned char alt, unsigned char neps, unsigned char cl, unsigned char sub, unsigned char pr)
{
    unsigned char d[9] = { 9, 4, 0, 0, 0, 0, 0, 0, 0 };
    d[2] = num; d[3] = alt; d[4] = neps; d[5] = cl; d[6] = sub; d[7] = pr;
    memcpy(g_B + g_N, d, 9);
    g_N += 9;
}
static void Ep(unsigned char addr, unsigned char attr, unsigned short mps)
{
    unsigned char d[7] = { 7, 5, 0, 0, 0, 0, 0 };
    d[2] = addr; d[3] = attr; d[4] = (unsigned char)mps; d[5] = (unsigned char)(mps >> 8);
    d[6] = (unsigned char)((attr & 3) == 3 ? 4 : 0);    /* bInterval: an interrupt endpoint's 4 */
    memcpy(g_B + g_N, d, 7);
    g_N += 7;
}
static void Raw(const unsigned char *p, unsigned long n)
{
    memcpy(g_B + g_N, p, n);
    g_N += n;
}
/* A SuperSpeed endpoint companion: bMaxBurst, bmAttributes, wBytesPerInterval. */
static void Comp(unsigned char burst, unsigned char attr, unsigned short bpi)
{
    unsigned char d[6] = { 6, 0x30, 0, 0, 0, 0 };
    d[2] = burst; d[3] = attr; d[4] = (unsigned char)bpi; d[5] = (unsigned char)(bpi >> 8);
    memcpy(g_B + g_N, d, 6);
    g_N += 6;
}
static void Seal(void)
{
    g_B[2] = (unsigned char)g_N;
    g_B[3] = (unsigned char)(g_N >> 8);
}
static void Ptp(unsigned char num)
{
    Iface(num, 0, 3, 6, 1, 1);
    Ep(0x81, 2, 512);
    Ep(0x02, 2, 512);
    Ep(0x83, 3, 28);
}

/* The walk on a copy at the very end of an allocation of its exact size;
   every read is checked by Acc. */
static int WalkAt0(const unsigned char *p, unsigned long n, int node, int super, unsigned char mps0, CFG_PICK *pk)
{
    unsigned char *c = (unsigned char *)malloc(n ? n : 1);
    int r, oob = g_OutOfBounds;
    memcpy(c, p, n);
    memset(pk, 0, sizeof(*pk));
    r = CfgWalk(c, n, node, super, mps0, pk);
    Check(g_OutOfBounds == oob, "the walk read past the end");
    if (r == CW_OK) Check(pk->IfOffset + 9 <= n, "accepted with the interface past the end");
    free(c);
    return r;
}

/* EP0 at 64 below SuperSpeed (right at high speed, allowed at full), 09h at SuperSpeed. */
static int WalkAt(const unsigned char *p, unsigned long n, int node, int super, CFG_PICK *pk)
{
    return WalkAt0(p, n, node, super, (unsigned char)(super ? 9 : 64), pk);
}

/* Below SuperSpeed, and at SuperSpeed. */
static int Walk(const unsigned char *p, unsigned long n, int node, CFG_PICK *pk) { return WalkAt(p, n, node, 0, pk); }
static int WalkS(const unsigned char *p, unsigned long n, int node, CFG_PICK *pk) { return WalkAt(p, n, node, 1, pk); }

static int g_DevSuper;

static int Dev(const unsigned char *p, unsigned long n)
{
    unsigned char *c = (unsigned char *)malloc(n ? n : 1);
    int r, oob = g_OutOfBounds;
    memcpy(c, p, n);
    g_DevSuper = -1;
    r = DevCheck(c, n, &g_DevSuper);
    Check(g_OutOfBounds == oob, "the device check read past the end");
    free(c);
    return r;
}

int main(void)
{
    CFG_PICK pk;
    unsigned char good[512];
    unsigned long goodN, i, k;
    int r, accepted = 0, refused = 0;
    char what[128];

    /* 1. single interface, whole device */
    Cfg(1); Ptp(0); Seal();
    r = Walk(g_B, g_N, -1, &pk);
    Check(r == CW_OK, "single-interface device accepted");
    Check(pk.Interface == 0 && pk.EpIn == 0x81 && pk.EpOut == 0x02 && pk.EpInt == 0x83, "single: pick");
    Check(pk.MpsIn == 512 && pk.MpsOut == 512 && pk.MpsInt == 28 && pk.IfOffset == 9, "single: packet sizes, offset");
    memcpy(good, g_B, g_N);
    goodN = g_N;

    /* 2. a composite's configuration: the node's interface is picked */
    Cfg(2); Ptp(0); Iface(1, 0, 2, 0xFF, 0x42, 1); Ep(0x84, 2, 512); Ep(0x05, 2, 512); Seal();
    Check(Walk(g_B, g_N, 0, &pk) == CW_OK && pk.Interface == 0, "composite MI_00 accepted");
    Check(Walk(g_B, g_N, 1, &pk) == CW_ENDPOINTS, "composite MI_01 (ADB, two endpoints) refused");
    Check(Walk(g_B, g_N, -1, &pk) == CW_WHOLE, "composite as a whole device refused");
    Check(Walk(g_B, g_N, 2, &pk) == CW_ABSENT, "composite MI_02 absent");

    /* 3. vendor class FF, alternate settings, class-specific records */
    {
        unsigned char cs[5] = { 5, 0x24, 0, 0x10, 1 };
        Cfg(1); Iface(0, 0, 3, 0xFF, 0xFF, 0); Raw(cs, 5); Ep(0x81, 2, 64); Ep(0x01, 2, 64); Ep(0x82, 3, 8);
        Iface(0, 1, 0, 0xFF, 0xFF, 0); Seal();
        r = Walk(g_B, g_N, -1, &pk);
        Check(r == CW_OK && pk.Class == 0xFF && pk.MpsIn == 64, "FF interface with a class record and an alternate setting");
    }

    /* 4. malformed variants of the good layout, each with its rule */
    Cfg(1); Ptp(0); Seal(); g_B[0] = 8;
    Check(Walk(g_B, g_N, -1, &pk) == CW_HEADER, "bLength 8 in the header");
    Cfg(1); Ptp(0); Seal(); g_B[1] = 4;
    Check(Walk(g_B, g_N, -1, &pk) == CW_HEADER, "wrong descriptor type");
    Cfg(1); Ptp(0); Seal(); g_B[2]++;
    Check(Walk(g_B, g_N, -1, &pk) == CW_HEADER, "wTotalLength one more than arrived");
    Cfg(1); Ptp(0); Seal(); g_B[2]--;
    Check(Walk(g_B, g_N, -1, &pk) == CW_HEADER, "wTotalLength one less than arrived");
    Cfg(0); Ptp(0); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_HEADER, "bNumInterfaces 0");
    Cfg(1); Ptp(0); Seal(); g_B[5] = 0;
    Check(Walk(g_B, g_N, -1, &pk) == CW_CONFIG, "bConfigurationValue 0");
    Cfg(1); Ptp(0); Seal(); g_B[5] = 0xFF;
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "bConfigurationValue FFh accepted");
    Cfg(1); Ptp(0); Seal(); g_B[7] = 0x00;
    Check(Walk(g_B, g_N, -1, &pk) == CW_CONFIG, "bmAttributes 00h (bit 7 clear)");
    Cfg(1); Ptp(0); Seal(); g_B[7] = 0x40;
    Check(Walk(g_B, g_N, -1, &pk) == CW_CONFIG, "bmAttributes 40h (self-powered, bit 7 clear)");
    for (k = 0; k < 5; k++) {
        Cfg(1); Ptp(0); Seal(); g_B[7] = (unsigned char)(0x80 | (1 << k));
        sprintf(what, "bmAttributes %02Xh (reserved bit %lu set)", g_B[7], k);
        Check(Walk(g_B, g_N, -1, &pk) == CW_CONFIG, what);
    }
    Cfg(1); Ptp(0); Seal(); g_B[7] = 0xE0;
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "bmAttributes E0h (self-powered, remote wakeup) accepted");
    Cfg(2); Ptp(0); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_COUNT, "bNumInterfaces 2, one present");
    Cfg(1); Ptp(0); Seal(); g_B[9] = 0;
    Check(Walk(g_B, g_N, -1, &pk) == CW_RECORD, "a record of bLength 0");
    Cfg(1); Ptp(0); Seal(); g_B[9] = 1;
    Check(Walk(g_B, g_N, -1, &pk) == CW_RECORD, "a record of bLength 1");
    Cfg(1); Ptp(0); Seal(); g_B[9 + 9 + 7 + 7] = 200;
    Check(Walk(g_B, g_N, -1, &pk) == CW_RECORD, "the last record's bLength past the end");
    Cfg(1); Ptp(0); Seal(); g_B[9] = 8;
    Check(Walk(g_B, g_N, -1, &pk) != CW_OK, "interface bLength 8");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); { unsigned char e6[6] = { 6, 5, 0x81, 2, 0, 2 }; Raw(e6, 6); } Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_TOO_SHORT, "endpoint bLength 6");
    Cfg(1); Ep(0x81, 2, 512); Ptp(0); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_STRAY, "endpoint before any interface");
    Cfg(1); Ptp(0); { unsigned char c2[9] = { 9, 2, 9, 0, 1, 1, 0, 0x80, 50 }; Raw(c2, 9); } Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_NESTED, "a second configuration descriptor inside");
    Cfg(2); Ptp(0); Ptp(0); Seal();
    Check(Walk(g_B, g_N, 0, &pk) == CW_COUNT, "interface 0 twice at alternate 0");
    Cfg(1); Iface(0, 0, 3, 8, 6, 0x50); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_CLASS, "mass storage class refused");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x82, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "two bulk IN, no bulk OUT");
    Cfg(1); Iface(0, 0, 4, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "bNumEndpoints 4, three present");
    Cfg(1); Iface(0, 0, 2, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "bNumEndpoints 2, three present");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 1, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "an isochronous endpoint");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 0); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "wMaxPacketSize 0");
    /* endpoint addresses */
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x80, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "bulk IN on endpoint 0");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x00, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "bulk OUT on endpoint 0");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x91, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "reserved address bit 4");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x42, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "reserved address bit 6");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x81, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "bulk IN and interrupt IN on one address");
    /* endpoint bmAttributes: reserved bits (USB 2.0 table 9-13, USB 3.2 table 9-26) */
    for (k = 2; k < 8; k++) {
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, (unsigned char)(2 | (1 << k)), 512); Ep(0x02, 2, 512);
        Ep(0x83, 3, 28); Seal();
        sprintf(what, "bulk IN bmAttributes %02lXh (reserved bit %lu)", 2UL | (1UL << k), k);
        Check(Walk(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, what);
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, (unsigned char)(2 | (1 << k)), 512);
        Ep(0x83, 3, 28); Seal();
        sprintf(what, "bulk OUT bmAttributes %02lXh (reserved bit %lu)", 2UL | (1UL << k), k);
        Check(Walk(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, what);
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512);
        Ep(0x83, (unsigned char)(3 | (1 << k)), 28); Seal();
        sprintf(what, "high-speed interrupt bmAttributes %02lXh (reserved bit %lu)", 3UL | (1UL << k), k);
        Check(Walk(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, what);
    }
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 0x82, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, "bulk IN 82h");
    /* an endpoint of an interface the driver does not own never reaches USBD */
    Cfg(2); Ptp(0); Iface(1, 0, 2, 0xFF, 0x42, 1); Ep(0x84, 0x82, 512); Ep(0x05, 2, 512); Seal();
    Check(Walk(g_B, g_N, 0, &pk) == CW_OK, "reserved bits on another interface's endpoint");
    /* packet sizes for the type and the speed */
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 100); Ep(0x02, 2, 100); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "bulk 100");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 64); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "bulk 512 and 64 (two speeds)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Ep(0x02, 2, 1024); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "bulk 1024 (SuperSpeed size)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 0x800 | 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "bulk with a high-bandwidth bit");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt 0");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 1025); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt 1025 at high speed");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 64); Ep(0x02, 2, 64); Ep(0x83, 3, 65); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt 65 at full speed");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x1000 | 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt, two additional transactions with 28 bytes");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x0800 | 600); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK && pk.MpsInt == 600 && pk.MultInt == 1, "high-bandwidth interrupt, one more transaction of 600 accepted");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x1000 | 700); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK && pk.MultInt == 2, "high-bandwidth interrupt, two more of 700 accepted");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x0800 | 512); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "high-bandwidth interrupt, one more of 512 (needs 513 or more)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x1800 | 1024); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt multiplier 3 (reserved)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 64); Ep(0x02, 2, 64); Ep(0x83, 3, 0x0800 | 600); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "a multiplier at full speed");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x8000 | 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt with a reserved bit");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 32); Ep(0x02, 2, 8); Ep(0x83, 3, 64); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "full speed, bulk 32 and 8, interrupt 64 accepted");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 1024); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "high speed, interrupt 1024 accepted");
    /* alternate settings */
    Cfg(1); Ptp(0); Iface(0, 1, 0, 6, 1, 1); Iface(0, 1, 0, 6, 1, 1); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_COUNT, "interface 0 alternate 1 twice");
    Cfg(2); Ptp(0); Iface(1, 1, 0, 0xFF, 0, 0); Seal();
    Check(Walk(g_B, g_N, 0, &pk) == CW_COUNT, "interface 1 with alternate 1 and no alternate 0");
    Cfg(1); Ptp(0); Iface(1, 1, 0, 0xFF, 0, 0); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_COUNT, "an alternate 1 of an interface with no alternate 0, the count matching");
    Cfg(2); Ptp(0); Iface(1, 0, 2, 0xFF, 0, 0); Ep(0x84, 2, 512); Seal();
    Check(Walk(g_B, g_N, 0, &pk) == CW_ENDPOINTS, "another interface's bNumEndpoints unlike its records");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 28); Iface(0, 1, 1, 6, 1, 1); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "an alternate's endpoint missing");
    /* SuperSpeed (USB 3.2 9.6.6, 9.6.7) */
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(15, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    r = WalkS(g_B, g_N, -1, &pk);
    Check(r == CW_OK && pk.Speed == CS_SUPER && pk.MpsIn == 1024 && pk.MpsInt == 28, "SuperSpeed layout accepted");
    Check(Walk(g_B, g_N, -1, &pk) == CW_SPEED, "companions below SuperSpeed refused");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0x05, 0); Ep(0x02, 2, 1024); Comp(0, 0x05, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_OK, "bulk MaxStreams ignored");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 1, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "interrupt companion bmAttributes not 0");
    /* an interrupt endpoint's usage type at SuperSpeed: 00b and 01b, not 10b or 11b */
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 0x13, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_OK, "SuperSpeed interrupt usage type notification (13h) accepted");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 0x13, 28); Seal();
    Check(Walk(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, "interrupt 13h at high speed");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 0x23, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, "SuperSpeed interrupt usage type 10b (23h)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 0x33, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_ATTRIBUTES, "SuperSpeed interrupt usage type 11b (33h)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Comp(0, 0, 0); Ep(0x02, 2, 512); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "bulk 512 at SuperSpeed");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Ep(0x02, 2, 1024); Comp(0, 0, 0); Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "a companion missing in the middle");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0); Ep(0x83, 3, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "the last companion missing");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Ep(0x02, 2, 1024); Ep(0x83, 3, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "no companions at SuperSpeed");
    {
        unsigned char c7[7] = { 7, 0x30, 0, 0, 0, 0, 0 };
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Raw(c7, 7); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_TOO_SHORT, "a companion of 7 bytes");
    }
    {
        unsigned char cs[5] = { 5, 0x25, 1, 0, 0 };
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Raw(cs, 5); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "a record between an endpoint and its companion");
    }
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Comp(0, 0, 0); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_SPEED, "a companion before any endpoint");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(16, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "bulk bMaxBurst 16");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(1, 0, 56); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt burst with 28 bytes (needs 1024)");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 1024); Comp(2, 0, 3072); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_OK, "interrupt 1024 with bMaxBurst 2 accepted");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 1024); Comp(3, 0, 4096); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt bMaxBurst 3");
    Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 0x0800 | 1024); Comp(0, 0, 1024); Seal();
    Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "a high-speed multiplier at SuperSpeed");
    Cfg(2); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
    Ep(0x83, 3, 28); Comp(0, 0, 28); Iface(1, 0, 2, 0xFF, 0x42, 1); Ep(0x84, 2, 1024); Comp(0, 0, 0);
    Ep(0x05, 2, 1024); Comp(0, 0, 0); Seal();
    Check(WalkS(g_B, g_N, 0, &pk) == CW_OK, "SuperSpeed composite MI_00 with the ADB interface's companions");

    /* bInterval, wBytesPerInterval, the default endpoint against the speed */
    {
        int v;
        Cfg(1); Ptp(0); Seal(); g_B[9 + 9 + 7 + 7 + 6] = 0;
        Check(Walk(g_B, g_N, -1, &pk) == CW_INTERVAL, "interrupt bInterval 0 at high speed");
        g_B[9 + 9 + 7 + 7 + 6] = 17;
        Check(Walk(g_B, g_N, -1, &pk) == CW_INTERVAL, "interrupt bInterval 17 at high speed");
        g_B[9 + 9 + 7 + 7 + 6] = 16;
        Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "interrupt bInterval 16 at high speed");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 64); Ep(0x02, 2, 64); Ep(0x83, 3, 8); Seal();
        g_B[9 + 9 + 7 + 7 + 6] = 255;
        Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "interrupt bInterval 255 at full speed");
        g_B[9 + 9 + 7 + 7 + 6] = 0;
        Check(Walk(g_B, g_N, -1, &pk) == CW_INTERVAL, "interrupt bInterval 0 at full speed");
        Check(WalkAt0(g_B, g_N, -1, 0, 8, &pk) == CW_INTERVAL, "(the same, EP0 8)");
        g_B[9 + 9 + 7 + 7 + 6] = 10;
        for (v = 1; v <= 64; v++) {
            int ok = v == 8 || v == 16 || v == 32 || v == 64, rr = WalkAt0(g_B, g_N, -1, 0, (unsigned char)v, &pk);
            sprintf(what, "full speed with bMaxPacketSize0 %d", v);
            Check(ok ? rr == CW_OK : rr == CW_DEVICE, what);
        }
        Cfg(1); Ptp(0); Seal();
        for (v = 8; v <= 64; v *= 2) {
            sprintf(what, "high speed with bMaxPacketSize0 %d", v);
            Check(WalkAt0(g_B, g_N, -1, 0, (unsigned char)v, &pk) == (v == 64 ? CW_OK : CW_DEVICE), what);
        }
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 28); Comp(0, 0, 28); Seal();
        Check(WalkAt0(g_B, g_N, -1, 1, 64, &pk) == CW_DEVICE, "SuperSpeed with bMaxPacketSize0 64");
        g_B[9 + 9 + 13 + 13 + 6] = 17;
        Check(WalkS(g_B, g_N, -1, &pk) == CW_INTERVAL, "interrupt bInterval 17 at SuperSpeed");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 28); Comp(0, 0, 0); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt wBytesPerInterval 0");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 28); Comp(0, 0, 29); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt wBytesPerInterval above the packet size");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 1024); Comp(2, 0, 3073); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_PACKET, "interrupt wBytesPerInterval 3073 with a burst of 3");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 1024); Comp(0, 0, 0); Ep(0x02, 2, 1024); Comp(0, 0, 0);
        Ep(0x83, 3, 1024); Comp(2, 0, 3072); Seal();
        Check(WalkS(g_B, g_N, -1, &pk) == CW_OK && pk.IntBytes == 3072, "the SuperSpeed poll is sized by wBytesPerInterval (3072)");
        Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep(0x02, 2, 512); Ep(0x83, 3, 0x1000 | 700); Seal();
        Check(Walk(g_B, g_N, -1, &pk) == CW_OK && pk.IntBytes == 2100, "the high-bandwidth poll is three transactions (2100)");
        Cfg(1); Ptp(0); Seal();
        Check(Walk(g_B, g_N, -1, &pk) == CW_OK && pk.IntBytes == 28, "a plain poll is one packet");
    }

    /* mtpemu.py's android profile at --speed super, byte for byte */
    {
        static const unsigned char dd[18] = { 0x12, 0x01, 0x00, 0x03, 0, 0, 0, 0x09, 0xFF, 0xFF, 0x60, 0, 0, 1, 1, 2, 3, 1 };
        static const unsigned char cf[] = {
            0x09, 0x02, 0x5c, 0x00, 0x02, 0x01, 0x00, 0x80, 0xfa,
            0x09, 0x04, 0x00, 0x00, 0x03, 0x06, 0x01, 0x01, 0x04,
            0x07, 0x05, 0x81, 0x02, 0x00, 0x04, 0x00, 0x06, 0x30, 0x00, 0x00, 0x00, 0x00,
            0x07, 0x05, 0x01, 0x02, 0x00, 0x04, 0x00, 0x06, 0x30, 0x00, 0x00, 0x00, 0x00,
            0x07, 0x05, 0x82, 0x03, 0x1c, 0x00, 0x06, 0x06, 0x30, 0x00, 0x00, 0x1c, 0x00,
            0x09, 0x04, 0x01, 0x00, 0x02, 0xff, 0x42, 0x01, 0x05,
            0x07, 0x05, 0x83, 0x02, 0x00, 0x04, 0x00, 0x06, 0x30, 0x00, 0x00, 0x00, 0x00,
            0x07, 0x05, 0x02, 0x02, 0x00, 0x04, 0x00, 0x06, 0x30, 0x00, 0x00, 0x00, 0x00 };
        Check(Dev(dd, 18) == CW_OK && g_DevSuper == 1, "the simulator's SuperSpeed device descriptor");
        Check(WalkS(cf, sizeof(cf), 0, &pk) == CW_OK && pk.Speed == CS_SUPER && pk.EpIn == 0x81 && pk.EpInt == 0x82,
              "the simulator's SuperSpeed android configuration, MI_00");
    }

    /* counts: limits checked before they are raised */
    {
        int e, rc;
        /* the construction that wrapped byte counters: 257 bulk IN, 257 bulk OUT, one interrupt IN */
        Cfg(1); Iface(0, 0, 3, 6, 1, 1);
        for (e = 0; e < 257; e++) Ep((unsigned char)(0x81 + (e % 15)), 2, 512);
        for (e = 0; e < 257; e++) Ep((unsigned char)(0x01 + (e % 15)), 2, 512);
        Ep(0x8F, 3, 28); Seal();
        Check(g_N < 4096, "the construction fits START's 4,096 bytes");
        Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "515 endpoint records on a three-endpoint interface");
        /* the same with bNumEndpoints 255 */
        g_B[9 + 4] = 255;
        Check(Walk(g_B, g_N, -1, &pk) == CW_ENDPOINTS, "bNumEndpoints 255");
        /* generated: the owned interface with n records and bNumEndpoints n or 3 */
        for (e = 0; e <= 40; e++) {
            int j;
            Cfg(1); Iface(0, 0, (unsigned char)e, 6, 1, 1);
            for (j = 0; j < e; j++) Ep((unsigned char)(j == 0 ? 0x81 : j == 1 ? 0x02 : j == 2 ? 0x83 : 0x80 | ((j % 15) + 1)),
                                       (unsigned char)(j == 2 ? 3 : 2), (unsigned short)(j == 2 ? 28 : 512));
            Seal();
            rc = Walk(g_B, g_N, -1, &pk);
            sprintf(what, "owned interface with %d endpoint records", e);
            Check(e == 3 ? rc == CW_OK : rc == CW_ENDPOINTS, what);
            g_B[9 + 4] = 3;
            rc = Walk(g_B, g_N, -1, &pk);
            sprintf(what, "owned interface with %d records and bNumEndpoints 3", e);
            Check(e == 3 ? rc == CW_OK : rc == CW_ENDPOINTS, what);
        }
        /* another interface with 31 endpoints */
        Cfg(2); Ptp(0); Iface(1, 0, 31, 0xFF, 0, 0);
        for (e = 0; e < 31; e++) Ep(0x81, 2, 512);
        Seal();
        Check(Walk(g_B, g_N, 0, &pk) == CW_ENDPOINTS, "another interface with 31 endpoints");
        /* across the configuration: 18 further interfaces of 30 endpoints (540 records) */
        Cfg(19); Ptp(0);
        for (e = 1; e <= 18; e++) {
            int j;
            Iface((unsigned char)e, 0, 30, 0xFF, 0, 0);
            for (j = 0; j < 30; j++) Ep(0x81, 2, 512);
        }
        Seal();
        Check(g_N < 4096, "the 540-record configuration fits 4,096 bytes");
        Check(Walk(g_B, g_N, 0, &pk) == CW_ENDPOINTS, "more than 512 endpoint records in a configuration");
        /* 65 interface settings */
        Cfg(1); Ptp(0);
        for (e = 1; e <= 64; e++) Iface(0, (unsigned char)e, 0, 6, 1, 1);
        Seal();
        Check(Walk(g_B, g_N, -1, &pk) == CW_COUNT, "65 interface settings");
        /* every address, twice, on the owned interface's three records */
        for (e = 1; e < 16; e++) {
            Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep((unsigned char)(0x80 | e), 2, 512); Ep(0x02, 2, 512);
            Ep((unsigned char)(0x80 | e), 3, 28); Seal();
            Check(Walk(g_B, g_N, -1, &pk) == CW_ADDRESS, "an IN address twice");
            Cfg(1); Iface(0, 0, 3, 6, 1, 1); Ep(0x81, 2, 512); Ep((unsigned char)e, 2, 512); Ep(0x8F, 3, 28); Seal();
            Check(Walk(g_B, g_N, -1, &pk) == CW_OK, "OUT and IN may share a number");
        }
    }

    /* the device descriptor */
    {
        unsigned char dd[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xF4, 0x46, 4, 0, 0, 0, 1, 2, 3, 1 };
        unsigned char t[18];
        Check(Dev(dd, 18) == CW_OK, "device descriptor accepted");
        Check(Dev(dd, 17) == CW_SHORT, "device descriptor of 17 bytes");
        memcpy(t, dd, 18); t[0] = 17;
        Check(Dev(t, 18) == CW_HEADER, "device bLength 17");
        memcpy(t, dd, 18); t[1] = 2;
        Check(Dev(t, 18) == CW_HEADER, "device bDescriptorType 2");
        memcpy(t, dd, 18); t[7] = 7;
        Check(Dev(t, 18) == CW_DEVICE, "bMaxPacketSize0 7");
        memcpy(t, dd, 18); t[7] = 0;
        Check(Dev(t, 18) == CW_DEVICE, "bMaxPacketSize0 0");
        memcpy(t, dd, 18); t[17] = 0;
        Check(Dev(t, 18) == CW_DEVICE, "bNumConfigurations 0");
        Check(Dev(dd, 18) == CW_OK && g_DevSuper == 0, "high speed is not SuperSpeed");
        memcpy(t, dd, 18); t[2] = 0x20; t[3] = 0x03; t[7] = 9;
        Check(Dev(t, 18) == CW_OK && g_DevSuper == 1, "bcdUSB 3.2 with bMaxPacketSize0 9: SuperSpeed");
        memcpy(t, dd, 18); t[2] = 0x10; t[3] = 0x02; t[7] = 9;
        Check(Dev(t, 18) == CW_DEVICE, "bMaxPacketSize0 9 with bcdUSB 2.1");
        memcpy(t, dd, 18); t[2] = 0x00; t[3] = 0x03; t[7] = 64;
        Check(Dev(t, 18) == CW_OK && g_DevSuper == 0, "bcdUSB 3.0 at high speed (64) is not SuperSpeed");
        for (i = 0; i < 18; i++) Dev(dd, i);    /* every truncation: in bounds */
    }
    Check(Walk(good, 8, -1, &pk) == CW_SHORT, "8 bytes");
    Check(CfgWalk(0, 9, -1, 0, 64, &pk) == CW_SHORT, "no buffer");

    /* 5. every truncation of the good layouts (with wTotalLength still the
       full length, and with it matching the cut) */
    for (i = 0; i < goodN; i++) {
        unsigned char t[512];
        memcpy(t, good, goodN);
        r = Walk(t, i, -1, &pk);
        sprintf(what, "truncated to %lu bytes (wTotalLength %lu) refused", i, goodN);
        Check(r != CW_OK, what);
        if (i >= 4) { t[2] = (unsigned char)i; t[3] = 0; }
        r = Walk(t, i, -1, &pk);
        sprintf(what, "truncated to %lu bytes with a matching wTotalLength refused", i);
        Check(r != CW_OK, what);
    }

    /* 6. every byte value at every offset of the good layout: the walk
       ends, stays in bounds (Walk checks), and what it accepts is
       self-consistent */
    for (i = 0; i < goodN; i++) {
        for (k = 0; k < 256; k++) {
            unsigned char t[512];
            memcpy(t, good, goodN);
            t[i] = (unsigned char)k;
            r = Walk(t, goodN, -1, &pk);
            if (r == CW_OK) {
                accepted++;
                Check(t[pk.IfOffset + 1] == 4 && t[pk.IfOffset] >= 9, "accepted pick is an interface descriptor");
                Check(pk.MpsIn && pk.MpsOut && pk.MpsInt, "accepted with a zero packet size");
            } else {
                refused++;
            }
        }
    }
    printf("cfgwalk: %d checks, %d failed, %d reads out of bounds; single-byte variants: %d accepted, %d refused\n",
           g_Checks, g_Fail, g_OutOfBounds, accepted, refused);
    printf("%s\n", g_Fail ? "FAIL" : "PASS");
    return g_Fail ? 1 : 0;
}
