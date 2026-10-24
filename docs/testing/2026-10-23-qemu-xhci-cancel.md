# qemu-xhci: Stop Endpoint's saved dequeue and No Op TDs, 2026-10-23

[`qemu-xhci-cancel.patch`](qemu-xhci-cancel.patch) (test-only, QEMU 11.1.0)
fixes the two `qemu-xhci` faults that usbcom's split-cancel legs found
([2026-10-22-usbcom98-legs.md](2026-10-22-usbcom98-legs.md), "The
zero-length IN, made to happen at a close"; xHCI98 task 43.64, its design
record 13 section 14). Both are emulator faults: xHCI98's non-head cancel
(the TD rewritten in place as No Ops, no Set TR Dequeue, the doorbell
restarts the ring) is xHCI 1.2c 4.6.9 option 3, as Linux does it.

## The two faults and the fixes (`hw/usb/hcd-xhci.c`)

1. **Stop Endpoint saved QEMU's fetch pointer.** `xhci_kick_epctx` fetches
   TDs ahead of the ones in flight; `xhci_stop_ep` cancelled them all
   (`xhci_ep_nuke_xfers`, under its "FIXME: endpoint stopped w/ xfers
   running, data might be lost") and wrote the fetch pointer into the
   Endpoint Context. A ring restarted by the doorbell alone therefore
   skipped the TD the endpoint stopped in and every fetched TD behind it.
   4.6.9: the endpoint saves its TR Dequeue Pointer "so that it can pick up
   where it left off". `xhci_stop_ep` now saves the first TRB, and its
   cycle state, of the oldest fetched transfer that has not completed, as
   `xhci_stall_ep` already does for a halt. The Stopped event it reports is
   on that TD's first TRB with its full length left, so a driver reads that
   nothing of it moved, which is what QEMU did (a cancelled `USBPacket`
   moves nothing). Stream endpoints keep the old behaviour (their rings are
   reset there).
2. **A No Op TD reached the device as a zero-length packet.**
   `xhci_xfer_create_sgl` skips a No Op TRB, but nothing tested the TD's
   type, so it was submitted as an empty `USBPacket`. 4.11.7 and 6.4.1.4:
   a No Op moves no data. A TD made only of No Op TRBs now completes
   Success at fetch (a Transfer Event if IOC), with no packet.

Trace events `usb_xhci_ep_stop_dequeue` (fetch pointer -> stopped TD) and
`usb_xhci_xfer_noop` record each.

## qtest (`tests/qtest/usb-hcd-xhci-ring-test.c`)

A minimal xHCI driver in qtest (reset, DCBAA, command and event rings,
Enable Slot, Address Device, SET_CONFIGURATION, Configure Endpoint) on
`qemu-xhci` with `usb-serial`, whose bulk IN NAKs while it has no input and
NAKs a zero-length IN too, so a No Op sent as a packet never completes.
Registered in `tests/qtest/meson.build` (i386/x86_64, needs
`CONFIG_USB_XHCI_PCI` and `CONFIG_USB_SERIAL`).

| Case | What it checks | Patched | Stock 11.1.0 |
|---|---|---|---|
| `stop-saves-stopped-td` | two TDs, the head NAKed in flight; Stop: Stopped event on the head, saved dequeue = the head; a second restart and Stop name it again | ok | not ok: dequeue `0x01007010` (the TD after it) |
| `noop-no-packet` | No Op (IOC) at the head: Success event at once, length 0; the Normal TD behind it left pending | ok | not ok: no event (the No Op went to the device as an IN and NAKed) |
| `cancel-behind-head` | three TDs; Stop; TD 1 rewritten as a No Op; doorbell; the head runs again (a later Stop names it) | ok | not ok: Stop names `0x01007010` (the No Op, fetched as an IN) |
| `cancel-behind-head-data` | the same, then 5 bytes reach the device: the head completes Short Packet with them, the No Op completes Success, TD 2 is left pending | ok | not ok: nothing completes in 200 ms (the ring is stuck on the No Op's IN) |

The stock run uses the same test binary with
`QTEST_QEMU_BINARY` set to scoop's QEMU 11.1.0, the emulator every earlier
usbcom leg ran on.

## Build

Kit `E:\usb-wdm-free-vm\qemu-cancel\qemu\`: QEMU 11.1.0 (tarball SHA-256
`6ee1d1a6...a858`, as `out\host\qemu-build-manifest.json`) +
`qemu-usbnet-hs.patch` + `qemu-xhci-cancel.patch`, MSYS2 MINGW64 GCC 16.1.0,
configured as the net9x kit's build plus `--enable-usb-redir` (MSYS2
package `mingw-w64-x86_64-usbredir` 0.15.0 added; without it QEMU has no
`usb-redir` and the usbcom model cannot be attached):
`--target-list=x86_64-softmmu --disable-whpx --disable-libusb --enable-png
--disable-tools --disable-docs --disable-guest-agent --disable-slirp
--disable-gtk --disable-sdl --disable-opengl --disable-virglrenderer
--disable-vnc --disable-curses --disable-spice --disable-werror
--disable-download --disable-plugins --disable-fdt --enable-usb-redir`.
As in the net9x kit, `scripts/symlink-install-tree.py` exits at once on
Windows (no symlink privilege; firmware comes from `-L` scoop's `share`);
that is a build workaround, not part of the patch.

| File | SHA-256 |
|---|---|
| `qemu-xhci-cancel.patch` | `c4a68ca2f1c42a678c3a90732d06bfb3d0d6ac1b59b02ddb46173702a624ebb0` |
| `qemu-system-x86_64.exe` (patched) | `390b66f178cd0366e1613a758e241751dc21e87ad4173ec8978d0fd3bc89d25f` |
| `usb-hcd-xhci-ring-test.exe` | `d87f304761b3b58ae2386c57f281674d06bd87f465af81a56915326942bed7df` |

## Guest legs

Kit `E:\usb-wdm-free-vm\qemu-cancel\` (scripts copied from the usbcom9x
and net9x kits, root and ports changed: 49120-49129), headless TCG
`pentium3,-apic`, `-smp 1 -m 256`, one fresh overlay per leg. Every leg is
QEMU: a VM pass, never a "Tested" cell.

- usbcom guest `x`: overlay on a copy of the usbcom9x kit's `g\x\x1.qcow2`
  (98 SE + xHCI98 3.1.0.0, usbcom installed; SHA-256 of the copy
  `6eb2231b...5f49`), the Uno model (`acmemu.py --speed high --mirror`)
  over `usb-redir` on `xh.0`.
- cdcnet guest `c`: overlay on a copy of the net9x kit's `w98\x1.qcow2`
  (98 SE + xHCI98 3.1.0.0 + cdcnet; `497580df...1bfe`), `usb-net
  highspeed=on` (ECM) on `xh.0`, the slirp side a stock QEMU
  `-machine none` on a dgram pair (as net9x `bridge.ps1`).

### usbcom: the non-head cancel, 24 closes (split flavour)

`COMTEST COM4 closes 24 16384` with the `split` test flavour (`USBCOM.SYS`
`381be3ab...`, as m4), which at each close cancels the third-oldest busy
bulk IN alone (xHCI98 rewrites that TD as a No Op behind two live ones,
then rings the doorbell) and then the oldest alone (the ring's head: Set TR
Dequeue). The model's JSONL names each request by QEMU's TRB address, read
by `scripts\cancels.py` and `scripts\step1.py`.

| Leg | QEMU | First Stop of each close: QEMU's next request | No Op TRB sent to the device | Zero-length INs at the model | COMTEST |
|---|---|---|---|---|---|
| s1 | stock 11.1.0 | 25 of 25 past every cancelled TD (the two live TDs ahead of the No Op never requested again) | 25 (as zero-length INs) | 25 | PASS 2/2 |
| c1 | patched | 25 of 25 the oldest TD, then the second, then the TD after the No Op | 0 | 0 | PASS 2/2 |

(25 closes: COMTEST's 24 and the reopen after them.) In c1 the oldest TD
was requested from the device again after every non-head cancel; the model
had no input pending at that point, so it stayed pending until step 2
cancelled it, as it should. That the restarted head completes when data
does arrive is the qtest `cancel-behind-head-data` case.

### usbcom: release build

Leg c2, patched QEMU, the free (release) flavour (`USBCOM.SYS`
`546b9b22...`, `USBCOMV.VXD` `5d07de95...`, from `out\serial\usbcom\w98\free`):
`COMTEST COM4 full` **PASS 58/58**, then `closes 24 16384` **PASS 2/2**
(24 closes, then open and a 4096-byte echo). The free build cancels a
pipe's transfers all together at a close, so its stops cancel every TD on
the ring; no zero-length IN reached the model (0), and 191 of 320 bulk IN
completions carried data.

### cdcnet: regression leg on the patched QEMU

Release `CDCNET.SYS` (`out\net\cdcnet\pkg\w98se-me`, `5c82934e...fe9`)
copied over the installed driver before the plug. NETTEST (`501a9f54...`):
DHCP, 21 ICMP echoes at seven frame sizes, 6 x 1 MiB TFTP, every byte
checked.

| Leg | QEMU build | Result |
|---|---|---|
| n1 | patched, before `usb-redir` was enabled (the same `hcd-xhci.c`) | PASS (rounds 3-6 seen with 0 retransmits; its NETTEST.TXT was replaced by n2's) |
| n2 | patched, final (`390b66f1...`) | PASS: 21/21 echoes, 6/6 MiB, 0 retransmits (4.5 s per MiB) |

## Evidence

`E:\usb-wdm-free-vm\qemu-cancel\evidence\`: `qtest-patched.txt`,
`qtest-stock.txt`, the 0xE9 logs `x-xhci-e9-{s1,c1,c2}.log`, the model's
JSONL `{s1,c1,c2}-uno-hs.jsonl`, the leg transcripts `leg-*.txt`,
`cdc-n*.txt`, `nettest-n2.txt`; screens under `shots\`.

## For xHCI98

xHCI98's own VM legs run stock QEMU 11.1.0; on it, any cancel of a TD
behind the head loses the TDs before it and sends a zero-length IN. On this
build both behave as the specification says, so legs that cancel single
transfers (usbcom's split flavour, any class driver that aborts one read of
several) read the driver and not the emulator. It is test-only and Windows
host-only (MSYS2 build, no slirp: networking through a stock QEMU bridge).
