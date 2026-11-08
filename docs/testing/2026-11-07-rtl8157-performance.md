# rtl815x speed with a real RTL8157, 2026-11-07

xHCI98 Phase 47 task: bring `rtl815x` as close as reasonable to the speed
of Realtek's own driver. The real RTL8157 of
`2026-11-07-rtl8157-hardware.md` (chip `1030`, `0BDA:8157`) was used again,
passed to QEMU guests on development host A, cabled straight to a host
10GbE port with link-local addresses; the host was not changed. Every
number is from guests under TCG emulation and is relative: it is not a
real machine's speed. Design: `docs/design/ethernet.md` section 4.9.

## The rig, measured first

- The test emulator of the earlier record (0.25 ms libusb poll, bulk
  pipelined) still made each passthrough transfer wait for QEMU's main
  loop: with an XP guest, a bulk-OUT or control transfer took about
  10 ms from submission to completion on the host side (usbmon, median;
  control at idle 11 ms, interrupt 15.5 ms), but about 0.9 ms with a
  Windows 10 guest (Realtek's driver, the earlier record's capture). The
  wait depended on how busy the guest kept the main loop, so the earlier
  comparison of the two drivers was skewed by the rig.
- A second test build (QEMU 11.1.2 of the same fork) adds a thread to
  `usb-host` on Windows that handles libusb events every 125 us under
  the big QEMU lock, on a high-resolution waitable timer, and opts the
  process out of Windows 11's timer-resolution throttling for windowless
  processes. A test tool only; not kept in any repository. With it, a
  bulk-OUT transfer took 0.37 ms (median) under XP, and the USB side was
  idle 75 to 87% of the time. All numbers below are on this build.
- With the rig out of the way, the guests' CPU is the limit. XP SP3 (2
  vCPUs) sending: 89 to 98% busy; receiving: about one vCPU busy. 2000
  SP4 (1 vCPU): the QEMU process at about one host CPU. Other workers'
  guests ran on the same host throughout (up to 25 QEMU processes), so
  single 20-second runs vary by 10 to 25%; only interleaved runs are
  compared.
- Transfer sizes (usbmon): under a send, `rtl815x`'s bulk-OUT transfers
  were already full: 10 frames of a 16 KiB buffer, 15354 and 14538 bytes,
  the pump taking the whole queue when a slot frees. One-frame transfers
  were the ACKs of a receive. Realtek's driver on Windows 10 sent 32848
  bytes per transfer under a send.

## What Realtek's driver does (usbmon, real RTL8157, Windows 10 x64)

From the earlier record's capture, decoded here:
- large sends: first dword `0xd0000000` (first and last segment,
  `GTSENDV4`), the TCP header offset field left 0, MSS 1460 in the second
  dword (`0x0b680000`), up to 32822 bytes a frame;
- TCP and UDP checksums: second dword `0x60440000` and `0xa0440000`
  (`TCP_CS`/`UDP_CS`, `IPV4_CS`, header offset 34); the IPv4 checksum
  alone for ICMP, `0x20000000`;
- the receive header's third dword as the chip writes it, also under
  `rtl815x`: TCP `0x02004400`, UDP `0x01004800`, ICMP `0x03004000`, ARP
  `0x07000000`.

These are the bits of Linux `r8152.c` (`tx_desc`, `rx_desc_v2`).

## Changes (branch `w47-r57perf`)

- `5760865`, `a7c7577`, `77e92ce`, `a1a0f01` (with merges of
  `w47-rel019` `877f471` and `0699ecc`):
  - Transmit transfers filled by bytes, up to 64 frames (all four
    packages; the others send one frame a transfer, as before).
  - NT task offload in the shared core (2000 DDK network guide chapter 9):
    `OID_TCP_TASK_OFFLOAD`, per-send checksum requests passed to the chip,
    receive checksum verdicts reported as succeeded (never failed). 98 SE
    and ME: none of it.
  - The RTL8157/8159 offer the TCP, UDP and IPv4 checksums on transmit,
    with 32 KiB transmit buffers. The large send and the receive
    checksums are in the code but not offered (below).
  - A send that cannot go fails alone, not its whole transfer.
  - The model `rtlemu.py` segments and checksums as the chip does and
    writes the receive checksum bits.
- **A frame with a checksum request is sent unpadded.** Padded to 60
  bytes, as the core padded every short frame, TCP ACKs with a checksum
  request left the real RTL8157 with bad checksums: the host dropped them
  and XP's receive fell to 0.44 Mb/s. Unpadded (as Linux and Realtek send
  them), receive ran at 89 Mb/s.
- **The large send was offered and never used.** 2000 SP4 and XP SP3
  enabled it (task offload set: IPv4, TCP, large send) but handed the
  driver no large send in any run, also with `MaxOffLoadSize` 64 KiB. It
  never ran on the chip, so it is not offered in this release.

## Speed

20-second runs, Mb/s, out = guest to host, in = host to guest.

| Guest | Build | out | in | Note |
|---|---|---|---|---|
| XP SP3 | `04d672d`, first test emulator (r57's baseline) | 34.5 | 67.3 | the rig capped it |
| XP SP3 | `04d672d` | 52.4, 52.7, 53.5 | 56.6, 65.3, 70.6 | interleaved |
| XP SP3 | `77e92ce` | 56.6, 56.6, 53.8 | 66.6, 71.3, 68.3 | interleaved; guest CPU 94-96% out, 57-60% in |
| 2000 SP4 | `04d672d` (trace build) | 163, 133, 147 | 212, 155, 161 | interleaved |
| 2000 SP4 | `a1a0f01` (trace; receive checksums offered) | 145, 151, 153 | 153, 146, 155 | interleaved with the row above |
| 2000 SP4 | `43126cd` (trace; final) | 186, 150 | 144, 171 | a later boot |
| Windows 10 x64, Realtek 10.68.20 | first test emulator (earlier record) | 244 | 339 | at 100% guest CPU; its copy of the driver was gone, not rerun |

- **The receive checksums were offered, then taken out.** With them,
  2000 received slower (median 153 against 161 Mb/s, mean 151 against
  176): TCP/IP kept the packets the chip had checked, the driver's
  64-packet receive pool ran down to 9 free (55 out) and 27% of the frames
  went up as copies. XP showed no gain either way. They are not offered in
  this release; a larger pool would be the next thing to try.
- One finding about this rig: many 2000 runs taken before these were
  void. A dialog took the keyboard and the driver was never switched;
  every number above comes from runs whose trace banner names the driver
  that loaded. The void runs, all of one driver, spread from 119 to 219
  Mb/s receiving, which is the noise of a single run here.

- XP: about +5% out and +7% in (`77e92ce`, receive checksums still
  offered), inside the noise of one run. XP's
  TCP/IP and NDIS cost per segment on an emulated processor is the
  limit; the checksum offloads remove only the checksum part, and
  without a large send nothing removes the per-segment work.
- 2000's stack is far lighter on the same rig: with `04d672d`, about 2.8
  times XP's speed out (148 against 52.9 Mb/s, means of 3) and 2.7 times
  in (176 against 64.2); different boots and build flavours (trace on
  2000, free on XP), so only a rough ratio.

## Legs (this branch)

| Leg | XP SP3 | 2000 SP4 | 98 SE |
|---|---|---|---|
| 300 s both ways at once, 0 mismatched bytes | `77e92ce`: out 2.68 GB (70.4 Mb/s), in 1.15 GB (30.6); merge `9f2d1fc`: out 2.15 GB (56.5), in 1.19 GB (31.6); final `43126cd`: out 2.56 GB (67.4), in 1.34 GB (35.7) | `43126cd`: out 3.41 GB (90.4), in 4.73 GB (126.0) | `a1a0f01`: out 598 MB (15.9), in 371 MB (9.9); deferred upcalls, no stall |
| Unplug under both-ways traffic, replug | pass on `77e92ce`, `9f2d1fc` and `43126cd`: "gone: 0 of 13 transfers still outstanding after the abort", halt, image reloaded, link 5000, ping 3/3 | `43126cd`: pass: "gone: 0 of 13 transfers still outstanding after the abort", halt, image reloaded, link 5000, ping 3/3 | pass: "gone: 0 of 13 transfers still outstanding after the abort", halt, image reloaded, link 5000; the link-local address came back about a minute later (98 SE's own address timing), then traffic |
| Disable, enable | pass by devcon on `77e92ce` and `9f2d1fc`: halt, no reply while disabled, reload, offload enabled again, traffic | `43126cd`, from Device Manager (devcon does not run on 2000): halt, no reply while disabled; enabled: reload, task offload set again, traffic | pass, from Device Manager: halt, Code 22, no reply; enabled: reload, link, traffic |
| Task offload as set by TCP/IP | final build: IPv4 and TCP checksums on transmit (earlier builds: also receive and the large send while offered) | the same | none (by design: no per-packet information on 98 SE) |

- 98 SE ran `a1a0f01`; `43126cd` differs from it only in the NT offload
  mask, which 98 SE never reads.
- Standby: the q35 machine as launched has no S3; not run.

## Other packages

The shared core changed for all four. `ax88179`, `ax88772` and `sr9700`
send one frame per transfer (their `TxBatch` is 1) and offer no offload,
so only the byte arithmetic of the fill is new for them. Host tests
(including the new transfer-fill and task-offload tests and mutation
runs on them) and the four simulator suites pass. Their legs on the
models were not run: the new test emulator has no `usb-redir`, and the
one try on the stock emulator (98 SE) hung at boot before the model was
reached.

## Image sizes (free builds)

| Driver | before (`0699ecc`) | after | change |
|---|---|---|---|
| `rtl815x.sys` x86 (NDIS 5.0) | 47392 | 49920 | +2528 |
| `rtl815x.sys` x64 (NDIS 5.1) | 56832 | 60928 | +4096 |
| `ax179f.sys` x86 | 25440 | 27584 | +2144 |
| `ax772f.sys` x86 | 26784 | 28896 | +2112 |
| `ax772f.sys` x64 | 35840 | 38400 | +2560 |
| `sr9700f.sys` x86 | 23584 | 25696 | +2112 |
| `sr9700f.sys` x64 | 31232 | 33792 | +2560 |

The RTL8157/8159 transmit buffers grow from 4 x 16 KiB to 4 x 32 KiB
(nonpaged; page-locked on 98 SE).

## Not done

- XP x64: the build (`ndis51`) passes its gates; no guest leg.
- Realtek's driver on the second test emulator: its copy was gone.
- Windows Vista/7 with `rtl815x` (`RTLXP7.INF`), whose stacks may use a
  large send; the large send would be offered only after such a leg.

## Evidence

Development host A, `E:\xhci98-vm\a347\r57perf\` (not tracked): `ev\`
(captures, bench logs, leg outputs), `shots\`, `pids.txt`, `STATE.md`.
