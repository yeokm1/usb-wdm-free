# rtl815x: an RTL8157 that loses its static settings, 2026-10-30

xHCI98 task 45.40 (its `docs/contributing/runs/run-45.md`, "45.40"): a
user's RTL8157 on 32-bit XP with `rtl815x` 0.1.4 under xHCI98 3.3.0.0, set
to a static address with no gateway beside a second adapter to a VPN
router, later took the default route and its LAN stopped. Simulator legs
only; not a release, not "Tested".

## What `rtl815x` does and does not do

Read in `src/net/rtl815x/` at 0.1.4 (unchanged since):

- NDIS Reset (`netcore.c` `Reset`, `netusb.c` `Reset`) resets the three
  pipes, applies the filter and reads the link; Halt and Initialize keep
  the devnode; no RESET_PORT or CYCLE_PORT is sent.
- The address (`ChooseAddress`) is the Advanced tab's NetworkAddress, the
  chip's own, or the kept FallbackAddress; it touches no TCP/IP setting.
- So `rtl815x` cannot make Windows install a new adapter. Windows keeps an
  adapter's TCP/IP settings under its devnode's `NetCfgInstanceId`; a USB
  instance id that changes, which the host controller's driver gives,
  makes a new adapter with DHCP. That is the mechanism the legs below
  reproduce; the report alone does not prove it was the user's (DHCP can
  be turned on for the same adapter, and a route added by other means).
- A separate path: `Recover` gives up after 30 failed recoveries and
  reports the adapter gone (`ReportGone`), and NDIS Reset ignores a pipe
  reset that fails. That leaves the LAN dead with the settings kept until
  the adapter is replugged or disabled and enabled - not the reported
  default route. Not changed here; a follow-up if a report shows it.

## The simulator's serial (`2b1c293`)

`rtlemu.py --serial TEXT` gives the device a serial number string
(iSerialNumber 3; none by default) and `--serial-stall N` answers the first
N reads of it in a run with a STALL. Host test
`test_serial_and_its_stall` (`python -B rtlsimtest.py`: 30 run, 15
skipped where the core DLL is not built).

## Legs

q35 XP SP3 and 7 SP1 x86, QEMU 11.1.0, the simulated RTL8157 at
SuperSpeed over usbredir with `--serial 000334C8D6B113C7`, `rtl815x`
0.1.4 (`RTL815X.SYS` `5e1ef6be...`, `RTLXP7.INF` `479bf50c...`), a QEMU
`rtl8139` on user networking as the second adapter.

Under xHCI98 3.3.0.0, after the adapter was set static: a replug whose
first three serial reads STALL was named by its port
(`USB\VID_0BDA&PID_8157\6&58728d&0&1` on XP, `...\6&1a1ca75f&0&1` on 7),
Windows installed a new adapter with DHCP (with a wizard on XP, silently on
7), and its default route through the simulated router outranked the
other adapter's. Two STALLs, or none, kept the first devnode and its
static address. The fix is xHCI98's (its design record 13 section 10.7,
"Identity across a re-enumeration", xHCI98 commits
`f2916a2d` and `54db4c51`): with it, the 3-STALL replug kept the first
devnode and its static address on XP and 7, and a replug whose every read
failed published nothing until the next connection. Its legs are in its
run sheet.
