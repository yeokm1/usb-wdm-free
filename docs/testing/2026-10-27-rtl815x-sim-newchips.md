# rtl815x simulator: the newer Realtek chips, 2026-10-27

Task 44.70 part A (owner, 2026-10-27): `src/net/rtl815x/rtlemu.py`
extended from the RTL8152, RTL8153 and RTL8153B to the RTL8153D, RTL8156,
RTL8156B, RTL8157 and RTL8159, so that rtl815x can be tested on them. Nobody
has these adapters; the owner accepted testing on the simulator only. Not a
release, not "Tested": everything here is a SIMULATED chip.

## Source

[linux]: Linux `drivers/net/usb/r8152.c` at tag `v7.3` (commit `091b0638`,
blob `f6168643`, 10520 lines), read for facts only (register addresses and
values, the order of accesses, descriptor layouts, version ids). Line
numbers below are of that file. Where a fact comes from a commit message,
the commit is named. `r8152.c` is GPL-2.0-only: no code, function names,
comments or structure of it are in the simulator or its tests; the names
are this project's (`MR_`, `UR_`, `PR_`, as `rtlchip.h`).

Facts not in `r8152.c` (the USB descriptors beyond what Linux checks, the
chips' power-up register values) are this model's choices and are marked
"chosen" below.

## What every newer chip has in the model

- Version: `MR_TX_CONFIG` (0xE610) high word, read as Linux reads it (a
  dword, `>> 16 & 0x7CF0`) [linux 9995-10093].
- USB: vendor configuration, one interface of class FF, bulk IN endpoint
  1, bulk OUT endpoint 2, interrupt IN endpoint 3: what Linux checks
  [linux 8518-8547]; Linux selects the configuration whose first interface
  is class FF [linux 10456-10483], and the model has that one
  configuration only. Vendor register requests are answered before a
  configuration is chosen too: Linux reads the chip id to choose one
  [linux 10464-10465, 10012-10017]. `--speed super` (the newer chips' default): bulk
  1024 bytes with a SuperSpeed companion (bMaxBurst 3, chosen), interrupt
  2 bytes, bInterval 8; a BOS with the USB 2.0 extension and the
  SuperSpeed capability. usbredir carries "super" as its fastest speed,
  so SuperSpeedPlus shows only in the descriptors (below). `--speed high`:
  bcdUSB 0x0210, bulk 512, a BOS of the USB 2.0 extension only. Linux's
  default advertisement has 2.5G and faster only when the USB link is
  SuperSpeed or faster [linux 10259-10280].
- PHY SRAM: two address/data pairs in the PHY, `PR_SRAM_ADDR`/`DATA`
  (0xA436/0xA438) and `PR_SRAM2_ADDR`/`DATA` (0xB87C/0xB87E) [linux 206-207, 220-221,
  1804-1814, 1956-1975]. A data write stores at the address and steps it by
  2; a data read does not step it. The step is inferred: Linux writes runs
  of data after one address write, and its next address follows on by 2
  per word (e.g. 0x8F9C, six words, then 0x8FA8 [linux 8036-8043]),
  while its read-modify-write rewrites the data at the same address
  [linux 1968-1975].
- PHY state (`PR_STATE` 0xA420, low 3 bits): external init (2) while
  `--ext-init` and either bit 3 or 1 of 0xA468 is set (Linux clears both
  there [linux 8421, 7746, 7988]); power-down (5) while the PHY's
  control register has its power-down bit (Linux clears it, then waits for
  LAN on [linux 8426-8432]); else LAN on (3) [linux 201, 634-637, 4172-4194].
  No link comes up unless the state is LAN on (`phy_not_on`); a write that
  sets or clears power-down restarts negotiation.
- Polled bits that complete: `MR_BOOT` autoload done (0xE004 bit 1) [linux
  410, 8402-8410]; MAC 0xE000 bit 8 after ALDPS off [linux 6101-6119]; the PHY
  patch request (0xB820 bit 4) answered by 0xB800 bit 6, and its release
  by the bit clearing [linux 4577-4610]; the PHY reset bit clears [linux
  6614-6624]; `MR_COMMAND` reset self-clears [linux 3440-3450]; the OOB
  list-ready and FIFO-empty bits and `MR_TX_CONFIG` TX empty are set
  [linux 5869-5882, 3765-3779]; the PHY flash: `MR_GPHY_FLASH` (0xD3AE)
  bit 1 set and `UR_GPHY` (0xD284) bit 2 (load done) set, so the wait of
  [linux 5884-5898] ends at once (chosen: an adapter with its PHY flash).
  With `--power-cut`: `UR_MISC0` (0xD81A) bit 0 set and `UR_GPHY` bit 6
  (backup-restore done) set, the 8159's wait [linux 8354-8364].
- Link: speed and duplex are resolved when autonegotiation completes
  (`--link-after`), from the PHY's own registers against a partner whose
  fastest speed is `--partner` (default 10000) and who advertises all
  slower speeds and pause: control 0xA400 (autonegotiation enable 0x1000;
  forced 100 0x2000, 1000 0x0040, full duplex 0x0100), advertisement
  0xA408 (10/100 half and full, pause 0x0400), 1000BASE-T control 0xA412
  (1000 full 0x0200), multi-gigabit control `PR_XG_CTRL` 0xA5D4 (2.5G
  0x0080, 5G 0x0100, 10G 0x1000) [linux 212, 6470-6627] (the partner
  offers 100 only when `--partner` is 100 or more). The fastest
  speed both sides offer and the chip supports wins. A restart is a write
  of the control register with autonegotiation restart, reset, or
  autonegotiation off. Link status, MAC side: `MR_LINK` (0xE908, word):
  link 0x02, full 0x01, 10 0x04, 100 0x08, 1000 0x10, 2.5G 0x0400, 5G
  0x1000, 10G 0x4000, receive and transmit pause 0x20 and 0x40 when
  negotiated with pause [linux 104, 745-758, 3459-3462]. PHY side: status 0xA402
  link and autonegotiation-complete bits, partner 0xA40A, 1000BASE-T
  status 0xA414 (0x0800), multi-gigabit status `PR_XG_STAT` 0xA5D6 (2.5G
  0x0020, 5G 0x0040, 10G 0x0800) [linux 213, 8923-8999].
- Interrupt report: 2 bytes on every link change; bit 0x0004 of the first
  word is the link state, set when up and clear when down [linux 773,
  2342-2353]. This is a fix for every chip: before this record the model
  set the bit on every change, down included, so a driver could only see
  link-down by reading `MR_LINK` (as the 2026-10-23 legs did).
- Receive: a frame is refused when its length with FCS exceeds
  `MR_RX_MAX_LEN` (0xC016; Linux writes 1522 [linux 6810-6822]); with
  aggregation off (`UR_CTRL` 0xD406 bit 4) one frame per transfer, else as
  many records as fit the request [linux 3037-3131]. Early size: the
  newer chips end a transfer once `UR_RX_EARLY_LEN` (0xD42E) times the
  chip's unit is reached, after the record that reaches it (an assumption
  of the model: the rule is not in `r8152.c`; the chip's early timeout is
  not modelled); a bulk-IN request shorter than that
  plus `MR_RX_MAX_LEN` plus one descriptor and its alignment is counted
  (`rx_request_below_early_size`). Linux sets the early size to its
  buffer less (1522 + descriptor + alignment), in units of 8 bytes for
  the 8156 family and 8153D and 16 bytes for the 8157/8159 [linux
  3676-3717].

## Per chip

| `--chip` | Chip | Id (Linux) | USB id | bcdUSB at SS | Fastest link | Descriptors | Linux's buffer |
|---|---|---|---|---|---|---|---|
| `8153d` | RTL8153D | 7420 (`RTL_VER_15`) | 0BDA:8053 | 0x0300 | 1000 | 24/8-byte rx, 8/4-byte tx | 48 KiB |
| `8156` | RTL8156 | 7030 (`RTL_VER_11`) | 0BDA:8156 | 0x0300 | 2500 | 24/8, 8/4 | 48 KiB |
| `8156-7020` | RTL8156 | 7020 (`RTL_VER_10`) | 0BDA:8156 | 0x0300 | 2500 | 24/8, 8/4 | 48 KiB |
| `8156b` | RTL8156B | 7410 (`RTL_VER_13`) | 0BDA:8156 | 0x0300 | 2500 | 24/8, 8/4 | 48 KiB |
| `8156b-7400` | RTL8156B | 7400 (`RTL_VER_12`) | 0BDA:8156 | 0x0300 | 2500 | 24/8, 8/4 | 48 KiB |
| `8157` | RTL8157 | 1030 (`RTL_VER_16`) | 0BDA:8157 | 0x0320 | 5000 | 16/16 both ways | 32 KiB |
| `8159` | RTL8159 | 2020 (`RTL_VER_17`) | 0BDA:815A | 0x0320 | 10000 (5000 with no PHY patch) | 16/16 both ways | 48 KiB |

Ids and versions [linux 10058-10082]; USB ids [linux 10398-10405] and, for
which chip each id is, commit `195aae32` ("support new chips": RTL8153C,
RTL8153D, RTL8156A, RTL8156B; 0x8053 added with them), `fd3c7d08` (RTL8157,
0x8157) and `ad2a55b6` (RTL8159, "the product ID is 0x815a"). Which id is
which chip: the 8156 family and the 8153D share one setup in Linux, the
8153D (`RTL_VER_15`) without 2.5G support [linux 9826-9850]; 5G for the
8157 and 2.5/5/10G for the 8159 [linux 9869-9910]. Buffers: `rx_buf_sz`
[linux 9823, 9848, 9887, 9909]. bcdUSB 0x0320 with a SuperSpeedPlus
capability (one symmetric 10 Gb/s sublink) for the 8157/8159 is chosen:
`r8152.c` only tests for SuperSpeed or faster. The model has only the
vendor configuration; whether these adapters also offer a CDC configuration
is not modelled.

### RTL8153D, RTL8156, RTL8156B

- Descriptors as the RTL8153: receive 24 bytes, length with FCS in bits
  0-14 of the first dword, records 8-byte aligned; transmit 8 bytes, length
  in bits 0-17, first and last segment bits 31 and 30, records 4-byte
  aligned [linux 821-861, 9702-9716].
- Early-size unit 8 bytes [linux 3699-3707].
- Linux's bring-up touches, besides what the model's register file keeps:
  `UR_CTRL` cleared of aggregation-off and receive-zero (bits 4 and 7)
  [linux 8500]; the PHY state and external-init bits; the PHY flash
  wait (`RTL_VER_13`, `15`); the patch request (`RTL_VER_11` to `15`); the
  SRAM pairs; for the 8156 (`RTL_VER_10`, `11`) an MDI swap only when
  `UR_UPS_CFG` (0xD842) bit 5 is set [linux 7636-7637]: the model leaves it
  clear, so that path is not taken.

### RTL8157, RTL8159

- Descriptors [linux 864-896, 9718-9732, 2716-2768]: receive 16 bytes,
  length with FCS in bits 17-31 of the first dword (VLAN-tag flag bit 3,
  a 2-bit field in bits 0-1 that Linux does not read; the model writes 0
  there), records 16-byte aligned. Transmit 16 bytes, records 16-byte
  aligned: the first dword carries the first/last segment bits and the
  length as the older header (Linux fills it the same way [linux
  2637, 2744-2760]), the third dword the length shifted left 4, the
  fourth the signature 0x15 << 27. The model takes the length from the
  third dword and refuses a record without the signature or whose first
  dword does not carry the same length.
- Receive format: Linux clears `UR_CTRL` bits 4 and 10 (and on the 8159 bit
  11) for "RX aggregation / 16 bytes RX descriptor" [linux 8491-8498]. The
  model starts with bits 10 (and 11) set (chosen) and refuses received
  frames while bit 10 is set (`rx_desc_mode`): what the chip would really
  send then is not known.
- Transmit signature: Linux clears USB 0xD4AE bit 1 ("TX descriptor
  Signature") [linux 8508-8509]; the model starts with it set (chosen) and
  counts transmits while it is (`tx_sig_check_left_on`).
- The ADV window [linux 160-162, 500-503, 1688-1802]: USB `UR_ADV_ADDR` 0xD5D6,
  `UR_ADV_DATA` 0xD5D8 (dword), `UR_ADV_CMD` 0xD5DC: busy bit 0, write bit
  1, bit 2 selects the IP space (else the BMU space). A command with busy
  set moves one dword and clears busy at once. Linux clears BMU 0x2300
  bit 3 ("Disable Rx Zero Len") [linux 8506-8507] and sets or clears IP
  0xC2C0 bit 29 [linux 3982-3988]; the model starts with both set
  (chosen) so that the driver's writes show.
- Early-size unit 16 bytes [linux 3708-3711].
- Linux's bring-up adds USB 0xCFFE bit 3 set and 0xD3CA bit 0 cleared
  [linux 8374-8377], power-cut handling through 0xCFFF bit 1 [linux
  4335-4345], and on unload clears USB 0xCF04 bits 0-2 and 7 [linux
  9688-9691]; these are plain registers in the model.

## Firmware patches

No blob is included and none is modelled; the chip runs as with none
loaded. Linux requests `rtl_nic/rtl8156a-2.fw` (`RTL_VER_11`),
`rtl8156b-2.fw` (`RTL_VER_13`, `RTL_VER_15`), `rtl8157-1.fw` and
`rtl8159-1.fw`; `RTL_VER_10` and `RTL_VER_12` have none [linux 9925-9993].
Without the file Linux logs "unable to load firmware patch" and goes on:
the patch step does nothing and the rest of the PHY setup runs [linux
5570-5579, 5659-5688, 7057-7083]. `RTL_VER_12`'s PHY setup has its own
patch-request block of PHY writes (0xB88E/0xB890 pairs) that runs without a
file [linux 7873-7893]; the model keeps those as plain registers.

The 8157 works without its patch. The 8159 does too, but commit `a51e171e`
states: "The RTL8159 (RTL_VER_17) requires firmware for its PHY in order to
work at connection speeds > 5GBit ... If no firmware is found, both the
RTL8157 and the RTL8159 will continue to work." So the model's 8159 links at
5 Gb/s at most; `--phy-patched` (`phy_patch=True`) lets it link at 10 Gb/s,
as if the patch had been loaded, to exercise the 10G status path. How a real
8159 without the patch behaves against a 10G partner (5G, or no link) is
not stated; the model takes 5G.

## Speed settings the chip refuses or needs

- 2.5, 5 and 10G are only advertised (`PR_XG_CTRL`); the control register
  can force 10, 100 or 1000 only, and Linux forces 1000 only with full
  duplex [linux 6476-6510]. The model refuses forced 1000 half (no link,
  `no_common_speed`) and ignores multi-gigabit advertisement bits a chip
  does not have (the 8153D's 2.5G bits).
- Linux's default advertisement has 2.5G and faster only on a SuperSpeed
  (or faster) USB link [linux 10259-10280]; an explicit setting can still
  ask for them [linux 9036-9051, 6581-6605]. The model counts a
  multi-gigabit link below SuperSpeed (`multigig_below_superspeed`) as a
  diagnostic of that default policy, not as a chip refusal.
- A powered-down PHY, or one still in external init, does not link.
- Nothing advertised that the partner has: no link (`no_common_speed`).

## Tests

`python -B rtlsimtest.py` (after `host.ps1`): 29 tests, OK with 7 expected
failures.

- `NewerModelTests` (14 tests, no driver core): identity and descriptors
  per chip at SuperSpeed and High Speed (the id read before a
  configuration); the SRAM step; polled bits; external init and power cut;
  no link while powered down or in external init; the ADV window; link at each partner speed (10 to
  10000 Mb/s) for each chip, and the 8159 with and without its patch, with
  `MR_LINK`, the PHY status and the interrupt report; refused and needed
  speed settings; transmit and receive framing per chip; the 16-byte
  receive mode and the signature bit; aggregation off; early-size units,
  and the early size ending a transfer.
- `NewerDriverTests` (7 tests, one per newer `--chip`): the driver's own
  core (`rtlcore.dll`) brought up on the chip at SuperSpeed, version, MAC,
  header sizes (16/16 for the 8157/8159, else 8/4), the 8157/8159 receive
  and transmit bits cleared, link at the chip's fastest speed through the
  interrupt report (`Status` 1 up, 0 down) and `Link`, ARP and ICMP through `TxHeader` and
  `RxParse`, the early size against the core's receive buffer, a cable
  pull, a 1000 Mb/s partner, High Speed (1000 Mb/s, no multi-gigabit
  advertised), the 8159 with its patch at 10000, and `Stop`. All seven are
  expected failures (`PENDING_CORE`) until the core's tables for the newer
  chips land (worker uwf-rtl-new, branch `rtl-new`): today `Start` refuses
  the ids. The same checks pass on the RTL8153 and RTL8153B at SuperSpeed
  (an ad-hoc run of the same method), so the harness itself works.
- The 8 older tests pass; the interrupt test now also checks the
  link-down report (`Status` 0).

Review: Codex (static review, against `r8152.c` v7.3), findings fixed in
the second commit: the interrupt report's link bit (P1), vendor requests
before a configuration (P1), no link with the PHY powered down (P1), the
early size ending a transfer (P1), the transmit length required in both
dwords (P2), the partner's 100 Mb/s abilities (P2), a BOS at High Speed
(P2), the multi-gigabit counter described as a default-policy diagnostic
(P2), the test count (P3).

## Limits

- The register semantics are this project's model of the facts above; a
  real chip's undocumented behaviour (firmware, timing, what the PHY
  reports during training, EEE, green ethernet, power saving) is not
  exercised. Registers the model gives no meaning keep what was written.
- Power-up values chosen to make the driver's clears visible
  (`UR_CTRL` bits 10 and 11, 0xD4AE bit 1, BMU 0x2300 bit 3, IP 0xC2C0
  bit 29, the PHY's default advertisement of every speed the chip has) are
  not known values of a real chip.
- SRAM is word-addressed in the model; Linux also edits odd SRAM addresses
  with byte masks (e.g. 0x8FFF, 0x8183), so the real SRAM may be byte
  addressed with words overlapping; the model keeps each address apart.
- USB descriptors other than the class and endpoint numbers are chosen;
  real descriptors (configurations, bMaxBurst, BOS, bcdUSB, Gen 2x2 for the
  8159 if so) need a dump from a real adapter.
- usbredir has no SuperSpeedPlus speed; a guest sees SuperSpeed.
- The PHY's MDI swap path of the RTL8156 (`RTL_VER_10`, `11`) is not
  exercised (its trigger bit is left clear).

REAL CHECK NEEDED: none of these chips is on the bench. When one is: its
`lsusb -v` descriptors, the version word, and a link at its fastest speed
with the trace's `MR_LINK` value.
