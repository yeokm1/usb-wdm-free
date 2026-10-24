/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2014 Realtek Semiconductor Corp. All rights reserved.
 * Copyright (c) 2020 Realtek Semiconductor Corp. All rights reserved.
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * RTL815X chip core: register names, values and sequences adapted from
 * Linux drivers/net/usb/r8152.c and include/linux/usb/r8152.h at
 * f47d223627f80cb7a1eb14c585fe6cb35de5e984 (ethernet.md sections 6.4 and
 * 9). GPL-2.0-only, as r8152.c is. Only the names this core uses.
 */
#ifndef RTLCHIP_H
#define RTLCHIP_H

#include "netchip.h"

/* include/linux/usb/r8152.h */
#define RTL8152_REQT_READ    0xc0
#define RTL8152_REQT_WRITE   0x40
#define RTL8152_REQ_GET_REGS 0x05
#define RTL8152_REQ_SET_REGS 0x05
#define BYTE_EN_DWORD        0xff
#define BYTE_EN_WORD         0x33
#define BYTE_EN_BYTE         0x11
#define BYTE_EN_SIX_BYTES    0x3f
#define BYTE_EN_START_MASK   0x0f
#define BYTE_EN_END_MASK     0xf0
#define MCU_TYPE_PLA         0x0100
#define MCU_TYPE_USB         0x0000

/* PLA registers */
#define PLA_IDR              0xc000
#define PLA_RCR              0xc010
#define PLA_RMS              0xc016
#define PLA_RXFIFO_CTRL0     0xc0a0
#define PLA_RXFIFO_FULL      0xc0a2
#define PLA_RXFIFO_CTRL1     0xc0a4
#define PLA_RXFIFO_CTRL2     0xc0a8
#define PLA_DMY_REG0         0xc0b0
#define PLA_FMC              0xc0b4
#define PLA_CFG_WOL          0xc0b6
#define PLA_TEREDO_CFG       0xc0bc
#define PLA_TEREDO_WAKE_BASE 0xc0c4
#define PLA_MAR              0xcd00
#define PLA_BACKUP           0xd000
#define PLA_BDC_CR           0xd1a0
#define PLA_TEREDO_TIMER     0xd2cc
#define PLA_REALWOW_TIMER    0xd2e8
#define PLA_SUSPEND_FLAG     0xd38a
#define PLA_INDICATE_FALG    0xd38c
#define PLA_EXTRA_STATUS     0xd398
#define PLA_EFUSE_DATA       0xdd00
#define PLA_EFUSE_CMD        0xdd02
#define PLA_LED_FEATURE      0xdd92
#define PLA_BOOT_CTRL        0xe004
#define PLA_LWAKE_CTRL_REG   0xe007
#define PLA_GPHY_INTR_IMR    0xe022
#define PLA_EEE_CR           0xe040
#define PLA_EEEP_CR          0xe080
#define PLA_MAC_PWR_CTRL     0xe0c0
#define PLA_MAC_PWR_CTRL2    0xe0ca
#define PLA_MAC_PWR_CTRL3    0xe0cc
#define PLA_MAC_PWR_CTRL4    0xe0ce
#define PLA_WDT6_CTRL        0xe428
#define PLA_TCR0             0xe610
#define PLA_TCR1             0xe612
#define PLA_MTPS             0xe615
#define PLA_TXFIFO_CTRL      0xe618
#define PLA_TXFIFO_FULL      0xe61a
#define PLA_RSTTALLY         0xe800
#define PLA_CR               0xe813
#define PLA_CRWECR           0xe81c
#define PLA_CONFIG34         0xe820
#define PLA_CONFIG5          0xe822
#define PLA_PHY_PWR          0xe84c
#define PLA_OOB_CTRL         0xe84f
#define PLA_CPCR             0xe854
#define PLA_MISC_1           0xe85a
#define PLA_OCP_GPHY_BASE    0xe86c
#define PLA_SFF_STS_7        0xe8de
#define PLA_PHYSTATUS        0xe908
#define PLA_CONFIG6          0xe90a

/* USB registers */
#define USB_USB2PHY          0xb41e
#define USB_SSPHYLINK1       0xb426
#define USB_SSPHYLINK2       0xb428
#define USB_U2P3_CTRL        0xb460
#define USB_CSR_DUMMY1       0xb464
#define USB_CSR_DUMMY2       0xb466
#define USB_CONNECT_TIMER    0xcbf8
#define USB_MSC_TIMER        0xcbfc
#define USB_BURST_SIZE       0xcfc0
#define USB_LPM_CONFIG       0xcfd8
#define USB_MISC_2           0xcfff
#define USB_USB_CTRL         0xd406
#define USB_TX_AGG           0xd40a
#define USB_RX_BUF_TH        0xd40c
#define USB_RX_EARLY_TIMEOUT 0xd42c
#define USB_RX_EARLY_SIZE    0xd42e
#define USB_PM_CTRL_STATUS   0xd432
#define USB_RX_EXTRA_AGGR_TMR 0xd432
#define USB_TX_DMA           0xd434
#define USB_UPT_RXDMA_OWN    0xd437
#define USB_TOLERANCE        0xd490
#define USB_LPM_CTRL         0xd41a
#define USB_BMU_RESET        0xd4b0
#define USB_U1U2_TIMER       0xd4da
#define USB_FW_TASK          0xd4e8
#define USB_UPS_CTRL         0xd800
#define USB_POWER_CUT        0xd80a
#define USB_MISC_0           0xd81a
#define USB_AFE_CTRL2        0xd824
#define USB_UPS_CFG          0xd842
#define USB_UPS_FLAGS        0xd848
#define USB_WDT11_CTRL       0xe43c

/* OCP (PHY) registers and SRAM */
#define OCP_ALDPS_CONFIG     0x2010
#define OCP_EEE_CONFIG1      0x2080
#define OCP_EEE_CONFIG2      0x2092
#define OCP_EEE_CONFIG3      0x2094
#define OCP_BASE_MII         0xa400
#define OCP_EEE_AR           0xa41a
#define OCP_EEE_DATA         0xa41c
#define OCP_PHY_STATUS       0xa420
#define OCP_NCTL_CFG         0xa42c
#define OCP_POWER_CFG        0xa430
#define OCP_EEE_CFG          0xa432
#define OCP_SRAM_ADDR        0xa436
#define OCP_SRAM_DATA        0xa438
#define OCP_DOWN_SPEED       0xa442
#define OCP_EEE_ADV          0xa5d0
#define OCP_PHY_PATCH_STAT   0xb800
#define OCP_PHY_PATCH_CMD    0xb820
#define OCP_ADC_IOFFSET      0xbcfc
#define OCP_ADC_CFG          0xbc06
#define OCP_SYSCLK_CFG       0xc416
#define SRAM_GREEN_CFG       0x8011
#define SRAM_LPF_CFG         0x8012
#define SRAM_10M_AMP1        0x8080
#define SRAM_10M_AMP2        0x8082
#define SRAM_IMPEDANCE       0x8084

/* bits */
#define RCR_AAP              0x00000001UL
#define RCR_APM              0x00000002UL
#define RCR_AM               0x00000004UL
#define RCR_AB               0x00000008UL
#define RCR_ACPT_ALL         (RCR_AAP | RCR_APM | RCR_AM | RCR_AB)
#define RXFIFO_THR1_NORMAL   0x00080002UL
#define RXFIFO_THR1_OOB      0x01800003UL
#define RXFIFO_THR2_FULL     0x00000060UL
#define RXFIFO_THR2_HIGH     0x00000038UL
#define RXFIFO_THR2_OOB      0x0000004aUL
#define RXFIFO_THR2_NORMAL   0x00a0
#define RXFIFO_THR3_FULL     0x00000078UL
#define RXFIFO_THR3_HIGH     0x00000048UL
#define RXFIFO_THR3_OOB      0x0000005aUL
#define RXFIFO_THR3_NORMAL   0x0110
#define TXFIFO_THR_NORMAL2   0x01000008UL
#define ECM_ALDPS            0x0002
#define FMC_FCR_MCU_EN       0x0001
#define EEEP_CR_EEEP_TX      0x0002
#define WDT6_SET_MODE        0x0010
#define TCR0_TX_EMPTY        0x0800
#define TCR0_AUTO_FIFO       0x0080
#define VERSION_MASK         0x7cf0
#define IFG_MASK             0x0308
#define IFG_144NS            0x0200
#define IFG_96NS             0x0300
#define MTPS_JUMBO           (12 * 1024 / 64)
#define MTPS_DEFAULT         (6 * 1024 / 64)
#define TALLY_RESET          0x0001
#define CR_RST               0x10
#define CR_RE                0x08
#define CR_TE                0x04
#define CRWECR_NORAML        0x00
#define CRWECR_CONFIG        0xc0
#define NOW_IS_OOB           0x80
#define TXFIFO_EMPTY         0x20
#define RXFIFO_EMPTY         0x10
#define LINK_LIST_READY      0x02
#define DIS_MCU_CLROOB       0x01
#define FIFO_EMPTY           (TXFIFO_EMPTY | RXFIFO_EMPTY)
#define RXDY_GATED_EN        0x0008
#define RE_INIT_LL           0x8000
#define MCU_BORW_EN          0x4000
#define CPCR_RX_VLAN         0x0040
#define MAGIC_EN             0x0001
#define TEREDO_SEL           0x8000
#define TEREDO_WAKE_MASK     0x7f00
#define TEREDO_RS_EVENT_MASK 0x00fe
#define OOB_TEREDO_EN        0x0001
#define ALDPS_PROXY_MODE     0x0001
#define EFUSE_READ_CMD       0x8000
#define EFUSE_DATA_BIT16     0x0080
#define LINK_ON_WAKE_EN      0x0010
#define LINK_OFF_WAKE_EN     0x0008
#define LANWAKE_CLR_EN       0x01
#define BWF_EN               0x0040
#define MWF_EN               0x0020
#define UWF_EN               0x0010
#define LED_MODE_MASK        0x0700
#define TX_10M_IDLE_EN       0x0080
#define PFM_PWM_SWITCH       0x0040
#define TEST_IO_OFF          0x0010
#define D3_CLK_GATED_EN      0x00004000UL
#define MCU_CLK_RATIO        0x07010f07UL
#define MCU_CLK_RATIO_MASK   0x0f0f0f0fUL
#define MAC_CLK_SPDWN_EN     0x8000
#define PLA_MCU_SPDWN_EN     0x4000
#define TX10MIDLE_EN         0x0100
#define GPHY_STS_MSK         0x0001
#define SPEED_DOWN_MSK       0x0002
#define SPDWN_RXDV_MSK       0x0004
#define SPDWN_LINKCHG_MSK    0x0008
#define EEE_RX_EN            0x0001
#define EEE_TX_EN            0x0002
#define AUTOLOAD_DONE        0x0002
#define LANWAKE_PIN          0x80
#define LINK_CHG_EVENT       0x01
#define UPCOMING_RUNTIME_D3  0x01
#define CUR_LINK_OK          0x8000
#define LINK_CHANGE_FLAG     0x0100
#define POLL_LINK_CHG        0x0001
#define USB2PHY_SUSPEND      0x01
#define USB2PHY_L1           0x02
#define DELAY_PHY_PWR_CHG    0x0002
#define PWD_DN_SCALE_MASK    0x3ffe
#define PWD_DN_SCALE(x)      ((x) << 1)
#define DYNAMIC_BURST        0x01
#define EP4_FULL_FC          0x01
#define LPM_U1U2_EN          0x0001
#define TX_AGG_MAX_THRESHOLD 0x03
#define RX_THR_HIGH          0x7a120180UL
#define RX_THR_B             0x00010001UL
#define TEST_MODE_DISABLE    0x00000001UL
#define TX_SIZE_ADJUST1      0x00000100UL
#define BMU_RESET_EP_IN      0x01
#define BMU_RESET_EP_OUT     0x02
#define OWN_UPDATE           0x01
#define OWN_CLEAR            0x02
#define FC_PATCH_TASK        0x0002
#define POWER_CUT            0x0100
#define RESUME_INDICATE      0x0001
#define UPS_FORCE_PWR_DOWN   0x01
#define RX_AGG_DISABLE       0x0010
#define RX_ZERO_EN           0x0080
#define U2P3_ENABLE          0x0001
#define PWR_EN               0x0001
#define PHASE2_EN            0x0008
#define UPS_EN               0x10
#define USP_PREWAKE          0x20
#define PCUT_STATUS          0x0001
#define COALESCE_SUPER       85000UL
#define COALESCE_HIGH        250000UL
#define COALESCE_SLOW        524280UL
#define TIMER11_EN           0x0001
#define FIFO_EMPTY_1FB       0x30
#define LPM_TIMER_500MS      0x04
#define LPM_TIMER_500US      0x0c
#define ROK_EXIT_LPM         0x02
#define SEN_VAL_MASK         0xf800
#define SEN_VAL_NORMAL       0xa000
#define SEL_RXIDLE           0x0100
#define SAW_CNT_1MS_MASK     0x0fff
#define UPS_FLAGS_R_TUNE          0x00000001UL
#define UPS_FLAGS_EN_10M_CKDIV    0x00000002UL
#define UPS_FLAGS_250M_CKDIV      0x00000004UL
#define UPS_FLAGS_EN_ALDPS        0x00000008UL
#define UPS_FLAGS_CTAP_SHORT_DIS  0x00000010UL
#define UPS_FLAGS_EN_EEE          0x00100000UL
#define UPS_FLAGS_EN_EEE_CKDIV    0x00400000UL
#define UPS_FLAGS_EEE_PLLOFF_100  0x00800000UL
#define UPS_FLAGS_EEE_PLLOFF_GIGA 0x01000000UL
#define UPS_FLAGS_EEE_CMOD_LV_EN  0x02000000UL
#define UPS_FLAGS_EN_GREEN        0x04000000UL
#define UPS_FLAGS_EN_FLOW_CTR     0x08000000UL
#define ENPWRSAVE            0x8000
#define ENPDNPS              0x0200
#define LINKENA              0x0100
#define DIS_SDSAVE           0x0010
#define PHY_STAT_MASK        0x0007
#define PHY_STAT_EXT_INIT    2
#define PHY_STAT_LAN_ON      3
#define PHY_STAT_PWRDN       5
#define PGA_RETURN_EN        0x0002
#define EEE_CLKDIV_EN        0x8000
#define EN_ALDPS             0x0004
#define EN_10M_PLLOFF        0x0001
#define EEE_10_CAP           0x2000
#define EEE_NWAY_EN          0x1000
#define TX_QUIET_EN          0x0200
#define RX_QUIET_EN          0x0100
#define SD_RISE_TIME_MASK    0x0070
#define SD_RISE_TIME(x)      ((x) << 4)
#define RG_DACQUIET_EN       0x0400
#define RG_LDVQUIET_EN       0x0200
#define FAST_SNR_MASK        0xff80
#define FAST_SNR(x)          ((x) << 7)
#define FUN_ADDR             0x0000
#define FUN_DATA             0x4000
#define CTAP_SHORT_EN        0x0040
#define EEE10_EN             0x0010
#define EN_EEE_CMODE         0x4000
#define EN_EEE_1000          0x2000
#define EN_10M_CLKDIV        0x0800
#define EN_10M_BGOFF         0x0080
#define PATCH_READY          0x0040
#define PATCH_REQUEST        0x0010
#define CKADSEL_L            0x0100
#define ADC_EN               0x0080
#define EN_EMI_L             0x0040
#define CLK_DIV_EXPO(x)      ((x) << 4)
#define GREEN_ETH_EN         0x8000
#define R_TUNE_EN            0x0800

/* PLA_PHYSTATUS (enum rtl_register_content) */
#define RTL_1000BPS          0x10
#define RTL_100BPS           0x08
#define RTL_10BPS            0x04
#define RTL_LINK_STATUS      0x02
#define RTL_FULL_DUP         0x01

/* MII (linux/mii.h) and MDIO (uapi/linux/mdio.h) */
#define MII_BMCR             0x00
#define MII_ADVERTISE        0x04
#define MII_CTRL1000         0x09
#define BMCR_RESET           0x8000
#define BMCR_ANENABLE        0x1000
#define BMCR_PDOWN           0x0800
#define BMCR_ANRESTART       0x0200
#define ADVERTISE_10HALF     0x0020
#define ADVERTISE_10FULL     0x0040
#define ADVERTISE_100HALF    0x0080
#define ADVERTISE_100FULL    0x0100
#define ADVERTISE_PAUSE_CAP  0x0400
#define ADVERTISE_PAUSE_ASYM 0x0800
#define ADVERTISE_1000HALF   0x0100
#define ADVERTISE_1000FULL   0x0200
#define MDIO_MMD_AN          7
#define MDIO_AN_EEE_ADV      60
#define MDIO_EEE_100TX       0x0002
#define MDIO_EEE_1000T       0x0004

#define INTR_LINK            0x0004

/* enum rtl_version */
#define RTL_VER_UNKNOWN 0
#define RTL_VER_01 1
#define RTL_VER_02 2
#define RTL_VER_03 3
#define RTL_VER_04 4
#define RTL_VER_05 5
#define RTL_VER_06 6
#define RTL_VER_07 7
#define RTL_VER_08 8
#define RTL_VER_09 9
#define RTL_VER_14 15         /* after RTL_TEST_01 and RTL_VER_10..13 */

/* enum spd_duplex, for the UPS flags */
#define NWAY_10M_HALF   0
#define NWAY_10M_FULL   1
#define NWAY_100M_HALF  2
#define NWAY_100M_FULL  3
#define NWAY_1000M_FULL 4

#define RTL_RX_DESC 24
#define RTL_RX_ALIGN 8
#define RTL_TX_DESC 8
#define RTL_TX_ALIGN 4
#define RTL_AGG_BUF 16384UL
#define RTL8152_RMS 1522      /* VLAN_ETH_FRAME_LEN + ETH_FCS_LEN */

typedef struct RTL_STATE {
    nc_u8 Version;            /* RTL_VER_* (Linux enum value) */
    nc_u8 UsbSpeed;           /* NC_SPEED_* */
    nc_u8 Gmii;               /* mii.supports_gmii */
    nc_u8 Carrier;            /* netif_carrier_ok: Link applied "up" */
    nc_u8 Green;              /* GREEN_ETHERNET flag */
    nc_u8 PhyReset;           /* PHY_RESET flag */
    nc_u16 OcpBase;           /* 0xffff: unknown (rtl_reset_ocp_base) */
    nc_u32 Coalesce;
    nc_u32 RxBufSize;
    nc_u16 EeeAdv;
    /* struct ups_info */
    nc_u8 UpsGreen, UpsAldps, UpsEee, UpsFlowControl, UpsEeeCkdiv, UpsEeeCmodLv;
    nc_u8 UpsRTune, Ups10mCkdiv, UpsEeePlloff100, UpsEeePlloffGiga, Ups250mCkdiv;
    nc_u8 UpsCtapShortOff, UpsSpeedDuplex;
    /* receive mode, applied while the carrier is on (_rtl8152_set_rx_mode) */
    nc_u32 RcrAccept;         /* RCR_* accept bits beyond AB|APM */
    nc_u8 Mar[8];             /* PLA_MAR bytes as written */
    nc_u8 Mac[6];
    nc_u32 Timeouts;          /* bounded polls that ran out */
} RTL_STATE;

/* Exposed for the host tests. */
nc_u8 RtlVersionFromTcr0(nc_u32 tcr0);
long RtlProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe);
long RtlRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats);
void RtlMulticastHash(nc_u32 filter, const nc_u8 *multicast, nc_u32 count, nc_u32 *rcr, nc_u8 mar[8]);

#endif
