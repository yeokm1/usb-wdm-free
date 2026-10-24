/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2014 Realtek Semiconductor Corp. All rights reserved.
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * RTL815X chip core: the RTL8152/RTL8152B (RTL_VER_01, 02, 07), RTL8153
 * (03 to 06), RTL8153B (08, 09) and RTL8153C (14) register sequences,
 * adapted from Linux drivers/net/usb/r8152.c at
 * f47d223627f80cb7a1eb14c585fe6cb35de5e984 (ethernet.md section 6.4),
 * behind the netchip.h interface. Function names follow Linux so each
 * routine can be compared with its source.
 *
 * Mapping to Linux: Start is probe (rtl8152_get_version, rtl_ops_init,
 * ops->init, hw_phy_cfg + rtl8152_set_speed from rtl_hw_phy_work_func_t,
 * set_ethernet_addr) followed by ndo_open (ops->up). Link is set_carrier
 * (ops->enable or ops->disable on a carrier change). SetFilter is
 * _rtl8152_set_rx_mode. Suspend is rtl8152_system_suspend (ops->down);
 * Resume is rtl8152_reset_resume (init, hw_phy_cfg, set_speed, address,
 * then up), since a Windows 98 or 2000 system sleep may cut the power.
 * Stop is ndo_stop plus disconnect (ops->down, ops->unload).
 *
 * Where this core departs from Linux, on purpose:
 * - No firmware patch is loaded (ethernet.md 6.4); Linux runs the same
 *   sequences when its request_firmware fails, which is this path.
 * - RX VLAN untagging is off (Linux enables it with the default
 *   NETIF_F_HW_VLAN_CTAG_RX feature): v1 has no VLAN, frames arrive tagged.
 * - No checksum offload or TSO: the transmit descriptor is FS|LS|length.
 * - No wake-up: the saved WoL options are 0, as for a device that cannot
 *   wake; runtime suspend (autosuspend_en) is never enabled.
 * - The MAC pass-through of docks (vendor_mac_passthru_addr_read) and
 *   the Dell TB16 RX-aggregation quirk are not done.
 * - The lower edge stops the USB transfers before disable runs, so the
 *   FIFO-empty waits of rtl_disable may run to their bound; they are
 *   bounded as in Linux (1000 polls), with a 1 ms sleep each.
 * - usb_enable_lpm is a host-side Linux call with no equivalent here.
 */
#include "rtlchip.h"

#define VER(s) ((s)->Version)

/* ------------------------------------------------------------------ */
/* Register access (generic_ocp_read/write and the helpers above them). */

static void Put32(nc_u8 *b, nc_u32 v)
{
    b[0] = (nc_u8)v; b[1] = (nc_u8)(v >> 8); b[2] = (nc_u8)(v >> 16); b[3] = (nc_u8)(v >> 24);
}

static nc_u32 Get32(const nc_u8 *b)
{
    return (nc_u32)b[0] | ((nc_u32)b[1] << 8) | ((nc_u32)b[2] << 16) | ((nc_u32)b[3] << 24);
}

static void get_registers(NET_IO *io, nc_u16 value, nc_u16 index, nc_u16 size, nc_u8 *data)
{
    NcIoRead(io, RTL8152_REQ_GET_REGS, value, index, data, size);
}

static void set_registers(NET_IO *io, nc_u16 value, nc_u16 index, nc_u16 size, const nc_u8 *data)
{
    NcIoWrite(io, RTL8152_REQ_SET_REGS, value, index, data, size);
}

static void generic_ocp_read(NET_IO *io, nc_u16 index, nc_u16 size, nc_u8 *data, nc_u16 type)
{
    const nc_u16 limit = (nc_u16)64;
    if ((size & 3) || !size || (index & 3) || (nc_u32)index + size > 0xffff) {
        if (io->Status >= 0) io->Status = NC_E_MALFORMED;
        return;
    }
    while (size) {
        nc_u16 n = (nc_u16)(size > limit ? limit : size);
        get_registers(io, index, type, n, data);
        index = (nc_u16)(index + n); data += n; size = (nc_u16)(size - n);
    }
}

static void generic_ocp_write(NET_IO *io, nc_u16 index, nc_u16 byteen, nc_u16 size,
                              const nc_u8 *data, nc_u16 type)
{
    const nc_u16 limit = (nc_u16)512;
    nc_u16 byteen_start, byteen_end, byen;
    if ((size & 3) || !size || (index & 3) || (nc_u32)index + size > 0xffff) {
        if (io->Status >= 0) io->Status = NC_E_MALFORMED;
        return;
    }
    byteen_start = (nc_u16)(byteen & BYTE_EN_START_MASK);
    byteen_end = (nc_u16)(byteen & BYTE_EN_END_MASK);
    byen = (nc_u16)(byteen_start | (byteen_start << 4));
    /* Split the first DWORD if the byte_en is not 0xff */
    if (byen != BYTE_EN_DWORD) {
        set_registers(io, index, (nc_u16)(type | byen), 4, data);
        index = (nc_u16)(index + 4); data += 4; size = (nc_u16)(size - 4);
    }
    if (size) {
        byen = (nc_u16)(byteen_end | (byteen_end >> 4));
        /* Split the last DWORD if the byte_en is not 0xff */
        if (byen != BYTE_EN_DWORD) size = (nc_u16)(size - 4);
        while (size) {
            nc_u16 n = (nc_u16)(size > limit ? limit : size);
            set_registers(io, index, (nc_u16)(type | BYTE_EN_DWORD), n, data);
            index = (nc_u16)(index + n); data += n; size = (nc_u16)(size - n);
        }
        /* Set the last DWORD */
        if (byen != BYTE_EN_DWORD) set_registers(io, index, (nc_u16)(type | byen), 4, data);
    }
}

static nc_u32 ocp_read_dword(NET_IO *io, nc_u16 type, nc_u16 index)
{
    nc_u8 b[4];
    generic_ocp_read(io, index, 4, b, type);
    return Get32(b);
}

static void ocp_write_dword(NET_IO *io, nc_u16 type, nc_u16 index, nc_u32 data)
{
    nc_u8 b[4];
    Put32(b, data);
    generic_ocp_write(io, index, BYTE_EN_DWORD, 4, b, type);
}

static nc_u16 ocp_read_word(NET_IO *io, nc_u16 type, nc_u16 index)
{
    nc_u8 b[4];
    nc_u16 byen = BYTE_EN_WORD;
    nc_u8 shift = (nc_u8)(index & 2);
    index &= ~3;
    byen = (nc_u16)(byen << shift);
    generic_ocp_read(io, index, 4, b, (nc_u16)(type | byen));
    return (nc_u16)(Get32(b) >> (shift * 8));
}

static void ocp_write_word(NET_IO *io, nc_u16 type, nc_u16 index, nc_u32 data)
{
    nc_u8 b[4];
    nc_u16 byen = BYTE_EN_WORD;
    nc_u8 shift = (nc_u8)(index & 2);
    data &= 0xffff;
    if (index & 2) {
        byen = (nc_u16)(byen << shift);
        data <<= (shift * 8);
        index &= ~3;
    }
    Put32(b, data);
    generic_ocp_write(io, index, byen, 4, b, type);
}

static nc_u8 ocp_read_byte(NET_IO *io, nc_u16 type, nc_u16 index)
{
    nc_u8 b[4];
    nc_u8 shift = (nc_u8)(index & 3);
    index &= ~3;
    generic_ocp_read(io, index, 4, b, type);
    return (nc_u8)(Get32(b) >> (shift * 8));
}

static void ocp_write_byte(NET_IO *io, nc_u16 type, nc_u16 index, nc_u32 data)
{
    nc_u8 b[4];
    nc_u16 byen = BYTE_EN_BYTE;
    nc_u8 shift = (nc_u8)(index & 3);
    data &= 0xff;
    if (index & 3) {
        byen = (nc_u16)(byen << shift);
        data <<= (shift * 8);
        index &= ~3;
    }
    Put32(b, data);
    generic_ocp_write(io, index, byen, 4, b, type);
}

static void ocp_dword_w0w1(NET_IO *io, nc_u16 type, nc_u16 index, nc_u32 clear, nc_u32 set)
{
    nc_u32 v = ocp_read_dword(io, type, index);
    ocp_write_dword(io, type, index, (v & ~clear) | set);
}

static void ocp_word_w0w1(NET_IO *io, nc_u16 type, nc_u16 index, nc_u16 clear, nc_u16 set)
{
    nc_u16 v = ocp_read_word(io, type, index);
    ocp_write_word(io, type, index, (nc_u16)((v & ~clear) | set));
}

static void ocp_byte_w0w1(NET_IO *io, nc_u16 type, nc_u16 index, nc_u8 clear, nc_u8 set)
{
    nc_u8 v = ocp_read_byte(io, type, index);
    ocp_write_byte(io, type, index, (nc_u8)((v & ~clear) | set));
}

#define ocp_dword_clr_bits(io, t, i, c) ocp_dword_w0w1(io, t, i, c, 0)
#define ocp_dword_set_bits(io, t, i, s) ocp_dword_w0w1(io, t, i, 0, s)
#define ocp_word_clr_bits(io, t, i, c)  ocp_word_w0w1(io, t, i, (nc_u16)(c), 0)
#define ocp_word_set_bits(io, t, i, s)  ocp_word_w0w1(io, t, i, 0, (nc_u16)(s))
#define ocp_byte_clr_bits(io, t, i, c)  ocp_byte_w0w1(io, t, i, (nc_u8)(c), 0)
#define ocp_byte_set_bits(io, t, i, s)  ocp_byte_w0w1(io, t, i, 0, (nc_u8)(s))

static void ocp_word_test_and_clr_bits(NET_IO *io, nc_u16 type, nc_u16 index, nc_u16 clear)
{
    nc_u16 v = ocp_read_word(io, type, index);
    if (v & clear) ocp_write_word(io, type, index, (nc_u16)(v & ~clear));
}

static nc_u16 ocp_reg_read(RTL_STATE *s, NET_IO *io, nc_u16 addr)
{
    nc_u16 base = (nc_u16)(addr & 0xf000);
    if (base != s->OcpBase) {
        ocp_write_word(io, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, base);
        s->OcpBase = base;
    }
    return ocp_read_word(io, MCU_TYPE_PLA, (nc_u16)((addr & 0x0fff) | 0xb000));
}

static void ocp_reg_write(RTL_STATE *s, NET_IO *io, nc_u16 addr, nc_u16 data)
{
    nc_u16 base = (nc_u16)(addr & 0xf000);
    if (base != s->OcpBase) {
        ocp_write_word(io, MCU_TYPE_PLA, PLA_OCP_GPHY_BASE, base);
        s->OcpBase = base;
    }
    ocp_write_word(io, MCU_TYPE_PLA, (nc_u16)((addr & 0x0fff) | 0xb000), data);
}

static void ocp_reg_w0w1(RTL_STATE *s, NET_IO *io, nc_u16 addr, nc_u16 clear, nc_u16 set)
{
    nc_u16 v = ocp_reg_read(s, io, addr);
    ocp_reg_write(s, io, addr, (nc_u16)((v & ~clear) | set));
}
#define ocp_reg_clr_bits(s, io, a, c) ocp_reg_w0w1(s, io, a, (nc_u16)(c), 0)
#define ocp_reg_set_bits(s, io, a, v) ocp_reg_w0w1(s, io, a, 0, (nc_u16)(v))

static void r8152_mdio_write(RTL_STATE *s, NET_IO *io, nc_u16 reg, nc_u16 value)
{
    ocp_reg_write(s, io, (nc_u16)(OCP_BASE_MII + reg * 2), value);
}

static nc_u16 r8152_mdio_read(RTL_STATE *s, NET_IO *io, nc_u16 reg)
{
    return ocp_reg_read(s, io, (nc_u16)(OCP_BASE_MII + reg * 2));
}

static void r8152_mdio_set_bit(RTL_STATE *s, NET_IO *io, nc_u16 reg, nc_u16 set)
{
    r8152_mdio_write(s, io, reg, (nc_u16)(r8152_mdio_read(s, io, reg) | set));
}

static void r8152_mdio_clr_bit(RTL_STATE *s, NET_IO *io, nc_u16 reg, nc_u16 clear)
{
    r8152_mdio_write(s, io, reg, (nc_u16)(r8152_mdio_read(s, io, reg) & ~clear));
}

static void r8152_mdio_test_and_clr_bit(RTL_STATE *s, NET_IO *io, nc_u16 reg, nc_u16 clear)
{
    nc_u16 v = r8152_mdio_read(s, io, reg);
    if (v & clear) r8152_mdio_write(s, io, reg, (nc_u16)(v & ~clear));
}

static void sram_write(RTL_STATE *s, NET_IO *io, nc_u16 addr, nc_u16 data)
{
    ocp_reg_write(s, io, OCP_SRAM_ADDR, addr);
    ocp_reg_write(s, io, OCP_SRAM_DATA, data);
}

static nc_u16 sram_read(RTL_STATE *s, NET_IO *io, nc_u16 addr)
{
    ocp_reg_write(s, io, OCP_SRAM_ADDR, addr);
    return ocp_reg_read(s, io, OCP_SRAM_DATA);
}

static void sram_write_w0w1(RTL_STATE *s, NET_IO *io, nc_u16 addr, nc_u16 clear, nc_u16 set)
{
    nc_u16 v = sram_read(s, io, addr);
    ocp_reg_write(s, io, OCP_SRAM_DATA, (nc_u16)((v & ~clear) | set));
}

static int Gone(NET_IO *io) { return io->Status < 0; }

/* ------------------------------------------------------------------ */
/* Small Linux routines. */

static void rxdy_gated_en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_MISC_1, RXDY_GATED_EN);
    else ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MISC_1, RXDY_GATED_EN);
}

static void r8153_teredo_off(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_03: case RTL_VER_04:
    case RTL_VER_05: case RTL_VER_06: case RTL_VER_07:
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_TEREDO_CFG,
                          TEREDO_SEL | TEREDO_RS_EVENT_MASK | OOB_TEREDO_EN);
        break;
    default:
        /* bits 0 to 7 are write-1-to-clear */
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_TEREDO_CFG, 0xff);
        break;
    }
    ocp_write_word(io, MCU_TYPE_PLA, PLA_WDT6_CTRL, WDT6_SET_MODE);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_REALWOW_TIMER, 0);
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_TEREDO_TIMER, 0);
}

static void rtl_reset_bmu(NET_IO *io)
{
    ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_BMU_RESET, BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
    ocp_byte_set_bits(io, MCU_TYPE_USB, USB_BMU_RESET, BMU_RESET_EP_IN | BMU_RESET_EP_OUT);
}

static void r8152b_reset_packet_filter(NET_IO *io)
{
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_FMC, FMC_FCR_MCU_EN);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_FMC, FMC_FCR_MCU_EN);
}

/* rtl8152_nic_reset, default branch: every version this core serves. */
static void rtl8152_nic_reset(RTL_STATE *s, NET_IO *io)
{
    int i;
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CR, CR_RST);
    for (i = 0; i < 1000; i++) {
        if (Gone(io)) break;
        if (!(ocp_read_byte(io, MCU_TYPE_PLA, PLA_CR) & CR_RST)) return;
        NcIoSleep(io, 1);
    }
    s->Timeouts++;
}

static nc_u16 rtl8152_get_speed(NET_IO *io)
{
    return ocp_read_word(io, MCU_TYPE_PLA, PLA_PHYSTATUS);
}

static void rtl_eee_plus_en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_EEEP_CR, EEEP_CR_EEEP_TX);
    else ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_EEEP_CR, EEEP_CR_EEEP_TX);
}

static void rtl_set_eee_plus(NET_IO *io)
{
    rtl_eee_plus_en(io, (rtl8152_get_speed(io) & RTL_10BPS) != 0);
}

static void rtl_rx_vlan_en(NET_IO *io, int enable)
{
    /* Every version this core serves uses PLA_CPCR. */
    if (enable) ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_CPCR, CPCR_RX_VLAN);
    else ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_CPCR, CPCR_RX_VLAN);
}

static void rtl_tally_reset(NET_IO *io)
{
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_RSTTALLY, TALLY_RESET);
}

static void wait_oob_link_list_ready(RTL_STATE *s, NET_IO *io)
{
    int i;
    for (i = 0; i < 1000; i++) {
        if (Gone(io)) return;
        if (ocp_read_byte(io, MCU_TYPE_PLA, PLA_OOB_CTRL) & LINK_LIST_READY) return;
        NcIoSleep(io, 1);
    }
    s->Timeouts++;
}

static void r8152_power_cut_en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_USB, USB_UPS_CTRL, POWER_CUT);
    else ocp_word_clr_bits(io, MCU_TYPE_USB, USB_UPS_CTRL, POWER_CUT);
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_PM_CTRL_STATUS, RESUME_INDICATE);
}

static void r8153_power_cut_en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_USB, USB_POWER_CUT, PWR_EN | PHASE2_EN);
    else ocp_word_clr_bits(io, MCU_TYPE_USB, USB_POWER_CUT, PWR_EN | PHASE2_EN);
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_MISC_0, PCUT_STATUS);
}

static void r8153b_power_cut_en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_USB, USB_POWER_CUT, PWR_EN | PHASE2_EN);
    else ocp_word_clr_bits(io, MCU_TYPE_USB, USB_POWER_CUT, PWR_EN);
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_MISC_0, PCUT_STATUS);
}

static void r8153_u1u2en(NET_IO *io, int enable)
{
    nc_u8 u1u2[8];
    int i;
    for (i = 0; i < 8; i++) u1u2[i] = (nc_u8)(enable ? 0xff : 0x00);
    generic_ocp_write(io, USB_TOLERANCE, BYTE_EN_SIX_BYTES, 8, u1u2, MCU_TYPE_USB);
}

static void r8153b_u1u2en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_USB, USB_LPM_CONFIG, LPM_U1U2_EN);
    else ocp_word_clr_bits(io, MCU_TYPE_USB, USB_LPM_CONFIG, LPM_U1U2_EN);
}

static void r8153_u2p3en(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_USB, USB_U2P3_CTRL, U2P3_ENABLE);
    else ocp_word_clr_bits(io, MCU_TYPE_USB, USB_U2P3_CTRL, U2P3_ENABLE);
}

static void r8153_mac_clk_speed_down(NET_IO *io, int enable)
{
    if (enable) ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2, MAC_CLK_SPDWN_EN);
    else ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL2, MAC_CLK_SPDWN_EN);
}

static void r8153_queue_wake(NET_IO *io, int enable)
{
    if (enable) ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_INDICATE_FALG, UPCOMING_RUNTIME_D3);
    else ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_INDICATE_FALG, UPCOMING_RUNTIME_D3);
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_SUSPEND_FLAG, LINK_CHG_EVENT);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_EXTRA_STATUS, LINK_CHANGE_FLAG);
}

/* __rtl_set_wol(tp, 0): no wake-up source. */
static void rtl_set_wol_none(NET_IO *io)
{
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_CONFIG34, LINK_ON_WAKE_EN);
    ocp_word_w0w1(io, MCU_TYPE_PLA, PLA_CONFIG5, UWF_EN | BWF_EN | MWF_EN, 0);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_CFG_WOL, MAGIC_EN);
}

/* rtl_runtime_suspend_enable(tp, false) with saved_wolopts 0. */
static void rtl_runtime_suspend_disable(NET_IO *io)
{
    rtl_set_wol_none(io);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_CONFIG34, LINK_OFF_WAKE_EN);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
}

static void r8152_aldps_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) {
        ocp_reg_write(s, io, OCP_ALDPS_CONFIG, ENPWRSAVE | ENPDNPS | LINKENA | DIS_SDSAVE);
    } else {
        ocp_reg_write(s, io, OCP_ALDPS_CONFIG, ENPDNPS | LINKENA | DIS_SDSAVE);
        NcIoSleep(io, 20);
    }
}

static void r8153_aldps_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) {
        ocp_reg_set_bits(s, io, OCP_POWER_CFG, EN_ALDPS);
    } else {
        int i;
        ocp_reg_clr_bits(s, io, OCP_POWER_CFG, EN_ALDPS);
        for (i = 0; i < 20; i++) {
            if (Gone(io)) return;
            NcIoSleep(io, 1);
            if (ocp_read_word(io, MCU_TYPE_PLA, 0xe000) & 0x0100) break;
        }
    }
    s->UpsAldps = (nc_u8)enable;
}

static void r8152_mmd_write(RTL_STATE *s, NET_IO *io, nc_u16 dev, nc_u16 reg, nc_u16 data)
{
    ocp_reg_write(s, io, OCP_EEE_AR, (nc_u16)(FUN_ADDR | dev));
    ocp_reg_write(s, io, OCP_EEE_DATA, reg);
    ocp_reg_write(s, io, OCP_EEE_AR, (nc_u16)(FUN_DATA | dev));
    ocp_reg_write(s, io, OCP_EEE_DATA, data);
    ocp_reg_write(s, io, OCP_EEE_AR, 0x0000);
}

static void r8152_eee_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) {
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_EEE_CR, EEE_RX_EN | EEE_TX_EN);
        ocp_reg_w0w1(s, io, OCP_EEE_CONFIG1, SD_RISE_TIME_MASK,
                     EEE_10_CAP | EEE_NWAY_EN | TX_QUIET_EN | RX_QUIET_EN | SD_RISE_TIME(1));
        ocp_reg_set_bits(s, io, OCP_EEE_CONFIG2, RG_DACQUIET_EN | RG_LDVQUIET_EN);
        ocp_reg_w0w1(s, io, OCP_EEE_CONFIG3, FAST_SNR_MASK, FAST_SNR(42));
    } else {
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_EEE_CR, EEE_RX_EN | EEE_TX_EN);
        ocp_reg_w0w1(s, io, OCP_EEE_CONFIG1, SD_RISE_TIME_MASK | EEE_10_CAP | EEE_NWAY_EN |
                     TX_QUIET_EN | RX_QUIET_EN, SD_RISE_TIME(7));
        ocp_reg_clr_bits(s, io, OCP_EEE_CONFIG2, RG_DACQUIET_EN | RG_LDVQUIET_EN);
        ocp_reg_w0w1(s, io, OCP_EEE_CONFIG3, FAST_SNR_MASK, (nc_u16)FAST_SNR(511));
    }
}

static void r8153_eee_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) {
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_EEE_CR, EEE_RX_EN | EEE_TX_EN);
        ocp_reg_set_bits(s, io, OCP_EEE_CFG, EEE10_EN);
    } else {
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_EEE_CR, EEE_RX_EN | EEE_TX_EN);
        ocp_reg_clr_bits(s, io, OCP_EEE_CFG, EEE10_EN);
    }
    s->UpsEee = (nc_u8)enable;
}

static void rtl_eee_enable(RTL_STATE *s, NET_IO *io, int enable)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:
        r8152_eee_en(s, io, enable);
        r8152_mmd_write(s, io, MDIO_MMD_AN, MDIO_AN_EEE_ADV, (nc_u16)(enable ? s->EeeAdv : 0));
        break;
    default:
        r8153_eee_en(s, io, enable);
        ocp_reg_write(s, io, OCP_EEE_ADV, (nc_u16)(enable ? s->EeeAdv : 0));
        break;
    }
}

static void r8152b_enable_fc(RTL_STATE *s, NET_IO *io)
{
    r8152_mdio_set_bit(s, io, MII_ADVERTISE, ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
    s->UpsFlowControl = 1;
}

static void rtl_green_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) sram_write_w0w1(s, io, SRAM_GREEN_CFG, 0, GREEN_ETH_EN);
    else sram_write_w0w1(s, io, SRAM_GREEN_CFG, GREEN_ETH_EN, 0);
    s->UpsGreen = (nc_u8)enable;
}

static void r8153b_green_en(RTL_STATE *s, NET_IO *io, int enable)
{
    if (enable) {
        sram_write(s, io, 0x8045, 0);        /* 10M abiq&ldvbias */
        sram_write(s, io, 0x804d, 0x1222);   /* 100M short abiq&ldvbias */
        sram_write(s, io, 0x805d, 0x0022);   /* 1000M short abiq&ldvbias */
    } else {
        sram_write(s, io, 0x8045, 0x2444);
        sram_write(s, io, 0x804d, 0x2444);
        sram_write(s, io, 0x805d, 0x2444);
    }
    rtl_green_en(s, io, 1);
}

static nc_u16 r8153_phy_status(RTL_STATE *s, NET_IO *io, nc_u16 desired)
{
    nc_u16 data = 0;
    int i;
    for (i = 0; i < 500; i++) {
        data = (nc_u16)(ocp_reg_read(s, io, OCP_PHY_STATUS) & PHY_STAT_MASK);
        if (desired) {
            if (data == desired) return data;
        } else if (data == PHY_STAT_LAN_ON || data == PHY_STAT_PWRDN || data == PHY_STAT_EXT_INIT) {
            return data;
        }
        NcIoSleep(io, 20);
        if (Gone(io)) return data;
    }
    s->Timeouts++;
    return data;
}

static int rtl_phy_patch_request(RTL_STATE *s, NET_IO *io, int request, int wait)
{
    nc_u16 check;
    int i;
    if (request) {
        ocp_reg_set_bits(s, io, OCP_PHY_PATCH_CMD, PATCH_REQUEST);
        check = 0;
    } else {
        ocp_reg_clr_bits(s, io, OCP_PHY_PATCH_CMD, PATCH_REQUEST);
        check = PATCH_READY;
    }
    for (i = 0; wait && i < 5000; i++) {
        if (Gone(io)) return -1;
        NcIoSleep(io, 1);
        if ((ocp_reg_read(s, io, OCP_PHY_PATCH_STAT) & PATCH_READY) ^ check) break;
    }
    if (request && wait && !(ocp_reg_read(s, io, OCP_PHY_PATCH_STAT) & PATCH_READY)) {
        rtl_phy_patch_request(s, io, 0, 0);
        return -1;
    }
    return 0;
}

static nc_u32 r8152_efuse_read(NET_IO *io, nc_u8 addr)
{
    nc_u32 v;
    ocp_write_word(io, MCU_TYPE_PLA, PLA_EFUSE_CMD, EFUSE_READ_CMD | addr);
    v = ocp_read_word(io, MCU_TYPE_PLA, PLA_EFUSE_CMD);
    v = (v & EFUSE_DATA_BIT16) << 9;     /* data of bit16 */
    v |= ocp_read_word(io, MCU_TYPE_PLA, PLA_EFUSE_DATA);
    return v;
}

/* r8153b_ups_flags is used only when UPS (runtime suspend) is enabled,
 * which this core never does; the Ups* fields keep the state Linux keeps. */

/* ------------------------------------------------------------------ */
/* PHY configuration and speed. */

static void rtl8152_set_speed(RTL_STATE *s, NET_IO *io)
{
    nc_u16 orig, new1, bmcr;
    int i;
    /* AUTONEG_ENABLE with the default advertising of rtl8152_probe_once:
     * 10/100 half and full, and 1000 full with a gigabit PHY. */
    orig = r8152_mdio_read(s, io, MII_ADVERTISE);
    new1 = (nc_u16)(orig & ~(ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL));
    new1 |= ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL;
    s->UpsSpeedDuplex = NWAY_100M_FULL;
    if (orig != new1) r8152_mdio_write(s, io, MII_ADVERTISE, new1);
    if (s->Gmii) {
        orig = r8152_mdio_read(s, io, MII_CTRL1000);
        new1 = (nc_u16)((orig & ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF)) | ADVERTISE_1000FULL);
        s->UpsSpeedDuplex = NWAY_1000M_FULL;
        if (orig != new1) r8152_mdio_write(s, io, MII_CTRL1000, new1);
    }
    bmcr = BMCR_ANENABLE | BMCR_ANRESTART;
    if (s->PhyReset) { s->PhyReset = 0; bmcr |= BMCR_RESET; }
    r8152_mdio_write(s, io, MII_BMCR, bmcr);
    if (bmcr & BMCR_RESET) {
        for (i = 0; i < 50; i++) {
            NcIoSleep(io, 20);
            if (Gone(io)) return;
            if (!(r8152_mdio_read(s, io, MII_BMCR) & BMCR_RESET)) break;
        }
    }
}

static void r8152b_hw_phy_cfg(RTL_STATE *s, NET_IO *io)
{
    rtl_eee_enable(s, io, 1);           /* tp->eee_en */
    r8152_aldps_en(s, io, 1);
    r8152b_enable_fc(s, io);
    s->PhyReset = 1;
}

static void r8153_hw_phy_cfg(RTL_STATE *s, NET_IO *io)
{
    r8153_aldps_en(s, io, 0);
    rtl_eee_enable(s, io, 0);
    if (VER(s) == RTL_VER_03) ocp_reg_clr_bits(s, io, OCP_EEE_CFG, CTAP_SHORT_EN);
    ocp_reg_set_bits(s, io, OCP_POWER_CFG, EEE_CLKDIV_EN);
    ocp_reg_set_bits(s, io, OCP_DOWN_SPEED, EN_10M_BGOFF);
    ocp_reg_set_bits(s, io, OCP_POWER_CFG, EN_10M_PLLOFF);
    sram_write(s, io, SRAM_IMPEDANCE, 0x0b13);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_PHY_PWR, PFM_PWM_SWITCH);
    sram_write(s, io, SRAM_LPF_CFG, 0xf70f);       /* LPF corner auto tune */
    sram_write(s, io, SRAM_10M_AMP1, 0x00af);      /* 10M amplitude */
    sram_write(s, io, SRAM_10M_AMP2, 0x0208);
    rtl_eee_enable(s, io, 1);
    r8153_aldps_en(s, io, 1);
    r8152b_enable_fc(s, io);
    if (VER(s) != RTL_VER_03 && VER(s) != RTL_VER_04) r8153_u2p3en(io, 1);
    s->PhyReset = 1;
}

static void r8153b_hw_phy_cfg(RTL_STATE *s, NET_IO *io)
{
    nc_u32 v;
    nc_u16 data;
    ocp_word_test_and_clr_bits(io, MCU_TYPE_USB, USB_MISC_0, PCUT_STATUS);
    r8153_aldps_en(s, io, 0);
    rtl_eee_enable(s, io, 0);
    ocp_write_word(io, MCU_TYPE_USB, USB_U1U2_TIMER, 500);   /* U1/U2/L1 idle timer, 500 us */
    data = r8153_phy_status(s, io, 0);
    if (data == PHY_STAT_PWRDN || data == PHY_STAT_EXT_INIT)
        r8152_mdio_clr_bit(s, io, MII_BMCR, BMCR_PDOWN);
    r8153b_green_en(s, io, s->Green);
    sram_write_w0w1(s, io, SRAM_GREEN_CFG, 0, R_TUNE_EN);
    ocp_reg_set_bits(s, io, OCP_NCTL_CFG, PGA_RETURN_EN);
    /* ADC bias calibration from efuse 0x7d: drop the dummy bit 3. */
    v = r8152_efuse_read(io, 0x7d);
    data = (nc_u16)(((v & 0x1fff0) >> 1) | (v & 0x7));
    if (data != 0xffff) ocp_reg_write(s, io, OCP_ADC_IOFFSET, data);
    /* UPS tx-link-pulse timing: swr_cnt_1ms_ini = 16000000 / rg_saw_cnt */
    v = ocp_reg_read(s, io, 0xc426) & 0x3fff;
    if (v) ocp_word_w0w1(io, MCU_TYPE_USB, USB_UPS_CFG, SAW_CNT_1MS_MASK,
                         (nc_u16)((16000000UL / v) & SAW_CNT_1MS_MASK));
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_PHY_PWR, PFM_PWM_SWITCH);
    /* Advance EEE */
    if (!rtl_phy_patch_request(s, io, 1, 1)) {
        ocp_reg_set_bits(s, io, OCP_POWER_CFG, EEE_CLKDIV_EN);
        s->UpsEeeCkdiv = 1;
        ocp_reg_set_bits(s, io, OCP_DOWN_SPEED, EN_EEE_CMODE | EN_EEE_1000 | EN_10M_CLKDIV);
        s->UpsEeeCmodLv = 1;
        s->Ups10mCkdiv = 1;
        s->UpsEeePlloffGiga = 1;
        ocp_reg_write(s, io, OCP_SYSCLK_CFG, 0);
        ocp_reg_write(s, io, OCP_SYSCLK_CFG, CLK_DIV_EXPO(5));
        s->Ups250mCkdiv = 1;
        rtl_phy_patch_request(s, io, 0, 1);
    }
    rtl_eee_enable(s, io, 1);
    r8153_aldps_en(s, io, 1);
    r8152b_enable_fc(s, io);
    s->PhyReset = 1;
}

static void hw_phy_cfg(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07: r8152b_hw_phy_cfg(s, io); break;
    case RTL_VER_08: case RTL_VER_09: r8153b_hw_phy_cfg(s, io); break;
    case RTL_VER_14: r8153b_hw_phy_cfg(s, io); s->UpsRTune = 1; break;  /* r8153c_hw_phy_cfg */
    default: r8153_hw_phy_cfg(s, io); break;
    }
}

static void wait_autoload(RTL_STATE *s, NET_IO *io)
{
    int i;
    for (i = 0; i < 500; i++) {
        if (Gone(io)) return;
        if (ocp_read_word(io, MCU_TYPE_PLA, PLA_BOOT_CTRL) & AUTOLOAD_DONE) return;
        NcIoSleep(io, 20);
    }
    s->Timeouts++;
}

static void r8153b_ups_en_off(RTL_STATE *s, NET_IO *io, int c)
{
    /* r8153b_ups_en(tp, false) and, with c, r8153c_ups_en(tp, false) */
    ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_POWER_CUT, UPS_EN | USP_PREWAKE);
    ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_MISC_2, UPS_FORCE_PWR_DOWN);
    if (ocp_read_word(io, MCU_TYPE_USB, USB_MISC_0) & PCUT_STATUS) {
        wait_autoload(s, io);
        if (Gone(io)) return;
        hw_phy_cfg(s, io);
        rtl8152_set_speed(s, io);
    }
    if (c) {
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_CONFIG34, 0x0100);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
    }
}

/* ------------------------------------------------------------------ */
/* init (ops->init) */

static void r8152b_init(RTL_STATE *s, NET_IO *io)
{
    r8152_mdio_test_and_clr_bit(s, io, MII_BMCR, BMCR_PDOWN);
    r8152_aldps_en(s, io, 0);
    if (VER(s) == RTL_VER_01) ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_LED_FEATURE, LED_MODE_MASK);
    r8152_power_cut_en(io, 0);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_PHY_PWR, TX_10M_IDLE_EN | PFM_PWM_SWITCH);
    ocp_dword_w0w1(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL, MCU_CLK_RATIO_MASK, MCU_CLK_RATIO | D3_CLK_GATED_EN);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_GPHY_INTR_IMR,
                   GPHY_STS_MSK | SPEED_DOWN_MSK | SPDWN_RXDV_MSK | SPDWN_LINKCHG_MSK);
    rtl_tally_reset(io);
    /* enable rx aggregation */
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_USB_CTRL, RX_AGG_DISABLE | RX_ZERO_EN);
}

static void r8153_init(RTL_STATE *s, NET_IO *io)
{
    nc_u8 lpm;
    r8153_u1u2en(io, 0);
    wait_autoload(s, io);
    r8153_phy_status(s, io, 0);
    if (VER(s) == RTL_VER_03 || VER(s) == RTL_VER_04 || VER(s) == RTL_VER_05)
        ocp_reg_write(s, io, OCP_ADC_CFG, CKADSEL_L | ADC_EN | EN_EMI_L);
    r8152_mdio_test_and_clr_bit(s, io, MII_BMCR, BMCR_PDOWN);
    r8153_phy_status(s, io, PHY_STAT_LAN_ON);
    r8153_u2p3en(io, 0);
    if (VER(s) == RTL_VER_04) {
        ocp_word_w0w1(io, MCU_TYPE_USB, USB_SSPHYLINK2, PWD_DN_SCALE_MASK, PWD_DN_SCALE(96));
        ocp_byte_set_bits(io, MCU_TYPE_USB, USB_USB2PHY, USB2PHY_L1 | USB2PHY_SUSPEND);
    } else if (VER(s) == RTL_VER_05) {
        ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_DMY_REG0, ECM_ALDPS);
        if (ocp_read_word(io, MCU_TYPE_USB, USB_BURST_SIZE) == 0)
            ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_CSR_DUMMY1, DYNAMIC_BURST);
        else
            ocp_byte_set_bits(io, MCU_TYPE_USB, USB_CSR_DUMMY1, DYNAMIC_BURST);
    } else if (VER(s) == RTL_VER_06) {
        if (ocp_read_word(io, MCU_TYPE_USB, USB_BURST_SIZE) == 0)
            ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_CSR_DUMMY1, DYNAMIC_BURST);
        else
            ocp_byte_set_bits(io, MCU_TYPE_USB, USB_CSR_DUMMY1, DYNAMIC_BURST);
        r8153_queue_wake(io, 0);
        if (rtl8152_get_speed(io) & RTL_LINK_STATUS)
            ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_EXTRA_STATUS, CUR_LINK_OK | POLL_LINK_CHG);
        else
            ocp_word_w0w1(io, MCU_TYPE_PLA, PLA_EXTRA_STATUS, CUR_LINK_OK, POLL_LINK_CHG);
    }
    ocp_byte_set_bits(io, MCU_TYPE_USB, USB_CSR_DUMMY2, EP4_FULL_FC);
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_WDT11_CTRL, TIMER11_EN);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_LED_FEATURE, LED_MODE_MASK);
    lpm = FIFO_EMPTY_1FB | ROK_EXIT_LPM;
    if (VER(s) == RTL_VER_04 && s->UsbSpeed != NC_SPEED_SUPER) lpm |= LPM_TIMER_500MS;
    else lpm |= LPM_TIMER_500US;
    ocp_write_byte(io, MCU_TYPE_USB, USB_LPM_CTRL, lpm);
    ocp_word_w0w1(io, MCU_TYPE_USB, USB_AFE_CTRL2, SEN_VAL_MASK, SEN_VAL_NORMAL | SEL_RXIDLE);
    ocp_write_word(io, MCU_TYPE_USB, USB_CONNECT_TIMER, 0x0001);
    r8153_power_cut_en(io, 0);
    rtl_runtime_suspend_disable(io);
    r8153_mac_clk_speed_down(io, 0);
    r8153_u1u2en(io, 1);
    ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_CONFIG6, LANWAKE_CLR_EN);
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_LWAKE_CTRL_REG, LANWAKE_PIN);
    /* rx aggregation */
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_USB_CTRL, RX_AGG_DISABLE | RX_ZERO_EN);
    rtl_tally_reset(io);
    s->Coalesce = s->UsbSpeed == NC_SPEED_SUPER ? COALESCE_SUPER :
                  s->UsbSpeed == NC_SPEED_HIGH ? COALESCE_HIGH : COALESCE_SLOW;
}

static void r8153b_init(RTL_STATE *s, NET_IO *io, int c)
{
    r8153b_u1u2en(io, 0);
    if (c) {
        /* r8153c_init: disable spi_en */
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_CONFIG5, 0x0008);
        ocp_word_set_bits(io, MCU_TYPE_USB, 0xcbf0, 0x0002);
    }
    wait_autoload(s, io);
    r8153_phy_status(s, io, 0);
    r8152_mdio_test_and_clr_bit(s, io, MII_BMCR, BMCR_PDOWN);
    r8153_phy_status(s, io, PHY_STAT_LAN_ON);
    r8153_u2p3en(io, 0);
    ocp_write_word(io, MCU_TYPE_USB, USB_MSC_TIMER, 0x0fff);   /* 0xfff * 8 ms */
    r8153b_power_cut_en(io, 0);
    r8153b_ups_en_off(s, io, c);
    r8153_queue_wake(io, 0);
    rtl_runtime_suspend_disable(io);
    if (rtl8152_get_speed(io) & RTL_LINK_STATUS)
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_EXTRA_STATUS, CUR_LINK_OK | POLL_LINK_CHG);
    else
        ocp_word_w0w1(io, MCU_TYPE_PLA, PLA_EXTRA_STATUS, CUR_LINK_OK, POLL_LINK_CHG);
    if (c || s->UsbSpeed == NC_SPEED_SUPER) r8153b_u1u2en(io, 1);
    r8153_mac_clk_speed_down(io, 1);        /* MAC clock speed down */
    if (c) {
        ocp_byte_clr_bits(io, MCU_TYPE_USB, USB_MISC_2, 0x80);
    } else {
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, PLA_MCU_SPDWN_EN);
        if (VER(s) == RTL_VER_09 && (ocp_read_byte(io, MCU_TYPE_PLA, 0xdc00) & 0x20))
            ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_PHY_PWR, TEST_IO_OFF);  /* 32QFN */
    }
    s->Green = 1;
    /* rx aggregation */
    ocp_word_clr_bits(io, MCU_TYPE_USB, USB_USB_CTRL, RX_AGG_DISABLE | RX_ZERO_EN);
    rtl_tally_reset(io);
    s->Coalesce = 15000;                    /* 15 us */
}

static void ops_init(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07: r8152b_init(s, io); break;
    case RTL_VER_08: case RTL_VER_09: r8153b_init(s, io, 0); break;
    case RTL_VER_14: r8153b_init(s, io, 1); break;
    default: r8153_init(s, io); break;
    }
}

/* ------------------------------------------------------------------ */
/* up / down (ops->up, ops->down), oob and first_init */

static void r8152b_exit_oob(RTL_STATE *s, NET_IO *io)
{
    ocp_dword_clr_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
    rxdy_gated_en(io, 1);
    r8153_teredo_off(s, io);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CR, 0x00);
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);
    wait_oob_link_list_ready(s, io);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
    wait_oob_link_list_ready(s, io);
    rtl8152_nic_reset(s, io);
    /* rx share fifo credit full threshold */
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_NORMAL);
    if (s->UsbSpeed == NC_SPEED_FULL) {
        ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_FULL);
        ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_FULL);
    } else {
        ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_HIGH);
        ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_HIGH);
    }
    /* TX share fifo free credit full threshold */
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, TXFIFO_THR_NORMAL2);
    ocp_write_byte(io, MCU_TYPE_USB, USB_TX_AGG, TX_AGG_MAX_THRESHOLD);
    ocp_write_dword(io, MCU_TYPE_USB, USB_RX_BUF_TH, RX_THR_HIGH);
    ocp_write_dword(io, MCU_TYPE_USB, USB_TX_DMA, TEST_MODE_DISABLE | TX_SIZE_ADJUST1);
    rtl_rx_vlan_en(io, 0);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_TCR0, TCR0_AUTO_FIFO);
}

/* rtl_disable, without the USB parts (the lower edge has stopped the
 * transfers already). */
static void rtl_disable(RTL_STATE *s, NET_IO *io)
{
    int i;
    if (Gone(io)) return;
    ocp_dword_clr_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
    rxdy_gated_en(io, 1);
    for (i = 0; i < 1000; i++) {
        if (Gone(io)) return;
        if ((ocp_read_byte(io, MCU_TYPE_PLA, PLA_OOB_CTRL) & FIFO_EMPTY) == FIFO_EMPTY) break;
        NcIoSleep(io, 1);
    }
    if (i == 1000) s->Timeouts++;
    for (i = 0; i < 1000; i++) {
        if (Gone(io)) return;
        if (ocp_read_word(io, MCU_TYPE_PLA, PLA_TCR0) & TCR0_TX_EMPTY) break;
        NcIoSleep(io, 1);
    }
    if (i == 1000) s->Timeouts++;
    rtl8152_nic_reset(s, io);
}

static void r8152b_enter_oob(RTL_STATE *s, NET_IO *io)
{
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_OOB);
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_OOB);
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_OOB);
    rtl_disable(s, io);
    wait_oob_link_list_ready(s, io);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
    wait_oob_link_list_ready(s, io);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);
    rtl_rx_vlan_en(io, 1);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_BDC_CR, ALDPS_PROXY_MODE);
    ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB | DIS_MCU_CLROOB);
    rxdy_gated_en(io, 0);
    ocp_dword_set_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_APM | RCR_AM | RCR_AB);
}

static void rtl8153_change_mtu(NET_IO *io)
{
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);   /* mtu_to_size(1500) */
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_MTPS, MTPS_JUMBO);
}

static void r8153_first_init(RTL_STATE *s, NET_IO *io)
{
    rxdy_gated_en(io, 1);
    r8153_teredo_off(s, io);
    ocp_dword_clr_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
    rtl8152_nic_reset(s, io);
    rtl_reset_bmu(io);
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
    ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);
    wait_oob_link_list_ready(s, io);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
    wait_oob_link_list_ready(s, io);
    rtl_rx_vlan_en(io, 0);
    rtl8153_change_mtu(io);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_TCR0, TCR0_AUTO_FIFO);
    rtl8152_nic_reset(s, io);
    /* rx share fifo credit full threshold */
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_NORMAL);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_NORMAL);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_NORMAL);
    /* TX share fifo free credit full threshold */
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, TXFIFO_THR_NORMAL2);
}

static void r8153_enter_oob(RTL_STATE *s, NET_IO *io)
{
    ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
    /* RX FIFO settings for OOB */
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, RXFIFO_THR1_OOB);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_OOB);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_OOB);
    rtl_disable(s, io);
    rtl_reset_bmu(io);
    wait_oob_link_list_ready(s, io);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
    wait_oob_link_list_ready(s, io);
    ocp_write_word(io, MCU_TYPE_PLA, PLA_RMS, 1522);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_MTPS, MTPS_DEFAULT);
    switch (VER(s)) {
    case RTL_VER_03: case RTL_VER_04: case RTL_VER_05: case RTL_VER_06:
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_TEREDO_CFG, TEREDO_WAKE_MASK);
        break;
    default:   /* RTL_VER_08, 09, 14: clear the teredo wake events */
        ocp_write_word(io, MCU_TYPE_PLA, PLA_TEREDO_WAKE_BASE, 0x00ff);
        break;
    }
    rtl_rx_vlan_en(io, 1);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_BDC_CR, ALDPS_PROXY_MODE);
    ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB | DIS_MCU_CLROOB);
    ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);
    rxdy_gated_en(io, 0);
    ocp_dword_set_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_APM | RCR_AM | RCR_AB);
}

static void ops_up(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:     /* rtl8152_up */
        r8152_aldps_en(s, io, 0);
        r8152b_exit_oob(s, io);
        r8152_aldps_en(s, io, 1);
        break;
    case RTL_VER_08: case RTL_VER_09:                      /* rtl8153b_up */
        r8153b_u1u2en(io, 0);
        r8153_u2p3en(io, 0);
        r8153_aldps_en(s, io, 0);
        r8153_first_init(s, io);
        ocp_write_dword(io, MCU_TYPE_USB, USB_RX_BUF_TH, RX_THR_B);
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, PLA_MCU_SPDWN_EN);
        r8153_aldps_en(s, io, 1);
        if (s->UsbSpeed == NC_SPEED_SUPER) r8153b_u1u2en(io, 1);
        break;
    case RTL_VER_14:                                       /* rtl8153c_up */
        r8153b_u1u2en(io, 0);
        r8153_u2p3en(io, 0);
        r8153_aldps_en(s, io, 0);
        rxdy_gated_en(io, 1);
        r8153_teredo_off(s, io);
        ocp_dword_clr_bits(io, MCU_TYPE_PLA, PLA_RCR, RCR_ACPT_ALL);
        rtl8152_nic_reset(s, io);
        rtl_reset_bmu(io);
        ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_OOB_CTRL, NOW_IS_OOB);
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, MCU_BORW_EN);
        wait_oob_link_list_ready(s, io);
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_SFF_STS_7, RE_INIT_LL);
        wait_oob_link_list_ready(s, io);
        rtl_rx_vlan_en(io, 0);
        /* rtl8153c_change_mtu at MTU 1500 */
        ocp_write_word(io, MCU_TYPE_PLA, PLA_RMS, RTL8152_RMS);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_MTPS, 10 * 1024 / 64);
        ocp_write_word(io, MCU_TYPE_PLA, PLA_TXFIFO_CTRL, 512 / 64);
        ocp_write_word(io, MCU_TYPE_PLA, PLA_TXFIFO_FULL, 2048 / 8);
        rtl8152_nic_reset(s, io);
        /* rx share fifo credit full threshold */
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL0, 0x02);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_RXFIFO_FULL, 0x08);
        ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL1, RXFIFO_THR2_NORMAL);
        ocp_write_word(io, MCU_TYPE_PLA, PLA_RXFIFO_CTRL2, RXFIFO_THR3_NORMAL);
        ocp_write_dword(io, MCU_TYPE_USB, USB_RX_BUF_TH, RX_THR_B);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_CONFIG34, 0x0100);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, PLA_MCU_SPDWN_EN);
        r8153_aldps_en(s, io, 1);
        r8153b_u1u2en(io, 1);
        break;
    default:                                               /* rtl8153_up */
        r8153_u1u2en(io, 0);
        r8153_u2p3en(io, 0);
        r8153_aldps_en(s, io, 0);
        r8153_first_init(s, io);
        ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_CONFIG6, LANWAKE_CLR_EN);
        ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_LWAKE_CTRL_REG, LANWAKE_PIN);
        ocp_word_clr_bits(io, MCU_TYPE_USB, USB_SSPHYLINK1, DELAY_PHY_PWR_CHG);
        r8153_aldps_en(s, io, 1);
        if (VER(s) != RTL_VER_03 && VER(s) != RTL_VER_04) r8153_u2p3en(io, 1);
        r8153_u1u2en(io, 1);
        break;
    }
}

static void ops_down(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:     /* rtl8152_down */
        r8152_power_cut_en(io, 0);
        r8152_aldps_en(s, io, 0);
        r8152b_enter_oob(s, io);
        r8152_aldps_en(s, io, 1);
        break;
    case RTL_VER_08: case RTL_VER_09: case RTL_VER_14:     /* rtl8153b_down */
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL3, PLA_MCU_SPDWN_EN);
        r8153b_u1u2en(io, 0);
        r8153_u2p3en(io, 0);
        r8153b_power_cut_en(io, 0);
        r8153_aldps_en(s, io, 0);
        r8153_enter_oob(s, io);
        r8153_aldps_en(s, io, 1);
        break;
    default:                                               /* rtl8153_down */
        ocp_byte_clr_bits(io, MCU_TYPE_PLA, PLA_CONFIG6, LANWAKE_CLR_EN);
        r8153_u1u2en(io, 0);
        r8153_u2p3en(io, 0);
        r8153_power_cut_en(io, 0);
        r8153_aldps_en(s, io, 0);
        r8153_enter_oob(s, io);
        r8153_aldps_en(s, io, 1);
        break;
    }
}

static void ops_unload(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: break;
    case RTL_VER_02: case RTL_VER_07: r8152_power_cut_en(io, 1); break;   /* rtl8152_unload */
    case RTL_VER_08: case RTL_VER_09: r8153b_power_cut_en(io, 0); break;  /* rtl8153b_unload */
    default: r8153_power_cut_en(io, 0); break;   /* rtl8153_unload (03-06 and 14) */
    }
}

/* ------------------------------------------------------------------ */
/* enable / disable (ops->enable, ops->disable) */

static void rtl_set_ifg(NET_IO *io, nc_u16 speed)
{
    if ((speed & (RTL_10BPS | RTL_100BPS)) && !(speed & RTL_FULL_DUP)) {
        ocp_word_w0w1(io, MCU_TYPE_PLA, PLA_TCR1, IFG_MASK, IFG_144NS);
        ocp_word_clr_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4, TX10MIDLE_EN);
    } else {
        ocp_word_w0w1(io, MCU_TYPE_PLA, PLA_TCR1, IFG_MASK, IFG_96NS);
        ocp_word_set_bits(io, MCU_TYPE_PLA, PLA_MAC_PWR_CTRL4, TX10MIDLE_EN);
    }
}

static void rtl_enable(RTL_STATE *s, NET_IO *io)
{
    r8152b_reset_packet_filter(io);
    ocp_byte_set_bits(io, MCU_TYPE_PLA, PLA_CR, CR_RE | CR_TE);
    if (VER(s) > RTL_VER_07)   /* r8153b_rx_agg_chg_indicate */
        ocp_write_byte(io, MCU_TYPE_USB, USB_UPT_RXDMA_OWN, OWN_UPDATE | OWN_CLEAR);
    rxdy_gated_en(io, 0);
}

static void ops_enable(RTL_STATE *s, NET_IO *io)
{
    nc_u32 early;
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:     /* rtl8152_enable */
        rtl_set_eee_plus(io);
        rtl_enable(s, io);
        return;
    default:                                               /* rtl8153_enable */
        break;
    }
    rtl_set_eee_plus(io);
    /* r8153_set_rx_early_timeout */
    if (VER(s) <= RTL_VER_06) {
        ocp_write_word(io, MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT, s->Coalesce / 8);
    } else {
        ocp_write_word(io, MCU_TYPE_USB, USB_RX_EARLY_TIMEOUT, 128 / 8);
        ocp_write_word(io, MCU_TYPE_USB, USB_RX_EXTRA_AGGR_TMR, s->Coalesce / 8);
    }
    /* r8153_set_rx_early_size: rx_buf_sz - rx_reserved_size(1500) */
    early = s->RxBufSize - (RTL8152_RMS + RTL_RX_DESC + RTL_RX_ALIGN);
    ocp_write_word(io, MCU_TYPE_USB, USB_RX_EARLY_SIZE, VER(s) <= RTL_VER_06 ? early / 4 : early / 8);
    rtl_set_ifg(io, rtl8152_get_speed(io));
    if (VER(s) == RTL_VER_09 || VER(s) == RTL_VER_14) {
        ocp_word_clr_bits(io, MCU_TYPE_USB, USB_FW_TASK, FC_PATCH_TASK);
        NcIoSleep(io, 1);
        ocp_word_set_bits(io, MCU_TYPE_USB, USB_FW_TASK, FC_PATCH_TASK);
    }
    rtl_enable(s, io);
}

static void ops_disable(RTL_STATE *s, NET_IO *io)
{
    switch (VER(s)) {
    case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:     /* rtl8152_disable */
        r8152_aldps_en(s, io, 0);
        rtl_disable(s, io);
        r8152_aldps_en(s, io, 1);
        break;
    default:                                               /* rtl8153_disable */
        r8153_aldps_en(s, io, 0);
        rtl_disable(s, io);
        rtl_reset_bmu(io);
        r8153_aldps_en(s, io, 1);
        break;
    }
}

/* _rtl8152_set_rx_mode, with the stored accept bits and hash. */
static void apply_rx_mode(RTL_STATE *s, NET_IO *io)
{
    nc_u32 rcr = ocp_read_dword(io, MCU_TYPE_PLA, PLA_RCR);
    rcr &= ~RCR_ACPT_ALL;
    rcr |= RCR_AB | RCR_APM | s->RcrAccept;
    generic_ocp_write(io, PLA_MAR, BYTE_EN_DWORD, 8, s->Mar, MCU_TYPE_PLA);
    ocp_write_dword(io, MCU_TYPE_PLA, PLA_RCR, rcr);
}

/* ------------------------------------------------------------------ */
/* The netchip.h interface. */

nc_u8 RtlVersionFromTcr0(nc_u32 tcr0)
{
    switch ((tcr0 >> 16) & VERSION_MASK) {
    case 0x4c00: return RTL_VER_01;
    case 0x4c10: return RTL_VER_02;
    case 0x5c00: return RTL_VER_03;
    case 0x5c10: return RTL_VER_04;
    case 0x5c20: return RTL_VER_05;
    case 0x5c30: return RTL_VER_06;
    case 0x4800: return RTL_VER_07;
    case 0x6000: return RTL_VER_08;
    case 0x6010: return RTL_VER_09;
    case 0x6400: return RTL_VER_14;
    default: return RTL_VER_UNKNOWN;   /* incl. RTL8156/8157/8159 (later) */
    }
}

long RtlProbe(const nc_u8 *device, nc_u32 deviceSize, const nc_u8 *const *config,
              const nc_u32 *size, nc_u32 count, NET_PROBE *probe)
{
    nc_u32 i;
    long status;
    if (NcParseDevice(device, deviceSize, probe) != NC_OK) return NC_E_MALFORMED;
    /* rtl8152_cfgselector_choose_configuration: the configuration whose
     * first interface is vendor class; rtl_check_vendor_ok: endpoints 1
     * (bulk IN), 2 (bulk OUT) and 3 (interrupt IN). */
    for (i = 0; i < count; i++) {
        if (!config[i] || !size[i]) continue;
        status = NcFindVendorInterface(config[i], size[i], 1, probe);
        if (status == NC_E_MALFORMED) return status;
        if (status != NC_OK) continue;
        if ((probe->In & 0x0f) != 1 || (probe->Out & 0x0f) != 2 || (probe->Interrupt & 0x0f) != 3)
            return NC_E_UNSUPPORTED;
        probe->ConfigIndex = (nc_u8)i;
        return NC_OK;
    }
    return NC_E_UNSUPPORTED;
}

static long RtlBringUp(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params, int resume)
{
    RTL_STATE *s = chip;
    nc_u8 b[8];
    int i;

    if (!resume) {
        nc_u8 *p = (nc_u8 *)s;
        nc_u32 k;
        for (k = 0; k < sizeof(*s); k++) p[k] = 0;
        s->UsbSpeed = probe->Speed;
        /* __rtl_get_hw_ver: PLA_TCR0, up to three tries. */
        for (i = 0; i < 3; i++) {
            if (NcIoTry(io, RTL8152_REQ_GET_REGS, PLA_TCR0, MCU_TYPE_PLA, b, 4) >= 0) break;
        }
        if (i == 3) { io->Status = NC_E_IO; return io->Status; }
        s->Version = RtlVersionFromTcr0(Get32(b));
        if (s->Version == RTL_VER_UNKNOWN) return NC_E_UNSUPPORTED;
        /* rtl_ops_init */
        switch (VER(s)) {
        case RTL_VER_01: case RTL_VER_02: case RTL_VER_07:
            s->RxBufSize = 16 * 1024; s->EeeAdv = MDIO_EEE_100TX; s->Gmii = 0;
            break;
        case RTL_VER_03: case RTL_VER_04: case RTL_VER_05: case RTL_VER_06:
            s->RxBufSize = s->UsbSpeed == NC_SPEED_SUPER ? 32 * 1024 : 16 * 1024;
            s->EeeAdv = MDIO_EEE_1000T | MDIO_EEE_100TX; s->Gmii = 1;
            break;
        default:
            s->RxBufSize = 32 * 1024; s->EeeAdv = MDIO_EEE_1000T | MDIO_EEE_100TX; s->Gmii = 1;
            break;
        }
    }
    s->OcpBase = 0xffff;                     /* rtl_reset_ocp_base */
    s->Carrier = 0;

    ops_init(s, io);
    hw_phy_cfg(s, io);
    rtl8152_set_speed(s, io);
    /* set_ethernet_addr: RTL_VER_01 reads PLA_IDR, the others PLA_BACKUP
     * and write it to PLA_IDR. */
    generic_ocp_read(io, (nc_u16)(VER(s) == RTL_VER_01 ? PLA_IDR : PLA_BACKUP), 8, b, MCU_TYPE_PLA);
    for (i = 0; i < 6; i++) s->Mac[i] = b[i];
    if (VER(s) != RTL_VER_01 && NcValidMac(s->Mac)) {
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
        b[6] = b[7] = 0;
        generic_ocp_write(io, PLA_IDR, BYTE_EN_SIX_BYTES, 8, b, MCU_TYPE_PLA);
        ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
    }
    ops_up(s, io);
    if (io->Status < 0) return io->Status;

    for (i = 0; i < 6; i++) params->Mac[i] = s->Mac[i];
    params->RxSize = s->RxBufSize;
    params->RxCount = 4;
    params->TxHeader = RTL_TX_DESC;
    params->TxAlign = RTL_TX_ALIGN;
    params->TxBatch = 32;
    params->TxSlots = 4;                     /* RTL8152_MAX_TX */
    params->TxSize = RTL_AGG_BUF;            /* agg_buf_sz */
    params->Version = s->Version;
    return NC_OK;
}

static long RtlStart(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    return RtlBringUp(chip, io, probe, params, 0);
}

static long RtlResume(void *chip, NET_IO *io, const NET_PROBE *probe, NET_PARAMS *params)
{
    return RtlBringUp(chip, io, probe, params, 1);
}

static long RtlSetMac(void *chip, NET_IO *io, const nc_u8 mac[6])
{
    RTL_STATE *s = chip;
    nc_u8 b[8];
    int i;
    for (i = 0; i < 6; i++) b[i] = mac[i];
    b[6] = b[7] = 0;
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_CONFIG);
    generic_ocp_write(io, PLA_IDR, BYTE_EN_SIX_BYTES, 8, b, MCU_TYPE_PLA);
    ocp_write_byte(io, MCU_TYPE_PLA, PLA_CRWECR, CRWECR_NORAML);
    if (io->Status < 0) return io->Status;
    for (i = 0; i < 6; i++) s->Mac[i] = mac[i];
    return NC_OK;
}

static long RtlLink(void *chip, NET_IO *io, NET_LINK *link)
{
    RTL_STATE *s = chip;
    nc_u16 speed = rtl8152_get_speed(io);
    if (io->Status < 0) return io->Status;
    link->RxLength = s->RxBufSize;
    if (speed & RTL_LINK_STATUS) {
        if (!s->Carrier) {
            ops_enable(s, io);
            apply_rx_mode(s, io);
            if (io->Status < 0) return io->Status;
            s->Carrier = 1;
        }
        link->Up = 1;
        link->Mbps = (speed & RTL_1000BPS) ? 1000 : (speed & RTL_100BPS) ? 100 : 10;
        link->FullDuplex = (speed & RTL_FULL_DUP) != 0;
    } else {
        if (s->Carrier) {
            s->Carrier = 0;
            ops_disable(s, io);
        }
        link->Up = 0; link->Mbps = 0; link->FullDuplex = 0;
    }
    return io->Status < 0 ? io->Status : NC_OK;
}

static int RtlPoll(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    nc_u16 speed = rtl8152_get_speed(io);
    if (io->Status < 0) return 0;
    return ((speed & RTL_LINK_STATUS) != 0) != (s->Carrier != 0);
}

void RtlMulticastHash(nc_u32 filter, const nc_u8 *multicast, nc_u32 count, nc_u32 *rcr, nc_u8 mar[8])
{
    nc_u32 mc[2] = { 0, 0 }, i, accept = 0;
    if (filter & NC_FILTER_PROMISCUOUS) {
        accept = RCR_AM | RCR_AAP;
        mc[0] = mc[1] = 0xffffffffUL;
    } else if ((filter & NC_FILTER_ALL_MULTICAST) || count > 32) {  /* multicast_filter_limit */
        accept = RCR_AM;
        mc[0] = mc[1] = 0xffffffffUL;
    } else if (filter & NC_FILTER_MULTICAST) {
        for (i = 0; i < count; i++) {
            nc_u32 bit = NcEtherCrc(multicast + 6 * i, 6) >> 26;
            mc[bit >> 5] |= 1UL << (bit & 31);
            accept = RCR_AM;
        }
    }
    /* tmp[0] = cpu_to_le32(swab32(mc_filter[1])), tmp[1] = ...(mc_filter[0]) */
    for (i = 0; i < 4; i++) {
        mar[i] = (nc_u8)(mc[1] >> (24 - 8 * i));
        mar[4 + i] = (nc_u8)(mc[0] >> (24 - 8 * i));
    }
    *rcr = accept;
}

static long RtlSetFilter(void *chip, NET_IO *io, nc_u32 filter, const nc_u8 *multicast, nc_u32 count)
{
    RTL_STATE *s = chip;
    RtlMulticastHash(filter, multicast, count, &s->RcrAccept, s->Mar);
    /* rtl8152_set_rx_mode acts only with the carrier on; set_carrier
     * applies it when the link comes up. */
    if (s->Carrier) apply_rx_mode(s, io);
    return io->Status < 0 ? io->Status : NC_OK;
}

static long RtlStop(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    if (VER(s) == RTL_VER_UNKNOWN) return NC_OK;   /* Start refused the chip */
    s->Carrier = 0;
    ops_down(s, io);
    ops_unload(s, io);
    return io->Status < 0 ? io->Status : NC_OK;
}

static long RtlSuspend(void *chip, NET_IO *io)
{
    RTL_STATE *s = chip;
    if (VER(s) == RTL_VER_UNKNOWN) return NC_OK;
    s->Carrier = 0;
    ops_down(s, io);
    return io->Status < 0 ? io->Status : NC_OK;
}

static int RtlStatus(void *chip, const nc_u8 *data, nc_u32 length)
{
    (void)chip;
    if (length < 2) return -1;
    return ((data[0] | (data[1] << 8)) & INTR_LINK) ? 1 : 0;
}

/* rx_bottom: each frame behind a 24-byte rx_desc, its length (opts1 bits
 * 0-14) counting the 4-byte FCS, the next descriptor 8-byte aligned. A
 * length below ETH_ZLEN ends the walk, as in Linux. Unlike Linux, a frame
 * running past the transfer rejects the whole transfer before anything
 * is emitted (ethernet.md section 3.3). */
long RtlRxParse(void *chip, const nc_u8 *data, nc_u32 length, NET_EMIT emit, void *context,
                NET_RX_STATS *stats)
{
    nc_u32 pass, at, frames = 0, dropped = 0;
    (void)chip;
    stats->Frames = 0; stats->Dropped = 0;
    if (length < 60) return 0;               /* urb->actual_length < ETH_ZLEN */
    for (pass = 0; pass < 2; pass++) {
        at = 0;
        frames = dropped = 0;
        while (length > at && length - at > RTL_RX_DESC) {
            nc_u32 pkt = Get32(data + at) & 0x7fff;
            if (pkt < 60) break;
            if (pkt > length - at - RTL_RX_DESC) return NC_E_MALFORMED;
            if (pkt - 4 > NC_ETH_MAX) dropped++;
            else {
                if (pass) emit(context, data + at + RTL_RX_DESC, pkt - 4);
                frames++;
            }
            at += RTL_RX_DESC + pkt;
            at = (at + RTL_RX_ALIGN - 1) & ~(nc_u32)(RTL_RX_ALIGN - 1);
        }
    }
    stats->Frames = frames;
    stats->Dropped = dropped;
    return (long)frames;
}

static void RtlTxHeader(void *chip, nc_u8 *hdr, nc_u32 length, nc_u32 maxPacket, int single)
{
    (void)chip; (void)maxPacket; (void)single;
    Put32(hdr, length | 0x80000000UL | 0x40000000UL);   /* TX_FS | TX_LS | len */
    Put32(hdr + 4, 0);
}

static int RtlTxEnd(void *chip, nc_u8 *buffer, nc_u32 *total, nc_u32 capacity, nc_u32 maxPacket)
{
    (void)chip; (void)buffer; (void)total; (void)capacity; (void)maxPacket;
    return 0;            /* r8152_tx_agg_fill submits no zero-length packet */
}

const NET_CHIP NetPackageChip = {
    "rtl815x",
    "usb-wdm-free RTL815X (Realtek RTL8152/RTL8153 USB Ethernet)",
    0x0001,
    RtlProbe, RtlStart, RtlSetMac, RtlLink, RtlPoll, RtlSetFilter, RtlStop, RtlSuspend, RtlResume,
    RtlStatus, RtlRxParse, RtlTxHeader, RtlTxEnd
};
