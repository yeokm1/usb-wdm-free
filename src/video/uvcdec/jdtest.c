/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Yeo Kheng Meng */
/*
 * Host test for uvcdec: the decoder core (uvcjpeg.c) on FFmpeg-made and
 * libjpeg-turbo-made MJPEG frames of a synthetic pattern, with and without
 * DHT, the ICM driver interface (uvcdec.c's DriverProc called directly;
 * nothing is installed or registered), every decode at each SIMD level this
 * host has (AVX2, SSE2, MMX) against plain C, and decode time per 1080p
 * frame at each SIMD level and in plain C.
 *
 *   JDTEST.EXE <vectors dir> [timing iterations]
 *
 * No camera capture is used: every frame is the pattern of vectors/mkvec.py.
 */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "jpeglib.h"
#include "uvcjpeg.h"

LRESULT CALLBACK DriverProc(DWORD id, HDRVR hdrvr, UINT msg,
	LPARAM lParam1, LPARAM lParam2);

static int failures;
static int checks;

#define CHECK(c, what) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

/* ---- the pattern, as mkvec.py makes it; B,G,R order (DIB) ---- */

static const unsigned char bars[8][3] = {
	{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
	{255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}
};

static unsigned char *pattern(int w, int h, int noise)
{
	unsigned char *p = (unsigned char *)malloc((size_t)w * h * 3);
	unsigned long seed = 1;
	int x, y, c;
	unsigned char *o = p;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int px[3];
			if (y < h / 2) {
				px[0] = bars[(x * 8) / w][0];
				px[1] = bars[(x * 8) / w][1];
				px[2] = bars[(x * 8) / w][2];
			} else {
				px[0] = (x * 255) / (w - 1);
				px[1] = (y * 255) / (h - 1);
				px[2] = 255 - px[0];
			}
			if (noise) {
				for (c = 0; c < 3; c++) {
					int v;
					seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
					v = px[c] + (int)((seed >> 16) & 31) - 16;
					px[c] = v < 0 ? 0 : (v > 255 ? 255 : v);
				}
			}
			o[0] = (unsigned char)px[2];	/* B */
			o[1] = (unsigned char)px[1];	/* G */
			o[2] = (unsigned char)px[0];	/* R */
			o += 3;
		}
	}
	return p;
}

/* PSNR of a B,G,R image (with row pitch) against the pattern */
static double psnr(const unsigned char *img, long pitch, int bpp,
	const unsigned char *ref, int w, int h)
{
	double se = 0.0;
	int x, y, c;

	for (y = 0; y < h; y++) {
		const unsigned char *s = img + (long)y * pitch;
		const unsigned char *r = ref + (long)y * w * 3;
		for (x = 0; x < w; x++)
			for (c = 0; c < 3; c++) {
				double d = (double)s[x * bpp + c] - r[x * 3 + c];
				se += d * d;
			}
	}
	if (se == 0.0)
		return 99.0;
	return 10.0 * log10(255.0 * 255.0 * 3.0 * w * h / se);
}

static int clamp(double v)
{
	return v < 0.0 ? 0 : (v > 255.0 ? 255 : (int)(v + 0.5));
}

/* YUY2 (studio range BT.601) back to B,G,R for comparison */
static unsigned char *yuy2ToBgr(const unsigned char *yuy, int w, int h)
{
	unsigned char *o = (unsigned char *)malloc((size_t)w * h * 3);
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			const unsigned char *p = yuy + ((long)y * w + (x & ~1)) * 2;
			double Y = 1.164 * ((x & 1 ? p[2] : p[0]) - 16);
			double U = p[1] - 128.0, V = p[3] - 128.0;
			unsigned char *d = o + ((long)y * w + x) * 3;
			d[2] = (unsigned char)clamp(Y + 1.596 * V);
			d[1] = (unsigned char)clamp(Y - 0.392 * U - 0.813 * V);
			d[0] = (unsigned char)clamp(Y + 2.017 * U);
		}
	return o;
}

/* ---- files and JPEG helpers ---- */

static unsigned char *load(const char *dir, const char *name, unsigned long *size)
{
	char path[MAX_PATH];
	FILE *f;
	unsigned char *b;
	long n;

	sprintf(path, "%s\\%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL) {
		printf("FAIL cannot open %s\n", path);
		failures++;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = (unsigned char *)malloc(n);
	fread(b, 1, n, f);
	fclose(f);
	*size = (unsigned long)n;
	return b;
}

/* removes every DHT segment ahead of SOS; returns the new size */
static unsigned long stripDht(const unsigned char *in, unsigned long n,
	unsigned char *out, int *removed)
{
	unsigned long p = 2, o = 2;

	*removed = 0;
	memcpy(out, in, 2);
	while (p + 4 <= n) {
		unsigned int m = in[p + 1];
		unsigned long len = ((unsigned long)in[p + 2] << 8) | in[p + 3];
		if (in[p] != 0xFF)
			return 0;
		if (m == 0xDA) {
			memcpy(out + o, in + p, n - p);
			return o + (n - p);
		}
		if (m == 0xC4)
			(*removed)++;
		else {
			memcpy(out + o, in + p, len + 2);
			o += len + 2;
		}
		p += len + 2;
	}
	return 0;
}

static int hasMarker(const unsigned char *b, unsigned long n, unsigned int m)
{
	unsigned long p = 2;

	while (p + 4 <= n && b[p] == 0xFF) {
		if (b[p + 1] == m)
			return 1;
		if (b[p + 1] == 0xDA)
			return 0;
		p += 2 + (((unsigned long)b[p + 2] << 8) | b[p + 3]);
	}
	return 0;
}

/* libjpeg-turbo encoder (test only): pattern to JPEG with chosen sampling */
static unsigned char *encode(const unsigned char *bgr, int w, int h,
	int gray, int hy, int vy, int quality, int restart, unsigned long *size)
{
	struct jpeg_compress_struct c;
	struct jpeg_error_mgr e;
	unsigned char *out = NULL;
	size_t outSize = 0;
	unsigned char *grayRow = NULL;
	int y;

	c.err = jpeg_std_error(&e);
	jpeg_create_compress(&c);
	jpeg_mem_dest(&c, &out, &outSize);
	c.image_width = (JDIMENSION)w;
	c.image_height = (JDIMENSION)h;
	c.input_components = gray ? 1 : 3;
	c.in_color_space = gray ? JCS_GRAYSCALE : JCS_EXT_BGR;
	jpeg_set_defaults(&c);
	jpeg_set_quality(&c, quality, TRUE);
	if (!gray) {
		c.comp_info[0].h_samp_factor = hy;
		c.comp_info[0].v_samp_factor = vy;
		c.comp_info[1].h_samp_factor = 1;
		c.comp_info[1].v_samp_factor = 1;
		c.comp_info[2].h_samp_factor = 1;
		c.comp_info[2].v_samp_factor = 1;
	}
	c.restart_interval = (unsigned int)restart;
	jpeg_start_compress(&c, TRUE);
	if (gray)
		grayRow = (unsigned char *)malloc(w);
	for (y = 0; y < h; y++) {
		JSAMPROW r = (JSAMPROW)(bgr + (long)y * w * 3);
		if (gray) {
			int x;
			for (x = 0; x < w; x++)
				grayRow[x] = r[x * 3 + 1];
			r = grayRow;
		}
		jpeg_write_scanlines(&c, &r, 1);
	}
	jpeg_finish_compress(&c);
	jpeg_destroy_compress(&c);
	free(grayRow);
	*size = (unsigned long)outSize;
	return out;
}

/* ---- core decode checks on one frame pair (with and without DHT) ---- */

static void checkFrame(UVCJ *j, const char *name, const unsigned char *jpg,
	unsigned long n, const unsigned char *ref, int w, int h,
	double minRgb, double minYuy)
{
	unsigned char *nodht = (unsigned char *)malloc(n);
	unsigned long m;
	int removed, r, mode;
	long pitch24 = ((w * 24 + 31) / 32) * 4;
	unsigned char *a = (unsigned char *)malloc(pitch24 * h);
	unsigned char *b = (unsigned char *)malloc(pitch24 * h);
	unsigned char *ya = (unsigned char *)malloc(w * h * 2);
	unsigned char *yb = (unsigned char *)malloc(w * h * 2);
	unsigned char *a32 = (unsigned char *)malloc(w * h * 4);
	char what[160];

	if (hasMarker(jpg, n, 0xC4)) {
		m = stripDht(jpg, n, nodht, &removed);
		sprintf(what, "%s: DHT stripped", name);
		CHECK(m > 0 && removed > 0 && !hasMarker(nodht, m, 0xC4), what);
	} else {
		memcpy(nodht, jpg, n);
		m = n;
	}
	for (mode = UVCJ_MODE_FAST; mode <= UVCJ_MODE_EXACT; mode++) {
		double p;
		const char *mn = mode == UVCJ_MODE_FAST ? "fast" : "exact";

		uvcjSetMode(j, mode);
		r = uvcjDecode(j, jpg, n, UVCJ_RGB24, a, pitch24, w, h);
		sprintf(what, "%s %s: RGB24 decodes", name, mn);
		CHECK(r == UVCJ_OK, what);
		r = uvcjDecode(j, nodht, m, UVCJ_RGB24, b, pitch24, w, h);
		sprintf(what, "%s %s: RGB24 decodes without DHT", name, mn);
		CHECK(r == UVCJ_OK, what);
		sprintf(what, "%s %s: no-DHT frame decodes identically", name, mn);
		CHECK(memcmp(a, b, pitch24 * h) == 0, what);
		p = psnr(a, pitch24, 3, ref, w, h);
		printf("  %-28s %-5s RGB24 PSNR %.2f dB\n", name, mn, p);
		sprintf(what, "%s %s: RGB24 PSNR %.2f >= %.1f", name, mn, p, minRgb);
		CHECK(p >= minRgb, what);

		r = uvcjDecode(j, nodht, m, UVCJ_RGB32, a32, (long)w * 4, w, h);
		sprintf(what, "%s %s: RGB32 equals RGB24", name, mn);
		{
			int x, y, same = r == UVCJ_OK;
			for (y = 0; y < h && same; y++)
				for (x = 0; x < w; x++)
					if (memcmp(a32 + ((long)y * w + x) * 4,
						b + (long)y * pitch24 + x * 3, 3) != 0 ||
					    a32[((long)y * w + x) * 4 + 3] != 0) {
						same = 0;
						break;
					}
			CHECK(same, what);
		}

		r = uvcjDecode(j, jpg, n, UVCJ_YUY2, ya, (long)w * 2, w, h);
		sprintf(what, "%s %s: YUY2 decodes", name, mn);
		CHECK(r == UVCJ_OK, what);
		r = uvcjDecode(j, nodht, m, UVCJ_YUY2, yb, (long)w * 2, w, h);
		sprintf(what, "%s %s: YUY2 no-DHT identical", name, mn);
		CHECK(r == UVCJ_OK && memcmp(ya, yb, w * h * 2) == 0, what);
		{
			unsigned char *back = yuy2ToBgr(ya, w, h);
			p = psnr(back, (long)w * 3, 3, ref, w, h);
			free(back);
		}
		printf("  %-28s %-5s YUY2  PSNR %.2f dB (through BT.601)\n", name, mn, p);
		sprintf(what, "%s %s: YUY2 PSNR %.2f >= %.1f", name, mn, p, minYuy);
		CHECK(p >= minYuy, what);
	}
	uvcjSetMode(j, UVCJ_MODE_FAST);
	free(nodht); free(a); free(b); free(ya); free(yb); free(a32);
}

static void testFiles(UVCJ *j, const char *dir)
{
	static const struct { const char *d, *n; int w, h, noise; double rgb, yuy; } v[] = {
		{"b320d.jpg", "b320n.jpg", 320, 240, 0, 30.0, 28.0},
		{"b640d.jpg", "b640n.jpg", 640, 480, 0, 30.0, 28.0},
		{NULL, "h1080n.jpg", 1920, 1080, 1, 26.0, 25.0},
		{NULL, "h1080en.jpg", 1920, 1080, 1, 25.0, 24.0},
	};
	int i;

	for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
		unsigned long dn = 0, nn = 0;
		unsigned char *d = v[i].d ? load(dir, v[i].d, &dn) : NULL;
		unsigned char *n = load(dir, v[i].n, &nn);
		unsigned char *ref = pattern(v[i].w, v[i].h, v[i].noise);
		unsigned int w, h;
		int comps, r;
		char what[128];

		if (n == NULL) {
			free(d);
			free(ref);
			continue;
		}
		sprintf(what, "%s carries no DHT", v[i].n);
		CHECK(!hasMarker(n, nn, 0xC4), what);
		r = uvcjHeader(j, n, nn, &w, &h, &comps);
		sprintf(what, "%s header %ux%u", v[i].n, w, h);
		CHECK(r == UVCJ_OK && (int)w == v[i].w && (int)h == v[i].h && comps == 3, what);
		if (d != NULL) {
			/* the FFmpeg frame with its DHT; stripping it must give the file */
			unsigned char *s = (unsigned char *)malloc(dn);
			int removed;
			unsigned long sn = stripDht(d, dn, s, &removed);
			sprintf(what, "%s stripped equals %s", v[i].d, v[i].n);
			CHECK(sn == nn && memcmp(s, n, nn) == 0, what);
			free(s);
			checkFrame(j, v[i].d, d, dn, ref, v[i].w, v[i].h, v[i].rgb, v[i].yuy);
		} else {
			checkFrame(j, v[i].n, n, nn, ref, v[i].w, v[i].h, v[i].rgb, v[i].yuy);
		}
		free(d);
		free(n);
		free(ref);
	}
}

static void testIjgFrames(UVCJ *j)
{
	static const struct { const char *name; int gray, hy, vy, restart; double rgb, yuy; } v[] = {
		{"ljt 640x480 2h1v (UVC 4:2:2)", 0, 2, 1, 0, 30.0, 28.0},
		{"ljt 640x480 2h2v (4:2:0)", 0, 2, 2, 0, 30.0, 28.0},
		{"ljt 640x480 1h1v (4:4:4)", 0, 1, 1, 0, 30.0, 28.0},
		{"ljt 640x480 2h1v DRI 40", 0, 2, 1, 40, 30.0, 28.0},
	};
	unsigned char *ref = pattern(640, 480, 0);
	UVCJ_STATS s0, s1;
	int i;

	for (i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
		unsigned long n;
		unsigned char *jpg = encode(ref, 640, 480, v[i].gray, v[i].hy, v[i].vy,
			90, v[i].restart, &n);
		char what[128];

		uvcjGetStats(j, &s0);
		checkFrame(j, v[i].name, jpg, n, ref, 640, 480, v[i].rgb, v[i].yuy);
		uvcjGetStats(j, &s1);
		sprintf(what, "%s: YUY2 raw path %s", v[i].name,
			v[i].hy == 2 ? "used" : "not used");
		/* checkFrame makes 4 YUY2 decodes */
		CHECK((s1.rawFrames - s0.rawFrames) == (unsigned long)(v[i].hy == 2 ? 4 : 0), what);
		free(jpg);
	}

	/* grayscale: RGB24 is grey, YUY2 chroma is 128 */
	{
		unsigned long n;
		unsigned char *jpg = encode(ref, 640, 480, 1, 1, 1, 90, 0, &n);
		unsigned char *a = (unsigned char *)malloc(640 * 480 * 3);
		unsigned char *ya = (unsigned char *)malloc(640 * 480 * 2);
		int r1 = uvcjDecode(j, jpg, n, UVCJ_RGB24, a, 640 * 3, 640, 480);
		int r2 = uvcjDecode(j, jpg, n, UVCJ_YUY2, ya, 640 * 2, 640, 480);
		int k, grey = 1, chroma = 1;

		for (k = 0; k < 640 * 480; k++)
			if (a[k * 3] != a[k * 3 + 1] || a[k * 3] != a[k * 3 + 2])
				grey = 0;
		for (k = 0; k < 640 * 480; k += 2)
			if (ya[k * 2 + 1] != 128 || ya[k * 2 + 3] != 128)
				chroma = 0;
		CHECK(r1 == UVCJ_OK && grey, "grayscale frame decodes to grey RGB24");
		CHECK(r2 == UVCJ_OK && chroma, "grayscale frame decodes to YUY2 with chroma 128");
		free(a); free(ya); free(jpg);
	}
	free(ref);
}

static void testErrors(UVCJ *j, const char *dir)
{
	unsigned long n;
	unsigned char *jpg = load(dir, "b320n.jpg", &n);
	unsigned char *out = (unsigned char *)malloc(320 * 240 * 4);
	unsigned char junk[4096];
	UVCJ_STATS s0, s1;
	int r, k;

	if (jpg == NULL)
		return;
	uvcjGetStats(j, &s0);
	r = uvcjDecode(j, jpg, n / 2, UVCJ_RGB24, out, 320 * 3, 320, 240);
	uvcjGetStats(j, &s1);
	CHECK(r == UVCJ_OK && s1.warnings > s0.warnings,
		"truncated frame decodes with a warning (grey remainder)");
	for (k = 0; k < (int)sizeof(junk); k++)
		junk[k] = (unsigned char)(k * 37 + 11);
	r = uvcjDecode(j, junk, sizeof(junk), UVCJ_RGB24, out, 320 * 3, 320, 240);
	CHECK(r == UVCJ_EBAD, "garbage frame refused");
	memset(junk, 0, sizeof(junk));
	r = uvcjDecode(j, junk, sizeof(junk), UVCJ_RGB24, out, 320 * 3, 320, 240);
	CHECK(r == UVCJ_EBAD, "all-zero frame refused");
	r = uvcjDecode(j, jpg, 3, UVCJ_RGB24, out, 320 * 3, 320, 240);
	CHECK(r == UVCJ_EBAD, "3-byte frame refused");
	/* SOI and headers only, no scan */
	r = uvcjDecode(j, jpg, 200, UVCJ_YUY2, out, 320 * 2, 320, 240);
	CHECK(r == UVCJ_OK || r == UVCJ_EBAD, "header-only frame handled");
	r = uvcjDecode(j, jpg, n, UVCJ_RGB24, out, 640 * 3, 640, 480);
	CHECK(r == UVCJ_ESIZE, "frame of another size refused");
	r = uvcjDecode(j, jpg, n, UVCJ_YUY2, out, 320 * 2, 319, 240);
	CHECK(r == UVCJ_EFORMAT, "odd YUY2 width refused");
	r = uvcjDecode(j, jpg, n, UVCJ_RGB24, out, 320 * 3, 320, 240);
	CHECK(r == UVCJ_OK, "decoder usable after errors");
	/* half scale */
	CHECK(uvcjSetScale(j, 2) == UVCJ_OK, "scale 1/2 accepted");
	r = uvcjDecode(j, jpg, n, UVCJ_YUY2, out, 160 * 2, 160, 120);
	CHECK(r == UVCJ_OK, "1/2 scale YUY2 decode");
	r = uvcjDecode(j, jpg, n, UVCJ_RGB24, out, 160 * 3, 160, 120);
	CHECK(r == UVCJ_OK, "1/2 scale RGB24 decode");
	CHECK(uvcjSetScale(j, 3) == UVCJ_EFORMAT, "scale 1/3 refused");
	uvcjSetScale(j, 1);
	free(jpg);
	free(out);
}

/* ---- SIMD against plain C ---- */

/* the SIMD levels x86 processors from the Pentium MMX on offer, best first */
static const struct { const char *name; unsigned int mask; } levels[] = {
	{"AVX2", UVCJ_SIMD_ALL},
	{"SSE2", UVCJ_SIMD_SSE2 | UVCJ_SIMD_SSE | UVCJ_SIMD_MMX},
	{"MMX ", UVCJ_SIMD_MMX},
	{"C   ", 0},
};
#define NLEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

/* a level is present when this host has its own best set */
static int levelPresent(int k)
{
	unsigned int have = uvcjSimdSupport() & levels[k].mask;

	if (levels[k].mask == 0)
		return 1;
	if (k == 0)
		return (have & UVCJ_SIMD_AVX2) != 0;
	if (k == 1)
		return (have & UVCJ_SIMD_SSE2) != 0;
	return (have & UVCJ_SIMD_MMX) != 0;
}

static long simdMaxDiff, simdBytes, simdDiffer;

/*
 * Decodes one frame in every format, mode and scale in plain C and at each
 * SIMD level present, and compares. The tolerance is none: every output
 * here is 8-bit integer arithmetic (ISLOW and IFAST IDCTs and their scaled
 * forms, merged and fancy upsampling, YCbCr to B,G,R), which libjpeg-turbo's
 * SIMD routines compute exactly as its C routines do, so a SIMD decode must
 * equal the plain C one byte for byte. Only the float IDCT, which the codec
 * never selects, could differ.
 */
static void simdFrame(UVCJ *j, const char *name, const unsigned char *jpg,
	unsigned long n, int w, int h)
{
	static const int fmts[3] = {UVCJ_RGB24, UVCJ_RGB32, UVCJ_YUY2};
	static const char *fn[3] = {"RGB24", "RGB32", "YUY2"};
	size_t cap = (size_t)w * h * 4 + 64;
	unsigned char *ref = (unsigned char *)malloc(cap);
	unsigned char *out = (unsigned char *)malloc(cap);
	int f, mode, sc, k;
	char what[200];

	for (k = 0; k < NLEVELS - 1; k++) {
		long worst[2], nd[2], nb[2];
		int ok = 1;

		worst[0] = worst[1] = nd[0] = nd[1] = nb[0] = nb[1] = 0;
		if (!levelPresent(k))
			continue;
		for (f = 0; f < 3; f++)
			for (mode = UVCJ_MODE_FAST; mode <= UVCJ_MODE_EXACT; mode++)
				for (sc = 1; sc <= 8; sc *= 2) {
					int ow = w / sc, oh = h / sc;
					long pitch, bytes, b;
					int r1, r2;

					if (w % sc || h % sc || (fmts[f] == UVCJ_YUY2 && (ow & 1)))
						continue;
					pitch = fmts[f] == UVCJ_RGB24 ? (long)ow * 3 :
						fmts[f] == UVCJ_RGB32 ? (long)ow * 4 : (long)ow * 2;
					bytes = pitch * oh;
					uvcjSetMode(j, mode);
					uvcjSetScale(j, sc);
					uvcjSetSimd(j, 0);
					memset(ref, 0x11, bytes);
					r1 = uvcjDecode(j, jpg, n, fmts[f], ref, pitch, ow, oh);
					uvcjSetSimd(j, levels[k].mask);
					memset(out, 0x22, bytes);
					r2 = uvcjDecode(j, jpg, n, fmts[f], out, pitch, ow, oh);
					if (r1 != UVCJ_OK || r2 != UVCJ_OK) {
						sprintf(what, "%s %s %s %s 1/%d: decodes (C %d, SIMD %d)",
							name, levels[k].name, fn[f],
							mode == UVCJ_MODE_FAST ? "fast" : "exact", sc, r1, r2);
						CHECK(0, what);
						ok = 0;
						continue;
					}
					for (b = 0; b < bytes; b++) {
						int d = ref[b] - out[b];
						if (d < 0)
							d = -d;
						if (d) {
							nd[mode]++;
							if (d > worst[mode])
								worst[mode] = d;
						}
					}
					nb[mode] += bytes;
				}
		uvcjSetSimd(j, UVCJ_SIMD_ALL);
		uvcjSetMode(j, UVCJ_MODE_FAST);
		uvcjSetScale(j, 1);
		printf("  %-24s %s vs C: fast %ld of %ld bytes differ (max %ld), exact %ld of %ld (max %ld)\n",
			name, levels[k].name, nd[0], nb[0], worst[0], nd[1], nb[1], worst[1]);
		sprintf(what, "%s %s vs C: fast mode byte-identical", name, levels[k].name);
		CHECK(ok && nd[0] == 0, what);
		sprintf(what, "%s %s vs C: exact mode byte-identical", name, levels[k].name);
		CHECK(ok && nd[1] == 0, what);
		if (worst[0] > simdMaxDiff)
			simdMaxDiff = worst[0];
		if (worst[1] > simdMaxDiff)
			simdMaxDiff = worst[1];
		simdBytes += nb[0] + nb[1];
		simdDiffer += nd[0] + nd[1];
	}
	free(ref);
	free(out);
}

static void testSimd(UVCJ *j, const char *dir)
{
	static const char *files[] = {"b320d.jpg", "b320n.jpg", "b640d.jpg", "b640n.jpg",
		"h1080n.jpg", "h1080en.jpg"};
	static const struct { const char *name; int gray, hy, vy, restart; } gen[] = {
		{"ljt 640x480 2h1v", 0, 2, 1, 0},
		{"ljt 640x480 2h2v", 0, 2, 2, 0},
		{"ljt 640x480 1h1v", 0, 1, 1, 0},
		{"ljt 640x480 2h1v DRI 40", 0, 2, 1, 40},
		{"ljt 640x480 gray", 1, 1, 1, 0},
	};
	unsigned char *ref = pattern(640, 480, 0);
	unsigned int have = uvcjSimdSupport();
	int i, k;

	printf("  SIMD sets on this host: %s%s%s%s(0x%02X)\n",
		have & UVCJ_SIMD_MMX ? "MMX " : "", have & UVCJ_SIMD_SSE ? "SSE " : "",
		have & UVCJ_SIMD_SSE2 ? "SSE2 " : "", have & UVCJ_SIMD_AVX2 ? "AVX2 " : "", have);
	for (k = 0; k < NLEVELS; k++)
		CHECK(uvcjSetSimd(j, levels[k].mask) == (levels[k].mask & have),
			"uvcjSetSimd returns the sets it allows");
	CHECK(uvcjSetSimd(j, 0) == 0, "plain C selectable");
	uvcjSetSimd(j, UVCJ_SIMD_ALL);
	for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
		unsigned long n;
		unsigned char *jpg = load(dir, files[i], &n);
		unsigned int w, h;
		int comps;

		if (jpg == NULL)
			continue;
		if (uvcjHeader(j, jpg, n, &w, &h, &comps) == UVCJ_OK)
			simdFrame(j, files[i], jpg, n, (int)w, (int)h);
		free(jpg);
	}
	for (i = 0; i < (int)(sizeof(gen) / sizeof(gen[0])); i++) {
		unsigned long n;
		unsigned char *jpg = encode(ref, 640, 480, gen[i].gray, gen[i].hy, gen[i].vy,
			90, gen[i].restart, &n);
		simdFrame(j, gen[i].name, jpg, n, 640, 480);
		free(jpg);
	}
	printf("  SIMD vs C over %ld bytes: %ld differ, by at most %ld\n",
		simdBytes, simdDiffer, simdMaxDiff);
	free(ref);
}

/* ---- the ICM driver interface ---- */

static void fillIn(BITMAPINFOHEADER *bi, DWORD fcc, LONG w, LONG h, DWORD size)
{
	memset(bi, 0, sizeof(*bi));
	bi->biSize = sizeof(*bi);
	bi->biWidth = w;
	bi->biHeight = h;
	bi->biPlanes = 1;
	bi->biBitCount = 24;
	bi->biCompression = fcc;
	bi->biSizeImage = size;
}

static void testIcm(const char *dir)
{
	ICOPEN o;
	ICINFO info;
	BITMAPINFOHEADER in, out, got;
	ICDECOMPRESS dc;
	ICDECOMPRESSEX dx;
	DWORD id;
	LRESULT r;
	unsigned long n;
	unsigned char *jpg = load(dir, "b320n.jpg", &n);
	long pitch = 320 * 3;
	unsigned char *buf = (unsigned char *)malloc(pitch * 240);
	unsigned char *top = (unsigned char *)malloc(pitch * 240);
	unsigned char *yuy = (unsigned char *)malloc(320 * 240 * 2);
	UVCJ *j = uvcjOpen();
	int y;

	if (jpg == NULL)
		return;
	CHECK(DriverProc(0, NULL, DRV_LOAD, 0, 0) == 1, "DRV_LOAD");
	memset(&o, 0, sizeof(o));
	o.dwSize = sizeof(o);
	o.fccType = mmioFOURCC('a', 'u', 'd', 'c');
	CHECK(DriverProc(0, NULL, DRV_OPEN, 0, (LPARAM)&o) == 0, "DRV_OPEN refuses audc");
	o.fccType = ICTYPE_VIDEO;
	o.fccHandler = mmioFOURCC('M', 'J', 'P', 'G');
	o.dwFlags = ICMODE_DECOMPRESS;
	id = (DWORD)DriverProc(0, NULL, DRV_OPEN, 0, (LPARAM)&o);
	CHECK(id != 0 && o.dwError == ICERR_OK, "DRV_OPEN vidc");
	if (id == 0)
		return;
	CHECK(DriverProc(id, NULL, ICM_GETINFO, (LPARAM)&info, sizeof(info)) == sizeof(info) &&
		info.fccHandler == mmioFOURCC('M', 'J', 'P', 'G') && info.fccType == ICTYPE_VIDEO &&
		info.szName[0] == L'u', "ICM_GETINFO");

	fillIn(&in, mmioFOURCC('M', 'J', 'P', 'G'), 320, 240, n);
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, 0) == ICERR_OK,
		"query MJPG, any output");
	fillIn(&got, mmioFOURCC('H', '2', '6', '4'), 320, 240, n);
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&got, 0) == ICERR_BADFORMAT,
		"query refuses H264");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_GET_FORMAT, (LPARAM)&in, 0) ==
		sizeof(BITMAPINFOHEADER), "get format size");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_GET_FORMAT, (LPARAM)&in, (LPARAM)&out) == ICERR_OK &&
		out.biCompression == BI_RGB && out.biBitCount == 24 && out.biWidth == 320 &&
		out.biHeight == 240 && out.biSizeImage == (DWORD)(pitch * 240), "get format RGB24 bottom-up");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&out) == ICERR_OK,
		"query MJPG to RGB24");
	got = out; got.biBitCount = 16;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&got) == ICERR_BADFORMAT,
		"query refuses RGB555");
	got = out; got.biWidth = 300;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&got) == ICERR_BADFORMAT,
		"query refuses a stretch");
	got = out; got.biWidth = 160; got.biHeight = 120;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&got) == ICERR_OK,
		"query accepts 1/2 size");
	got = out; got.biCompression = mmioFOURCC('Y', 'U', 'Y', '2'); got.biBitCount = 16;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&got) == ICERR_OK,
		"query accepts YUY2");
	got.biBitCount = 32; got.biCompression = BI_RGB;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_QUERY, (LPARAM)&in, (LPARAM)&got) == ICERR_OK,
		"query accepts RGB32");
	CHECK(DriverProc(id, NULL, ICM_COMPRESS_QUERY, (LPARAM)&out, (LPARAM)&in) == ICERR_BADFORMAT,
		"compress refused");

	/* bottom-up RGB24 through ICM equals the core's top-down decode flipped */
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_BEGIN, (LPARAM)&in, (LPARAM)&out) == ICERR_OK,
		"decompress begin");
	memset(&dc, 0, sizeof(dc));
	dc.lpbiInput = &in;
	dc.lpInput = jpg;
	dc.lpbiOutput = &out;
	dc.lpOutput = buf;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS, (LPARAM)&dc, sizeof(dc)) == ICERR_OK,
		"decompress frame");
	uvcjDecode(j, jpg, n, UVCJ_RGB24, top, pitch, 320, 240);
	{
		int same = 1;
		for (y = 0; y < 240; y++)
			if (memcmp(buf + (long)(239 - y) * pitch, top + (long)y * pitch, pitch) != 0)
				same = 0;
		CHECK(same, "ICM RGB24 is the bottom-up DIB of the decoded frame");
	}
	memset(buf, 0x5A, pitch * 240);
	dc.dwFlags = ICDECOMPRESS_HURRYUP;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS, (LPARAM)&dc, sizeof(dc)) == ICERR_OK &&
		buf[0] == 0x5A && buf[pitch * 240 - 1] == 0x5A, "HURRYUP skips the decode");
	dc.dwFlags = 0;
	in.biSizeImage = 0;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS, (LPARAM)&dc, sizeof(dc)) == ICERR_BADFORMAT,
		"frame with no size refused");
	in.biSizeImage = 64;
	r = DriverProc(id, NULL, ICM_DECOMPRESS, (LPARAM)&dc, sizeof(dc));
	CHECK(r == ICERR_OK || r == ICERR_ERROR, "short frame handled");
	in.biSizeImage = n;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESS_END, 0, 0) == ICERR_OK, "decompress end");

	/* YUY2 through DECOMPRESSEX, top-down whatever the height's sign */
	got = out; got.biCompression = mmioFOURCC('Y', 'U', 'Y', '2'); got.biBitCount = 16;
	got.biSizeImage = 320 * 240 * 2;
	memset(&dx, 0, sizeof(dx));
	dx.lpbiSrc = &in;
	dx.lpSrc = jpg;
	dx.lpbiDst = &got;
	dx.lpDst = yuy;
	dx.dxSrc = 320; dx.dySrc = 240; dx.dxDst = 320; dx.dyDst = 240;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESSEX_QUERY, (LPARAM)&dx, sizeof(dx)) == ICERR_OK,
		"decompressex query");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESSEX_BEGIN, (LPARAM)&dx, sizeof(dx)) == ICERR_OK,
		"decompressex begin");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESSEX, (LPARAM)&dx, sizeof(dx)) == ICERR_OK,
		"decompressex frame");
	{
		unsigned char *ref = (unsigned char *)malloc(320 * 240 * 2);
		uvcjDecode(j, jpg, n, UVCJ_YUY2, ref, 320 * 2, 320, 240);
		CHECK(memcmp(ref, yuy, 320 * 240 * 2) == 0, "ICM YUY2 equals the core decode");
		free(ref);
	}
	dx.xSrc = 8;
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESSEX_QUERY, (LPARAM)&dx, sizeof(dx)) == ICERR_BADFORMAT,
		"decompressex refuses a sub-rectangle");
	CHECK(DriverProc(id, NULL, ICM_DECOMPRESSEX_END, 0, 0) == ICERR_OK, "decompressex end");
	CHECK(DriverProc(id, NULL, DRV_CLOSE, 0, 0) == 1, "DRV_CLOSE");
	CHECK(DriverProc(0, NULL, ICM_GETINFO, (LPARAM)&info, sizeof(info)) == ICERR_BADHANDLE,
		"no instance, no service");
	uvcjClose(j);
	free(jpg); free(buf); free(top); free(yuy);
}

/* ---- decode time per frame ---- */

static double now(void)
{
	static LARGE_INTEGER f;
	LARGE_INTEGER c;

	if (f.QuadPart == 0)
		QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart;
}

static void timeFrame(UVCJ *j, const char *name, const unsigned char *jpg,
	unsigned long n, int w, int h, int iters)
{
	static const struct { const char *label; int fmt, mode, scale; } c[] = {
		{"RGB24 fast  ", UVCJ_RGB24, UVCJ_MODE_FAST, 1},
		{"RGB24 exact ", UVCJ_RGB24, UVCJ_MODE_EXACT, 1},
		{"YUY2  fast  ", UVCJ_YUY2, UVCJ_MODE_FAST, 1},
		{"YUY2  exact ", UVCJ_YUY2, UVCJ_MODE_EXACT, 1},
		{"RGB24 fast/2", UVCJ_RGB24, UVCJ_MODE_FAST, 2},
		{"YUY2  fast/2", UVCJ_YUY2, UVCJ_MODE_FAST, 2},
	};
	unsigned char *out = (unsigned char *)malloc((size_t)w * h * 4);
	int k, i, lv;

	printf("  %s (%lu bytes, %dx%d):\n", name, n, w, h);
	for (lv = 0; lv < NLEVELS; lv++) {
	if (!levelPresent(lv))
		continue;
	uvcjSetSimd(j, levels[lv].mask);
	for (k = 0; k < (int)(sizeof(c) / sizeof(c[0])); k++) {
		int ow = w / c[k].scale, oh = h / c[k].scale;
		long pitch = c[k].fmt == UVCJ_YUY2 ? ow * 2 : ((ow * 24 + 31) / 32) * 4;
		double t0, t, best = 1e9;
		int ok = 1;

		uvcjSetMode(j, c[k].mode);
		uvcjSetScale(j, c[k].scale);
		uvcjDecode(j, jpg, n, c[k].fmt, out, pitch, ow, oh);	/* warm */
		for (i = 0; i < iters; i++) {
			t0 = now();
			if (uvcjDecode(j, jpg, n, c[k].fmt, out, pitch, ow, oh) != UVCJ_OK)
				ok = 0;
			t = now() - t0;
			if (t < best)
				best = t;
		}
		printf("    %s %s %7.2f ms/frame best of %d, %6.1f fps on this host, %.1f source Mpixel/s\n",
			levels[lv].name, c[k].label, best * 1000.0, iters, 1.0 / best,
			(double)w * h / best / 1e6);
		CHECK(ok, "timed decodes succeed");
	}
	}
	uvcjSetSimd(j, UVCJ_SIMD_ALL);
	uvcjSetMode(j, UVCJ_MODE_FAST);
	uvcjSetScale(j, 1);
	free(out);
}

int main(int argc, char **argv)
{
	UVCJ *j;
	int iters = argc > 2 ? atoi(argv[2]) : 20;

	if (argc < 2) {
		printf("usage: JDTEST.EXE <vectors dir> [timing iterations]\n");
		return 2;
	}
	j = uvcjOpen();
	CHECK(j != NULL, "decoder opens");
	if (j == NULL)
		return 1;
	printf("FFmpeg-made frames (vectors/):\n");
	testFiles(j, argv[1]);
	printf("libjpeg-turbo-made frames:\n");
	testIjgFrames(j);
	testErrors(j, argv[1]);
	printf("SIMD against plain C:\n");
	testSimd(j, argv[1]);
	testIcm(argv[1]);
	if (iters > 0) {
		unsigned long n;
		unsigned char *f;
		unsigned char *ref = pattern(1920, 1080, 1);

		printf("Decode time per 1080p frame (single thread, MSVC 6.0 /O2; each SIMD level, then plain C):\n");
		f = load(argv[1], "h1080n.jpg", &n);
		if (f) { timeFrame(j, "h1080n.jpg FFmpeg 4:2:2 (2h2v/1h2v), no DHT", f, n, 1920, 1080, iters); free(f); }
		f = load(argv[1], "h1080en.jpg", &n);
		if (f) { timeFrame(j, "h1080en.jpg FFmpeg 4:2:0, no DHT", f, n, 1920, 1080, iters); free(f); }
		f = encode(ref, 1920, 1080, 0, 2, 1, 85, 0, &n);
		timeFrame(j, "ljt 4:2:2 2h1v q85 (the UVC layout)", f, n, 1920, 1080, iters);
		free(f);
		free(ref);
	}
	uvcjClose(j);
	printf("%d checks, %d failures\n", checks, failures);
	printf(failures ? "FAIL\n" : "PASS\n");
	return failures ? 1 : 0;
}
