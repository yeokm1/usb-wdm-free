# `EEEMaxSupportSpeed` on the 1 Gb/s class's Advanced tab, with a simulated RTL8153D, 2026-11-17

Owner, 2026-11-17: "If the ethernet drivers support certain options, it
should have the options configurable in Device Manager." An audit of every
Ethernet driver's keywords against its INFs found one value read and
applied but not offered: `EEEMaxSupportSpeed` on the RTL8153D, which
reaches `rtl815x`'s 1 Gb/s install section (`RTL_GE`). It is offered there
now (design [ethernet.md](../design/ethernet.md) section 4.10.2). Every leg
here ran against the simulator (`rtlemu.py`); no physical adapter was used
and none is claimed.

## Build

- Branch `netopts`, the INF change `e3917c3`. Built INFs (CRLF):
  `RTL81598.INF` `157f07cd...`, `RTL815X.INF` `ade4f430...`,
  `RTLXP7.INF` `a223ed02...`. The driver is the trace build of the same
  tree (`ndis5 trace`, test-only, never released) `rtl815x.sys`
  `1f9e4ec6...`; the code is unchanged from `main`. Test aid `NETSET.EXE`
  (`src/net/cdcnet/netset.c`).
- `release.ps1 -Check -Part x86` over a definition holding only
  `net/rtl815x` (the 0.1.11 entry): checks PASS.

## Rig

QEMU 11.1.0 (stock), TCG, headless, one vCPU; throwaway overlays of the
installed bases `vm\wxp.qcow2` (XP SP3, Windows' own EHCI) and
`vm\w98se.qcow2` (98 SE, NUSB 3.3's EHCI); `usb-ehci`, the simulator over
`usb-redir` on its port 1 at High Speed (`rtlemu.py --chip 8153d`, chip
7420 on `0BDA:8053`; one more plug with `--chip 8153b --pid 8053`); the
driver's trace on QEMU's debug console. The files from a virtual floppy;
dialogs answered from screenshots.

## XP SP3 (`RTLXP7.INF`, Windows' own EHCI)

| Step | Result |
|---|---|
| Install by the Found New Hardware wizard | "Realtek RTL8153D USB Gigabit Ethernet (wdm-free)", `InfSection` `RTL_GE`; `NETSET`: `EEEMaxSupportSpeed` = "1000"; trace `setting EEEMaxSupportSpeed = 1000`. PASS |
| The Advanced tab (adapter properties) | "EEE up to" listed with the other seven; value "1 Gb/s"; the list holds 1 Gb/s, 10 Mb/s, 100 Mb/s. PASS |
| "EEE up to" set to 100 Mb/s and Energy-Efficient Ethernet to On in the dialog, OK | the adapter restarted at once; trace `setting *EEE = 1`, `setting EEEMaxSupportSpeed = 100`; the simulator's link-up: EEE MAC 3, EEE advertisement 2 (100 only), 2.5G 0; `NETSET`: `EEEMaxSupportSpeed` = "100", `*EEE` = "1". PASS |
| `NETSET ... EEEMaxSupportSpeed 10`, unplug and replug | trace `setting EEEMaxSupportSpeed = 10`; link-up EEE advertisement 0. PASS |
| The same key with an RTL8153B (6010) behind `0BDA:8053` | trace `setting EEEMaxSupportSpeed = 10`, then `settings this chip cannot apply (table bits): 8` (the entry is not applied there, as designed). PASS |

A first replug landed on EHCI port 2, a new device instance; it was
cancelled and every later plug pinned to port 1.

## 98 SE (`RTL81598.INF`, NUSB 3.3's EHCI)

| Step | Result |
|---|---|
| Install by the Add New Hardware wizard from the floppy, restart declined | "Realtek RTL8153-based USB Ethernet (wdm-free)", `InfSection` `RTL_GE`; `NETSET`: `EEEMaxSupportSpeed` = "1000"; trace `setting EEEMaxSupportSpeed = 1000`. PASS |
| Network, the adapter's Properties, Advanced (`netdi.dll`) | "EEE up to" listed with the other seven; value "1 Gb/s"; the list holds 1 Gb/s, 10 Mb/s, 100 Mb/s. PASS |
| "EEE up to" 10 Mb/s and Energy-Efficient Ethernet On in the dialog, OK, OK, restart declined | `NETSET`: `EEEMaxSupportSpeed` = "10", `*EEE` = "1" (text). PASS |
| Unplug and replug (the adapter restarts) | trace `setting *EEE = 1`, `setting EEEMaxSupportSpeed = 10`; link-up EEE MAC 3, EEE advertisement 0. PASS |

## Not run

ME, 2000 (`RTL815X.INF`), XP x64 and Vista (the x64 `RTLXP7.INF`
sections carry the same lines); xHCI98; a real RTL8153D. On 7 the entry
reaches no device: `RTLXP7.INF`'s 7 models sections list no id that
takes `RTL_GE` (design section 7.6).

Later the same day (design 4.10.2): the entry does nothing on the
RTL8153, 8153B and 8153C behind the same ids (the last row of the XP
table), which the owner's rule of 2026-11-17 does not allow; the driver
is to trim the tab to the chip at start (owner, 2026-11-17). The other
drivers gained no entry, so none of their legs ran here.
