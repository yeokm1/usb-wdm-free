/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * RTL815X chip core: the register map this package uses on Realtek
 * RTL8152/8153-family USB Ethernet controllers, under this project's own
 * names (ethernet.md section 6.4). Addresses and bit values are facts of
 * the chips' vendor register interface; only the ones the core and its
 * tests touch are listed.
 *
 * The chip has three register spaces reached through one vendor control
 * request: the MAC space ("PLA" in Realtek's terms), the USB space, and
 * the PHY's own registers seen through a 4 KiB window in the MAC space.
 */
#ifndef RTLCHIP_H
#define RTLCHIP_H

#include "netchip.h"

/* ---- the vendor request ---------------------------------------------
 * bRequest 0x05 both ways (bmRequestType 0xC0 in, 0x40 out); wValue is
 * the dword-aligned register address and wIndex the space in its high
 * byte. On a write, wIndex's low byte enables byte lanes: 0xff writes
 * every byte of the request; a one-dword write with 0x33 (or 0xcc)
 * writes only bytes 0-1 (2-3), with 0x11 shifted left n only byte n.
 * Reads always return whole dwords; this core names the word's lanes in
 * a word read and none in byte and dword reads. Reads move at most 64
 * bytes, writes 512. */
#define RTL_REQ          0x05
#define RTL_SPACE_MAC    0x0100
#define RTL_SPACE_USB    0x0000
#define RTL_LANES_ALL    0xff
#define RTL_LANES_WORD   0x33   /* bytes 0-1 (shifted left 2 for bytes 2-3) */
#define RTL_LANES_BYTE   0x11   /* byte 0 (shifted left n for byte n) */

/* ---- MAC space -------------------------------------------------------- */
#define MR_NODE_ID       0xc000 /* station address, 6 bytes */
#define MR_RX_ACCEPT     0xc010 /* receive accept bits (dword) */
#define MR_RX_MAX_LEN    0xc016
#define MR_RXFIFO_TH1    0xc0a0
#define MR_RXFIFO_TH1_HI 0xc0a2
#define MR_RXFIFO_TH2    0xc0a4
#define MR_RXFIFO_TH3    0xc0a8
#define MR_SPARE0        0xc0b0
#define MR_FILTER_MCU    0xc0b4
#define MR_WOL           0xc0b6
#define MR_TEREDO        0xc0bc
#define MR_TEREDO_WAKE   0xc0c4
#define MR_HASH          0xcd00 /* 64-bin multicast hash, 8 bytes */
#define MR_NODE_ID_SAVED 0xd000 /* the address the chip loaded at power-up */
#define MR_BDC           0xd1a0
#define MR_TEREDO_TMR    0xd2cc
#define MR_WOW_TMR       0xd2e8
#define MR_SUSPEND_FLAG  0xd38a
#define MR_INDICATE      0xd38c
#define MR_EXTRA_STATUS  0xd398
#define MR_PACKAGE       0xdc00
#define MR_EFUSE_DATA    0xdd00
#define MR_EFUSE_CMD     0xdd02
#define MR_LED           0xdd92
#define MR_POWER_STATE   0xe000
#define MR_BOOT          0xe004
#define MR_LANWAKE       0xe007
#define MR_PHY_IRQ_MASK  0xe022
#define MR_EEE           0xe040
#define MR_EEE_PLUS      0xe080
#define MR_POWER1        0xe0c0
#define MR_POWER2        0xe0ca
#define MR_POWER3        0xe0cc
#define MR_POWER4        0xe0ce
#define MR_WDT6          0xe428
#define MR_TX_CONFIG     0xe610 /* high word: chip id */
#define MR_TX_CONFIG1    0xe612
#define MR_TX_MAX_PKT    0xe615 /* byte, in 64-byte units */
#define MR_TXFIFO_TH     0xe618
#define MR_TXFIFO_FULL   0xe61a
#define MR_TALLY         0xe800
#define MR_COMMAND       0xe813
#define MR_CFG_LOCK      0xe81c
#define MR_CFG34         0xe820
#define MR_CFG5          0xe822
#define MR_PHY_POWER     0xe84c
#define MR_OOB           0xe84f
#define MR_CPLUS         0xe854
#define MR_MISC1         0xe85a
#define MR_PHY_PAGE      0xe86c /* base of the PHY window */
#define MR_SFF7          0xe8de
#define MR_LINK          0xe908
#define MR_CFG6          0xe90a
#define MR_PHY_WINDOW    0xb000 /* 0xb000-0xbfff: PHY register page | offset */

/* MR_RX_ACCEPT */
#define ACC_ALL_PHYS     0x01
#define ACC_MY_PHYS      0x02
#define ACC_MCAST        0x04
#define ACC_BCAST        0x08
#define ACC_MASK         0x0f
/* MR_COMMAND */
#define CMD_RESET        0x10
#define CMD_RX_ON        0x08
#define CMD_TX_ON        0x04
/* MR_CFG_LOCK */
#define CFG_LOCKED       0x00
#define CFG_UNLOCKED     0xc0
/* MR_OOB */
#define OOB_ACTIVE       0x80
#define OOB_FIFOS_EMPTY  0x30   /* transmit 0x20, receive 0x10 */
#define OOB_LIST_READY   0x02
#define OOB_HOLD         0x01
/* MR_TX_CONFIG */
#define TXC_ID_MASK      0x7cf0 /* of the high word */
#define TXC_EMPTY        0x0800
#define TXC_AUTO_FIFO    0x0080
/* MR_TX_CONFIG1 */
#define IFG_FIELD        0x0308
#define IFG_SHORT        0x0300 /* 96 ns */
#define IFG_LONG         0x0200 /* 144 ns */
/* MR_CPLUS, MR_MISC1, MR_BOOT, MR_WOL, MR_CFG5 */
#define CPLUS_RX_VLAN    0x0040
#define MISC1_RX_GATE    0x0008
#define BOOT_LOADED      0x0002
#define WOL_MAGIC        0x0001
#define CFG5_WAKE_FRAMES 0x0070
/* MR_LINK (word) */
#define LNK_1000         0x10
#define LNK_100          0x08
#define LNK_10           0x04
#define LNK_UP           0x02
#define LNK_FULL         0x01

/* ---- USB space -------------------------------------------------------- */
#define UR_U2_PHY        0xb41e
#define UR_SS_LINK1      0xb426
#define UR_SS_LINK2      0xb428
#define UR_U2P3          0xb460
#define UR_DUMMY1        0xb464
#define UR_DUMMY2        0xb466
#define UR_SPI           0xcbf0
#define UR_CONNECT_TMR   0xcbf8
#define UR_MSC_TMR       0xcbfc
#define UR_BURST         0xcfc0
#define UR_LPM_CFG       0xcfd8
#define UR_MISC2         0xcfff
#define UR_CTRL          0xd406
#define UR_TX_AGG        0xd40a
#define UR_RX_BUF_TH     0xd40c
#define UR_LPM_CTRL      0xd41a
#define UR_RX_EARLY_TMO  0xd42c
#define UR_RX_EARLY_LEN  0xd42e
#define UR_PM_STATUS     0xd432 /* the extra aggregation timer from the 8153B on */
#define UR_TX_DMA        0xd434
#define UR_RXDMA_OWN     0xd437
#define UR_U1U2          0xd490 /* 6 bytes */
#define UR_BMU           0xd4b0
#define UR_U1U2_TMR      0xd4da
#define UR_FW_TASK       0xd4e8
#define UR_UPS           0xd800
#define UR_POWER_CUT     0xd80a
#define UR_MISC0         0xd81a
#define UR_AFE2          0xd824
#define UR_UPS_CFG       0xd842
#define UR_WDT11         0xe43c

/* UR_CTRL */
#define CTRL_RX_AGG_OFF  0x0010
#define CTRL_RX_ZERO     0x0080

/* ---- PHY registers (through the window) -------------------------------- */
/* IEEE 802.3 clause 22 registers sit at PR_MII + 2 * number. */
#define PR_MII           0xa400
#define MII_CONTROL      0
#define MII_AN_ADV       4
#define MII_GIG_CONTROL  9
#define MII(n)           ((nc_u16)(PR_MII + 2 * (n)))
#define MIICTL_RESET     0x8000
#define MIICTL_AN_ENABLE 0x1000
#define MIICTL_POWERDOWN 0x0800
#define MIICTL_AN_RESTART 0x0200
#define ANADV_10_100     0x01e0 /* 10 and 100, half and full duplex */
#define ANADV_PAUSE      0x0c00 /* symmetric and asymmetric pause */
#define GIGCTL_1000_MASK 0x0300
#define GIGCTL_1000_FULL 0x0200
/* IEEE 802.3 clause 45, MMD 7 (auto-negotiation) register 60: EEE advertisement */
#define EEEADV_100TX     0x0002
#define EEEADV_1000T     0x0004

#define PR_ALDPS_A       0x2010 /* 8152 family */
#define PR_EEE_A1        0x2080
#define PR_EEE_A2        0x2092
#define PR_EEE_A3        0x2094
#define PR_MMD_CTRL      0xa41a
#define PR_MMD_DATA      0xa41c
#define PR_STATE         0xa420
#define PR_NCTL          0xa42c
#define PR_POWER         0xa430
#define PR_EEE_CFG       0xa432
#define PR_SRAM_ADDR     0xa436
#define PR_SRAM_DATA     0xa438
#define PR_SPEED_DOWN    0xa442
#define PR_EEE_ADV       0xa5d0
#define PR_PATCH_STATE   0xb800
#define PR_PATCH_CMD     0xb820
#define PR_ADC_CFG       0xbc06
#define PR_ADC_OFFSET    0xbcfc
#define PR_SYSCLK        0xc416
#define PR_SAW_COUNT     0xc426

/* PR_STATE, low three bits */
#define PHYST_MASK       0x0007
#define PHYST_EXT_INIT   2
#define PHYST_LAN_ON     3
#define PHYST_POWER_DOWN 5
/* PR_PATCH_CMD, PR_PATCH_STATE */
#define PATCH_ASK        0x0010
#define PATCH_GRANTED    0x0040

/* ---- PHY SRAM (through PR_SRAM_ADDR/PR_SRAM_DATA) --------------------- */
#define SR_GREEN         0x8011
#define SR_LPF           0x8012
#define SR_BIAS_10       0x8045
#define SR_BIAS_100      0x804d
#define SR_BIAS_1000     0x805d
#define SR_AMP_10A       0x8080
#define SR_AMP_10B       0x8082
#define SR_IMPEDANCE     0x8084

/* ---- interrupt-IN report and descriptors ------------------------------ */
#define IRQ_LINK         0x0004 /* in the first word of the interrupt report */
#define RX_DESC_LEN      24     /* receive record header; frame length in bits 0-14 */
#define RX_RECORD_ALIGN  8
#define TX_DESC_LEN      8
#define TX_ALIGN         4
#define TX_FIRST_LAST    0xc0000000UL
#define RX_MAX_LEN       1522   /* 1500 + Ethernet header + VLAN tag + FCS */
#define TX_BUFFER        16384UL

/* ---- chip ids (MR_TX_CONFIG high word & TXC_ID_MASK) ------------------
 * RTL8152 family: 4C00, 4C10, 4800. RTL8153: 5C00, 5C10, 5C20, 5C30.
 * RTL8153B: 6000, 6010. RTL8153C: 6400. */
#define ID_4C00          0x4c00
#define ID_4C10          0x4c10
#define ID_4800          0x4800
#define ID_5C00          0x5c00
#define ID_5C10          0x5c10
#define ID_5C20          0x5c20
#define ID_5C30          0x5c30
#define ID_6000          0x6000
#define ID_6010          0x6010
#define ID_6400          0x6400

/* The core's state, in the NET_CHIP_STATE bytes the lower edge provides. */
typedef struct RTL_STATE {
    nc_u16 Id;                /* ID_*; 0 until Start accepted the chip */
    nc_u16 Kind;              /* the chip's K_* bit (rtlchip.c) */
    nc_u8 UsbSpeed;           /* NC_SPEED_* */
    nc_u8 Carrier;            /* Link last applied "up" */
    nc_u8 SetUpOnce;          /* an 8153B/C finished its first setup */
    nc_u8 Spare;
    nc_u16 PhyPage;           /* page in MR_PHY_PAGE; 0xffff unknown */
    nc_u16 Spare2;
    nc_u32 Accept;            /* ACC_* bits beyond ACC_BCAST | ACC_MY_PHYS */
    nc_u8 Hash[8];            /* MR_HASH bytes as written */
    nc_u8 Mac[6];
    nc_u32 Timeouts;          /* bounded waits that ran out */
} RTL_STATE;

/* Exposed for the host tests. */
nc_u16 RtlChipId(nc_u32 txConfig);   /* ID_* of a supported chip, else 0 */
long RtlProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
long RtlRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats);
void RtlMulticastHash(nc_u32 filter, const nc_u8 *multicast, nc_u32 count, nc_u32 *accept,
                      nc_u8 hash[8]);

#endif
