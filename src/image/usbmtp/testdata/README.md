# usbmtp library host cases and fixtures

The host test of the protocol library (`docs/design/mtp.md` 7.9, "The host
test"; 11.3; 11.4; 12, "M1's library stage") is written here first, as
cases, before any library code (7.9, "Order of work", step 1). Nothing
here is built or run yet: `hosttest.c`, `simdev.c` and `hosttest.ps1` come
with the library and read these files. Every expected result is written by
hand from the design and the specifications, never produced by the library
or by `simdev.c` (7.9, "Expected results written independently").

Where a case and the design differ, the design holds and the case is
corrected. The trace from the design to the case ids is in
`docs/design/mtp.md` 12.1, with the statements no case covers and the
questions the cases wait on.

## Files

| File | What |
|---|---|
| `*.hex` | a fixture: bytes written by hand from a specification table, as hex text (below) |
| `*.expect` | the fixture of the same name decoded, written by hand |
| `cases/<area>.cas` | the cases of one area: what each sets up and does |
| `cases/<area>.expect` | the observable result of each case of that area |

Areas: `frame` (containers and data phases), `data` (datasets, property
lists, caps, the decoding budget, fuzzing, allocation), `event`, `sess`
(sessions, ids, the open), `job` (jobs, callbacks, re-entry), `cancel`
(the latch, the handshake, 7.5), `life` (references, locks, close,
GIVENUP, exit), `power` (the hold, the registry, the program's refusal,
the scan), `if11` (interface 1.1), `ops` (operation contracts, uploads,
deletion, Android, uncertainty), `names`, `dates`, `copy` (7.6 to 7.8),
`redir` (the library against `mtpemu.py` through `hostredir.c`).

## Fixtures (`*.hex`)

Hex byte pairs, whitespace free; `#` starts a comment to the end of the
line. Each fixture starts with a comment naming the specification section
it comes from. A container fixture holds the whole container, header
included; a dataset fixture holds a data phase's payload only. Multi-byte
values are little-endian, as on the wire.

So that a fixture can be checked by reading, every field's bytes follow
a comment `@<offset> <name> <type> = <value> (<n> bytes)` (a property
list element's four parts each have one, under a heading comment), and
the last line is `# total <n> bytes`. The values are chosen so that a
parser that skips, swaps or narrows a field cannot pass by chance: no
field with identical halves, no handle, storage id, transaction id or
parameter that fits in one byte, every UINT64 with both halves nonzero
and different, and every fixed-width field nonzero and distinct where
the specification allows it; a field whose value the specification or
the fixture's purpose fixes (a container type, an operation code, a
datatype code, a deliberately empty array, a sentinel) keeps it.
`pltypes1` and `pltypes2` carry one property-list element of each
scalar datatype and each array datatype of MTP 1.1, 3.2 (signed values
negative, every byte distinct), the unknown properties skipped by size.

A fixture's `.expect` holds `name = value` lines, one per decoded field,
in the dataset's order, every field the parser decodes, none left out;
arrays as `name[] = a, b, c` (hex for codes and
handles, `h` suffix); strings in double quotes with `\uXXXX` for a unit
outside ASCII; `result = MTPE_OK` (or the code the parse must return).
Its `counts` block names every count field the mutations of 7.9
("Malformed datasets") change: `at <offset> <size> elem <bytes> cap <n>`.

## Case scripts (`cases/*.cas`)

```
case <ID>
  title  <one line>
  from   <design places, e.g. 7.9/Jobs and cancel; 12/host/MtpQueue>
  rule   A | B | A,B          Q23's rule(s) it runs under (cases with a hold)
  waits  Q<n> [(only ...)]    depends on an open owner question (mtp.md 14):
                              not judged until answered; "(only ...)" names
                              the lines that wait, the rest stands (none
                              is open now: every CQ of mtp.md 12.1 and
                              Q14 to Q17, Q24 to Q27 are answered)
  waits  capture              its fixtures are not yet captured (Q16: a
                              real device's dataset, committed only after
                              the owner has seen its readable fields)
  <step>
  ...
end
```

`ID` is `<AREA>-<nnn>`, stable: a case that is withdrawn keeps its id
with `title RETIRED` and no steps, and an id is never reused; a case
split in two keeps its id on the first part and gives the others the
same id with a letter (`LIF-015b`). A case without `rule` does not
involve the hold and runs once. `rule B` is Q23's alternative, built as
a test configuration of the library (the owner decided (A); 12 asks for
both where the hold is involved).

**Rule-only steps and rows.** In a case with `rule A,B`, a step, a table
row or an `.expect` line prefixed `[A]` applies only under (A) and one
prefixed `[B]` only under (B); under the other rule the step is not
executed, the row not run, the line not checked (a table row carries
the prefix right after its letter: `m  [A] button ...`). A step that
exists under one rule only (`button` is (A)'s; a lapse is (B)'s) is
always prefixed, or sits in a prefixed row. Unprefixed lines hold under
every rule the case runs.

Steps run in order on the test's control thread, under the execution
contract below: nothing about the order of threads is left to the
system's scheduler.

| Step | Meaning |
|---|---|
| `dev d0 [k=v ...]` | a simulated device behind `MTPIO_OPS` (`simdev.c`); settings below |
| `power P [k=v]` | an `MTP_POWER` (`MtpPowerStart`), its hidden window on the build host |
| `open d0 [-> r]` | `MtpOpen` on d0 with P, from a test thread |
| `close d0` | `MtpClose` |
| `queue d0 jN { body } [-> r]` | `MtpQueue` with a `run` that does `body`; each job records its `run` and `drop` calls |
| `cancel d0 jN` / `cancel d0 0` | `MtpCancel` with that job's id, or 0 |
| `retain d0` / `release d0` | `MtpRetain` / `MtpRelease` by a test thread |
| `bcast <PBT_...>` | a `WM_POWERBROADCAST` sent to P's hidden window; the control thread waits for its answer, which is recorded |
| `bcast <PBT_...> async` | the same sent from a test thread of its own; the control thread goes on at once; `wait answer` waits for it |
| `button` | (A)'s "Use the device now": the post to the power thread's window |
| `names d0 [-> r]` | `MtpNames` from a name thread |
| `scan [d0 ...]` | one scan: `GET_INFO` on every n through info handles |
| `exit` | the program's exit sequence of 7.9, "Program exit" |
| `clock +ms` / `clock =ms` | the fake clock (`GetTickCount` as the library sees it) |
| `sim d0 <setting>` | change the simulated device now (below) |
| `at <hook>[#k] [d0] : <step>` | when a library thread reaches that hook (its k-th time), stop it there, do the step on the directive thread, let it go when the step is done |
| `at <hook>[#k] [d0] : async <step>` | the same, but the step starts on a test thread of its own and the stopped thread is let go at once (required where the step needs a lock the stopped thread holds) |
| `hold <hook>[#k] [d0]` | stop the thread there until `go <hook>`; every `hold` has its `go`, or the case is a script error |
| `sweep <hook> .. <hook> : <step>` | the whole case once per hook point recorded between the two in a run without the step, the step done at that point (below, "Sweeps") |
| `wait <condition>` | until it holds; a wait that cannot end fails the case (below, "Deadlock and the 600 s limit") |

A job body is a sequence of operations as `run` makes them:
`GetObject h=5 sink=s` (a library call by its name without `Mtp`),
`queue ...`, `cancel ...`, `close`, `post` (`run`'s final act). A sink,
source or progress callback is named `s`, `src` or `prog`, with what it
does: `src short=100` (delivers 100 bytes fewer), `sink call=GetObject`
(tries an operation from inside).

### simdev settings

| Setting | Meaning |
|---|---|
| `mps=8..1024` | bulk `wMaxPacketSize` |
| `form=combined` / `split` | the data phase form it sends and expects |
| `zlp=1` / `0` | sends the zero-length packet after a whole-packet run |
| `info=<bytes>` | `GET_INFO`'s returned size (0 to 52), `fail=<win32>` to fail it |
| `cbsize=<n>` `minor=<n>` `major=<n>` `gen=<n>` `flags=<f>` | `GET_INFO` fields |
| `gate=<bits>` `held=<mask>` `vetoes=<n>` `forced=<n>` | interface 1.1 power fields |
| `gateclose at=<ms> code=995|21` | the gate closes then: calls pending then end with `code` (995 as the gate's stop ends them); calls made while it is closed fail 21; with nothing pending it ends nothing; on a closed gate it does nothing |
| `lapse` | the driver's `QUERY_HOLD_MS` lapse, applied in `GET_INFO` as the driver does |
| `fault <name>` | one of `mtpemu.py`'s `faults` (11.2), or a host-only fault below |
| `stuck <call>` | that call pends and never returns until `sim d0 unstick` (then 995); `ABORT` reports its pipe busy |
| `stuck <call> until-abort` | that call pends until an `ABORT` of its pipe, which ends it 995 at once and reports quiet |
| `stuck <call> abort-proof` | the same as `stuck`, said explicitly where an `ABORT` is expected to fail to end it |
| `<call>#<sel> returns=<win32> count=<n>` | that call returns that code and reports that byte count |
| `quiet=0` | `EVENTS` stop and `ABORT` report not quiet until `quiet=1` |
| `ops=<list>` | the operations GetDeviceInfo lists |
| `strings=SIMULATED` / `real` | the device's strings |
| `store <tree>` | its storages and objects, as in each area file's header |

Host-only faults (beyond 11.2's): `fail <call> <win32>`, `short <call>
<bytes>`, `stall <in|out>`, `respond-now <code>` (a response in place of
the data phase), `status <bytes>` (a Get Device Status reply), `reset-ok
0` (Device Reset fails), `claim busy=<n>` (170 n times).

### The default device

`dev d0` with no settings is: `mps=512 form=combined zlp=1 info=52
major=1 minor=1 gen=7 flags=1 gate=0 held=0 vetoes=0 forced=0
strings=SIMULATED`, endpoints 81h in, 02h out, 83h interrupt, operations
every one of 7.9's first-cut table with the five Android ones, one
storage and this tree:

| Handle | Object | Parent | Size |
|---|---|---|---|
| storage `00010001h` | "Internal shared storage", read-write, 8 GiB, 4 GiB free | - | - |
| 1 | folder `DCIM` | root | 0 |
| 2 | `a.jpg` | 1 | 1,000 |
| 3 | `b.jpg` | 1 | 70,000 |
| 4 | `big.bin` | root | 5,242,880 |
| 5 | `empty.txt` | root | 0 |
| 6 | folder `Music` | root | 0 |
| 7 | `song.mp3` | 6 | 3,000,000 |
| 8 | folder `sub` | 6 | 0 |
| 9 | `x.txt` | 8 | 10 |

Every listing `simdev.c` gives (GetObjectHandles, GetObjectPropList,
GetStorageIDs) is in ascending handle order unless a case's `sim` says
otherwise.

A case with a `power` step starts P before anything else, and the
implicit `dev d0; open d0` of an area's header comes after the case's
`power` and `dev` steps and opens with P; a case with no `power` step
runs with a default `power P` started first, so every open device is
attached to a registry.

`open d0` on it sends GetDeviceInfo (tid 0) and OpenSession (tid 0), so
the first transaction of a job carries tid 1. "Opened" in a case means
`open d0` returned `MTPE_OK`. A `copy` job is `GetObject h=2 sink=s;
GetObject h=3 sink=s; post` (two files of one selection, one job).

### Hook points

A hook point is **scheduling instrumentation**, not a callback: a call
`MTP_TESTPOINT(name)` compiled into the host test's build of the library
only (the shipping build has none), which hands control to the harness
and calls no program code. It is not one of `MtpInit`'s trace and
allocation hooks, which are callbacks and are never called under a
library lock (I2). A point may therefore sit inside a hold: the table
says, for each, what the thread holds there and on which side of its
named change it stands. `#k` counts from 1 (below, "Matching").

| Point | Thread | Holds | Where, exactly |
|---|---|---|---|
| `io.dequeue.pre` | I/O | nothing | before the dequeue's hold |
| `io.dequeue.in` | I/O | the section | inside the dequeue's hold, **after** its decision: the head job taken off the queue, its session tag and cancel flag read, `Busy` set (or, with HELD or an empty queue, the decision to wait made); before the hold is left |
| `io.op.begin` | I/O | nothing | an operation entered, before its admission hold (session tag, cancel flag, edit state) |
| `io.submit.pre.in` | I/O | the section | the handshake's step 1, **after** its checks passed and *in call* was set |
| `io.call` | I/O | nothing | immediately before the driver call |
| `io.ret` | I/O | nothing | immediately after the driver call returned, before step 2's hold |
| `io.submit.post.in` | I/O | the section | step 2, **after** the result was published and *in call* cleared |
| `io.phase.<P>` | I/O | nothing | right after the hold in which the phase became P |
| `io.publish.in` | I/O | the section | **after** `run`'s end was published: `Busy` cleared, the job retired as current |
| `io.recover.<n>`, `io.recover.6.<m>` | I/O | nothing | before 7.5 step n (step 6's part m) begins: before its first call, or, for step 5, before its hold; reached once per step, not per repeated query |
| `io.getinfo` | I/O | nothing | before a `GET_INFO` |
| `io.close.<n>` | I/O | nothing | before close step n |
| `io.cleanup.quiet` | I/O | nothing | before each quiet check of the GIVENUP cleanup |
| `cx.abort` | cancel | nothing | **after** the hold that checked the cancel work's identity, before the `ABORT` |
| `cx.publish.in` | cancel | the section | **after** the outcome (mask, *in call*) was published |
| `cx.deadline` | cancel | nothing | a passed deadline found, **before** the hold that acts on it |
| `ev.wait` | event | nothing | before each `WAIT_EVENT` |
| `ev.got` | event | nothing | after a `WAIT_EVENT` returned **a container**, before its checks; a `WAIT_EVENT` that ends by its time-out or fails does not reach it |
| `ev.route.in` | event | the section | **after** the event was applied to the engine, its refresh job queued, or (that job's allocation failing) a full refresh asked: reached for every routed event |
| `pw.mark.in` | power | the registry lock | **after** the query was marked and "no hold" reset, before the snapshot |
| `pw.snap.<d>` | power | the registry lock | **after** d's snapshot reference was added and d's section released |
| `pw.ask.<d>` | power | nothing | before `MtpPower` on d |
| `pw.outcome.in` | power | the registry lock | **after** the outcome was recorded and the mark cleared |
| `pw.unsnap.<d>` | power | nothing | before d's snapshot reference is dropped |
| `pw.end.in` | power | the registry lock | the ending routine, **after** the state was set to none, before the first device |
| `pw.end.dev.<d>.in` | power | the registry lock and d's section | **after** d's old session was invalidated (on a resume) and HELD cleared and d's queue event set |
| `pw.end.signal.in` | power | the registry lock | **after** "no hold" was set |
| `pw.suspend.in` | power | the registry lock | the unsolicited suspend, **after** SUSPENDING was set and "no hold" reset, before the first device |
| `pw.suspend.dev.<d>.in` | power | the registry lock and d's section | **after** d's HELD was set |
| `open.alloc` | open's caller | nothing | after the device was allocated, before registration |
| `open.register.in` | open's caller | the registry lock and the new section | **after** the device was linked and the power state read |
| `open.held.wait` | open's caller | nothing | before each wait on "no hold" |
| `open.step.<s>` | open's caller or I/O | nothing | before step s: `handle info1 claim info2 ctl bindctl evt bindevt timeouts threads devinfo session recovery evstart` |
| `open.undo.<s>` | open's caller | nothing | before step s is undone |
| `open.detach.in` | open's caller | the registry lock and the section | **after** DETACHED was set and the device unlinked |
| `close.req.in` | the caller of `MtpClose` | the section | **after** CLOSING was set, the cancel latched, the deadline armed and the close queued; before the owning reference is dropped |
| `entry.ref` | any caller of a device call | nothing | **after** the entry hold (the entry reference taken and the state read), before the call's next hold; `hold-inside <call>` stops a test thread here |
| `reg.detach.in` | I/O | the registry lock and the section | **after** DETACHED (and CLOSED) was set and the device unlinked |
| `ref.zero.in` | any | the section | **after** a release brought the count to 0 |
| `ref.free` | any | nothing | before the free, both locks released |
| `names.admit.in` | name thread | the registry lock | **after** admission was decided (and the count raised) |
| `names.call` | name thread | nothing | before `GET_STRING` |
| `names.return` | name thread | nothing | after `GET_STRING` returned, before the count is lowered |
| `scan.read` | scan thread | nothing | before each `GET_INFO` of a scan |

### The execution contract

Every case runs the same way every time; nothing depends on the host's
scheduler.

- **X1. One thread at a time.** The library's host build routes its
  locks, events, waits, sleeps, thread creation and `GetTickCount`
  through an internal table (`MTPSYS`, as `MTPIO_OPS` routes the
  driver), which the host test implements over its scheduler and the
  shipping build maps straight to Win32. Every thread of a case (the
  control thread, the directive thread, test threads, and every library
  thread) is a real thread, but only the one holding the baton runs. The
  baton passes only when the running thread **blocks**: a lock held by
  another thread, a wait whose object is not signalled, a pending
  driver call (`simdev.c` completes a call at once unless it pends: a
  `WAIT_EVENT` with no event, a `stuck` call, a `delay`, a `rate`, a
  `gateclose` ahead), a stop at a hook point, or the thread's end. A
  hook point with no directive on it is not a switch.
- **X2. Who runs next.** When the running thread blocks, the baton goes
  to the first runnable thread in this fixed rank: the directive
  thread, the control thread, test threads in creation order, then per
  device in `dev` order its I/O, cancel and event threads, then the
  power thread, the scan thread, name threads and open callers in
  creation order. A thread made runnable (an event set, a call
  completed) does not preempt the running one, with one exception,
  **the lock handoff**: a thread that releases a lock on which others
  are blocked hands the lock and the baton at once to the first of them
  in rank. So an `async` step blocked on a stopped thread's lock (X6)
  runs as soon as that hold is left, before the released thread goes
  on. Note that `simdev.c` completes calls at once, so a library thread
  runs a transfer to its end unless a call pends or a directive stops
  it: a case that needs another thread to act in between says where,
  by a directive.
- **X3. The fake clock.** Time moves only by `clock` steps and by the
  idle advance. `clock +ms` advances in jumps to each timed expiry
  inside the interval (a timed wait, a `simdev.c` delay or rate
  completion, a scheduled gate close), in time order and, for equal
  times, in rank order; at each jump every thread made runnable runs
  until all block again before the next jump. So a 500 ms timer fires
  ten times in `clock +5000`. **The idle advance:** when every thread is
  blocked and the control thread is in a `wait`, the clock jumps to the
  earliest timed expiry, as above. `clock =ms` sets an absolute time by
  the same jumps. A deadline of D ms armed at t has passed at the first
  check with `now - t >= D`; the observer's checks come every 500 ms
  from when it began watching, so a deadline is acted on at most 500 ms
  late, and a case that names a time names the window (`GIVENUP within
  [t+20000, t+20500]`).
- **X4. Directives are installed before execution.** Every `at`, `hold`
  and `sweep` of a case is installed when the case starts, before its
  first step, wherever it is written; its position in the script only
  documents. A directive therefore catches a point reached before the
  step it is written after.
- **X5. Matching.** A directive names a point, an occurrence and a
  device: `<point>[#k] [dN]` (default the case's only device, else d0).
  Occurrences are counted per point name and device from after the
  first `open` of that device returned, in the deterministic order of
  X1; `open.*` points count from the case's start. `#k` with a
  qualifier (`#last-data`, `#zlp`, `#zlpwrite`, `#<op>.cmd`,
  `#<op>.data.<n>`, `#<op>.<n>`, `#endedit`, `#jN.n`) counts only the
  occurrences that qualify. A directive fires once, at its first match;
  `#each` fires at every match. A directive that never fires is a
  script error at teardown, unless it is written with `?` (`at? ...`),
  which a sweep row or a table row that does not reach it uses.
- **X6. A stopped thread.** At a directive's point the reaching thread
  stops, keeping whatever the table says it holds. An `at` step runs
  on the directive thread, which outranks every other (X2); the stopped
  thread is let go when the step ends. A step that needs a lock the
  stopped thread holds (by the step table below) is a **script error**
  unless it is written `async`; an `async` step starts on a test thread
  of its own, blocks on that lock, and runs once the stopped thread has
  left its hold, the stopped thread being let go at once. Steps that
  take no library lock and may run synchronously anywhere: `sim`,
  `clock`, `check` and every observation (read through the harness:
  at a point inside a hold, a field under that lock is read without
  taking it, its holder being stopped), `go`, `wait <hook>`. Steps that
  take a device's section: `cancel`, `queue`, `close`, `retain`,
  `release`, `poke` and a library call. Steps that take the registry
  lock, then sections: `bcast`, `button`, `open`, `names`. A `sim` step
  takes no library lock, but what it causes may need one (an injected
  event is routed by the event thread under the section): an `at` step
  never `wait`s for such an effect while its thread is stopped inside
  that lock; the wait is written after the directive, on the control
  thread. A `hold` keeps its thread stopped until its `go`; a `go` for
  a point not yet reached waits for it (as `wait <hook>`) and then lets
  it go.
- **X7. Asynchronous broadcasts and actions.** `bcast ... async`,
  `button` (a posted message), `open d0 async`, `exit async` and
  `async <step>` never block the control thread; their outcomes are
  read by `wait answer`, `wait open d0 returned`, `wait step <label>`
  (an `async` step may carry `-> <label>`). A synchronous `bcast` is
  allowed only when nothing the answer needs is held by a directive.
- **X8. Deadlock and the 600 s limit.** When every thread is blocked,
  no timed expiry is pending and the control thread is in a `wait`, the
  case fails at once: as a **script error** if a `hold` stands or an
  `at` step is blocked (the script made it), else as a **library
  failure** (a deadlock in the library). A `wait` still unmet after 600
  s of fake clock fails the case as a library failure.
- **X9. Sweeps.** `sweep A .. B : step` first runs the case without the
  step, recording every point occurrence (point, thread, device, k) from
  the first occurrence of A to the first of B after it, inclusive, in
  X1's order. It then runs the case once per recorded occurrence,
  stopping that occurrence and doing the step there (`async` implied
  where X6 requires it). By X1 every run is identical up to that point.
  A sweep may name `A#k`. Each run has its own teardown and invariants.
  **The end:** B must be reached by the recording run; a sweep whose
  recording run ends (or reaches teardown) without B is a script error,
  never a shorter interval taken silently. A case whose table rows end
  on different paths (a recovery that fails part-way, say) names a B for
  each row, reachable in that row. **Add or replace:** the step is added
  at the point, and the script's own steps run as written, unless the
  sweep says `: <step> instead of {L}`: then the script step labelled
  `{L}` (a label at the start of a step, `{L} release d0`) runs in the
  recording run only and is skipped in every run of the sweep, the
  injected step taking its place. A step that would act twice when
  added (a release of one reference, say) is always written with
  `instead of`.
- **X10. Teardown.** After the last step, in this order: (1) a `hold`
  never let go, or a directive never fired (X5), is a script error;
  (2) the observables of the `.expect` are taken: `wire`, `ctl`,
  `notice`, `run`, `drop`, `state` and the rest describe the case at
  the end of its steps, not teardown; (3) every device the program
  still owns is closed (`MtpClose`), which drops its queued jobs with
  `MTPE_CLOSING` even while held (7.9, "Close": the close is taken
  whether or not the device is held), so I4 holds over the whole case;
  then every reference a script took and did not give back (a `retain`
  without its `release`) is released by the harness, and every block a
  library call returned to a script and not yet given back (a listing,
  a DeviceInfo copy) is freed through the library's own free call: a
  script error in an ordinary run, the harness's ownership cleanup in an
  allocation run (X11); (4) the idle advance runs, with nothing let go,
  until nothing more happens without a pending simulated call returning
  or a simulated condition being reset (at most 600 s of fake clock: so
  every deadline of the library within that time passes here, an open
  waiting on a hold ends by its own 40 s deadline, a close's by its
  30 s, a `delay` of up to 600 s completes); (5) the retained inventory
  is checked (I9, first part) against **the retention rule R**, which
  derives it from where the run got to at this step: live are exactly
  (a) the inventory `dN` of each device that still has a library call
  pending in `simdev.c` (held by `stuck`, by a hang fault such as
  `ep0_hang` or `out_hang`, or by a `delay` longer than step (4)
  reached), or that is GIVENUP and still in its quiescence loop because
  a persistent simulated condition (`abort-mask=<m> always`,
  `events-stop-out=0 always`, `quiet=0`) keeps its quiet checks
  failing; (b) `names(n)` for each name read whose `GET_STRING` is still
  pending; (c) `registry` when `MtpPowerStop` has already run (a case's
  `exit`) and (a) or (b) is not empty; and nothing else (the case's own
  `MTP_POWER`, freed at (7), is not counted here). A case's `alloc =
  retained ...` line states what R gives for its ordinary run at this
  step and must equal it; (6) every simulated call held pending is let
  return (`sim unstick`: 995 for a pending read or write, the hang
  faults ended the same way), every persistent non-quiet condition is
  reset to quiet (`abort-mask=0`, `events-stop-out=1`, `quiet=1`, a
  draining `held` mask cleared), every gate reopened, every hold of the
  power thread ended by `PBT_APMRESUMEAUTOMATIC`, and the idle advance
  runs until every device is CLOSED and its references released, or
  600 s; (7)
  `MtpPowerStop`; (8) the balance (I9, second part). A job whose `run`
  is still in a library call at (3) is finished by (3)'s latched cancel
  and (6); its `run` must still return. Teardown's own transactions and
  drops are not compared with the `.expect` (they are under I4 and U1 to
  U4); an `.expect` line prefixed `after teardown:` is checked after (8).
- **X11. The allocation sweep** (7.9, "Allocation": every n of every
  case). Every case is run again once per n, for n = 1, 2, ... until a
  run makes fewer than n allocations: the n-th call of the allocation
  hook (counted from the case's start over every thread in X1's order)
  returns NULL. In such a run:
  - **Sweeps.** A sweep case's allocation runs use its ordinary
    recording (X9), never a new one: for each recorded injection point
    and each n there is one run, and the plain recording run (no
    injection) is swept by n too. The point is matched by its identity
    (point, thread, device, k). The injection is prospective: it fires
    when the run reaches the chosen point, as in any other run; if the
    run never reaches it (the failed allocation came first: an open that
    fails before LIF-026's answer, say), it is retired with the other
    unreached directives (below), no script error. A run that reached
    the point and fired, and only then failed an allocation that keeps
    it from reaching B (PWR-004 injected at `open.register.in`, then an
    allocation of the open failing before `open.step.session`), is valid
    as it is: nothing of the injection is undone, and B unreached is not
    an error in an allocation run.
  - **What fails.** The call whose allocation failed returns
    `MTPE_NOMEM`; an allocation not inside a call the program made takes
    the action 7.9 gives it (an event dropped and a full refresh asked;
    an open's undo; a thread's work ended with the device given up).
  - **One stopping rule, for the control script and for job bodies
    alike.** A script (the control thread's steps, a test thread's, or a
    job body run by `run`) stops at the first of its own calls that
    returns `MTPE_NOMEM`: its later steps are not done. A job body that
    stops still does its `post` and returns from `run`, as a program's
    `run` ends on an error; the control script goes on, since the job's
    failure is not one of its own calls. The control script also stops
    at a `wait` that X8 would fail, at a `check` that does not hold, or
    at a step whose directive can no longer be reached, because of the
    failed allocation: in an allocation run none of these fails the case
    by itself.
  - **Entering teardown.** When the control script stops (or ends), X10
    runs with these changes: at (1), every directive not yet fired is
    retired, not a script error; every thread stopped at a hook is let
    go, in rank order, its `at` step not done; every `hold` is let go;
    at (2), no `.expect` line is checked but those prefixed `nomem:`;
    at (3), the harness's ownership cleanup gives back what the stopped
    script still held. **Pending `async` steps and test threads** are
    not required to finish early: they run on during (3) and (4) as far
    as they can without a pending call returning, and whatever is still
    out at (5) is counted by R (a name read pending, say; an open that
    waited on a hold has ended by its deadline in step (4)); after (6)'s
    releases they run to their end, and each is joined
    before (7). At (5), the retained inventory is R's for this run, as
    far as it got (LIF-021 with its n-th allocation in d2's open: d0 and
    d1, already stuck, are retained, d2 is not); (6) to (8) as X10, the
    balance always required.
  - **What must hold.** I1 to I10 and U1 to U4 throughout, every
    accepted job ended by `run` or `drop` (I4, teardown included), no
    fault, and the balance. DAT-011 is one case written to name the
    outcomes this rule gives for a scripted session.

### Further steps, settings and qualifiers

Used by the area files; each means what it says here.

- Control: `table <cols> ... end` runs the case once per row, `<col>`
  standing for the row's value (`every fixture of ...` lists rows by
  reference); `if <cond> : <step>`; `for <range> : <step>`; `par { thread
  t1: ...; thread t2: ... }` runs steps on two test threads at once;
  `check <condition>` fails the case at once when false; `go <hook>` lets a
  `hold` go; `wait job jN done`, `wait idle d0` (no job queued or running,
  no cancel work out), `wait state d0 S`, `wait notice d0 N`, `wait
  <hook>[#k]` (until that occurrence, counted as X5, has been reached;
  at once if it was; the reaching thread is not stopped), `wait answer`,
  `wait open d0 returned`, `wait scan done`, `wait step <label>` (an
  `async` step done), `wait event-callback d0 done[#k]` (the event
  callback's job, the k-th, has returned).
- Selectors of a driver call in `sim` settings: `<call>#k` is the k-th
  such call of the device counted as X5 (`read#3`, `write#1`), or a
  qualifier of X5 (`write#sendobject.data.2`, `read#zlp`); `<call>` alone
  is every such call.
- Calls: `call <Op> d0 ... from=test` (a library call from a test thread);
  `open d0 async` (from another test thread, not waited for); `open d0
  host.event={...} host.notice={...}` (the callbacks of `MTP_HOST`);
  `open d0 testflag=1` (7.9's explicit test flag); `init hooks.trace={...}
  hooks.alloc={...}` (`MtpInit`'s hooks); `queue d0 jN run=NULL`; `queue d0
  jN user {...}` (a user command, for its message); `queue ... drop={...}`
  (what `drop` does); `allocfail next` (the allocation hook's next call
  returns NULL); `free-ctx` (`run` frees its context as its last
  act); `rec ev` (the callback records its target); `enum -> g` (`MtpEnum`'s
  generation); `bring d0 to <state>` and `finish d0` (drive d0 into a state
  by the steps of `life.cas`, then release and clean up); `free-host d0`
  (the test frees d0's `MTP_HOST` after CLOSED); `hold-inside <call>` /
  `release-thread t1` (a test thread held inside a library call);
  `poke d0 next-tid=<n>` (the harness sets a field under the section);
  `program cli [/powerwait <s>]` (the process has no window thread, as
  `MTPEXP /c`); `cli <command>`; `system 4.10|4.90` (what `GetVersionEx`
  reports); `block window <ms>` (the window thread held by a test hook);
  `signal d0 queue-event` (a spurious wake of the dequeue); `exit async`.
- Callbacks: `sink=s(after=<bytes> cancel=0)`, `sink=s(call=<Op> -> r)`,
  `sink=crc` (a CRC-32 of what arrives), `src=src(short=<n>|fail-after=<n>|
  fails-at-open|chunks=<sizes>|call=<Op> -> r)`, `src=formula`.
- simdev, further: `obj <n>` / `obj huge <n>` / `store+=h=<h>:"<name>":
  parent=<p>:size=<n>[:formula]` (objects added), `storages=2`, `ops=+X` /
  `ops=-X`, `ep.in=` / `ep.out=`, `speed=full|high|super`, `unclaimed` (a
  device the program cannot claim: scan and names only), `rate=<bytes per
  s of fake clock>`, `abort-mask=<m> [always]`, `events-stop-out=<0|1>
  [always]`, `clock-per-status=<ms>`, `respond <Op> [h=<h>] <code>
  [params=<n>|param=<v>|data=<n>|length=<n> sends=<n>|sentinel sends=<n>]`
  (the device's answer to that operation), `respond-now <Op> <code>`,
  `corrupt <Op> <what>`, `delay <Op>|<call>#<sel> <ms>` (that operation's
  first call, or that call, pends <ms> of fake clock before it completes;
  a gate closing meanwhile ends it 995; `CLASS.<request>#k` selects the
  k-th class request of that kind), `end <call> code=<win32>
  [gate=<g>]` (that pending call ends now with that code, the gate
  showing g), `fail next <win32>` (the next read or write fails so),
  `after-5 <how>` (what `GET_INFO` on a
  new info handle shows after a 5), `remove [pending-returns=later]`,
  `unplug`, `replug [gen=<n>] [same-gen]`, `readback <field>=<v>`,
  `proplist <fault>`, `storageids <ids>`, `delete non-empty-folder=<code>`,
  `responder=android-header-at-0` (puts data that arrives with a header
  packet at offset 0, the fault libmtp describes), `queue-response tid=..
  count=<n>`, `event <name> [params] [tid=]`, `event-raw <hex>`,
  `event-wait blocks`, `gateclose ... reopen` / `gate=0 reopen` (the gate
  opens again), `lapse QUERY_HOLD_MS=<ms>`, `query-accepted`, `quiet=0|1`,
  `claim busy=<n>`, `status <hex> ; then <hex>` (Get Device Status replies
  in turn, the last repeated).
- Hook qualifiers (counting: X5 below); `#last-data` (the read or write that completes the data
  phase), `#zlp` (the read after a whole-packet data phase), `#zlpwrite`
  (`WRITE_ZLP`), `#<op>.cmd`, `#<op>.data.<n>`, `#<op>.<n>` (the n-th call
  of that operation's transaction), `#endedit`, `#j2.1` (job j2's first),
  `queue.return` (the test's `MtpQueue` returned), `gateclose` (simdev's
  gate closed).
- Pure-function steps (`names`, `dates`, `copy`): `map`, `map-copy`,
  `show`, `todevice`, `readdate`, `setdate`, `todevice-date`, `copy ... to
  ... [replace]`, `folder ... holds ...`; `data.cas` names its own.

Shorthands for control-handle sequences, used in every `.expect`:
`SETTLE` = `ABORT in|out -> 0`; `HC(t)` = `SETTLE, CANCEL tid=t, STATUS ->
0400 0120` (7.5 steps 2 to 5); `RESET6` = `EVENTS stop -> 1, SETTLE,
RESET, RESET_PIPE in, RESET_PIPE out, RESET_PIPE int, STATUS -> 0400
0120` (7.5 step 6.1 to 6.6; step 6.7 was withdrawn, CQ24); `EVSTART` =
`EVENTS start`, which comes once, after the new session's OpenSession
succeeded (7.3), so a recovered device's `ctl` reads `[..., RESET6,
EVSTART]` with the OpenSession between them on the wire; `NOCANCEL` = no
`CANCEL`, `RESET` or `RESET_PIPE`. `bytes=` counts the object's payload
only (7.9, "The bytes moved"; 0 for an operation that moves no object).

## Expected results (`cases/*.expect`)

```
<ID>
  <observable> <expectation>
  ...
end
```

| Observable | Meaning |
|---|---|
| `ret r = CODE [flags=F|G] [resp=XXXXh] [reason=MTPR_X] [win32=n] [bytes=n]` | the call's code and its `MTP_RESULT`; flags not named are clear (`flags=none` says so); `reason=` the named reason of 7.9's table (every `MTPE_PROTOCOL` and `MTPE_ARG` names one; `flags include F` leaves the others unchecked) |
| `run jN = k` / `drop jN = CODE` / `drop jN = none` | callback counts and the code `drop` got; `on io` / `on cx` names the thread |
| `order: a < b < c` | the order in which those things happened |
| `wire d0 = [ ... ]` | the exact sequence of containers d0 received (operation names, `tid=`); `wire d0 !has X`; `wire d0 after <mark> = []` |
| `ctl d0 = [ ... ]` | the control handle's calls in order (`ABORT`, `EVENTS stop`, `CANCEL tid`, `STATUS`, `RESET_PIPE in|out|int`, `RESET`) |
| `notice d0 = [ ... ]` | the notices posted, in order, each once |
| `answer = TRUE|DENY` / `pwr d0 = BUSY|AGREED` | a query's answer, and a device's `MtpPower` |
| `state d0 = S` / `held d0 = 0|1` / `busy d0 = 0|1` / `session d0 = n` | device fields read under its section by the harness |
| `alloc = balanced` / `alloc = retained <inventory>` | I9: at teardown's step (5), what the retention rule R (X10) gives: nothing, or exactly the inventory named (`d0` = d0's `MTP_DEV` with its buffers, handles' contexts and events; `names(n)` = a name read's context; `registry`) |
| `msg = <id>` | the program's message (13), by the id given in `power.cas`'s header; texts stay drafts |
| `never <event>` | across every point of the case (and of a sweep) |
| `trace has <tag>` | the trace hook saw it |

A `sweep` case's expectations hold at every point unless prefixed
`at <hook>:`, which names the points where a line differs.

## Invariants of every case

Checked in every case, every sweep point and every run of the
allocation sweep (X11), teardown included, not repeated in the `.expect`
files. `MTPSYS` (X1) tells the harness which locks each thread holds.

- I1. No driver call is entered with a device's section or the registry
  lock held: the fake transport fails the case (7.9, "Which thread
  answers").
- I2. No program callback (`run`, `drop`, the event callback, a sink,
  source or progress callback, the notice callback) and no public hook
  (`MtpInit`'s trace and allocation hooks) is called with a library lock
  held; nor is any wait, sleep, free, thread creation or join made with
  one held. Hook points (scheduling instrumentation, above) are not
  callbacks and are exempt.
- I3. The lock order: the registry lock is never taken by a thread
  holding a section; a thread holding the registry lock holds at most one
  section at a time; every reference count changes under its device's
  section (7.9, "The lock order").
- I4. For every `MtpQueue` that returned `MTPE_OK`, exactly one of `run`
  and `drop` is called, once, over the whole case, teardown included;
  after it returns the library makes no access to `ctx` (the harness
  unmaps the job's `ctx` block when `run` or `drop` returns). The
  library's own bookkeeping of the job and the release of its reference
  follow the return and are allowed. For any other return, neither is
  called and `ctx` is never touched.
- I5. No access to a freed block: the allocation hook fills freed blocks
  and keeps them unmapped until the case ends.
- I6. Ownership of memory: each call's own temporary allocations are
  freed by its return; a block a call returns to its caller is the
  caller's until given back by the library's own free call. Frees are
  instrumented like allocations: a free of a block the hook did not
  allocate, or freed already, fails the case.
- I7. The handles: a second call on a handle with one pending fails the
  case (7.9, "The three handles"); reads, writes, `WRITE_ZLP` and
  `SET_TIMEOUTS` are made only on the owner handle by the I/O thread
  (or by the open's caller before the threads start); `WAIT_EVENT` only
  on the event handle by the event thread; the control handle by the
  cancel thread only while cancel work it owns is outstanding, and by
  the I/O thread only after that work's outcome was published or the
  cancel thread joined.
- I8. After GIVENUP no new call is submitted on any handle of the
  device but the cleanup's quiet checks (`EVENTS` stop and `ABORT` on
  the control handle); calls already submitted may complete.
- I9. The inventory, in two parts (X10): at teardown's step (5), every
  block the allocation hook handed out is freed, except exactly the
  retained inventory X10's retention rule R gives for the run (which a
  case's `alloc = retained ...` line states, and `alloc = balanced`
  states as empty), block by block, not by count; then, at step (8),
  after every retained call has returned and its cleanup run, the
  balance is exact. A `check alloc retained <inv>` step checks the
  first part at that point of the script.
- I10. Within a session, transaction ids rise by one, skip FFFFFFFFh,
  and wrap from FFFFFFFEh to 1 (7.3). Exceptions: transactions outside a
  session carry 0 (GetDeviceInfo and OpenSession at the open, and the
  resume job's OpenSession); the stale response read of 7.5 step 5
  carries the cancelled id; `poke d0 next-tid=` sets the next id.

## Universal assertions

Checked like the invariants, in every case and every run:

- U1. **Hold admission.** While a device is HELD (from the hold that
  set HELD to the one that cleared it), nothing new is admitted: no
  job's `run` starts, no resume job transaction, no open's first driver
  call, no name read, no CloseSession, no refresh job, and no
  transaction of any internal job. The only transactions admitted are
  those of a job whose `run` began before HELD was set (an unsolicited
  suspend's running job) and that job's recovery.
- U2. **Session-tagged admission.** No job whose session tag differs
  from the device's session count runs or submits: it is dropped with
  `MTPE_SESSION` (or a code earlier in the order); an operation of a
  running job whose tag differs returns `MTPE_SESSION` with nothing
  sent (7.3, CQ4).
- U3. **Cancel-work identity.** Every `ABORT` of the cancel thread is
  made for the (job id, transaction id) current in the hold that
  checked it; no `ABORT`, deadline action or recovery call of job N
  happens after job N+1 was dequeued; no job is dequeued while cancel
  work is outstanding.
- U4. **No automatic replay.** A mutating command container
  (SendObjectInfo, SendObject, DeleteObject, SetObjectPropValue, 95C2h
  to 95C5h) whose outcome is uncertain (it may have reached the device
  and no complete response was read) is never sent again by the
  library, in that call or any later one. Otherwise each library call
  `run` made sends exactly the mutating transactions its operation's
  specification names, each once: `MtpSendObject` its SendObjectInfo
  and then its SendObject (the reserved pair), `MtpNewFolder` its
  SendObjectInfo, an Android call its one container, `MtpDelete` its
  DeleteObject; and beyond those only: the named cleanups (an edit's
  cleanup EndEditObject; an upload's cleanup DeleteObject of its
  reserved handle, in Q24's states (i) and (iii)), and a folder's
  deletion's own steps (7.4; 7.9, "Deleting a folder"): after a
  folder's DeleteObject was settled by a complete PartialDeletion
  (2012h) response, its children and then the folder once more, each
  child folder the same way. So in a deletion one handle gets at most
  two DeleteObject, the second only after a settled 2012h for the first.
