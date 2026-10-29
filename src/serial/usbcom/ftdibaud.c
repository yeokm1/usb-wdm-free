/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftdibaud.c - the FTDI baud rate divisors of usbcom's FTDI chip module
 * (docs/design/serial.md 6.3). Adapted from Linux v6.12
 * drivers/usb/serial/ftdi_sio.c (commit adc218676eef), the functions
 * ftdi_232am_baud_base_to_divisor, ftdi_232bm_baud_base_to_divisor and
 * ftdi_2232h_baud_base_to_divisor, which that file licenses under
 * GPL-2.0-or-later. Changed here: C89 types of this package, the clocks
 * passed in by the caller, no kernel helpers. NOTICE.md lists this file.
 *
 * Notices of the original file:
 *
 *    Copyright (C) 2009 - 2013
 *        Johan Hovold (jhovold@gmail.com)
 *    Copyright (C) 1999 - 2001
 *        Greg Kroah-Hartman (greg@kroah.com)
 *          Bill Ryder (bryder@sgi.com)
 *    Copyright (C) 2002
 *        Kuba Ober (kuba@mareimbrium.org)
 *
 * Changes Copyright (C) 2026 Yeo Kheng Meng
 */
#include "chipcore.h"

/* DIV_ROUND_CLOSEST for the positive values used here. */
static AC_U32 FtRound(AC_U32 n, AC_U32 d)
{
    return (n + d / 2) / d;
}

AC_U32 AcFtdi232amDivisor(AC_U32 baud, AC_U32 base)
{
    AC_U32 divisor;
    /* divisor shifted 3 bits to the left */
    AC_U32 divisor3 = FtRound(base, 2 * baud);
    if ((divisor3 & 0x7) == 7)
        divisor3++;                 /* round x.7/8 up to x+1 */
    divisor = divisor3 >> 3;
    divisor3 &= 0x7;
    if (divisor3 == 1)
        divisor |= 0xc000;          /* +0.125 */
    else if (divisor3 >= 4)
        divisor |= 0x4000;          /* +0.5 */
    else if (divisor3 != 0)
        divisor |= 0x8000;          /* +0.25 */
    else if (divisor == 1)
        divisor = 0;                /* special case for maximum baud rate */
    return divisor & 0xFFFF;
}

static const AC_U8 g_DivFrac[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };

AC_U32 AcFtdi232bmDivisor(AC_U32 baud, AC_U32 base)
{
    AC_U32 divisor;
    /* divisor shifted 3 bits to the left */
    AC_U32 divisor3 = FtRound(base, 2 * baud);
    divisor = divisor3 >> 3;
    divisor |= (AC_U32)g_DivFrac[divisor3 & 0x7] << 14;
    /* Deal with special cases for highest baud rates. */
    if (divisor == 1)               /* 1.0 */
        divisor = 0;
    else if (divisor == 0x4001)     /* 1.5 */
        divisor = 1;
    return divisor;
}

AC_U32 AcFtdi2232hDivisor(AC_U32 baud, AC_U32 base)
{
    AC_U32 divisor;
    AC_U32 divisor3;

    /* hi-speed baud rate is 10-bit sampling instead of 16-bit */
    divisor3 = FtRound(8 * (base / 10), baud);

    divisor = divisor3 >> 3;
    divisor |= (AC_U32)g_DivFrac[divisor3 & 0x7] << 14;
    /* Deal with special cases for highest baud rates. */
    if (divisor == 1)               /* 1.0 */
        divisor = 0;
    else if (divisor == 0x4001)     /* 1.5 */
        divisor = 1;
    /*
     * Set this bit to turn off a divide by 2.5 on baud rate generator
     * This enables baud rates up to 12Mbaud but cannot reach below 1200
     * baud with this bit set
     */
    divisor |= 0x00020000;
    return divisor;
}
