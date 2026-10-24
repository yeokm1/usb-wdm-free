// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * xpad98: Xbox 360 wired (XUSB) and wireless receiver packets.
 *
 * Adapted from Linux drivers/input/joystick/xpad.c ("Xbox gamepad
 * driver"), torvalds/linux commit a804c5742bff224d4cb4d25dd383554d598be32b:
 * xpad360_process_packet, xpad360w_process_packet, xpad_start_input,
 * xpad_inquiry_pad_presence, xpad_send_led_command,
 * xpad_identify_controller, xpad360w_poweroff_controller and
 * xpad_play_effect. That file's notice:
 *
 * Copyright (c) 2002 Marko Friedemann <mfr@bmx-chemnitz.de>
 *               2004 Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *                    Steven Toth <steve@toth.demon.co.uk>,
 *                    Franz Lehner <franz@caos.at>,
 *                    Ivan Hawkes <blackhawk@ivanhawkes.com>
 *               2005 Dominic Cerquetti <binary1230@yahoo.com>
 *               2006 Adam Buchbinder <adam.buchbinder@gmail.com>
 *               2007 Jan Kratochvil <honza@jikos.cz>
 *               2010 Christoph Fritz <chf.fritz@googlemail.com>
 *
 * Changes for xpad98 (C) 2026 Yeo Kheng Meng: the Linux input events
 * become a normalised XP_PAD state; packet builders write caller
 * buffers; lengths are checked before any byte is read.
 */
#include "xpcore.h"

static XS16 Le16(const XU8 *p)
{
    return (XS16)(XU16)(p[0] | ((XU16)p[1] << 8));
}

/* Linux reports ~value for Y so that up is negative. */
static XS16 Inv(XS16 v)
{
    return (XS16)~v;
}

void XpPadReset(XP_PAD *pad, XU16 triggerMax)
{
    pad->LX = pad->LY = pad->RX = pad->RY = 0;
    pad->LT = pad->RT = 0;
    pad->TriggerMax = triggerMax;
    pad->Buttons = 0;
    pad->Dpad = 0;
    pad->Present = 0;
}

int XpPadEqual(const XP_PAD *a, const XP_PAD *b)
{
    return a->LX == b->LX && a->LY == b->LY && a->RX == b->RX &&
        a->RY == b->RY && a->LT == b->LT && a->RT == b->RT &&
        a->TriggerMax == b->TriggerMax && a->Buttons == b->Buttons &&
        a->Dpad == b->Dpad && a->Present == b->Present;
}

/* The XUSB pad report: data[0] 0x00 marks pad data, 20 bytes long; this
 * reads up to data[13]. */
#define XUSB_MIN 14

static void X360Body(const XU8 *data, XP_PAD *pad)
{
    XU16 b = 0;
    pad->Dpad = (XU8)(data[2] & 0x0F);
    if (data[2] & 0x10) b |= XP_B_START;
    if (data[2] & 0x20) b |= XP_B_BACK;
    if (data[2] & 0x40) b |= XP_B_LS;
    if (data[2] & 0x80) b |= XP_B_RS;
    if (data[3] & 0x10) b |= XP_B_A;
    if (data[3] & 0x20) b |= XP_B_B;
    if (data[3] & 0x40) b |= XP_B_X;
    if (data[3] & 0x80) b |= XP_B_Y;
    if (data[3] & 0x01) b |= XP_B_LB;
    if (data[3] & 0x02) b |= XP_B_RB;
    if (data[3] & 0x04) b |= XP_B_GUIDE;
    pad->Buttons = b;
    pad->LT = data[4];
    pad->RT = data[5];
    pad->TriggerMax = 255;
    pad->LX = Le16(data + 6);
    pad->LY = Inv(Le16(data + 8));
    pad->RX = Le16(data + 10);
    pad->RY = Inv(Le16(data + 12));
}

unsigned XpX360Parse(const XU8 *data, unsigned len, XP_PAD *pad)
{
    if (len < XUSB_MIN || data[0] != 0x00)
        return 0;
    X360Body(data, pad);
    pad->Present = 1;
    return XP_EV_DATA;
}

/*
 * The receiver's envelope (xpad360w_process_packet):
 *  00.3 - status change: 01.7 (pad present) is valid
 *  01.7 - controller present
 *  01.0 with data[1] == 0x01 - pad state follows at byte 4
 */
unsigned XpX360wParse(const XU8 *data, unsigned len, XP_PAD *pad)
{
    unsigned ev = 0;
    if (len < 2)
        return 0;
    if (data[0] & 0x08) {
        XU8 present = (XU8)((data[1] & 0x80) != 0);
        if (present != pad->Present) {
            if (present) {
                ev |= XP_EV_CONNECT;
            } else {
                ev |= XP_EV_DISCONNECT;
                XpPadReset(pad, 255);       /* centred, nothing pressed */
            }
            pad->Present = present;
        }
    }
    if (data[1] != 0x01 || len < 4 + XUSB_MIN || data[4] != 0x00)
        return ev;
    if (!pad->Present)
        return ev;      /* Linux drops data while no input device exists */
    X360Body(data + 4, pad);
    return ev | XP_EV_DATA;
}

/* xpad_send_led_command, XTYPE_XBOX360: 01 03 cc */
unsigned XpX360Led(XU8 *out, unsigned command)
{
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = (XU8)(command % 16);
    return 3;
}

static unsigned Zero12(XU8 *out)
{
    unsigned i;
    for (i = 0; i < 12; i++)
        out[i] = 0;
    return 12;
}

/* xpad_send_led_command, XTYPE_XBOX360W: 00 00 08 (40+cc), 12 bytes */
unsigned XpX360wLed(XU8 *out, unsigned command)
{
    Zero12(out);
    out[2] = 0x08;
    out[3] = (XU8)(0x40 + command % 16);
    return 12;
}

/* xpad_inquiry_pad_presence: 08 00 0F C0, 12 bytes */
unsigned XpX360wPresence(XU8 *out)
{
    Zero12(out);
    out[0] = 0x08;
    out[2] = 0x0F;
    out[3] = 0xC0;
    return 12;
}

/* xpad360w_poweroff_controller: 00 00 08 C0, 12 bytes */
unsigned XpX360wPowerOff(XU8 *out)
{
    Zero12(out);
    out[2] = 0x08;
    out[3] = 0xC0;
    return 12;
}

/* xpad_play_effect, XTYPE_XBOX360: 00 08 00 SS WW 00 00 00 */
unsigned XpX360Rumble(XU8 *out, XU8 strong, XU8 weak)
{
    unsigned i;
    for (i = 0; i < 8; i++)
        out[i] = 0;
    out[1] = 0x08;
    out[3] = strong;
    out[4] = weak;
    return 8;
}

/* xpad_play_effect, XTYPE_XBOX360W: 00 01 0F C0 00 SS WW, 12 bytes */
unsigned XpX360wRumble(XU8 *out, XU8 strong, XU8 weak)
{
    Zero12(out);
    out[1] = 0x01;
    out[2] = 0x0F;
    out[3] = 0xC0;
    out[5] = strong;
    out[6] = weak;
    return 12;
}

/* xpad_identify_controller: the segment for pad number n, blink then on. */
unsigned XpLedForPlayer(unsigned player)
{
    return (player % 4) + 2;
}
