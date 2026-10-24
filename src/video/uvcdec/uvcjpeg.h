/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
/*
 * uvcjpeg: the MJPEG frame decoder behind uvcdec.dll, on libjpeg-turbo 3.2.0
 * with its x86 SIMD (MMX, SSE2, AVX2) chosen at run time and plain C where
 * the processor or the system offers none.
 * No Windows headers: the ICM layer (uvcdec.c) and the host test both use it.
 *
 * A UVC MJPEG frame is one baseline JPEG image that may carry no DHT segment
 * (UVC 1.1 MJPEG payload, section 3.3): libjpeg-turbo then uses the standard
 * tables of ISO 10918-1 Annex K.3 itself (jdhuff.c, std_huff_tables from
 * jstdhuff.c), so no table is inserted here.
 */
#ifndef UVCJPEG_H
#define UVCJPEG_H

/* Output formats */
#define UVCJ_RGB24	1	/* B,G,R bytes */
#define UVCJ_RGB32	2	/* B,G,R,0 bytes */
#define UVCJ_YUY2	3	/* Y0 U Y1 V, BT.601 studio range (16-235/240) */

/* Results */
#define UVCJ_OK		0
#define UVCJ_EBAD	1	/* not a decodable JPEG frame */
#define UVCJ_ESIZE	2	/* frame size is not the size asked for */
#define UVCJ_EFORMAT	3	/* format or layout this decoder does not offer */
#define UVCJ_EMEM	4

/* Speed choices (uvcjSetMode) */
#define UVCJ_MODE_FAST	0	/* IFAST IDCT, merged (non-fancy) upsampling */
#define UVCJ_MODE_EXACT	1	/* ISLOW IDCT, fancy upsampling (libjpeg default) */

/* SIMD instruction sets (libjpeg-turbo's JSIMD_* values) */
#define UVCJ_SIMD_MMX	0x01
#define UVCJ_SIMD_SSE	0x04	/* used by libjpeg-turbo only for float DCTs */
#define UVCJ_SIMD_SSE2	0x08
#define UVCJ_SIMD_AVX2	0x80
#define UVCJ_SIMD_ALL	0xFFu

typedef struct UVCJ UVCJ;

typedef struct UVCJ_STATS {
	unsigned long frames;	/* decoded, including those with warnings */
	unsigned long errors;	/* refused or abandoned */
	unsigned long warnings;	/* libjpeg warnings (corrupt data, early end) */
	unsigned long rawFrames;/* YUY2 frames taken through raw (downsampled) output */
} UVCJ_STATS;

UVCJ *uvcjOpen(void);
void uvcjClose(UVCJ *j);
void uvcjSetMode(UVCJ *j, int mode);
/* scale: 1, 2, 4 or 8 for 1/scale output (libjpeg IDCT scaling) */
int uvcjSetScale(UVCJ *j, int scale);
void uvcjGetStats(const UVCJ *j, UVCJ_STATS *s);

/*
 * The SIMD sets this processor and system can run: CPUID's, less SSE and
 * SSE2 when the system has not enabled them (a probe instruction faults,
 * as on Windows 95), less AVX2 unless the system saves YMM state. 0 means
 * plain C. uvcjOpen uses all of them.
 */
unsigned int uvcjSimdSupport(void);
/* Limits the decoder to the sets in mask (0: plain C); returns those used */
unsigned int uvcjSetSimd(UVCJ *j, unsigned int mask);

/* Reads the frame's headers only: image width and height (before scaling)
 * and the number of components. */
int uvcjHeader(UVCJ *j, const unsigned char *data, unsigned long size,
	unsigned int *width, unsigned int *height, int *components);

/*
 * Decodes one frame into dst, whose first byte is the top-left pixel; row
 * steps pitch bytes (negative for a bottom-up DIB, with dst at its last
 * row). width and height are the output size, which must equal the frame's
 * size divided by the scale. YUY2 needs an even width.
 */
int uvcjDecode(UVCJ *j, const unsigned char *data, unsigned long size,
	int format, unsigned char *dst, long pitch,
	unsigned int width, unsigned int height);

#endif
