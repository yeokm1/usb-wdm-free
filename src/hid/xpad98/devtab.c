// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * xpad98: the pads this driver knows by VID/PID.
 *
 * The Xbox 360 (XTYPE_XBOX360), receiver (XTYPE_XBOX360W) and Xbox One
 * (XTYPE_XBOXONE) rows of xpad_device[] in Linux
 * drivers/input/joystick/xpad.c ("Xbox gamepad driver"), torvalds/linux
 * commit a804c5742bff224d4cb4d25dd383554d598be32b, with their names,
 * MAP_TRIGGERS_TO_BUTTONS, MAP_DPAD_TO_BUTTONS (not for the receiver,
 * where Linux reports a hat anyway) and FLAG_DELAY_INIT. That file's
 * notice:
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
 * Changes for xpad98 (C) 2026 Yeo Kheng Meng: the original Xbox (XID)
 * rows are dropped (not planned, design section 1); keyboards and
 * keypads with an XInput function (Wooting, Azeron) are marked
 * XP_F_NOINF, because a whole-device INF match would take their keyboard
 * away too (design 5.1); the table of the 25 ids Masahiko Morii's x360c
 * matches (design 5.1, from the owner's check of x360c's final builds)
 * is this project's own.
 */
#include "xpcore.h"

/* Sorted by VID, PID. */
static const XP_DEVICE Devices[] = {
    { 0x0079, 0x18D4, XP_FAM_X360, 0, "GPD Win 2 X-Box Controller" },
    { 0x0351, 0x1000, XP_FAM_X360, 0, "CRKD LP Blueberry Burst Pro Edition (Xbox)" },
    { 0x0351, 0x2000, XP_FAM_X360, 0, "CRKD LP Black Tribal Edition (Xbox) " },
    { 0x03EB, 0xFF01, XP_FAM_X360, XP_F_NOINF, "Wooting One (Legacy)" },
    { 0x03EB, 0xFF02, XP_FAM_X360, XP_F_NOINF, "Wooting Two (Legacy)" },
    { 0x03F0, 0x038D, XP_FAM_X360, 0, "HyperX Clutch" },
    { 0x03F0, 0x048D, XP_FAM_X360, 0, "HyperX Clutch" },
    { 0x03F0, 0x0495, XP_FAM_GIP, 0, "HyperX Clutch Gladiate" },
    { 0x03F0, 0x07A0, XP_FAM_GIP, 0, "HyperX Clutch Gladiate RGB" },
    { 0x03F0, 0x08B6, XP_FAM_GIP, 0, "HyperX Clutch Gladiate" },
    { 0x03F0, 0x09B4, XP_FAM_GIP, 0, "HyperX Clutch Tanto" },
    { 0x044F, 0xB326, XP_FAM_X360, 0, "Thrustmaster Gamepad GP XID" },
    { 0x044F, 0xD01E, XP_FAM_GIP, 0, "ThrustMaster, Inc. ESWAP X 2 ELDEN RING EDITION" },
    { 0x045E, 0x028E, XP_FAM_X360, 0, "Microsoft X-Box 360 pad" },
    { 0x045E, 0x028F, XP_FAM_X360, 0, "Microsoft X-Box 360 pad v2" },
    { 0x045E, 0x0291, XP_FAM_X360W, 0, "Xbox 360 Wireless Receiver (XBOX)" },
    { 0x045E, 0x02A9, XP_FAM_X360W, 0, "Xbox 360 Wireless Receiver (Unofficial)" },
    { 0x045E, 0x02D1, XP_FAM_GIP, 0, "Microsoft X-Box One pad" },
    { 0x045E, 0x02DD, XP_FAM_GIP, 0, "Microsoft X-Box One pad (Firmware 2015)" },
    { 0x045E, 0x02E3, XP_FAM_GIP, 0, "Microsoft X-Box One Elite pad" },
    { 0x045E, 0x02EA, XP_FAM_GIP, 0, "Microsoft X-Box One S pad" },
    { 0x045E, 0x0719, XP_FAM_X360W, 0, "Xbox 360 Wireless Receiver" },
    { 0x045E, 0x0B00, XP_FAM_GIP, 0, "Microsoft X-Box One Elite 2 pad" },
    { 0x045E, 0x0B0A, XP_FAM_GIP, 0, "Microsoft X-Box Adaptive Controller" },
    { 0x045E, 0x0B12, XP_FAM_GIP, 0, "Microsoft Xbox Series S|X Controller" },
    { 0x046D, 0xC21D, XP_FAM_X360, 0, "Logitech Gamepad F310" },
    { 0x046D, 0xC21E, XP_FAM_X360, 0, "Logitech Gamepad F510" },
    { 0x046D, 0xC21F, XP_FAM_X360, 0, "Logitech Gamepad F710" },
    { 0x046D, 0xC242, XP_FAM_X360, 0, "Logitech Chillstream Controller" },
    { 0x046D, 0xCAA3, XP_FAM_X360, 0, "Logitech DriveFx Racing Wheel" },
    { 0x0502, 0x1305, XP_FAM_X360, 0, "Acer NGR200" },
    { 0x056E, 0x2004, XP_FAM_X360, 0, "Elecom JC-U3613M" },
    { 0x06A3, 0xF51A, XP_FAM_X360, 0, "Saitek P3600" },
    { 0x0738, 0x4503, XP_FAM_GIP, 0, "Mad Catz Racing Wheel" },
    { 0x0738, 0x4716, XP_FAM_X360, 0, "Mad Catz Wired Xbox 360 Controller" },
    { 0x0738, 0x4718, XP_FAM_X360, 0, "Mad Catz Street Fighter IV FightStick SE" },
    { 0x0738, 0x4726, XP_FAM_X360, 0, "Mad Catz Xbox 360 Controller" },
    { 0x0738, 0x4728, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Street Fighter IV FightPad" },
    { 0x0738, 0x4736, XP_FAM_X360, 0, "Mad Catz MicroCon Gamepad" },
    { 0x0738, 0x4738, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Wired Xbox 360 Controller (SFIV)" },
    { 0x0738, 0x4740, XP_FAM_X360, 0, "Mad Catz Beat Pad" },
    { 0x0738, 0x4758, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Arcade Game Stick" },
    { 0x0738, 0x4A01, XP_FAM_GIP, XP_F_TRIGBTN, "Mad Catz FightStick TE 2" },
    { 0x0738, 0x9871, XP_FAM_X360, 0, "Mad Catz Portable Drum" },
    { 0x0738, 0xB726, XP_FAM_X360, 0, "Mad Catz Xbox controller - MW2" },
    { 0x0738, 0xB738, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz MVC2TE Stick 2" },
    { 0x0738, 0xBEEF, XP_FAM_X360, 0, "Mad Catz JOYTECH NEO SE Advanced GamePad" },
    { 0x0738, 0xCB02, XP_FAM_X360, 0, "Saitek Cyborg Rumble Pad - PC/Xbox 360" },
    { 0x0738, 0xCB03, XP_FAM_X360, 0, "Saitek P3200 Rumble Pad - PC/Xbox 360" },
    { 0x0738, 0xCB29, XP_FAM_X360, 0, "Saitek Aviator Stick AV8R02" },
    { 0x0738, 0xF738, XP_FAM_X360, 0, "Super SFIV FightStick TE S" },
    { 0x07FF, 0xFFFF, XP_FAM_X360, 0, "Mad Catz GamePad" },
    { 0x0B05, 0x1A38, XP_FAM_GIP, 0, "ASUS ROG RAIKIRI" },
    { 0x0B05, 0x1ABB, XP_FAM_GIP, 0, "ASUS ROG RAIKIRI PRO" },
    { 0x0B05, 0x1C91, XP_FAM_X360, 0, "ASUS ROG RAIKIRI II" },
    { 0x0B05, 0x1C92, XP_FAM_X360, 0, "ASUS ROG RAIKIRI II WIRELESS" },
    { 0x0B05, 0x1C96, XP_FAM_GIP, 0, "ASUS ROG RAIKIRI II XBOX" },
    { 0x0B05, 0x1D04, XP_FAM_GIP, 0, "ASUS ROG RAIKIRI II XBOX WIRELESS" },
    { 0x0DB0, 0x1901, XP_FAM_X360, 0, "Micro Star International Xbox360 Controller for Windows" },
    { 0x0E6F, 0x0105, XP_FAM_X360, XP_F_DPADBTN, "HSM3 Xbox360 dancepad" },
    { 0x0E6F, 0x0113, XP_FAM_X360, 0, "Afterglow AX.1 Gamepad for Xbox 360" },
    { 0x0E6F, 0x011F, XP_FAM_X360, 0, "Rock Candy Gamepad Wired Controller" },
    { 0x0E6F, 0x0131, XP_FAM_X360, 0, "PDP EA Sports Controller" },
    { 0x0E6F, 0x0133, XP_FAM_X360, 0, "Xbox 360 Wired Controller" },
    { 0x0E6F, 0x0139, XP_FAM_GIP, 0, "Afterglow Prismatic Wired Controller" },
    { 0x0E6F, 0x013A, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x0146, XP_FAM_GIP, 0, "Rock Candy Wired Controller for Xbox One" },
    { 0x0E6F, 0x0147, XP_FAM_X360, 0, "PDP Marvel Xbox 360 Controller" },
    { 0x0E6F, 0x015C, XP_FAM_GIP, XP_F_TRIGBTN, "PDP Xbox One Arcade Stick" },
    { 0x0E6F, 0x015D, XP_FAM_GIP, 0, "PDP Mirror's Edge Official Wired Controller for Xbox One" },
    { 0x0E6F, 0x0161, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x0162, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x0163, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x0164, XP_FAM_GIP, 0, "PDP Battlefield One" },
    { 0x0E6F, 0x0165, XP_FAM_GIP, 0, "PDP Titanfall 2" },
    { 0x0E6F, 0x0201, XP_FAM_X360, 0, "Pelican PL-3601 'TSZ' Wired Xbox 360 Controller" },
    { 0x0E6F, 0x0213, XP_FAM_X360, 0, "Afterglow Gamepad for Xbox 360" },
    { 0x0E6F, 0x021F, XP_FAM_X360, 0, "Rock Candy Gamepad for Xbox 360" },
    { 0x0E6F, 0x0246, XP_FAM_GIP, 0, "Rock Candy Gamepad for Xbox One 2015" },
    { 0x0E6F, 0x024C, XP_FAM_GIP, 0, "PDP Victrix Pro BFG Wired Controller for Xbox" },
    { 0x0E6F, 0x02A0, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x02A1, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x02A2, XP_FAM_GIP, 0, "PDP Wired Controller for Xbox One - Crimson Red" },
    { 0x0E6F, 0x02A4, XP_FAM_GIP, 0, "PDP Wired Controller for Xbox One - Stealth Series" },
    { 0x0E6F, 0x02A6, XP_FAM_GIP, 0, "PDP Wired Controller for Xbox One - Camo Series" },
    { 0x0E6F, 0x02A7, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x02A8, XP_FAM_GIP, 0, "PDP Xbox One Controller" },
    { 0x0E6F, 0x02AB, XP_FAM_GIP, 0, "PDP Controller for Xbox One" },
    { 0x0E6F, 0x02AD, XP_FAM_GIP, 0, "PDP Wired Controller for Xbox One - Stealth Series" },
    { 0x0E6F, 0x02B3, XP_FAM_GIP, 0, "Afterglow Prismatic Wired Controller" },
    { 0x0E6F, 0x02B8, XP_FAM_GIP, 0, "Afterglow Prismatic Wired Controller" },
    { 0x0E6F, 0x0301, XP_FAM_X360, 0, "Logic3 Controller" },
    { 0x0E6F, 0x0346, XP_FAM_GIP, 0, "Rock Candy Gamepad for Xbox One 2016" },
    { 0x0E6F, 0x0401, XP_FAM_X360, 0, "Logic3 Controller" },
    { 0x0E6F, 0x0413, XP_FAM_X360, 0, "Afterglow AX.1 Gamepad for Xbox 360" },
    { 0x0E6F, 0x0501, XP_FAM_X360, 0, "PDP Xbox 360 Controller" },
    { 0x0E6F, 0xF900, XP_FAM_X360, 0, "PDP Afterglow AX.1" },
    { 0x0F0D, 0x000A, XP_FAM_X360, 0, "Hori Co. DOA4 FightStick" },
    { 0x0F0D, 0x000C, XP_FAM_X360, 0, "Hori PadEX Turbo" },
    { 0x0F0D, 0x000D, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Stick EX2" },
    { 0x0F0D, 0x0016, XP_FAM_X360, XP_F_TRIGBTN, "Hori Real Arcade Pro.EX" },
    { 0x0F0D, 0x001B, XP_FAM_X360, XP_F_TRIGBTN, "Hori Real Arcade Pro VX" },
    { 0x0F0D, 0x0063, XP_FAM_GIP, XP_F_TRIGBTN, "Hori Real Arcade Pro Hayabusa (USA) Xbox One" },
    { 0x0F0D, 0x0067, XP_FAM_GIP, 0, "HORIPAD ONE" },
    { 0x0F0D, 0x0078, XP_FAM_GIP, XP_F_TRIGBTN, "Hori Real Arcade Pro V Kai Xbox One" },
    { 0x0F0D, 0x00C5, XP_FAM_GIP, XP_F_TRIGBTN, "Hori Fighting Commander ONE" },
    { 0x0F0D, 0x00DC, XP_FAM_X360, XP_F_TRIGBTN, "HORIPAD FPS for Nintendo Switch" },
    { 0x0F0D, 0x0151, XP_FAM_GIP, 0, "Hori Racing Wheel Overdrive for Xbox Series X" },
    { 0x0F0D, 0x0152, XP_FAM_GIP, 0, "Hori Racing Wheel Overdrive for Xbox Series X" },
    { 0x0F0D, 0x01B2, XP_FAM_GIP, 0, "HORI Taiko No Tatsujin Drum Controller" },
    { 0x1038, 0x1430, XP_FAM_X360, 0, "SteelSeries Stratus Duo" },
    { 0x1038, 0x1431, XP_FAM_X360, 0, "SteelSeries Stratus Duo" },
    { 0x10F5, 0x7005, XP_FAM_GIP, 0, "Turtle Beach Recon Controller" },
    { 0x10F5, 0x7008, XP_FAM_GIP, 0, "Turtle Beach Recon Controller" },
    { 0x10F5, 0x7073, XP_FAM_GIP, 0, "Turtle Beach Stealth Ultra Controller" },
    { 0x11C9, 0x55F0, XP_FAM_X360, 0, "Nacon GC-100XF" },
    { 0x11FF, 0x0511, XP_FAM_X360, 0, "PXN V900" },
    { 0x1209, 0x2882, XP_FAM_X360, 0, "Ardwiino Controller" },
    { 0x12AB, 0x0004, XP_FAM_X360, XP_F_DPADBTN, "Honey Bee Xbox360 dancepad" },
    { 0x12AB, 0x0301, XP_FAM_X360, 0, "PDP AFTERGLOW AX.1" },
    { 0x12AB, 0x0303, XP_FAM_X360, XP_F_TRIGBTN, "Mortal Kombat Klassic FightStick" },
    { 0x1430, 0x4748, XP_FAM_X360, 0, "RedOctane Guitar Hero X-plorer" },
    { 0x1430, 0xF801, XP_FAM_X360, 0, "RedOctane Controller" },
    { 0x146B, 0x0601, XP_FAM_X360, 0, "BigBen Interactive XBOX 360 Controller" },
    { 0x146B, 0x0604, XP_FAM_X360, XP_F_TRIGBTN, "Bigben Interactive DAIJA Arcade Stick" },
    { 0x1532, 0x0A00, XP_FAM_GIP, XP_F_TRIGBTN, "Razer Atrox Arcade Stick" },
    { 0x1532, 0x0A03, XP_FAM_GIP, 0, "Razer Wildcat" },
    { 0x1532, 0x0A29, XP_FAM_GIP, 0, "Razer Wolverine V2" },
    { 0x1532, 0x0A57, XP_FAM_X360, 0, "Razer Wolverine V3 Pro (Wired)" },
    { 0x1532, 0x0A59, XP_FAM_X360, 0, "Razer Wolverine V3 Pro (2.4 GHz Dongle)" },
    { 0x15E4, 0x3F00, XP_FAM_X360, 0, "Power A Mini Pro Elite" },
    { 0x15E4, 0x3F0A, XP_FAM_X360, 0, "Xbox Airflo wired controller" },
    { 0x15E4, 0x3F10, XP_FAM_X360, 0, "Batarang Xbox 360 controller" },
    { 0x162E, 0xBEEF, XP_FAM_X360, 0, "Joytech Neo-Se Take2" },
    { 0x1689, 0xFD00, XP_FAM_X360, 0, "Razer Onza Tournament Edition" },
    { 0x1689, 0xFD01, XP_FAM_X360, 0, "Razer Onza Classic Edition" },
    { 0x1689, 0xFE00, XP_FAM_X360, 0, "Razer Sabertooth" },
    { 0x16D0, 0x1103, XP_FAM_X360, XP_F_NOINF, "Azeron Cyro" },
    { 0x16D0, 0x113C, XP_FAM_X360, XP_F_NOINF, "Azeron Cyborg" },
    { 0x16D0, 0x1192, XP_FAM_X360, XP_F_NOINF, "Azeron Classic/Compact" },
    { 0x16D0, 0x1212, XP_FAM_X360, XP_F_NOINF, "Azeron Cyro Lefty" },
    { 0x16D0, 0x12F7, XP_FAM_X360, XP_F_NOINF, "Azeron Cyborg II" },
    { 0x16D0, 0x13EA, XP_FAM_X360, XP_F_NOINF, "Azeron Keyzen" },
    { 0x17EF, 0x6182, XP_FAM_X360, 0, "Lenovo Legion Controller for Windows" },
    { 0x1949, 0x041A, XP_FAM_X360, 0, "Amazon Game Controller" },
    { 0x1A86, 0xE310, XP_FAM_X360, 0, "Legion Go S" },
    { 0x1B1C, 0x2B1F, XP_FAM_X360, 0, "Corsair Novablade Pro (Wired)" },
    { 0x1B1C, 0x2B2B, XP_FAM_X360, 0, "Corsair Novablade Pro (2.4 GHz Receiver)" },
    { 0x1BAD, 0x0002, XP_FAM_X360, 0, "Harmonix Rock Band Guitar" },
    { 0x1BAD, 0x0003, XP_FAM_X360, XP_F_DPADBTN, "Harmonix Rock Band Drumkit" },
    { 0x1BAD, 0x0130, XP_FAM_X360, XP_F_DPADBTN, "Ion Drum Rocker" },
    { 0x1BAD, 0xF016, XP_FAM_X360, 0, "Mad Catz Xbox 360 Controller" },
    { 0x1BAD, 0xF018, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Street Fighter IV SE Fighting Stick" },
    { 0x1BAD, 0xF019, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Brawlstick for Xbox 360" },
    { 0x1BAD, 0xF021, XP_FAM_X360, 0, "Mad Cats Ghost Recon FS GamePad" },
    { 0x1BAD, 0xF023, XP_FAM_X360, 0, "MLG Pro Circuit Controller (Xbox)" },
    { 0x1BAD, 0xF025, XP_FAM_X360, 0, "Mad Catz Call Of Duty" },
    { 0x1BAD, 0xF027, XP_FAM_X360, 0, "Mad Catz FPS Pro" },
    { 0x1BAD, 0xF028, XP_FAM_X360, 0, "Street Fighter IV FightPad" },
    { 0x1BAD, 0xF02E, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz Fightpad" },
    { 0x1BAD, 0xF030, XP_FAM_X360, 0, "Mad Catz Xbox 360 MC2 MicroCon Racing Wheel" },
    { 0x1BAD, 0xF036, XP_FAM_X360, 0, "Mad Catz MicroCon GamePad Pro" },
    { 0x1BAD, 0xF038, XP_FAM_X360, 0, "Street Fighter IV FightStick TE" },
    { 0x1BAD, 0xF039, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz MvC2 TE" },
    { 0x1BAD, 0xF03A, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz SFxT Fightstick Pro" },
    { 0x1BAD, 0xF03D, XP_FAM_X360, XP_F_TRIGBTN, "Street Fighter IV Arcade Stick TE - Chun Li" },
    { 0x1BAD, 0xF03E, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz MLG FightStick TE" },
    { 0x1BAD, 0xF03F, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz FightStick SoulCaliber" },
    { 0x1BAD, 0xF042, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz FightStick TES+" },
    { 0x1BAD, 0xF080, XP_FAM_X360, XP_F_TRIGBTN, "Mad Catz FightStick TE2" },
    { 0x1BAD, 0xF501, XP_FAM_X360, 0, "HoriPad EX2 Turbo" },
    { 0x1BAD, 0xF502, XP_FAM_X360, XP_F_TRIGBTN, "Hori Real Arcade Pro.VX SA" },
    { 0x1BAD, 0xF503, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Stick VX" },
    { 0x1BAD, 0xF504, XP_FAM_X360, XP_F_TRIGBTN, "Hori Real Arcade Pro. EX" },
    { 0x1BAD, 0xF505, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Stick EX2B" },
    { 0x1BAD, 0xF506, XP_FAM_X360, 0, "Hori Real Arcade Pro.EX Premium VLX" },
    { 0x1BAD, 0xF900, XP_FAM_X360, 0, "Harmonix Xbox 360 Controller" },
    { 0x1BAD, 0xF901, XP_FAM_X360, 0, "Gamestop Xbox 360 Controller" },
    { 0x1BAD, 0xF903, XP_FAM_X360, 0, "Tron Xbox 360 controller" },
    { 0x1BAD, 0xF904, XP_FAM_X360, 0, "PDP Versus Fighting Pad" },
    { 0x1BAD, 0xF906, XP_FAM_X360, XP_F_TRIGBTN, "MortalKombat FightStick" },
    { 0x1BAD, 0xFA01, XP_FAM_X360, 0, "MadCatz GamePad" },
    { 0x1BAD, 0xFD00, XP_FAM_X360, 0, "Razer Onza TE" },
    { 0x1BAD, 0xFD01, XP_FAM_X360, 0, "Razer Onza" },
    { 0x1EE9, 0x1590, XP_FAM_X360, 0, "ZOTAC Gaming Zone" },
    { 0x20BC, 0x5134, XP_FAM_X360, 0, "BETOP BTP-KP50B Xinput Dongle" },
    { 0x20BC, 0x514A, XP_FAM_X360, 0, "BETOP BTP-KP50C Xinput Dongle" },
    { 0x20D6, 0x2001, XP_FAM_GIP, 0, "BDA Xbox Series X Wired Controller" },
    { 0x20D6, 0x2009, XP_FAM_GIP, 0, "PowerA Enhanced Wired Controller for Xbox Series X|S" },
    { 0x20D6, 0x2064, XP_FAM_GIP, 0, "PowerA Wired Controller for Xbox" },
    { 0x20D6, 0x281F, XP_FAM_X360, 0, "PowerA Wired Controller For Xbox 360" },
    { 0x20D6, 0x400B, XP_FAM_GIP, 0, "PowerA FUSION Pro 4 Wired Controller" },
    { 0x20D6, 0x890B, XP_FAM_GIP, 0, "PowerA MOGA XP-Ultra Controller" },
    { 0x2345, 0xE00B, XP_FAM_X360, 0, "Machenike G5 Pro Controller" },
    { 0x24C6, 0x5000, XP_FAM_X360, XP_F_TRIGBTN, "Razer Atrox Arcade Stick" },
    { 0x24C6, 0x5300, XP_FAM_X360, 0, "PowerA MINI PROEX Controller" },
    { 0x24C6, 0x5303, XP_FAM_X360, 0, "Xbox Airflo wired controller" },
    { 0x24C6, 0x530A, XP_FAM_X360, 0, "Xbox 360 Pro EX Controller" },
    { 0x24C6, 0x531A, XP_FAM_X360, 0, "PowerA Pro Ex" },
    { 0x24C6, 0x5397, XP_FAM_X360, 0, "FUS1ON Tournament Controller" },
    { 0x24C6, 0x541A, XP_FAM_GIP, 0, "PowerA Xbox One Mini Wired Controller" },
    { 0x24C6, 0x542A, XP_FAM_GIP, 0, "Xbox ONE spectra" },
    { 0x24C6, 0x543A, XP_FAM_GIP, 0, "PowerA Xbox One wired controller" },
    { 0x24C6, 0x5500, XP_FAM_X360, 0, "Hori XBOX 360 EX 2 with Turbo" },
    { 0x24C6, 0x5501, XP_FAM_X360, 0, "Hori Real Arcade Pro VX-SA" },
    { 0x24C6, 0x5502, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Stick VX Alt" },
    { 0x24C6, 0x5503, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Edge" },
    { 0x24C6, 0x5506, XP_FAM_X360, 0, "Hori SOULCALIBUR V Stick" },
    { 0x24C6, 0x550D, XP_FAM_X360, 0, "Hori GEM Xbox controller" },
    { 0x24C6, 0x550E, XP_FAM_X360, XP_F_TRIGBTN, "Hori Real Arcade Pro V Kai 360" },
    { 0x24C6, 0x5510, XP_FAM_X360, XP_F_TRIGBTN, "Hori Fighting Commander ONE (Xbox 360/PC Mode)" },
    { 0x24C6, 0x551A, XP_FAM_GIP, 0, "PowerA FUSION Pro Controller" },
    { 0x24C6, 0x561A, XP_FAM_GIP, 0, "PowerA FUSION Controller" },
    { 0x24C6, 0x581A, XP_FAM_GIP, 0, "ThrustMaster XB1 Classic Controller" },
    { 0x24C6, 0x5B00, XP_FAM_X360, 0, "ThrustMaster Ferrari 458 Racing Wheel" },
    { 0x24C6, 0x5B02, XP_FAM_X360, 0, "Thrustmaster, Inc. GPX Controller" },
    { 0x24C6, 0x5B03, XP_FAM_X360, 0, "Thrustmaster Ferrari 458 Racing Wheel" },
    { 0x24C6, 0x5D04, XP_FAM_X360, 0, "Razer Sabertooth" },
    { 0x24C6, 0xFAFE, XP_FAM_X360, 0, "Rock Candy Gamepad for Xbox 360" },
    { 0x2563, 0x058D, XP_FAM_X360, 0, "OneXPlayer Gamepad" },
    { 0x294B, 0x3303, XP_FAM_GIP, 0, "Snakebyte GAMEPAD BASE X" },
    { 0x294B, 0x3404, XP_FAM_GIP, 0, "Snakebyte GAMEPAD RGB X" },
    { 0x2993, 0x2001, XP_FAM_X360, 0, "TECNO Pocket Go" },
    { 0x2DC8, 0x2000, XP_FAM_GIP, 0, "8BitDo Pro 2 Wired Controller fox Xbox" },
    { 0x2DC8, 0x200F, XP_FAM_GIP, 0, "8BitDo Ultimate 3-mode Controller for Xbox" },
    { 0x2DC8, 0x201E, XP_FAM_GIP, 0, "8BitDo Pro 2 Wired Controller for Xbox" },
    { 0x2DC8, 0x3106, XP_FAM_X360, 0, "8BitDo Ultimate Wireless / Pro 2 Wired Controller" },
    { 0x2DC8, 0x3109, XP_FAM_X360, 0, "8BitDo Ultimate Wireless Bluetooth" },
    { 0x2DC8, 0x310A, XP_FAM_X360, 0, "8BitDo Ultimate 2C Wireless Controller" },
    { 0x2DC8, 0x310B, XP_FAM_X360, 0, "8BitDo Ultimate 2 Wireless Controller" },
    { 0x2DC8, 0x6001, XP_FAM_X360, 0, "8BitDo SN30 Pro" },
    { 0x2E24, 0x0423, XP_FAM_GIP, 0, "Hyperkin DuchesS Xbox One pad" },
    { 0x2E24, 0x0652, XP_FAM_GIP, 0, "Hyperkin Duke X-Box One pad" },
    { 0x2E24, 0x1688, XP_FAM_GIP, 0, "Hyperkin X91 X-Box One pad" },
    { 0x2E95, 0x0504, XP_FAM_GIP, 0, "SCUF Gaming Controller" },
    { 0x31E3, 0x1100, XP_FAM_X360, XP_F_NOINF, "Wooting One" },
    { 0x31E3, 0x1200, XP_FAM_X360, XP_F_NOINF, "Wooting Two" },
    { 0x31E3, 0x1210, XP_FAM_X360, XP_F_NOINF, "Wooting Lekker" },
    { 0x31E3, 0x1220, XP_FAM_X360, XP_F_NOINF, "Wooting Two HE" },
    { 0x31E3, 0x1230, XP_FAM_X360, XP_F_NOINF, "Wooting Two HE (ARM)" },
    { 0x31E3, 0x1300, XP_FAM_X360, XP_F_NOINF, "Wooting 60HE (AVR)" },
    { 0x31E3, 0x1310, XP_FAM_X360, XP_F_NOINF, "Wooting 60HE (ARM)" },
    { 0x3285, 0x0603, XP_FAM_GIP, 0, "Nacon Pro Compact controller for Xbox" },
    { 0x3285, 0x0607, XP_FAM_X360, 0, "Nacon GC-100" },
    { 0x3285, 0x0614, XP_FAM_GIP, 0, "Nacon Pro Compact" },
    { 0x3285, 0x0646, XP_FAM_GIP, 0, "Nacon Pro Compact" },
    { 0x3285, 0x0662, XP_FAM_X360, 0, "Nacon Revolution5 Pro" },
    { 0x3285, 0x0663, XP_FAM_GIP, 0, "Nacon Evol-X" },
    { 0x3507, 0x000B, XP_FAM_X360, 0, "ZENAIM LEVERLESS" },
    { 0x3537, 0x1004, XP_FAM_X360, 0, "GameSir T4 Kaleid" },
    { 0x3537, 0x100F, XP_FAM_X360, 0, "GameSir Nova 2 Lite" },
    { 0x3537, 0x1010, XP_FAM_GIP, 0, "GameSir G7 SE" },
    { 0x3651, 0x1000, XP_FAM_X360, 0, "CRKD SG" },
    { 0x366C, 0x0005, XP_FAM_GIP, XP_F_DELAYINIT, "ByoWave Proteus Controller" },
    { 0x37D7, 0x2501, XP_FAM_X360, 0, "Flydigi Apex 5" },
    { 0x413D, 0x2104, XP_FAM_X360, 0, "Black Shark Green Ghost Gamepad" },
};

#define NUM_DEVICES (sizeof(Devices) / sizeof(Devices[0]))

/* Morii's x360c, final 2010 builds (design 5.1): left to that driver. */
static const XU16 X360c[][2] = {
    { 0x045E, 0x028E },
    { 0x046D, 0xC242 },
    { 0x0738, 0xCB02 },
    { 0x0738, 0x4716 }, { 0x0738, 0x4718 }, { 0x0738, 0x4726 },
    { 0x0738, 0x4728 }, { 0x0738, 0x4736 }, { 0x0738, 0x4738 },
    { 0x0738, 0xA728 },
    { 0x1BAD, 0xF028 }, { 0x1BAD, 0xF029 }, { 0x1BAD, 0xF039 },
    { 0x0F0D, 0x000A }, { 0x0F0D, 0x000B }, { 0x0F0D, 0x000C },
    { 0x0F0D, 0x000D }, { 0x0F0D, 0x000F }, { 0x0F0D, 0x0016 },
    { 0x0F0D, 0x0031 },
    { 0x1BAD, 0xF500 }, { 0x1BAD, 0xF501 }, { 0x1BAD, 0xF502 },
    { 0x1BAD, 0xF504 }, { 0x1BAD, 0xF506 },
};

#define NUM_X360C (sizeof(X360c) / sizeof(X360c[0]))

const XP_DEVICE *XpFindDevice(XU16 vid, XU16 pid)
{
    unsigned lo = 0, hi = NUM_DEVICES;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        const XP_DEVICE *d = &Devices[mid];
        XU32 key = ((XU32)vid << 16) | pid;
        XU32 k = ((XU32)d->Vid << 16) | d->Pid;
        if (k == key)
            return d;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

unsigned XpDeviceCount(void)
{
    return NUM_DEVICES;
}

const XP_DEVICE *XpDeviceAt(unsigned i)
{
    return i < NUM_DEVICES ? &Devices[i] : 0;
}

int XpIsX360c(XU16 vid, XU16 pid)
{
    unsigned i;
    for (i = 0; i < NUM_X360C; i++)
        if (X360c[i][0] == vid && X360c[i][1] == pid)
            return 1;
    return 0;
}

unsigned XpX360cCount(void)
{
    return NUM_X360C;
}

void XpX360cAt(unsigned i, XU16 *vid, XU16 *pid)
{
    *vid = X360c[i][0];
    *pid = X360c[i][1];
}

const char *XpFamilyName(unsigned family)
{
    switch (family) {
    case XP_FAM_X360: return "Xbox 360 compatible pad";
    case XP_FAM_X360W: return "Xbox 360 wireless pad";
    case XP_FAM_GIP: return "Xbox One compatible pad";
    }
    return "Xbox compatible pad";
}
