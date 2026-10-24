/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
/*
 * uvcjpeg: MJPEG frame decoding on libjpeg-turbo 3.2.0 (turbo/, with the
 * MSVC 6.0 patches README.md lists).
 * This software is based in part on the work of the Independent JPEG Group.
 *
 * One libjpeg decompress object lives as long as the decoder and is reused
 * for every frame. Errors longjmp back here; the object is then aborted and
 * stays usable. YUY2 from a 2h1v or 2h2v YCbCr frame takes libjpeg's raw
 * (downsampled) output and packs it, skipping upsampling and colour
 * conversion; any other layout goes through full-resolution YCbCr.
 *
 * SIMD: libjpeg-turbo picks MMX, SSE2 or AVX2 routines per frame from the
 * decompress object's master->simd_support, which it fills from CPUID
 * (simd/i386/jsimdcpu.asm) the first time it is needed. uvcjOpen fills it
 * first, from uvcjSimdSupport: CPUID's sets less those the system has not
 * enabled. CPUID says what the processor has, not whether Windows saves the
 * XMM registers on a task switch; Windows 95 does not, and an SSE
 * instruction then faults. So SSE and SSE2 are kept only if one runs.
 * With no set left the library's plain C routines decode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define JPEG_INTERNALS		/* struct jpeg_decomp_master, for simd_support */
#include "jpeglib.h"
#include "jerror.h"
#include "uvcjpeg.h"

/* simd/i386/jsimdcpu.asm: CPUID's JSIMD_* sets (AVX2 only with OS XSAVE) */
extern unsigned int jpeg_simd_cpu_support(void);

#define UVCJ_MAXROWS	16	/* rows asked of jpeg_read_scanlines at once */

struct uvcj_err {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
};

struct UVCJ {
	struct jpeg_decompress_struct cinfo;
	struct uvcj_err err;
	int mode;
	int scale;
	unsigned char ylut[256];
	unsigned char clut[256];
	/* scratch, grown on demand */
	unsigned char *row;		/* one full-resolution row, 4 bytes a pixel */
	unsigned long rowSize;
	unsigned char *raw;		/* raw planes for one iMCU row */
	unsigned long rawSize;
	UVCJ_STATS stats;
};

static void uvcjErrorExit(j_common_ptr cinfo)
{
	struct uvcj_err *e = (struct uvcj_err *)cinfo->err;
	longjmp(e->jump, 1);
}

static void uvcjEmit(j_common_ptr cinfo, int level)
{
	if (level < 0)
		cinfo->err->num_warnings++;
}

static void uvcjOutput(j_common_ptr cinfo)
{
	(void)cinfo;
}

#ifdef _MSC_VER
/* xorps xmm0, xmm0 (SSE) and pxor xmm0, xmm0 (SSE2), as bytes: MSVC 6.0's
 * inline assembler knows neither. Each faults (invalid opcode) unless the
 * system has enabled the XMM state (CR4.OSFXSR). */
static int uvcjRunsSse(void)
{
	__try {
		__asm _emit 0x0F __asm _emit 0x57 __asm _emit 0xC0
	} __except (1) {
		return 0;
	}
	return 1;
}

static int uvcjRunsSse2(void)
{
	__try {
		__asm _emit 0x66 __asm _emit 0x0F __asm _emit 0xEF __asm _emit 0xC0
	} __except (1) {
		return 0;
	}
	return 1;
}
#endif

unsigned int uvcjSimdSupport(void)
{
	static int known;
	static unsigned int sets;
	unsigned int s;

	if (known)
		return sets;
	s = jpeg_simd_cpu_support();
#ifdef _MSC_VER
	if ((s & (UVCJ_SIMD_SSE | UVCJ_SIMD_SSE2 | UVCJ_SIMD_AVX2)) && !uvcjRunsSse())
		s &= ~(unsigned int)(UVCJ_SIMD_SSE | UVCJ_SIMD_SSE2 | UVCJ_SIMD_AVX2);
	if ((s & (UVCJ_SIMD_SSE2 | UVCJ_SIMD_AVX2)) && !uvcjRunsSse2())
		s &= ~(unsigned int)(UVCJ_SIMD_SSE2 | UVCJ_SIMD_AVX2);
#endif
	sets = s;
	known = 1;
	return sets;
}

unsigned int uvcjSetSimd(UVCJ *j, unsigned int mask)
{
	unsigned int s = uvcjSimdSupport() & mask;

	j->cinfo.master->simd_support = s;
	return s;
}

UVCJ *uvcjOpen(void)
{
	UVCJ *j;
	int i;

	j = (UVCJ *)calloc(1, sizeof(*j));
	if (j == NULL)
		return NULL;
	j->cinfo.err = jpeg_std_error(&j->err.pub);
	j->err.pub.error_exit = uvcjErrorExit;
	j->err.pub.emit_message = uvcjEmit;
	j->err.pub.output_message = uvcjOutput;
	if (setjmp(j->err.jump)) {
		jpeg_destroy_decompress(&j->cinfo);
		free(j);
		return NULL;
	}
	jpeg_create_decompress(&j->cinfo);
	(void)uvcjSetSimd(j, UVCJ_SIMD_ALL);
	j->mode = UVCJ_MODE_FAST;
	j->scale = 1;
	/* JPEG (JFIF) YCbCr is full range; YUY2 is BT.601 studio range */
	for (i = 0; i < 256; i++) {
		j->ylut[i] = (unsigned char)(16 + (i * 219 + 127) / 255);
		j->clut[i] = (unsigned char)(16 + (i * 224 + 127) / 255);
	}
	return j;
}

void uvcjClose(UVCJ *j)
{
	if (j == NULL)
		return;
	jpeg_destroy_decompress(&j->cinfo);
	free(j->row);
	free(j->raw);
	free(j);
}

void uvcjSetMode(UVCJ *j, int mode)
{
	j->mode = mode == UVCJ_MODE_EXACT ? UVCJ_MODE_EXACT : UVCJ_MODE_FAST;
}

int uvcjSetScale(UVCJ *j, int scale)
{
	if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
		return UVCJ_EFORMAT;
	j->scale = scale;
	return UVCJ_OK;
}

void uvcjGetStats(const UVCJ *j, UVCJ_STATS *s)
{
	*s = j->stats;
}

static int uvcjGrow(unsigned char **buf, unsigned long *have, unsigned long need)
{
	unsigned char *p;

	if (*have >= need)
		return 1;
	p = (unsigned char *)malloc(need);
	if (p == NULL)
		return 0;
	free(*buf);
	*buf = p;
	*have = need;
	return 1;
}

/* Starts the frame: source, headers. Called under the caller's setjmp. */
static void uvcjBegin(UVCJ *j, const unsigned char *data, unsigned long size)
{
	j->err.pub.num_warnings = 0;
	jpeg_mem_src(&j->cinfo, data, (size_t)size);
	(void)jpeg_read_header(&j->cinfo, TRUE);
}

int uvcjHeader(UVCJ *j, const unsigned char *data, unsigned long size,
	unsigned int *width, unsigned int *height, int *components)
{
	if (data == NULL || size < 4)
		return UVCJ_EBAD;
	if (setjmp(j->err.jump)) {
		jpeg_abort_decompress(&j->cinfo);
		return UVCJ_EBAD;
	}
	uvcjBegin(j, data, size);
	*width = j->cinfo.image_width;
	*height = j->cinfo.image_height;
	*components = j->cinfo.num_components;
	jpeg_abort_decompress(&j->cinfo);
	return UVCJ_OK;
}

/* 2h1v or 2h2v YCbCr with luma at the maximum factors: raw output applies */
static int uvcjRawFits(struct jpeg_decompress_struct *c)
{
	jpeg_component_info *y = &c->comp_info[0];
	jpeg_component_info *u = &c->comp_info[1];
	jpeg_component_info *v = &c->comp_info[2];

	if (c->num_components != 3 || c->jpeg_color_space != JCS_YCbCr)
		return 0;
	if (y->h_samp_factor != c->max_h_samp_factor ||
	    y->v_samp_factor != c->max_v_samp_factor)
		return 0;
	if (u->h_samp_factor != v->h_samp_factor ||
	    u->v_samp_factor != v->v_samp_factor)
		return 0;
	if (u->h_samp_factor * 2 != y->h_samp_factor)
		return 0;
	if (u->v_samp_factor != y->v_samp_factor &&
	    u->v_samp_factor * 2 != y->v_samp_factor)
		return 0;
	return 1;
}

static void uvcjRawYuy2(UVCJ *j, unsigned char *dst, long pitch,
	unsigned int width, unsigned int height)
{
	struct jpeg_decompress_struct *c = &j->cinfo;
	JSAMPROW rows[3][4 * DCTSIZE * 2];
	JSAMPARRAY planes[3];
	unsigned long stride[3], nrows[3], need, off;
	unsigned int lines, l, x, y0, vr;
	int ci;

	/* sizes from the component layout after jpeg_start_decompress */
	need = 0;
	for (ci = 0; ci < 3; ci++) {
		jpeg_component_info *cp = &c->comp_info[ci];
		stride[ci] = (unsigned long)cp->width_in_blocks * cp->DCT_scaled_size;
		nrows[ci] = (unsigned long)cp->v_samp_factor * cp->DCT_scaled_size;
		if (nrows[ci] > sizeof(rows[0]) / sizeof(rows[0][0]))
			ERREXIT(c, JERR_BUFFER_SIZE);
		need += stride[ci] * nrows[ci];
	}
	if (!uvcjGrow(&j->raw, &j->rawSize, need))
		ERREXIT1(c, JERR_OUT_OF_MEMORY, 0);
	off = 0;
	for (ci = 0; ci < 3; ci++) {
		unsigned long r;
		for (r = 0; r < nrows[ci]; r++) {
			rows[ci][r] = j->raw + off;
			off += stride[ci];
		}
		planes[ci] = rows[ci];
	}
	vr = (unsigned int)(c->comp_info[0].v_samp_factor /
		c->comp_info[1].v_samp_factor);

	while (c->output_scanline < c->output_height) {
		y0 = c->output_scanline;
		lines = jpeg_read_raw_data(c, planes,
			(JDIMENSION)(c->max_v_samp_factor * c->min_DCT_scaled_size));
		if (lines == 0)
			break;
		for (l = 0; l < lines && y0 + l < height; l++) {
			const unsigned char *yp = rows[0][l];
			const unsigned char *up = rows[1][l / vr];
			const unsigned char *vp = rows[2][l / vr];
			unsigned char *o = dst + (long)(y0 + l) * pitch;
			const unsigned char *yl = j->ylut, *cl = j->clut;

			for (x = 0; x < width / 2; x++) {
				o[0] = yl[yp[0]];
				o[1] = cl[up[x]];
				o[2] = yl[yp[1]];
				o[3] = cl[vp[x]];
				o += 4;
				yp += 2;
			}
		}
	}
	j->stats.rawFrames++;
}

static void uvcjScanlines(UVCJ *j, int format, unsigned char *dst, long pitch,
	unsigned int width, unsigned int height)
{
	struct jpeg_decompress_struct *c = &j->cinfo;
	JSAMPROW rows[UVCJ_MAXROWS];
	unsigned int y, n, i, x;
	int comps = c->output_components;

	if (format == UVCJ_YUY2 &&
	    !uvcjGrow(&j->row, &j->rowSize,
		(unsigned long)width * 4 * UVCJ_MAXROWS))
		ERREXIT1(c, JERR_OUT_OF_MEMORY, 0);

	while (c->output_scanline < c->output_height) {
		y = c->output_scanline;
		n = c->output_height - y;
		if (n > UVCJ_MAXROWS)
			n = UVCJ_MAXROWS;
		for (i = 0; i < n; i++) {
			if (format != UVCJ_YUY2)
				rows[i] = dst + (long)(y + i) * pitch;
			else
				rows[i] = j->row + (unsigned long)i * width * 4;
		}
		n = jpeg_read_scanlines(c, rows, n);
		if (n == 0)
			break;
		if (format == UVCJ_RGB24)
			continue;
		if (format == UVCJ_RGB32) {
			/* libjpeg-turbo writes B,G,R,0xFF; BI_RGB wants the 4th byte 0 */
			for (i = 0; i < n; i++) {
				unsigned char *o = rows[i] + 3;
				for (x = 0; x < width; x++, o += 4)
					*o = 0;
			}
			continue;
		}
		for (i = 0; i < n && y + i < height; i++) {
			const unsigned char *s = rows[i];
			unsigned char *o = dst + (long)(y + i) * pitch;

			if (comps == 1) {
				for (x = 0; x < width; x++) {
					o[0] = j->ylut[s[x]];
					o[1] = 128;
					o += 2;
				}
			} else {
				/* full-resolution YCbCr: average each pair's chroma */
				for (x = 0; x < width / 2; x++) {
					o[0] = j->ylut[s[0]];
					o[1] = j->clut[(s[1] + s[4] + 1) >> 1];
					o[2] = j->ylut[s[3]];
					o[3] = j->clut[(s[2] + s[5] + 1) >> 1];
					o += 4;
					s += 6;
				}
			}
		}
	}
}

int uvcjDecode(UVCJ *j, const unsigned char *data, unsigned long size,
	int format, unsigned char *dst, long pitch,
	unsigned int width, unsigned int height)
{
	struct jpeg_decompress_struct *c = &j->cinfo;
	volatile int result = UVCJ_EBAD;

	if (data == NULL || size < 4 || dst == NULL) {
		j->stats.errors++;
		return UVCJ_EBAD;
	}
	if (format != UVCJ_RGB24 && format != UVCJ_RGB32 && format != UVCJ_YUY2)
		return UVCJ_EFORMAT;
	if (format == UVCJ_YUY2 && (width & 1))
		return UVCJ_EFORMAT;

	if (setjmp(j->err.jump)) {
		jpeg_abort_decompress(c);
		j->stats.errors++;
		return result;
	}
	uvcjBegin(j, data, size);
	c->scale_num = 1;
	c->scale_denom = (unsigned int)j->scale;
	if (j->mode == UVCJ_MODE_EXACT) {
		c->dct_method = JDCT_ISLOW;
		c->do_fancy_upsampling = TRUE;
	} else {
		c->dct_method = JDCT_IFAST;
		c->do_fancy_upsampling = FALSE;
	}
	c->raw_data_out = FALSE;
	if (format == UVCJ_YUY2) {
		if (uvcjRawFits(c))
			c->raw_data_out = TRUE;
		else if (c->jpeg_color_space == JCS_YCbCr)
			c->out_color_space = JCS_YCbCr;
		else if (c->jpeg_color_space == JCS_GRAYSCALE)
			c->out_color_space = JCS_GRAYSCALE;
		else {
			result = UVCJ_EFORMAT;
			ERREXIT(c, JERR_CONVERSION_NOTIMPL);
		}
	} else {
		if (c->jpeg_color_space != JCS_YCbCr &&
		    c->jpeg_color_space != JCS_GRAYSCALE &&
		    c->jpeg_color_space != JCS_RGB) {
			result = UVCJ_EFORMAT;
			ERREXIT(c, JERR_CONVERSION_NOTIMPL);
		}
		/* B,G,R(,X) as a Windows DIB has them */
		c->out_color_space = format == UVCJ_RGB32 ? JCS_EXT_BGRX : JCS_EXT_BGR;
	}
	jpeg_calc_output_dimensions(c);
	if (c->output_width != width || c->output_height != height) {
		result = UVCJ_ESIZE;
		ERREXIT1(c, JERR_IMAGE_TOO_BIG, (int)width);
	}
	result = UVCJ_EBAD;
	(void)jpeg_start_decompress(c);
	if (c->raw_data_out)
		uvcjRawYuy2(j, dst, pitch, width, height);
	else
		uvcjScanlines(j, format, dst, pitch, width, height);
	if (c->output_scanline < c->output_height)
		jpeg_abort_decompress(c);	/* suspended: cannot with a memory source */
	else
		(void)jpeg_finish_decompress(c);
	j->stats.warnings += (unsigned long)j->err.pub.num_warnings;
	j->stats.frames++;
	return UVCJ_OK;
}
