/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Narrow UM-ONE mk2 profile; see umroland.c for source notices.
 */
#ifndef UMROLAND_H
#define UMROLAND_H
typedef struct UM_ROLAND {
    unsigned int Interface, Alternate, InAddress, OutAddress;
    unsigned int InType, OutType, InInterval, MaxPacket;
    unsigned int InCables, OutCables;
} UM_ROLAND;
/* Recognize only the captured 0582:012A single-cable, full-speed profile.
 * Caller chooses alt0 (bulk/bulk) or alt1 (interrupt IN/bulk OUT).
 * No USB requests, mode changes, standard jack IDs, or class-parser fallback.
 * Failure zeroes output. Input/output storage must not alias.
 * This does not identify the physical COMP/TAB switch position.
 */
int UmMatchUmOne2(const unsigned char *device,unsigned int deviceBytes,
    const unsigned char *config,unsigned int configBytes,
    unsigned int alternate,UM_ROLAND *result);
#endif
