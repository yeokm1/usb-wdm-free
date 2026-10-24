/* SPDX-License-Identifier: GPL-2.0-or-later
 * ASIX AX88179/178A USB 3.0/2.0 to Gigabit Ethernet Devices
 * Copyright (C) 2011-2013 ASIX
 * Copyright (C) 2026 Yeo Kheng Meng
 * Register/request facts adapted from Linux v6.12 ax88179_178a.c.
 * Original bounded descriptor/control planner. No device I/O or allocation.
 */
#ifndef AXCTRL_H
#define AXCTRL_H
#define AX_CTRL_OK 0
#define AX_CTRL_BAD 1
#define AX_CTRL_SPEED_MISMATCH 2
#define AX_CTRL_UNSUPPORTED 3
#define AX_SPEED_FULL 1
#define AX_SPEED_HIGH 2
#define AX_SPEED_SUPER 3 /* own enum: not libusb's raw enum4 */
#define AX_WARN_STATUS_INTERVAL_ZERO 1
typedef struct AX_CTRL_DEVICE {
    unsigned int Valid, Speed, UsbRevision, Configuration, Interface;
    unsigned int BulkIn, BulkOut, StatusIn, InPacket, OutPacket, StatusPacket;
    unsigned int InBurst, OutBurst, StatusBurst, StatusInterval, StatusBytes;
    unsigned int ConfigBytes, Warnings;
} AX_CTRL_DEVICE;

/* Exact 0B95:1790, one vendor FF/FF/00 interface0/alt0 and endpoints
 * 81 status,82 IN,03 OUT. Caller supplies ACTUALLY observed link speed;
 * bcdUSB never substitutes for it. Config wTotalLength bounds the walk.
 * USB2 rejects SS companions/MPS1024; SS requires bounded companions.
 * Failure zeroes result. Input/output storage must not alias.
 * This is a transport-profile check, not a complete USB conformance test.
 */
int AxCtrlDescribe(const unsigned char *device, unsigned int deviceBytes,
    const unsigned char *config, unsigned int configBytes, unsigned int speed,
    AX_CTRL_DEVICE *result);

#define AX_STEP_READ 1
#define AX_STEP_WRITE 2
#define AX_STEP_DELAY 3
#define AX_TAG_NONE 0
#define AX_TAG_USB_STATUS 1
#define AX_TAG_MAC 2
#define AX_TAG_PHY_STATUS 3
#define AX_TAG_AUTODETACH_EEPROM 4
#define AX_GATE_OBSERVATIONS 1
#define AX_GATE_AUTODETACH_DECISION 2
#define AX_CTRL_MAX_STEPS 8
typedef struct AX_CTRL_STEP {
    unsigned int Kind, RequestType, Request, Value, Index, Length, ResultTag;
    unsigned int DelayMs;
    unsigned char Data[8];
} AX_CTRL_STEP;
typedef struct AX_CTRL_PLAN {
    unsigned int Valid, Count, HasWrites, NextGate;
    /* Plans are intentionally incomplete: executor MUST stop at NextGate.
     * No plan here certifies RX/TX-ready hardware initialization. */
    AX_CTRL_STEP Step[AX_CTRL_MAX_STEPS];
} AX_CTRL_PLAN;

/* Pure plan construction. No call executes a USB request or a delay.
 * Diagnostic plan reads MAC/bus/PHY. PowerPrefix is a MUTATING candidate
 * reset/clock sequence from pinned Linux, with exact 500/200ms waits,
 * ending at an EEPROM-dependent auto-detach gate. It must not be treated
 * as a complete init sequence or executed without separate authorization.
 */
int AxCtrlReadPlan(const AX_CTRL_DEVICE *device, AX_CTRL_PLAN *plan);
int AxCtrlPowerPrefix(const AX_CTRL_DEVICE *device, AX_CTRL_PLAN *plan);
/* The following setup/response/MAC/PHY helpers return Boolean 1/0,
 * unlike the AX_CTRL_* status codes returned by the plan builders. */
int AxCtrlSetup(const AX_CTRL_STEP *step, unsigned char setup[8]);
int AxCtrlResponse(const AX_CTRL_STEP *step, unsigned int actual);

/* Preserve a valid unicast MAC; never generate one or silently rewrite it.
 * Locally administered addresses are valid. Exact six-byte input required. */
int AxCtrlMac(const unsigned char *bytes, unsigned int length);
typedef struct AX_CTRL_LINK {
    unsigned int Up, Megabits, FullDuplex;
} AX_CTRL_LINK;
int AxCtrlPhy(const unsigned char *bytes, unsigned int length, AX_CTRL_LINK *link);
/* Derive candidate link parameters from observed hardware status. Returns
 * mismatch if MAC register USB-speed bits disagree with caller speed.
 * Queue bytes/medium value follow pinned Linux link_reset; NO writes done.
 * Unknown/reserved PHY speed and invalid MAC are rejected. MTU/jumbo,
 * FIFO polling, receive filters, checksum/EEE/autoneg remain external gates.
 */
typedef struct AX_CTRL_LINK_PLAN {
    unsigned int Valid, Medium, ReceiveBufferBytes;
    unsigned char Queue[5];
    AX_CTRL_LINK Link;
} AX_CTRL_LINK_PLAN;
int AxCtrlLinkPlan(const AX_CTRL_DEVICE *device, const unsigned char mac[6],
    const unsigned char *usbStatus, unsigned int usbBytes,
    const unsigned char *phyStatus, unsigned int phyBytes, AX_CTRL_LINK_PLAN *plan);
#endif
