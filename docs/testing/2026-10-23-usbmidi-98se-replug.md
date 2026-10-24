# usbmidi: the Windows 98 SE replug limitation, fix attempt, 2026-10-23

Owner, 2026-10-23: try to find a fix for the 98 SE replug limitation
(design 13/15/18; [the M1 record](2026-10-22-usbmidi-m1.md), "Windows 98 SE
replug and freezes"). Result: **no fix found in the driver or its INF**;
the runs below show why the one fix that was in reach (a new devnode per
arrival) cannot work, and narrow the defect to 98 SE's WDM MIDI client
side. No Microsoft binary was read (registry exports and the driver's own
debug-port trace only).

Kit `E:\usb-wdm-free-vm\midi-replug\` (monitor 49160, simulator 49161),
QEMU TCG, 1 vCPU, overlays of the M1 kit's 98 SE image `w98r.qcow2`
(QEMU UHCI), `midiemu.py` `multi` (FFFF:0002, two ports) and `lone`
(FFFF:0003) profiles, traced checked builds (`UMTRACE`).

## Runs

| Run | What | Result |
|---|---|---|
| u1 | Plug `multi`, loopback, unplug; plug `lone` (another devnode, first arrival in this boot); unplug; replug `multi` | `multi` 355/355 PASS; then **`lone` fails too**: `MIDITEST 0 0` `WinMM failure=4` (MMSYSERR_ALLOCATED); `multi` replug: open in/out 4/4 |
| e1 | Filters registered under `KSCATEGORY_AUDIO` only (no RENDER/CAPTURE) | WinMM lists **no** MIDI port (0 in, 0 out); so WinMM's MIDI list comes from the RENDER/CAPTURE interfaces |
| q1 | The adapter refuses `IRP_MN_QUERY_DEVICE_RELATIONS` / `TargetDeviceRelation` | WinMM lists **no** MIDI port: the client that opens each filter and asks for its target relation (a target-device-change registration) is the one that builds WinMM's MIDI ports |
| o2 | `multi`, loopback PASS, then an **orderly** removal before the unplug (`MIDITEST eject`: `CM_Query_Remove_SubTree` + `CM_Remove_SubTree`, both 0; trace: QUERY_REMOVE, REMOVE), unplug, replug | Replug: `0 0` and `1 1` `WinMM failure=4`. A removal that is queried first does not help |
| w1 | Control: a MIDI removal, then Windows' own `usbaudio` (QEMU `usb-audio`) arriving | Not settled: the guest stopped with a fatal exception 0E in VMM during the first loopback (the TCG freeze of the M1 record) |

Readings, from the trace (opens = `IRP_MJ_CREATE` on the filters):

- Each arrival has two groups of filter opens: one client opens each
  filter and reads its pin and topology properties; another opens it, sends
  `TargetDeviceRelation`, and closes. u1: first arrival 8 + 16 of the
  second kind; the `lone` arrival after the removal 4 + 2; the `multi`
  replug 8 + 8. o2: 17, then 9 after the orderly removal. After **any**
  removal of a WDM MIDI devnode, about half of the second kind no longer
  come, for every devnode that arrives afterwards in that boot, and WinMM
  then answers MMSYSERR_ALLOCATED on every open of those devnodes without
  a create reaching the driver (as in the M1 record).
- Registry (`MediaResources\midi\wdmaud.drv<nnnn>`): the removed device's
  entry goes to `Active=0` on removal and back to `Active=1` with a new
  `DevNode` value on the replug; nothing stale was found there.
- Devices that were already present when another one was removed keep
  working (M1 record, e2/e4); only arrivals after a removal fail.

## Why there is no driver fix

- A child devnode per arrival (the costly alternative of the M1 record,
  built and run for this record: the driver as a bus driver on the USB
  function, a `USBMIDI\PORTS` child with a new instance number per arrival
  in a boot, the PortCls adapter on the child; diff kept in the kit as
  `child-attempt.diff`, not committed) cannot help: u1 shows that a devnode
  the boot has never seen fails in the same way once any MIDI devnode has
  been removed.
- The failure follows a removal whatever its kind (surprise, or queried
  first), whatever the reference strings, names or devnode values (M1
  record), and is shared by every later arrival, so it is state in 98 SE's
  WDM MIDI client side (the component that registers on each filter's
  target device), not in anything one device's driver leaves behind.
  The driver releases everything on REMOVE (trace: shutdown, release,
  `DriverUnload`).
- Making the driver invisible to that client (no `TargetDeviceRelation`,
  or no RENDER/CAPTURE interfaces) removes the MIDI ports altogether.

So the limitation stands as documented: on 98 SE, restart Windows after a
USB MIDI device has been unplugged (any USB MIDI device, not only the one
that was removed, needs the restart before it can be opened). Plugging the
devices in before Windows starts, or keeping them plugged, works.

## Not run

The 98 SE replug legs under NUSB 3.3 EHCI and xHCI98 and the 2000/XP
regression replug the task listed for a fix: there is no fix to run them
on. Both host stacks already show the same failure (M1 record; 98 SE legs
records), and o2/u1 show it is above the USB stack.
