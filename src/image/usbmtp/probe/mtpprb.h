/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mtpprb.h - controls of MTPPRB.SYS, the M0 probe driver (docs/design/
 * mtp.md 5.6), shared with MTPPROBE.EXE. Not shipped.
 *
 * Transfers and resets are refused (ERROR_NOT_READY) until the device has
 * started and once QUERY_STOP, STOP or REMOVE has begun.
 *
 * Copyright (C) 2026 Yeo Kheng Meng
 */
#ifndef MTPPRB_H
#define MTPPRB_H

#define PRB_CODE(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)

/* A WriteFile of 1 to 64 bytes is a bulk OUT transfer on the PRB_INT slot
   (the interrupt read's), stopped as PRB_INT. */

/* in: ULONG which (PRB_BIN, PRB_INT, PRB_CTL, PRB_WAIT): stop that slot's request by
   IoCancelIrp of its lower IRP. out (optional): ULONG, the request stopped,
   0 if the slot had no active request (the stop did nothing). */
#define IOCTL_PRB_CANCEL     PRB_CODE(1)
/* in: ULONG which (PRB_BIN, PRB_INT): the same, by URB_FUNCTION_ABORT_PIPE
   from a worker (the slot's abort IRP). out: as IOCTL_PRB_CANCEL. */
#define IOCTL_PRB_ABORT      PRB_CODE(2)
/* in: ULONG which (PRB_BIN, PRB_INT): URB_FUNCTION_RESET_PIPE on the
   control slot, reserving that pipe's slot; pending until the stack answers,
   cancellable (IOCTL_PRB_CANCEL of PRB_CTL, or the caller's cancel) */
#define IOCTL_PRB_RESET      PRB_CODE(3)
/* in: ULONG index into the driver's status list; completes at once with
   that status and Information = 3 */
#define IOCTL_PRB_STATUS     PRB_CODE(4)
/* out: 4 to 64 bytes: a pending interrupt IN read (completes with the event) */
#define IOCTL_PRB_INT        PRB_CODE(5)
/* out: 4 to 64 bytes: still image Get Device Status (class request 67h),
   pending until the device answers */
#define IOCTL_PRB_DEVSTATUS  PRB_CODE(6)
/* out: PRB_COUNTERS_BYTES: the driver's counters, ten ULONGs: creates,
   cleanups, closes, IRPs seen for a removed (zombie) or deleted device,
   actions dropped, REMOVE received, IoDeleteDevice done, removals leaked,
   zombies (removed devices kept for open handles), IRPs that came on a
   handle of another device. Answered also through a handle kept open
   after REMOVE (the removal milestones). The driver-wide counters count
   every device of the session. */
#define IOCTL_PRB_COUNTERS   PRB_CODE(7)
#define PRB_COUNTERS_BYTES   40
/* in: none; out: none: the design's software wait (WAIT_EVENT), a request
   with no lower IRP, pending until stopped (IOCTL_PRB_CANCEL of PRB_WAIT),
   cancelled or removed */
#define IOCTL_PRB_WAIT       PRB_CODE(9)
/* out: PRB_STATE_ULONGS ULONGs, read under the device's lock: started, then
   for PRB_BIN, PRB_INT, PRB_CTL, PRB_WAIT each: slot state (slot.h SS_*), request
   number, and lower IRP out (bit 0) | cancel routine set (bits 8-15) |
   pins (bits 16-23). A request is pending in the stack when its state is
   SS_ACTIVE (1), bit 0 is set and pins are 0 (its IoCallDriver returned);
   a wait is pending when it is SS_ACTIVE with its cancel routine set. */
#define IOCTL_PRB_STATE      PRB_CODE(8)
#define PRB_STATE_ULONGS 13

#define PRB_BIN 0
#define PRB_INT 1
#define PRB_CTL 2
#define PRB_WAIT 3     /* the software wait (stops only) */

#endif
