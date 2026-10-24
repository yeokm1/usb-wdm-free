# cdcnet CDC-NCM on 98 SE with a simulated NCM function, 2026-10-23

Development legs, not a release and not "Tested". Until now cdcnet's NCM
path had run only in host parser tests: QEMU's `usb-net` has no NCM, and no
NCM device is on the bench (`simulator-coverage.md`, rank 3).

## The simulator

`src/net/cdcnet/ncmemu.py` (branch `simcov`, a74727c, fix 61ebf86): a
SIMULATED CDC NCM 1.0 function over usbredir, original from the NCM 1.0 and
ECM 1.2 specifications. Device class 02 (no IAD); NCM control interface
02/0D/00 with header, union, Ethernet (iMACAddress 020000770002) and NCM
functional descriptors and an interrupt notification endpoint; data
interface 0A/00/01 with an endpoint-less alternate 0 and bulk IN/OUT in
alternate 1. IDs 0525:A4A2, so `CDCT98.INF` (the cdcnet test INF for QEMU
`usb-net`) binds it. It answers GET_NTB_PARAMETERS and the NTB
format/input-size/max-datagram/CRC requests and the packet and multicast
filters, sends NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE after the
data alternate and around a simulated cable pull (`--flap-after`), builds
receive NTB16s with the NDP16 in front, at the end, or split in two, and
checks every host NTB16 against NCM 1.0 and its advertised
divisor/remainder/alignment (violations logged as `ntb_out_bad`). Behind it
is rtlemu.py's SIMULATED host 192.168.77.1 (ARP, ICMP echo, DHCP).

Host tests: `python -B ncmsimtest.py` (6 tests) against cdcnet's own
`cdcdesc.c` and `cdcncm.c` (x64 DLL from `host.ps1`).

## Rig

Kit `E:\usb-wdm-free-vm\simcov` (monitor 49200, simulator 49201;
`attachncm.sh`, `detachncm.sh`, `modelncm.ps1`), overlay `s1` of
`net9x\base\w98-nusb-hub.qcow2` (98 SE, NUSB 3.3, xHCI98 3.1.0.0). Driver:
`build.cmd ndis5 trace` of `simcov` (source as `working`), `cdcnet.sys`
SHA-256 `af5b1b97...4619`, `CDCT98.INF` `d13e6ccf...78bc`.

## xHCI98 3.1.0.0: PASS

Simulator `--caps 0x21 --out-divisor 8 --out-remainder 2 --out-alignment 8
--in-layout split`.

- Install: Add New Hardware found "CDC NCM Ethernet - SIMULATED", chose
  `A:\CDCT98.INF`, and asked for a restart.
- Trace: "configuration 0: probe 0 protocol 3" (NCM), "ncm: in 16384 out
  16384 div 8 rem 2 align 8 dg 32", "mac 2:0:0:77:0:2", "initialized:
  protocol 3, high speed, link 1". SET_NTB_INPUT_SIZE 16384, packet filter
  0x1C.
- The first boot after the restart stopped the simulator: a single
  datagram in the split layout indexed a second NDP16 that it had not
  built (simulator defect, fixed in 61ebf86 with a host test). After a
  replug with the fixed simulator: DHCP DISCOVER/OFFER/REQUEST/ACK, 33 host
  NTBs with 0 layout violations.
- `ping 192.168.77.1`: 8/8 at 1472 bytes, 6/6 at 32 bytes.

## NUSB 3.3 EHCI: PASS

Simulator `--caps 0x3b --ntb32 --out-divisor 512 --out-remainder 14
--out-alignment 16 --in-layout end --flap-after 60 --flap-for 6`: NTB32
offered, so the host must select NTB16; CRC, max-datagram and 8-byte
input-size capabilities; the hardest transmit layout.

- The device bound without a prompt; trace "ncm: in 16384 out 16384 div
  512 rem 14 align 16 dg 32", "initialized: protocol 3, high speed, link 1".
- The host sent SET_NTB_FORMAT 0 (NTB16) and SET_CRC_MODE 0; DHCP REQUEST
  renewed the lease.
- `ping -n 30 -l 1472`: 28/30, the two lost replies inside the 6-second
  cable pull; 5 notifications delivered (connection, speed, disconnect,
  connection, speed). cdcnet does not trace link changes, so whether NDIS
  saw MEDIA_DISCONNECT is not shown here.
- 63 host NTBs, 0 layout violations; 30 device NTBs with the NDP16 at the
  end, all accepted.
- Removal: "stopped; 0 malformed transfers", "halted: tx 63/3 rx 30
  nobuf 0".

## Verdict

cdcnet NCM on 98 SE: probe, NCM setup (including NTB format selection,
CRC mode and the 8-byte input size), NTB16 receive in all three layouts
and transmit with non-default divisor, remainder and alignment, DHCP and
ping to 1472 bytes PASS under xHCI98 and NUSB's EHCI, with the simulator.
No driver defect found. Owed: a trace line for link notifications (the
NDIS media status on a cable pull is unshown), ME, 2000 and later NT, and
a real NCM device (REAL CHECK: an RTL8156- or AX88179A-class adapter in
NCM mode, or an Android phone in NCM tethering, on 98 SE under both stacks:
install, trace "protocol 3", DHCP, `ping -l 1472`, a cable pull).

## Link notifications, traced (branch `cdcnet-trace`)

Trace lines for each interrupt completion, each notification and the NDIS
media indication (`cdcnet-trace` 77ed167, 1b6f513; trace and checked
flavours only). Rerun on 98 SE with `ncmemu.py --flap-after`: the simulator
sends five notifications, NETWORK_CONNECTION 1, CONNECTION_SPEED_CHANGE,
NETWORK_CONNECTION 0, NETWORK_CONNECTION 1, CONNECTION_SPEED_CHANGE.

- NUSB 3.3 EHCI: three 8-byte completions, "indicate MEDIA_DISCONNECT"
  then "indicate MEDIA_CONNECT". Neither 16-byte SPEED_CHANGE completed a
  transfer, with the notification endpoint at wMaxPacketSize 16 or 64
  (`--int-max 64`); where they are lost (QEMU's EHCI with usb-redir, or
  NUSB) is not settled.
- xHCI98 3.1.0.0: completions of 8 bytes (connection 1), **24 bytes**
  (`a1 2a` ... `a1 00` at offset 16: SPEED_CHANGE and the disconnect in one
  transfer), 8 bytes (connection 1). The driver handled only the first
  notification of the 24 bytes, so **no MEDIA_DISCONNECT was indicated**.

**Driver defect (reported, not fixed here):** cdcnet posts 64-byte
interrupt transfers (`cdcusb.c`, IntSlot.Size 64) and `Notification()`
handles only the first notification of a completion. A 16-byte
CONNECTION_SPEED_CHANGE on a 16-byte endpoint is a full packet and does not
end the transfer, so the next notification joins it and is dropped. Fix:
walk the completion (each notification is 8 + wLength bytes).
