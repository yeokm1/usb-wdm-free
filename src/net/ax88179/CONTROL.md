# AX88179 portable descriptor/control planning, 2026-10-21

`host.ps1` builds a host-only DLL from this actual C planner. The original
`axread.py --libusb <DLL> --output <new-json-path>` helper uses it to validate
the connected exact 0B95:1790 profile and construct the three diagnostic
reads. Its executor accepts only bounded vendor IN steps, validates the
whole plan first, and has no OUT/configuration/reset/interface-claim path.
Output includes the raw MAC address and belongs in ignored `out/`.
This helper is a host observation tool, not a Windows guest driver.

The separately authorized `axnet.py` executor now performs bounded volatile
initialization and real DHCP/router ARP/ICMP, using the same C planner and
framing core. It has a strict volatile-write allowlist, preserves the MAC,
restores captured MAC-register state and closes its interface. It does not
change this read-only helper's behavior. Its first physical results and
remaining limits are recorded in the
[physical traffic log](../../../docs/testing/2026-10-21-ax88179-physical.md).

`axctrl.c/.h` is a bounded, allocation-free planning core. It does not
open a device, submit USB requests, delay a thread, select a configuration,
read a real MAC/register, write hardware or integrate with NDIS. Passing
its host tests is not a working Ethernet driver or an initialization pass.

## References and licence

Vendor request/register/link facts and the four queue parameter rows are
adapted from official Linux v6.12
[`drivers/net/usb/ax88179_178a.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/net/usb/ax88179_178a.c),
commit `adc218676eef25575469234709c2d87185ca223a`. The original source is
retained unchanged in ignored `docs/references/linux-v6.12/`; its SHA-256
is `1c851bd414cb0251723cc50ae5c652032870ddcdd7ceef60b3d7e96f25e68e05`.
The [reference acquisition record](../../../docs/testing/2026-10-21-ax88179-framing.md)
records full origin, Git blob verification and source licence.

Both adapted files retain ASIX's 2011-2013 copyright and
`GPL-2.0-or-later` SPDX tags, and have individual rows in `NOTICE.md`.
Their parser, plan representation and tests are original implementations.
No Microsoft/vendor binary code, disassembly or DDK/WDK sample was used.
The mirrored ASIX datasheet does not supply implementation licensing or a
complete vendor request protocol and is not silently treated as a verified
manufacturer master.

USB2 packet limits follow the downloaded official USB 2.0 specification,
section 5.8.3 and chapter 9 standard descriptors. The
[published bulk-transfer overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-bulk-and-interrupt-transfer)
also distinguishes full/high/SuperSpeed packet maxima 64/512/1024.
The [SuperSpeed companion declaration](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbspec/ns-usbspec-_usb_superspeed_endpoint_companion_descriptor)
documents burst, streams and interval fields. Only the stated profile is
validated; this module is not a complete USB compliance validator.

## Descriptor profile and the real virtualization mismatch

`AxCtrlDescribe` requires an exact 18-byte device descriptor for
`0B95:1790`, one configuration, one interface 0/alternate 0 with class
`FF/FF/00`, status IN `81`, bulk IN `82` and bulk OUT `03`. It bounds the
configuration walk by `wTotalLength`, validates descriptor steps and exact
endpoint count, and rejects duplicate/wrong endpoints, extra interfaces,
unknown layouts and unsupported stream features. Failed output is zero.

The caller supplies the **observed connection speed** through the module's
own `AX_SPEED_FULL/HIGH/SUPER` enum. This is not libusb's enum: libusb raw
SuperSpeed value 4 must be mapped explicitly to `AX_SPEED_SUPER` (3).
`bcdUSB` is retained as metadata and never substitutes for link speed.

- Full-speed bulk maxima must be one of 8/16/32/64; high-speed must be 512;
  SuperSpeed must be 1024. Bulk packet reserved bits are rejected.
- The profile's status endpoint is eight bytes, with a valid nonzero
  polling interval. High/SuperSpeed intervals above 16 are refused.
- USB2 paths reject SuperSpeed companions. SuperSpeed paths require an
  immediate bounded six-byte companion for each endpoint, burst count
  1..16, no streams/reserved attribute bits, and zero bulk interval bytes.
- The captured physical status companion has interval bytes zero. This is
  retained with `AX_WARN_STATUS_INTERVAL_ZERO`, not presented as a proven
  schedulable interrupt interval. Status polling/interrupt I/O is not
  implemented here. Valid bulk-profile output does not certify that
  periodic companion value against a complete USB3 specification.

The unchanged physical SuperSpeed fixture in `testdata/ax179/` passes
this bulk profile at observed SuperSpeed, with that explicit status warning.
Both its SS descriptors and the separately captured guest descriptor set
in `testdata/ax2k/` are rejected when the caller observes high speed. The
guest set also has EP0 byte 64 rather than the SS exponent 9, so it is not
accepted as a valid SuperSpeed device descriptor either. See the
[actual Windows 2000 run](../../../docs/testing/2026-10-21-ax88179-2k-descriptors.md).
No captured bytes are patched to manufacture a passing hardware result.

The tests construct **synthetic** USB2 configurations by removing SS
companions and selecting legal 512-byte or 64-byte bulk endpoints, with an
appropriate USB2 device descriptor. Those prove parser behavior only;
they are not a new physical capture or a demonstrated QEMU speed conversion.

## Explicit plans and their stop gates

`AxCtrlReadPlan` returns three vendor/device-IN requests from the pinned
Linux read helpers. The exact setup bytes are tested independently:

| Result tag | bmRequestType / bRequest | wValue / wIndex / wLength |
|---|---|---|
| Physical USB status | `C0 / 01` | `0002 / 0001 / 0001` |
| MAC address | `C0 / 01` | `0010 / 0006 / 0006` |
| PHY status | `C0 / 02` | `0003 / 0011 / 0002` |

The plan ends at `AX_GATE_OBSERVATIONS`. These are descriptions of
potential reads, not an assertion that hardware has been queried. A future
executor must check transport status and exact response length before
decoding; `AxCtrlResponse` checks only the latter. None of these routines
implies permission to perform live vendor controls merely by being called.

`AxCtrlPowerPrefix` builds only the source-proven beginning of Linux's
reset sequence, marked `HasWrites=1`:

1. Vendor MAC write `40/01`, value `0026`, index `0002`, length 2, data
   `00 00` (PHY power/reset).
2. Same register write, data `20 00` (`IPRL`).
3. A required 500 ms delay step.
4. Vendor MAC write `40/01`, value `0033`, index `0001`, length 1, data
   `03` (the source's two clock-enable bits).
5. A required 200 ms delay step.
6. Vendor EEPROM-IN read `C0/04`, value `0043`, index `0001`, length 2.

It then **must stop** at `AX_GATE_AUTODETACH_DECISION`. Linux's next
auto-detach branch depends on that EEPROM word and fresh clock/PHY-power
register values. The module does not guess those values, skip the branch,
write EEPROM, or label this prefix as a complete initialization. It does
not set configuration, allocate pipes or claim RX/TX is enabled.
`AxCtrlSetup` serializes a bounded read/write step into the eight-byte
little-endian setup packet; delay steps cannot be serialized as requests.
The payload remains separate in `Step.Data`.

`AxCtrlMac` accepts exactly six nonzero unicast bytes, including locally
administered addresses. It rejects all-zero, multicast and broadcast
addresses. It neither synthesizes a random address nor rewrites the node
ID, unlike optional policy in the Linux network integration.

`AxCtrlPhy` decodes the documented Linux `GMII_PHY_PHYSR` word: link bit
`0400`, duplex `2000`, and speed mask `C000` for 10/100/1000. Reserved
linked-speed encoding and all-ones responses are rejected; link-down is
represented without an invented speed.

`AxCtrlLinkPlan` requires a valid MAC, exact bus/PHY reply lengths and a
hardware USB-status speed that agrees with the caller-observed speed.
When link is up it derives, without writing registers, the source's
candidate medium bits and queue image:

| Ethernet / USB condition | Queue bytes | Candidate RX buffer |
|---|---|---:|
| 1000 Mb/s, SS | `07 4F 00 12 FF` | 20480 |
| 1000 Mb/s, HS | `07 20 03 16 FF` | 24576 |
| 100 Mb/s, SS or HS | `07 AE 07 18 FF` | 26624 |
| 10 Mb/s or full-speed USB | `07 CC 4C 18 08` | 26624 |

The medium base is `0130` (receive plus TX/RX flow-control bits), with
the source's gigabit/125MHz, 100Mb/s and duplex bits added as appropriate.
No jumbo mode is selected. These are candidate parameters from an observed
snapshot, not a complete link-reset write plan. A future implementation
still needs bounded FIFO readiness handling, receive-filter policy,
checksum mode, MAC policy, PHY/EEE/autoneg sequencing, actual buffer and
host-stack limits, power/PnP lifetime and failure recovery. No array in
this module may be treated as a complete hardware-ready sequence.

## Verification

Run `ctrl.cmd` to build and execute `CTRLTST.EXE` with VC6 `/W4 /WX`.
Recorded result: **PASS: 66542 AX88179 descriptor/control/link checks**.
The tests load the exact physical and guest fixture files, reject the
actual SS-on-EHCI descriptor mismatch, and separately label synthetic
FS/HS success paths. Additional checks cover guarded truncation reads,
all-byte single-bit descriptor mutations, setup byte order and response
lengths, failure zeroing, MAC edge cases, all 65536 PHY word values,
18 Ethernet-speed/USB-speed/duplex combinations and bus-speed disagreement.

The firmware/register inputs are synthetic; no new physical state was read
or modified by this work. Existing framing tests are a separate core and
are not broadened into an NDIS, USB transfer or target-OS success claim.
