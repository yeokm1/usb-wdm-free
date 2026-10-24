# Windows libusb event-loop scheduling review, 2026-10-21

Source-only review; no QEMU implementation/build, driver, VM or host binding
was changed. The audio agent owns the current physical experiment. This is
a concrete candidate for coordinated testing, not proof of a packet-loss fix.

All upstream anchors below are from QEMU v11.1.0 commit
`84f07211cc5b4fc6a371559bf8a5de4fb068e648`. Main-loop, async and network
sources carry Fabrice Bellard/QEMU-contributor MIT permissions; the inspected
Xen/VFIO examples are GPL2-compatible. No Microsoft/vendor binary code or kit
sample was consulted. Official raw files were compared in memory against the
corresponding local source; the cited unmodified files matched byte-for-byte.

## Clock and callback contracts

- [`include/qemu/timer.h` lines22-42](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/include/qemu/timer.h#L22)
  says REALTIME continues while stopped and should not drive virtual-machine
  state. VIRTUAL stops with the VM; HOST additionally reflects wall-clock
  changes and is intended for emulated real-time sources.
- [`util/qemu-timer.c` line650](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/util/qemu-timer.c#L650)
  maps REALTIME to `get_clock()`. On Windows,
  [`timer.h` line827](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/include/qemu/timer.h#L827)
  implements that with QueryPerformanceCounter. Switching to HOST would
  introduce wall-clock behavior rather than solve host-I/O readiness.
- [`include/qemu/main-loop.h` lines356-365](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/include/qemu/main-loop.h#L356)
  documents the main-loop BQL contract. Windows
  [`util/main-loop.c` lines522-551](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/util/main-loop.c#L522)
  releases BQL for the host wait and reacquires it before dispatch.
- A bottom half alone does not freeze while paused.
  [`util/async.c` lines158-225](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/util/async.c#L158)
  invokes scheduled callbacks without a runstate test. Its optional
  MemReentrancyGuard traces/marks reentrancy; it is not a pause guard.
- Upstream USB POSIX fd callbacks pump libusb directly in the main loop.
  Windows instead uses the VIRTUAL poll timer in
  [`host-libusb.c` lines259-304](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/hw/usb/host-libusb.c#L259).
  That timer already suspends ordinary event dispatch while paused.

REALTIME -> schedule BH -> BQL/runstate-guarded event pump is a plausible
separation of host wakeup from guest-facing work. It still needs an explicit
runstate guard and paused idle policy; merely adding a BH or BQL does not
make an unguarded REALTIME callback acceptable. It also adds lifecycle state
that a more direct existing upstream mechanism can avoid.

## Narrower candidate: main-loop poll notifier

Use the external-library integration pattern already present in
[`net/slirp.c` lines372-390](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/net/slirp.c#L372).
Its FILL callback supplies the host wait timeout; its OK/ERR callback pumps
the library after the wait. Registration is at line682 and removal before
library destruction at line162. `MainLoopPoll` uses a uint32 timeout in
milliseconds, declared in main-loop.h lines437-450.

`main_loop_wait` notifies FILL before deriving the host wait and OK/ERR after
return, in [`util/main-loop.c` lines565-600](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/util/main-loop.c#L565).
It takes the earlier deadline when combining this timeout with QEMU timers.
This lets Windows libusb cap an actual host wait without selecting a QEMU
timer clock and without a helper thread or BH.

Proposed Windows-only functions/state:

| Item | Narrow contract |
|---|---|
| Global `Notifier usb_host_poll_notifier` | Owned by the existing process-wide libusb context; never opaque per-device/ring storage. Register once after successful context creation. |
| `usb_host_poll_notify(..., MainLoopPoll *poll)` FILL | Assert/document BQL. If RUNNING, cap timeout to1ms when ordinary or ISO activity exists, otherwise1000ms. Use MIN with the already requested timeout. If stopped, do not cap it to1ms and do not pump; the explicit resume kick handles restart. |
| Same function OK/ERR | If RUNNING, invoke the existing zero-timeout libusb event pump once under BQL. Do not deliver callbacks from a helper thread. Error reporting may record libusb failure, but must not invent successful completion. |
| `usb_host_poll_kick()` | Wake the main loop with `qemu_notify_event()` when new work can require an earlier wait. This is essential: a vCPU may submit while the main loop is already sleeping on an old idle timeout. It is not enough to change a counter that the next FILL would read. |
| VM-change callback | Resume kicks the main loop promptly; paused/stopped execution never forces a1ms loop merely because transfers are outstanding. Existing USB auto-scan's VM-change handler may be extended carefully or a dedicated global handler may be registered once. |
| Context shutdown | If context destruction is ever introduced, remove notifier/change handler before context teardown. Existing source keeps its global context for process lifetime; no new per-device finalizer should remove shared state while another host device uses it. |

Upstream networking also explicitly gates delivery with
[`net.c` lines632-645](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/net/net.c#L632)
and flushes/purges at VM-state changes, lines1692-1712. This is evidence for
the lifecycle pattern, not a claim that network queue policy can simply be
copied to USB isochronous data.

The Windows `qemu_add_polling_cb` alternative is insufficient by itself:
main-loop.h lines118-135 explicitly warns that callbacks may be delayed by
an arbitrarily long main-loop wait. A separate blocking poll thread plus a
Windows event is suggested there, but libusb's public Windows backend does
not provide a simple existing QEMU-ready fd integration. Running libusb
callbacks on such a thread would require a much larger callback ownership
and BQL handoff redesign. It is not the proposed narrow change.

## Pause, close/drain and orphan boundaries

1. **No new callback concurrency.** Both notifier dispatch and current
   virtual-timer dispatch execute in the main-loop/BQL domain. Explicit
   close/drain calls remain in that same serialization domain. Preserve this;
   do not put `libusb_handle_events_timeout` on an independent thread.
2. **Paused parity.** In-flight host operations can finish physically while
   paused, but ordinary notifier dispatch leaves callbacks pending until
   resume, just as the existing Windows VIRTUAL timer does. This does not
   promise continuous physical audio through a paused guest. It avoids
   paused1ms busy wakeups and guest-facing callbacks from the normal pump.
3. **Explicit teardown is separate.** Upstream
   [`usb_host_abort_xfers` line1072](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/hw/usb/host-libusb.c#L1072)
   cancels requests and explicitly pumps the global context while draining,
   up to100 waits of2.5ms, even during a close initiated while stopped.
   That can also dispatch callbacks belonging to another USB host device.
   This already exists; the new normal pump must not claim to eliminate all
   possible guest-state changes during every paused teardown path.
4. **Do not pretend a global pump can select only orphans.** libusb may
   invoke any ready callback in its context. Safely pumping only orphan
   completions while deferring other completions would require separate
   completion staging, beyond this proposal.
5. **ISO ring destruction is not completion.**
   [`usb_host_iso_free_xfer` line606](https://github.com/qemu/qemu/blob/84f07211cc5b4fc6a371559bf8a5de4fb068e648/hw/usb/host-libusb.c#L606)
   nulls an in-flight transfer's user_data and frees the ring-side wrapper.
   The later callback's null branch (line523 onward) releases the libusb
   transfer/buffer. A global ISO-active count must decrement at actual
   callback entry, **before** the null-user-data branch, without reading a
   freed ring/device. Submit failure must not leave an active count. Keep the
   existing candidate's independently tested accounting rather than derive
   activity from rings that have already been destroyed.
6. **Timeout/leak branch is not a lifetime proof.** The existing ordinary
   abort path's limit stops waiting and unlinks requests to avoid freeing
   lower-owned storage. This review does not certify every later ordinary
   callback after that pathological timeout. The notifier must preserve
   serialized ownership rather than introduce concurrent cleanup on top of
   that existing limitation.

No hard1ms execution guarantee follows from a1ms host wait ceiling: OS
scheduling, BQL contention and long callbacks can delay the main loop.
Pumping more often is a scheduling candidate, not proof of zero USB loss.

## Required checks before physical comparison

- Active/idle FILL preserves an earlier timeout, never uses a negative or
  overflowing millisecond value, and performs no VM-facing work itself.
- A submission during an idle wait kicks it; resume does likewise. Paused
  state produces no1ms rescheduling or normal libusb dispatch.
- Pump callbacks run under BQL, with no new thread or nested-concurrent
  libusb dispatcher. A queued no-device BH cannot use released device state.
- Submit failure, normal completion and completion after ISO ring free
  maintain exact global activity counts and do not access a freed wrapper.
- Close while paused can still complete its explicit drain. Paused unplug,
  resume after completions and no-device completion are exercised without
  inventing success or prematurely freeing lower-owned buffers.
- A/B uses the same guest binary and KISO settings, with exact QEMU hashes,
  raw packet statuses, host timeline and bounded evidence gate. Do not merge
  guest safety changes or extra control writes into that scheduling comparison.

## Implementation and physical comparison

The subsequent `qemu-poll.patch` implements this plan. An independent
actual-source harness passed 50000 randomized timeout/pause cases, notifier
registration and resume checks, reentry guards, ISO buffer patterns and
submit/orphan accounting. These checks establish source behavior only.

The physical X4 comparison used QEMU SHA256
`4BCC83D69230267CE44E3C3C2BB17313437B686B84B15C26A560E68CE9F572A7`
with the unchanged C7 guest driver, corrected raw-status libusb, lean tracing
and 16 ISO buffers. `w7-poll1` failed: the two-second player measured
813ms GetTickCount and 4765ms QPC; 463 OUT callbacks had zero raw errors,
but 65 feedback callbacks contained 50 packet errors. There were no empty
ring drops. Host callback gaps still exceeded one second. The notifier is
therefore a tested scheduling experiment, not a playback reliability fix.
