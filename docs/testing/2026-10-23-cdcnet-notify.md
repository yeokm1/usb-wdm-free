# cdcnet notification walk: 98 SE reruns, 2026-10-23

Development legs, not a release. They check branch `cdcnet-notify`
(based on `cdcnet-trace`): `Notification()` now handles every notification
of an interrupt completion (`CdcNotifyNext`, `CdcNotifyFeed`), and the
interrupt transfer is one `wMaxPacketSize` (ethernet.md section 6.1).
The defect it fixes was found with the SIMULATED NCM function on branch
`simcov` (`2026-10-23-cdcnet-ncm-sim.md` there): under xHCI98 a
CONNECTION_SPEED_CHANGE and the following NETWORK_CONNECTION 0 arrived in
one 24-byte completion and the disconnect was dropped.

Rig: kit `E:\usb-wdm-free-vm\simcov` (monitor 49200, simulator 49201),
98 SE overlay `s1` of `net9x\base\w98-nusb-hub.qcow2` (NUSB 3.3, xHCI98
3.1.0.0). Driver: `build.cmd ndis5 trace` of `cdcnet-notify` 591a083,
`cdcnet.sys` SHA-256 `50e6f19f...e759`, copied over
`C:\WINDOWS\SYSTEM\cdcnet.sys` before a restart. Simulator `ncmemu.py`
(`simcov` 89feed8), defaults (interrupt endpoint 16 bytes) plus
`--flap-after`.

## NCM, xHCI98: PASS

The simulator sends NETWORK_CONNECTION 1 and CONNECTION_SPEED_CHANGE on
the data alternate, then for the cable pull NETWORK_CONNECTION 0, and
NETWORK_CONNECTION 1 with CONNECTION_SPEED_CHANGE. The trace now shows
every one, each in its own completion (16 bytes for the speed change, the
endpoint's size):

```
interrupt 8 bytes: a1 0          notification 0: link 1, 100000 kb/s
interrupt 16 bytes: a1 2a        notification 2a: link 1, 480000 kb/s
interrupt 8 bytes: a1 0          notification 0: link 0, 480000 kb/s
                                 indicate MEDIA_DISCONNECT
interrupt 8 bytes: a1 0          notification 0: link 1, 480000 kb/s
                                 indicate MEDIA_CONNECT
interrupt 16 bytes: a1 2a        notification 2a: link 1, 480000 kb/s
```

After the flap: `ping -n 6 -l 1472 192.168.77.1` 6/6, 0 host NTB layout
violations. Before the fix the same leg lost the disconnect (no
MEDIA_DISCONNECT).

## NCM, NUSB 3.3 EHCI: PASS for the link, speed change not received

The three NETWORK_CONNECTION notifications arrive (8 bytes each),
"indicate MEDIA_DISCONNECT" then "indicate MEDIA_CONNECT"; `ping -l 1472`
6/6 after the flap; removal "stopped; 0 malformed transfers, 0 truncated
notifications". Neither 16-byte CONNECTION_SPEED_CHANGE completes a
transfer, now with 16-byte transfers as before with 64-byte ones: the
packet is lost below the driver.

**REAL CHECK NEEDED** (NUSB EHCI and a full-size interrupt packet): on a
real EHCI controller with NUSB 3.3 on 98 SE, a CDC-ECM or NCM device whose
notification endpoint is 16 bytes (an RTL8153 in ECM mode, an
AX88179A/RTL8156 in NCM mode, or an Android phone in NCM tethering) with
the cdcnet trace build: plug in, pull and replug the cable. Look for the
trace line "interrupt 16 bytes: a1 2a" and "notification 2a" (QEMU's EHCI
with usb-redir never delivered them), and MEDIA_DISCONNECT/MEDIA_CONNECT.

## ECM regression, xHCI98: PASS

QEMU's own `usb-net` (stock, Full Speed, `netdev user`) hot-plugged on
`xhci.0`: probe "configuration 1: probe 0 protocol 2", "initialized:
protocol 2, full speed, link 1", MAC 52:54:00:12:34:56; its status
endpoint answers NETWORK_CONNECTION 1 on every poll, each an 8-byte
completion handled once; `ping -n 6 -l 1472 10.0.2.2` 6/6; removal "0
malformed transfers, 0 truncated notifications".
