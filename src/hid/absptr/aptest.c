/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host tests of absptr's portable core (apcore.c; docs/design/absptr.md
 * sections 5 and 6). Built by test.ps1 with the repository's MSVC 6.0.
 *
 *   aptest       run every test; exit 0 only if all pass
 *
 * The descriptors are absemu.py's profiles, byte for byte (the simulator
 * the legs use), plus edge cases written here.
 *
 * The D_QEMUTAB bytes are QEMU's tablet report descriptor (hw/usb/dev-hid.c,
 * QEMU v11.1.0), carried as a test vector under that file's notice:
 *   Copyright (c) 2005 Fabrice Bellard
 *   Copyright (c) 2007 OpenMoko, Inc.  (andrew@openedhand.com)
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * The D_PENPARTNER bytes are QEMU's Wacom PenPartner report descriptor
 * (hw/usb/dev-wacom.c, QEMU v11.1.0, as absemu.py carries it), under that
 * file's notice:
 *   Copyright (c) 2006 Openedhand Ltd.
 *   Author: Andrzej Zaborowski <balrog@zabor.org>
 *   Based on hw/usb-hid.c: Copyright (c) 2005 Fabrice Bellard
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#pragma warning(disable:4127)  /* the CHECK macro's while (0) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "apcore.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static unsigned char work[65536];

/* absemu.py's descriptors (generated from its PROFILES). */
static const AU8 D_TABLET[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};
static const AU8 D_KVM16[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x35, 0x00, 0x47,
    0xFF, 0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x06, 0xC0, 0xC0,
};
static const AU8 D_KVM12[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0xFF, 0x0F,
    0x75, 0x0C, 0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};
static const AU8 D_POINTER[] = {
    0x05, 0x01, 0x09, 0x01, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F,
    0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};
static const AU8 D_TOUCH[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x22, 0xA1, 0x02, 0x09, 0x42,
    0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00,
    0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0xFF, 0x0F, 0x65, 0x11, 0x55, 0x0E,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x65, 0x00, 0x55, 0x00, 0x35, 0x00,
    0x45, 0x00, 0x05, 0x0D, 0xC0, 0xC0,
};
static const AU8 D_TOUCHMOUSE[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x22, 0xA1, 0x02,
    0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0xFF, 0x0F, 0x65, 0x11,
    0x55, 0x0E, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x65, 0x00, 0x55, 0x00,
    0x35, 0x00, 0x45, 0x00, 0x05, 0x0D, 0xC0, 0xC0, 0x05, 0x01, 0x09, 0x02,
    0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0xFF, 0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F, 0x75, 0x10,
    0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};
static const AU8 D_MTOUCH[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x22, 0xA1, 0x02,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03, 0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01,
    0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF,
    0x0F, 0x35, 0x00, 0x46, 0xFF, 0x0F, 0x65, 0x11, 0x55, 0x0E, 0x75, 0x10,
    0x95, 0x02, 0x81, 0x02, 0x65, 0x00, 0x55, 0x00, 0x35, 0x00, 0x45, 0x00,
    0x05, 0x0D, 0xC0, 0x09, 0x22, 0xA1, 0x02, 0x09, 0x42, 0x15, 0x00, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x09,
    0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09,
    0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x0F, 0x35, 0x00, 0x46, 0xFF,
    0x0F, 0x65, 0x11, 0x55, 0x0E, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x65,
    0x00, 0x55, 0x00, 0x35, 0x00, 0x45, 0x00, 0x05, 0x0D, 0xC0, 0x09, 0x54,
    0x15, 0x00, 0x25, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x85, 0x02,
    0x09, 0x55, 0x25, 0x02, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, 0xC0,
};
static const AU8 D_PEN[] = {
    0x05, 0x0D, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x20, 0xA1, 0x00, 0x09, 0x42,
    0x09, 0x44, 0x09, 0x45, 0x09, 0x3C, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02, 0x95, 0x03, 0x81, 0x03, 0x05, 0x01,
    0x09, 0x30, 0x15, 0x00, 0x26, 0x20, 0x4E, 0x35, 0x00, 0x46, 0x20, 0x4E,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x09, 0x31, 0x26, 0x98, 0x3A, 0x46,
    0x98, 0x3A, 0x81, 0x02, 0x05, 0x0D, 0x09, 0x30, 0x26, 0xFF, 0x03, 0x75,
    0x10, 0x95, 0x01, 0x81, 0x02, 0xC0, 0xC0,
};
static const AU8 D_PENPARTNER[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
    0x95, 0x03, 0x81, 0x06, 0x95, 0x03, 0x81, 0x01, 0xC0, 0xC0, 0x05, 0x0D,
    0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0xA1, 0x00, 0x06, 0x00, 0xFF, 0x09,
    0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x07, 0x81, 0x02,
    0xC0, 0x09, 0x01, 0x85, 0x63, 0x95, 0x07, 0x81, 0x02, 0x09, 0x01, 0x85,
    0x02, 0x95, 0x01, 0xB1, 0x02, 0x09, 0x01, 0x85, 0x03, 0x95, 0x01, 0xB1,
    0x02, 0xC0,
};

/* QEMU hw/usb/dev-hid.c qemu_tablet_hid_report_descriptor (MIT): 5
 * buttons, X/Y 0..7FFF, a wheel. */
static const AU8 D_QEMUTAB[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0,
};

/* A relative mouse: no source. */
static const AU8 D_RELMOUSE[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
};

/* One Mouse collection: absolute X/Y in report 1, relative in report 2. */
static const AU8 D_ABSREL[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0x85, 0x02, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0,
};

/* A pen with signed X/Y (-16384..16383), a 4-byte extended Tip usage,
 * Push/Pop around a padding field, a long item, and a Device
 * Configuration collection with Device Mode in feature report 3. */
static const AU8 D_EDGE[] = {
    0x05, 0x0d, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01,
    0x0b, 0x42, 0x00, 0x0d, 0x00,               /* Usage (Digitizer:Tip), 4 bytes */
    0x09, 0x32,                                 /* In Range */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0xa4,                                       /* Push */
    0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
    0xb4,                                       /* Pop: Size 1, Count 2 again */
    0xfe, 0x02, 0x10, 0xaa, 0xbb,               /* a long item */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0xc0, 0x26, 0xff, 0x3f,         /* Logical -16384..16383 */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
    0xc0,
    0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x03,
    0x09, 0x23, 0xa1, 0x02,                     /* Device Settings */
    0x09, 0x52, 0x09, 0x53, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x02, 0xb1, 0x02,
    0xc0, 0xc0,
};

static const AU8 D_UNBALANCED[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0, 0xc0 };
static const AU8 D_SHORT[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x26, 0xff };

static int Parse(const AU8 *d, unsigned n, AP_SOURCE *s)
{
    return ApParse(d, n, work, s);
}

static int Near(unsigned got, double frac)
{
    long want = (long)(frac * 32767 + 0.5), d = (long)got - want;
    return d >= -40 && d <= 40;     /* within 0.13 % */
}

static void Put16(AU8 *p, unsigned v)
{
    p[0] = (AU8)v;
    p[1] = (AU8)(v >> 8);
}

/* The reports absemu.py builds (mouse_report, finger, ...). */
static unsigned MouseRep(AU8 *r, unsigned bits, double x, double y, unsigned buttons)
{
    unsigned top = bits == 16 ? 65535 : bits == 15 ? 32767 : 4095;
    unsigned vx = (unsigned)(x * top + 0.5), vy = (unsigned)(y * top + 0.5);
    r[0] = (AU8)buttons;
    if (bits == 12) {
        unsigned long p = vx | ((unsigned long)vy << 12);
        r[1] = (AU8)p; r[2] = (AU8)(p >> 8); r[3] = (AU8)(p >> 16); r[4] = 0;
        return 5;
    }
    Put16(r + 1, vx);
    Put16(r + 3, vy);
    r[5] = 0;
    return 6;
}

static unsigned Finger(AU8 *r, int tip, int range, double x, double y)
{
    r[0] = (AU8)((tip ? 1 : 0) | (range ? 2 : 0));
    Put16(r + 1, (unsigned)(x * 4095 + 0.5));
    Put16(r + 3, (unsigned)(y * 4095 + 0.5));
    return 5;
}

/* absemu.py's mtouch report: id 1, two slots (tip byte, contact id, X,
 * Y in 0..4095), contact count. */
static void MtRep(AU8 *r, int tip0, unsigned id0, double x0, double y0, int tip1, unsigned id1,
                  double x1, double y1, unsigned count)
{
    memset(r, 0, 14);
    r[0] = 1;
    r[1] = (AU8)(tip0 ? 1 : 0); r[2] = (AU8)id0;
    Put16(r + 3, (unsigned)(x0 * 4095 + 0.5));
    Put16(r + 5, (unsigned)(y0 * 4095 + 0.5));
    r[7] = (AU8)(tip1 ? 1 : 0); r[8] = (AU8)id1;
    Put16(r + 9, (unsigned)(x1 * 4095 + 0.5));
    Put16(r + 11, (unsigned)(y1 * 4095 + 0.5));
    r[13] = (AU8)count;
}

static void TestDescriptor(void)
{
    AU8 out[128];
    unsigned n = ApReportDescriptor(out, sizeof(out));
    CHECK(n == sizeof(D_TABLET));
    CHECK(memcmp(out, D_TABLET, sizeof(D_TABLET)) == 0);
    CHECK(ApReportDescriptor(out, 10) == 0);
}

static void TestMouse(void)
{
    AP_SOURCE s;
    AP_STATE st;
    AU8 r[16];
    unsigned n;

    CHECK(Parse(D_TABLET, sizeof(D_TABLET), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_MOUSE && !s.UsesIds && s.ReportBytes == 6);
    CHECK(s.X.BitOffset == 8 && s.X.Size == 16 && s.Y.BitOffset == 24);
    CHECK(s.Button[0].Present && s.Button[1].Present && s.Button[2].Present);
    ApStateReset(&st);
    n = MouseRep(r, 15, 0.25, 0.75, 1);
    CHECK(ApTranslate(&s, r, n, &st) == 1);
    CHECK(Near(st.X, 0.25) && Near(st.Y, 0.75) && st.Buttons == AP_BTN_LEFT);
    CHECK(ApTranslate(&s, r, n, &st) == 0);         /* unchanged */
    CHECK(ApTranslate(&s, r, 3, &st) == 0);         /* short report */

    CHECK(Parse(D_KVM16, sizeof(D_KVM16), &s) == AP_OK && s.Kind == AP_KIND_MOUSE);
    CHECK(s.X.LogMax == 65535 && !s.X.Signed);
    n = MouseRep(r, 16, 0.9, 0.1, 2);
    CHECK(ApTranslate(&s, r, n, &st) == 1 && Near(st.X, 0.9) && Near(st.Y, 0.1) && st.Buttons == AP_BTN_RIGHT);

    CHECK(Parse(D_KVM12, sizeof(D_KVM12), &s) == AP_OK && s.Kind == AP_KIND_MOUSE);
    CHECK(s.X.Size == 12 && s.Y.BitOffset == 20 && s.ReportBytes == 5);
    n = MouseRep(r, 12, 0.5, 0.98, 4);
    CHECK(ApTranslate(&s, r, n, &st) == 1 && Near(st.X, 0.5) && Near(st.Y, 0.98) && st.Buttons == AP_BTN_MIDDLE);

    CHECK(Parse(D_POINTER, sizeof(D_POINTER), &s) == AP_OK && s.Kind == AP_KIND_MOUSE);
    CHECK(Parse(D_QEMUTAB, sizeof(D_QEMUTAB), &s) == AP_OK && s.Kind == AP_KIND_MOUSE);
    CHECK(Parse(D_RELMOUSE, sizeof(D_RELMOUSE), &s) == AP_E_NOSOURCE);
    /* absolute X/Y in report 1, relative X/Y in report 2 of the same
     * collection: report 1 is the source */
    CHECK(Parse(D_ABSREL, sizeof(D_ABSREL), &s) == AP_OK && s.Kind == AP_KIND_MOUSE && s.ReportId == 1);
}

static void TestTouch(void)
{
    AP_SOURCE s;
    AP_STATE st;
    AU8 r[16], m[16];
    unsigned n;

    CHECK(Parse(D_TOUCH, sizeof(D_TOUCH), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_TOUCH && s.Tip.Present && s.InRange.Present && !s.UsesIds);
    ApStateReset(&st);
    n = Finger(r, 1, 1, 0.1, 0.2);
    CHECK(ApTranslate(&s, r, n, &st) == 1 && Near(st.X, 0.1) && Near(st.Y, 0.2) && st.Buttons == AP_BTN_LEFT);
    n = Finger(r, 0, 1, 0.8, 0.8);                  /* lifted: no move, no button */
    CHECK(ApTranslate(&s, r, n, &st) == 1 && Near(st.X, 0.1) && st.Buttons == 0);

    /* Both collections: the digitizer is chosen; the mouse report (id 2)
     * is not the source's and changes nothing. */
    CHECK(Parse(D_TOUCHMOUSE, sizeof(D_TOUCHMOUSE), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_TOUCH && s.UsesIds && s.ReportId == 1 && s.ReportBytes == 6);
    r[0] = 1;
    n = 1 + Finger(r + 1, 1, 1, 0.6, 0.4);
    CHECK(ApTranslate(&s, r, n, &st) == 1 && Near(st.X, 0.6) && Near(st.Y, 0.4));
    m[0] = 2;
    n = 1 + MouseRep(m + 1, 15, 0.1, 0.1, 0);
    CHECK(ApTranslate(&s, m, n, &st) == 0 && Near(st.X, 0.6));

    /* Report framing (HID 1.11 8.4): every input report's length is
     * known, the longest sizes the reads, and a transfer holding the
     * unchosen report and then the chosen one splits at the boundary. */
    CHECK(s.InBytes[1] == 6 && s.InBytes[2] == 7 && s.InBytes[3] == 0 && s.MaxInBytes == 7);
    {
        AU8 two[16];
        unsigned off = 0, k, got = 0;
        memcpy(two, m, 7);
        two[7] = 1;
        (void)Finger(two + 8, 1, 1, 0.2, 0.7);
        CHECK(ApReportLength(&s, two, 13) == 7);
        CHECK(ApReportLength(&s, two + 7, 6) == 6);
        CHECK(ApReportLength(&s, two + 7, 4) == 6);    /* longer than what is left */
        while (off < 13 && (k = ApReportLength(&s, two + off, 13 - off)) != 0 && k <= 13 - off) {
            got += ApTranslate(&s, two + off, k, &st);
            off += k;
        }
        CHECK(off == 13 && got == 1 && Near(st.X, 0.2) && Near(st.Y, 0.7) && st.Buttons == 1);
        two[0] = 3;
        CHECK(ApReportLength(&s, two, 13) == 0);
    }

    /* Two contacts: the first one moves the pointer. */
    CHECK(Parse(D_MTOUCH, sizeof(D_MTOUCH), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_TOUCH && s.ReportId == 1 && s.X.BitOffset == 16 && s.ReportBytes == 14);
    CHECK(!s.DevMode.Present);
    CHECK(s.NumSlots == 2 && s.Slot[0].Id.Present && s.Slot[1].Id.Present && s.Count.Present);
    CHECK(s.Slot[1].X.BitOffset == 64 && s.Count.BitOffset == 96);
    ApStateReset(&st);
    /* contact id 5 touches in slot 0 (count 1) */
    MtRep(r, 1, 5, 0.3, 0.7, 0, 0, 0.0, 0.0, 1);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.3) && Near(st.Y, 0.7) && st.Buttons == AP_BTN_LEFT);
    /* a second finger (id 6) lands in slot 0, id 5 moves to slot 1: the
     * pointer stays with id 5 */
    MtRep(r, 1, 6, 0.9, 0.9, 1, 5, 0.4, 0.6, 2);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.4) && Near(st.Y, 0.6) && st.Buttons == AP_BTN_LEFT);
    /* id 5 lifts: released, the other finger does not take over */
    MtRep(r, 1, 6, 0.9, 0.9, 0, 5, 0.4, 0.6, 2);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.4) && st.Buttons == 0);
    /* with nothing followed, the touching contact is taken */
    MtRep(r, 1, 6, 0.8, 0.2, 0, 0, 0.0, 0.0, 1);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.8) && st.Buttons == AP_BTN_LEFT);
    /* id 6 missing from the next frame (count 1, another id) and the
     * frame after: released by omission at the next frame's start */
    MtRep(r, 1, 9, 0.1, 0.1, 0, 0, 0.0, 0.0, 1);
    CHECK(ApTranslate(&s, r, 14, &st) == 0 && st.Buttons == AP_BTN_LEFT);
    /* the release comes alone, at the old position; then id 9 is taken */
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && st.Buttons == 0 && Near(st.X, 0.8));
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && st.Buttons == AP_BTN_LEFT && Near(st.X, 0.1));
    /* hybrid: a frame of 3 contacts in two reports (count 3, then 0); the
     * followed id 9 is in the second report */
    MtRep(r, 1, 1, 0.5, 0.5, 1, 2, 0.5, 0.5, 3);
    CHECK(ApTranslate(&s, r, 14, &st) == 0 && st.Remaining == 1);
    MtRep(r, 1, 9, 0.2, 0.3, 1, 3, 0.9, 0.9, 0);    /* slot 1 is beyond the frame */
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.2) && Near(st.Y, 0.3) && st.Remaining == 0);
    /* null slots beyond the count are not read */
    MtRep(r, 1, 9, 0.25, 0.25, 0, 9, 0.0, 0.0, 1);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && Near(st.X, 0.25) && st.Buttons == AP_BTN_LEFT);
    /* a report cut short in slot 0's X/Y changes nothing, tracking included */
    {
        AP_STATE before = st;
        MtRep(r, 1, 3, 0.6, 0.6, 0, 0, 0.0, 0.0, 1);
        CHECK(ApTranslate(&s, r, 4, &st) == 0);
        CHECK(st.Tracking == before.Tracking && st.TrackId == before.TrackId && st.Seen == before.Seen &&
              st.Remaining == before.Remaining && st.X == before.X);
    }
    /* a count of 0 outside a frame (absemu's lift report): released */
    MtRep(r, 0, 9, 0.25, 0.25, 0, 0, 0.0, 0.0, 0);
    CHECK(ApTranslate(&s, r, 14, &st) == 1 && st.Buttons == 0 && !st.Tracking && Near(st.X, 0.25));
    CHECK(ApTranslate(&s, r, 14, &st) == 0);
}

static void TestPen(void)
{
    AP_SOURCE s;
    AP_STATE st;
    AU8 r[16];

    CHECK(Parse(D_PEN, sizeof(D_PEN), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_PEN && s.Tip.Present && s.Barrel.Present && s.Eraser.Present && s.InRange.Present);
    CHECK(s.X.LogMax == 20000 && s.Y.LogMax == 15000 && s.ReportBytes == 7);
    ApStateReset(&st);
    memset(r, 0, sizeof(r));
    r[0] = 0x10;                                    /* in range, hovering */
    Put16(r + 1, 5000);
    Put16(r + 3, 7500);
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && Near(st.X, 0.25) && Near(st.Y, 0.5) && st.Buttons == 0);
    r[0] = 0x13;                                    /* tip + barrel */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == (AP_BTN_LEFT | AP_BTN_RIGHT));
    r[0] = 0x14;                                    /* eraser touching: a tip */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == AP_BTN_LEFT);
    r[0] = 0x00;                                    /* out of range */
    Put16(r + 1, 20000);
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && Near(st.X, 0.25) && st.Buttons == 0);
}

static void TestPenPartner(void)
{
    AP_SOURCE s;
    AP_STATE st;
    AU8 r[8], mode[4];

    /* QEMU's model descriptor has no absolute X/Y: no generic source. */
    CHECK(Parse(D_PENPARTNER, sizeof(D_PENPARTNER), &s) == AP_E_NOSOURCE);
    ApPenPartnerSource(&s);
    CHECK(s.Kind == AP_KIND_PENPARTNER);
    CHECK(ApPenPartnerModeData(mode, sizeof(mode)) == 2 && mode[0] == 2 && mode[1] == 2);
    mode[0] = 0; mode[1] = 2;
    CHECK(ApPenPartnerModeOk(mode, 2));
    mode[1] = 1;
    CHECK(!ApPenPartnerModeOk(mode, 2));
    ApStateReset(&st);
    r[0] = 2;
    Put16(r + 1, 2520);
    Put16(r + 3, 945);
    r[5] = 0; r[6] = (AU8)(signed char)-127;        /* hovering */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && Near(st.X, 0.5) && Near(st.Y, 0.25) && st.Buttons == 0);
    r[6] = 0;                                       /* touching */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == AP_BTN_LEFT);
    r[5] = 0x40;                                    /* side switch */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == (AP_BTN_LEFT | AP_BTN_RIGHT));
    r[5] = 0x20; r[6] = 0;                          /* eraser touching */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == AP_BTN_LEFT);
    r[0] = 1; r[5] = 0x00;                          /* report 1 out of proximity */
    CHECK(ApTranslate(&s, r, 7, &st) == 1 && st.Buttons == 0);
    r[0] = 0x01; r[1] = 0; r[2] = 0; r[3] = 0;      /* QEMU HID mode: 4 bytes */
    CHECK(ApTranslate(&s, r, 4, &st) == 0);
}

static void TestEdges(void)
{
    AP_SOURCE s;
    AP_STATE st;
    AU8 r[16], f[16];
    unsigned n;

    CHECK(Parse(D_EDGE, sizeof(D_EDGE), &s) == AP_OK);
    CHECK(s.Kind == AP_KIND_PEN && s.UsesIds && s.ReportId == 1);
    CHECK(s.Tip.Present && s.Tip.BitOffset == 0 && s.InRange.BitOffset == 1);
    CHECK(s.X.Signed && s.X.LogMin == -16384 && s.X.LogMax == 16383 && s.X.BitOffset == 8);
    CHECK(s.DevMode.Present && s.DevModeId == 3 && s.DevModeBytes == 3);
    CHECK(ApDevModeValue(&s) == 2);
    f[0] = 3; f[1] = 0; f[2] = 7;                  /* as read: mode 0, identifier 7 */
    CHECK(ApDevModeApply(&s, f, 3) == 1 && f[0] == 3 && f[1] == 2 && f[2] == 7);
    f[1] = 0xff;
    CHECK(ApDevModeApply(&s, f, 3) == 1 && f[1] == 2);
    CHECK(ApDevModeApply(&s, f, 1) == 0);           /* too short */
    f[0] = 4;
    CHECK(ApDevModeApply(&s, f, 3) == 0);           /* another report */
    n = 0;
    ApStateReset(&st);
    r[0] = 1; r[1] = 0x03;                          /* tip, in range */
    Put16(r + 2, 0xC000);                           /* -16384 */
    Put16(r + 4, 0x3FFF);                           /* 16383 */
    CHECK(ApTranslate(&s, r, 6, &st) == 1 && st.X == 0 && st.Y == 32767 && st.Buttons == AP_BTN_LEFT);
    Put16(r + 2, 0);
    CHECK(ApTranslate(&s, r, 6, &st) == 1 && Near(st.X, 0.5));
    r[0] = 2;                                       /* another report id */
    CHECK(ApTranslate(&s, r, 6, &st) == 0);
    r[0] = 1;
    CHECK(ApTranslate(&s, r, 4, &st) == 0);         /* Y beyond the report */

    CHECK(Parse(D_UNBALANCED, sizeof(D_UNBALANCED), &s) == AP_E_BALANCE);
    CHECK(Parse(D_SHORT, sizeof(D_SHORT), &s) == AP_E_SHORT);
    CHECK(Parse(D_TABLET, AP_DESC_MAX + 1, &s) == AP_E_LIMIT);
}

static void TestReport(void)
{
    AP_STATE st;
    AU8 r[8];
    st.X = 0x1234; st.Y = 0x7fff; st.Buttons = 5;
    CHECK(ApBuildReport(&st, r, sizeof(r)) == AP_REPORT_LEN);
    CHECK(r[0] == 5 && r[1] == 0x34 && r[2] == 0x12 && r[3] == 0xff && r[4] == 0x7f && r[5] == 0);
    CHECK(ApBuildReport(&st, r, 5) == 0);
    CHECK(ApWorkSize() <= sizeof(work));
}

int main(void)
{
    TestDescriptor();
    TestMouse();
    TestTouch();
    TestPen();
    TestPenPartner();
    TestEdges();
    TestReport();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
