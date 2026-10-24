// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * xpad98: Xbox One and Series pads (GIP) - init sequence and input.
 *
 * Adapted from Linux drivers/input/joystick/xpad.c ("Xbox gamepad
 * driver"), torvalds/linux commit a804c5742bff224d4cb4d25dd383554d598be32b:
 * the GIP_* constants, the xboxone_* init packets, xboxone_init_packets[],
 * xpad_prepare_next_init_packet, xpadone_process_packet and
 * xpadone_ack_mode_report. That file's notice:
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
 * Linux credits the GIP magic numbers to xpadneo's gip-dissector and to
 * medusalix's xone, and the report format to kylelemons/xbox.
 *
 * Changes for xpad98 (C) 2026 Yeo Kheng Meng: an iterator over the init
 * table instead of the Linux output ring; input becomes a normalised
 * XP_PAD; lengths are checked before any byte is read. Elite paddles,
 * the Share button and the profile button are not decoded (design 5.4).
 */
#include "xpcore.h"

#define GIP_CMD_ACK          0x01
#define GIP_CMD_ANNOUNCE     0x02
#define GIP_CMD_IDENTIFY     0x04
#define GIP_CMD_POWER        0x05
#define GIP_CMD_AUTHENTICATE 0x06
#define GIP_CMD_VIRTUAL_KEY  0x07
#define GIP_CMD_RUMBLE       0x09
#define GIP_CMD_LED          0x0a
#define GIP_CMD_FIRMWARE     0x0c
#define GIP_CMD_INPUT        0x20

#define GIP_SEQ0 0x00
#define GIP_OPT_ACK      0x10
#define GIP_OPT_INTERNAL 0x20
#define GIP_PL_LEN(N) (N)
#define GIP_PWR_ON 0x00
#define GIP_LED_ON 0x01
#define GIP_MOTOR_ALL 0x0F

/* Required for all Xbox One pads with 2015 or later firmware. */
static const XU8 xboxone_power_on[] = {
    GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(1), GIP_PWR_ON
};

/* Xbox One S (045e:02ea) and Elite Series 2 (045e:0b00) after Bluetooth. */
static const XU8 xboxone_s_init[] = {
    GIP_CMD_POWER, GIP_OPT_INTERNAL, GIP_SEQ0, 0x0f, 0x06
};

/* Elite Series 2: additional input data (paddles). */
static const XU8 extra_input_packet_init[] = {
    0x4d, 0x10, 0x01, 0x02, 0x07, 0x00
};

/* Titanfall 2 (0e6f:0165) and HORIPAD ONE (0f0d:0067). */
static const XU8 xboxone_hori_ack_id[] = {
    GIP_CMD_ACK, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(9),
    0x00, GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0x3a, 0x00, 0x00, 0x00, 0x80, 0x00
};

/* Sent by default on Windows; some pads (most PDP) need it for input. */
static const XU8 xboxone_led_on[] = {
    GIP_CMD_LED, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(3), 0x00, GIP_LED_ON, 0x14
};

/* Most PDP pads need it to start sending input. */
static const XU8 xboxone_auth_done[] = {
    GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(2), 0x01, 0x00
};

/* Some PowerA pads (24c6:543a) need a rumble to start sending input... */
static const XU8 xboxone_rumblebegin_init[] = {
    GIP_CMD_RUMBLE, 0x00, GIP_SEQ0, GIP_PL_LEN(9),
    0x00, GIP_MOTOR_ALL, 0x00, 0x00, 0x1D, 0x1D, 0xFF, 0x00, 0x00
};

/* ...stopped at once, before the motors spin up. */
static const XU8 xboxone_rumbleend_init[] = {
    GIP_CMD_RUMBLE, 0x00, GIP_SEQ0, GIP_PL_LEN(9),
    0x00, GIP_MOTOR_ALL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

typedef struct GIP_INIT {
    XU16 Vid, Pid;          /* 0 matches any */
    const XU8 *Data;
    XU8 Len;
} GIP_INIT;

#define PKT(v, p, d) { (v), (p), (d), (XU8)sizeof(d) }

/* Linux xboxone_init_packets[]: which packets, and in what order. */
static const GIP_INIT InitPackets[] = {
    PKT(0x0e6f, 0x0165, xboxone_hori_ack_id),
    PKT(0x0f0d, 0x0067, xboxone_hori_ack_id),
    PKT(0x0000, 0x0000, xboxone_power_on),
    PKT(0x045e, 0x02ea, xboxone_s_init),
    PKT(0x045e, 0x0b00, xboxone_s_init),
    PKT(0x045e, 0x0b00, extra_input_packet_init),
    PKT(0x0000, 0x0000, xboxone_led_on),
    PKT(0x0000, 0x0000, xboxone_auth_done),
    PKT(0x24c6, 0x541a, xboxone_rumblebegin_init),
    PKT(0x24c6, 0x542a, xboxone_rumblebegin_init),
    PKT(0x24c6, 0x543a, xboxone_rumblebegin_init),
    PKT(0x24c6, 0x541a, xboxone_rumbleend_init),
    PKT(0x24c6, 0x542a, xboxone_rumbleend_init),
    PKT(0x24c6, 0x543a, xboxone_rumbleend_init),
};

#define NUM_INIT (sizeof(InitPackets) / sizeof(InitPackets[0]))

unsigned XpGipNextInit(XU16 vid, XU16 pid, unsigned *cursor, XU8 *out)
{
    while (*cursor < NUM_INIT) {
        const GIP_INIT *p = &InitPackets[(*cursor)++];
        unsigned i;
        if (p->Vid != 0 && p->Vid != vid)
            continue;
        if (p->Pid != 0 && p->Pid != pid)
            continue;
        for (i = 0; i < p->Len; i++)
            out[i] = p->Data[i];
        return p->Len;
    }
    return 0;
}

static XU16 Le16u(const XU8 *p)
{
    return (XU16)(p[0] | ((XU16)p[1] << 8));
}

/* GIP_CMD_INPUT reads up to data[17]. */
#define GIP_INPUT_MIN 18

unsigned XpGipParse(const XU8 *data, unsigned len, XP_PAD *pad,
                    XU8 *ack, unsigned *ackLen)
{
    *ackLen = 0;
    if (len < 4)
        return 0;

    if (data[0] == GIP_CMD_VIRTUAL_KEY) {
        unsigned ev = 0;
        if (len < 5)
            return 0;
        /* The pad repeats the report, and stops reporting Guide, until it
         * is acknowledged (xpadone_ack_mode_report). */
        if (data[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
            static const XU8 mode_report_ack[] = {
                GIP_CMD_ACK, GIP_OPT_INTERNAL, GIP_SEQ0, GIP_PL_LEN(9),
                0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02,
                0x00, 0x00, 0x00, 0x00, 0x00
            };
            unsigned i;
            for (i = 0; i < sizeof(mode_report_ack); i++)
                ack[i] = mode_report_ack[i];
            ack[2] = data[2];
            *ackLen = sizeof(mode_report_ack);
            ev |= XP_EV_ACK;
        }
        if (data[4] & 0x03)
            pad->Buttons |= XP_B_GUIDE;
        else
            pad->Buttons &= (XU16)~XP_B_GUIDE;
        pad->Present = 1;
        return ev | XP_EV_DATA;
    }

    if (data[0] == GIP_CMD_ANNOUNCE)
        return XP_EV_ANNOUNCE;

    if (data[0] == GIP_CMD_INPUT) {
        XU16 b;
        if (len < GIP_INPUT_MIN)
            return 0;
        b = (XU16)(pad->Buttons & XP_B_GUIDE);  /* Guide has its own report */
        if (data[4] & 0x04) b |= XP_B_START;    /* Menu */
        if (data[4] & 0x08) b |= XP_B_BACK;     /* View */
        if (data[4] & 0x10) b |= XP_B_A;
        if (data[4] & 0x20) b |= XP_B_B;
        if (data[4] & 0x40) b |= XP_B_X;
        if (data[4] & 0x80) b |= XP_B_Y;
        if (data[5] & 0x10) b |= XP_B_LB;
        if (data[5] & 0x20) b |= XP_B_RB;
        if (data[5] & 0x40) b |= XP_B_LS;
        if (data[5] & 0x80) b |= XP_B_RS;
        pad->Buttons = b;
        pad->Dpad = (XU8)(data[5] & 0x0F);  /* up, down, left, right */
        pad->LT = Le16u(data + 6);
        pad->RT = Le16u(data + 8);
        if (pad->LT > 1023) pad->LT = 1023;
        if (pad->RT > 1023) pad->RT = 1023;
        pad->TriggerMax = 1023;
        pad->LX = (XS16)Le16u(data + 10);
        pad->LY = (XS16)~(XS16)Le16u(data + 12);
        pad->RX = (XS16)Le16u(data + 14);
        pad->RY = (XS16)~(XS16)Le16u(data + 16);
        pad->Present = 1;
        return XP_EV_DATA;
    }

    /* GIP_CMD_FIRMWARE carries Elite 2 paddles only; not decoded. */
    return 0;
}
