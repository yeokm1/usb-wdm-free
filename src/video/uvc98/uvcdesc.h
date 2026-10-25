/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded USB/UVC descriptor model. No allocation or OS/USB I/O.
 */
#ifndef UVCDESC_H
#define UVCDESC_H
#include "uvcctrl.h"
#define UVC_DESC_OK 0
#define UVC_DESC_BAD 1
#define UVC_DESC_LIMIT 2
#define UVC_DESC_UNSUPPORTED 3
#define UVC_DESC_MAX_STREAMS 8
#define UVC_DESC_MAX_FRAMES 128
#define UVC_DESC_MAX_ALTS 128
#define UVC_DESC_YUY2 1
#define UVC_DESC_MJPEG 2
#define UVC_DESC_NV12 3
#define UVC_DESC_I420 4
#define UVC_DESC_M420 5
#define UVC_PLANE_YUYV 1
#define UVC_PLANE_Y 2
#define UVC_PLANE_UV 3
#define UVC_PLANE_U 4
#define UVC_PLANE_V 5
#define UVC_PLANE_Y_EVEN 6
#define UVC_PLANE_Y_ODD 7
typedef struct UVC_DESC_PLANE {
    unsigned int Role, Rows;
    unsigned long Offset, RowBytes, Stride;
} UVC_DESC_PLANE;
typedef struct UVC_DESC_LAYOUT {
    unsigned int Kind, BitsPerPixel, MacroWidth, MacroHeight, PlaneCount;
    unsigned long FrameBytes;
    UVC_DESC_PLANE Plane[3];
} UVC_DESC_LAYOUT;
/* Tight UVC wire layout, not a padded Windows display surface. M420 uses
 * three row sequences (even Y, odd Y, interleaved UV), not three planes.
 * Width/height must fit descriptor WORDs. Failure leaves output unchanged.
 * MJPEG/unknown kinds have no fixed uncompressed layout and are rejected. */
int UvcDescLayout(unsigned int kind, unsigned int width, unsigned int height,
    UVC_DESC_LAYOUT *layout);
/* Recompute validated layout and bound one entire row within caller buffer.
 * No pixel access/conversion. Outputs unchanged on failure. */
int UvcDescRow(unsigned int kind, unsigned int width, unsigned int height,
    unsigned int plane, unsigned int row, unsigned long bufferBytes,
    unsigned long *offset, unsigned long *rowBytes);
#define UVC_DESC_FULL_SPEED 1
#define UVC_DESC_HIGH_SPEED 2
typedef struct UVC_DESC_STREAM {
    unsigned int Interface, Endpoint, Terminal, StillMethod;
} UVC_DESC_STREAM;
typedef struct UVC_DESC_FRAME {
    unsigned int Stream, Format, Frame, Kind, Width, Height;
    unsigned long FrameBytes, DescriptorMaxBytes, DefaultInterval;
    unsigned int IntervalOffset, IntervalCount; /* 0 = continuous triple */
} UVC_DESC_FRAME;
typedef struct UVC_DESC_ALT {
    unsigned int Stream, Alternate, Endpoint, Type, Interval;
    unsigned int PacketBytes, Transactions, Capacity, ServiceMicroframes;
} UVC_DESC_ALT;
typedef struct UVC_DESC {
    const unsigned char *Bytes; /* caller retains immutable configuration */
    unsigned int Length, Valid, Revision, Control, Speed;
    unsigned int StreamCount, FrameCount, AltCount;
    UVC_DESC_STREAM Stream[UVC_DESC_MAX_STREAMS];
    UVC_DESC_FRAME Frame[UVC_DESC_MAX_FRAMES];
    UVC_DESC_ALT Alt[UVC_DESC_MAX_ALTS];
} UVC_DESC;

/* Explicit VC interface anchor; only its baInterfaceNr collection is parsed.
 * Honors configuration and VC/VS class wTotalLength, plus containing IAD
 * when present. Other functions are never selected. Full/high speed only.
 * YUY2, NV12, I420, M420 and MJPEG are exposed. An uncompressed format
 * with another GUID is validated like the exposed ones (frame count,
 * default frame, each frame) and its frames are left out. MPEG-2 TS, DV,
 * frame-based, stream-based, H.264, H.264 simulcast, VP8 and VP8
 * simulcast formats count toward bNumFormats and are skipped with their
 * frames, unvalidated. A descriptor with a reserved format subtype is
 * not counted: in place of an advertised format it leaves bNumFormats
 * unmatched and the stream is refused; beside all advertised formats it
 * is ignored.
 * Failure leaves Valid=0: no other output fields may be consumed.
 * This is not a complete entity-graph/control-bitmap validator.
 */
int UvcDescParse(const unsigned char *bytes, unsigned int length,
    unsigned int controlInterface, unsigned int speed, UVC_DESC *result);
int UvcDescInterval(const UVC_DESC *desc, unsigned int frameIndex,
    unsigned long interval);
/* Validate device-adjusted IN probe result against this stream/frame and
 * choose a sufficient smallest isochronous alternate (or bulk alternate0).
 * No payload clamp, allocation, bandwidth reservation or COMMIT is done.
 * Caller MUST enforce a separate negotiated frame-allocation budget,
 * especially MJPEG; the deprecated descriptor maximum is not a hard clamp.
 * USB-valid endpoint intervals do not imply a Windows transport supports them.
 * On success outputs are indices into Frame[]/Alt[]. */
int UvcDescSelect(const UVC_DESC *desc, unsigned int streamIndex,
    const UVC_CTRL *control, unsigned int *frameIndex, unsigned int *altIndex);
#endif
