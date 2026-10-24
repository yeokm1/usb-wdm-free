/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Yeo Kheng Meng */
/*
 * uvcdec.dll: a Video for Windows Installable Compression Manager (ICM)
 * codec driver that decompresses FOURCC 'MJPG' to RGB24, RGB32 and YUY2,
 * for UVC webcams' MJPEG streams on Windows 98 SE, ME and 2000.
 * This software is based in part on the work of the Independent JPEG Group.
 *
 * The driver interface is VFW.H's (MSVC 6.0): DriverProc with DRV_* and the
 * ICM_* messages. Decompression only; compression is refused. Imports only
 * KERNEL32 (static C runtime), so it has no dependency 98 SE lacks.
 */
#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include "uvcjpeg.h"

#define FCC_MJPG	mmioFOURCC('M', 'J', 'P', 'G')
#define FCC_mjpg	mmioFOURCC('m', 'j', 'p', 'g')
#define FCC_YUY2	mmioFOURCC('Y', 'U', 'Y', '2')
#define FCC_UVCD	mmioFOURCC('u', 'v', 'c', 'd')

typedef struct UVCDEC {
	DWORD magic;
	UVCJ *jpeg;
	int begun;
	int format;	/* UVCJ_* of the begun output */
	int scale;
	LONG width;	/* output size */
	LONG height;
	LONG pitch;	/* signed, top-down step */
	LONG offset;	/* byte offset of the top row in the output buffer */
} UVCDEC;

#define UVCDEC_MAGIC	0x43445655UL	/* 'UVDC' */

static void uvcdecCopyName(WCHAR *dst, const char *src, int max)
{
	int i;

	for (i = 0; i + 1 < max && src[i] != 0; i++)
		dst[i] = (WCHAR)(unsigned char)src[i];
	dst[i] = 0;
}

static int uvcdecInputOk(const BITMAPINFOHEADER *bi)
{
	if (bi == NULL || bi->biSize < sizeof(BITMAPINFOHEADER))
		return 0;
	if (bi->biCompression != FCC_MJPG && bi->biCompression != FCC_mjpg)
		return 0;
	if (bi->biWidth <= 0 || bi->biHeight == 0)
		return 0;
	if (bi->biWidth > 8192 || bi->biHeight > 8192 || bi->biHeight < -8192)
		return 0;
	return 1;
}

static LONG uvcdecAbs(LONG v)
{
	return v < 0 ? -v : v;
}

/*
 * Checks an output format against the input. On success fills the format,
 * scale, size and the signed pitch and top-row offset of the output buffer.
 */
static int uvcdecOutputOk(const BITMAPINFOHEADER *in,
	const BITMAPINFOHEADER *out, UVCDEC *d)
{
	LONG iw, ih, ow, oh, pitch;
	int scale, format, topdown;

	if (out == NULL || out->biSize < sizeof(BITMAPINFOHEADER))
		return 0;
	if (out->biPlanes != 1 && out->biPlanes != 0)
		return 0;
	if (out->biCompression == BI_RGB && out->biBitCount == 24)
		format = UVCJ_RGB24;
	else if (out->biCompression == BI_RGB && out->biBitCount == 32)
		format = UVCJ_RGB32;
	else if (out->biCompression == FCC_YUY2 && out->biBitCount == 16)
		format = UVCJ_YUY2;
	else
		return 0;
	iw = in->biWidth;
	ih = uvcdecAbs(in->biHeight);
	ow = out->biWidth;
	oh = uvcdecAbs(out->biHeight);
	if (ow <= 0 || oh == 0)
		return 0;
	/* 1:1, or libjpeg's IDCT scaling to 1/2, 1/4, 1/8 */
	for (scale = 1; scale <= 8; scale *= 2)
		if (ow == (iw + scale - 1) / scale && oh == (ih + scale - 1) / scale)
			break;
	if (scale > 8)
		return 0;
	if (format == UVCJ_YUY2 && (ow & 1))
		return 0;
	if (format == UVCJ_YUY2) {
		pitch = ow * 2;
		topdown = 1;	/* YUV DIBs are top-down whatever the sign */
	} else {
		pitch = ((ow * out->biBitCount + 31) / 32) * 4;
		topdown = out->biHeight < 0;
	}
	if (d != NULL) {
		d->format = format;
		d->scale = scale;
		d->width = ow;
		d->height = oh;
		if (topdown) {
			d->pitch = pitch;
			d->offset = 0;
		} else {
			d->pitch = -pitch;
			d->offset = pitch * (oh - 1);
		}
	}
	return 1;
}

static LRESULT uvcdecGetInfo(ICINFO *info, DWORD size)
{
	if (info == NULL)
		return sizeof(ICINFO);
	if (size < sizeof(ICINFO))
		return 0;
	ZeroMemory(info, sizeof(ICINFO));
	info->dwSize = sizeof(ICINFO);
	info->fccType = ICTYPE_VIDEO;
	info->fccHandler = FCC_MJPG;
	info->dwFlags = 0;
	info->dwVersion = 0x00010000;
	info->dwVersionICM = ICVERSION;
	uvcdecCopyName(info->szName, "uvcdec", 16);
	uvcdecCopyName(info->szDescription,
		"uvcdec MJPEG decoder (based on IJG libjpeg)", 128);
	return sizeof(ICINFO);
}

static LRESULT uvcdecQuery(const BITMAPINFOHEADER *in,
	const BITMAPINFOHEADER *out)
{
	if (!uvcdecInputOk(in))
		return ICERR_BADFORMAT;
	if (out != NULL && !uvcdecOutputOk(in, out, NULL))
		return ICERR_BADFORMAT;
	return ICERR_OK;
}

static LRESULT uvcdecGetFormat(const BITMAPINFOHEADER *in,
	BITMAPINFOHEADER *out)
{
	LONG pitch;

	if (!uvcdecInputOk(in))
		return out == NULL ? 0 : ICERR_BADFORMAT;
	if (out == NULL)
		return sizeof(BITMAPINFOHEADER);
	ZeroMemory(out, sizeof(BITMAPINFOHEADER));
	out->biSize = sizeof(BITMAPINFOHEADER);
	out->biWidth = in->biWidth;
	out->biHeight = uvcdecAbs(in->biHeight);
	out->biPlanes = 1;
	out->biBitCount = 24;
	out->biCompression = BI_RGB;
	pitch = ((out->biWidth * 24 + 31) / 32) * 4;
	out->biSizeImage = (DWORD)(pitch * out->biHeight);
	out->biXPelsPerMeter = in->biXPelsPerMeter;
	out->biYPelsPerMeter = in->biYPelsPerMeter;
	return ICERR_OK;
}

static LRESULT uvcdecBegin(UVCDEC *d, const BITMAPINFOHEADER *in,
	const BITMAPINFOHEADER *out)
{
	if (!uvcdecInputOk(in) || !uvcdecOutputOk(in, out, d))
		return ICERR_BADFORMAT;
	if (d->jpeg == NULL) {
		d->jpeg = uvcjOpen();
		if (d->jpeg == NULL)
			return ICERR_MEMORY;
	}
	if (uvcjSetScale(d->jpeg, d->scale) != UVCJ_OK)
		return ICERR_BADFORMAT;
	d->begun = 1;
	return ICERR_OK;
}

static LRESULT uvcdecFrame(UVCDEC *d, DWORD flags,
	const BITMAPINFOHEADER *in, const void *src,
	const BITMAPINFOHEADER *out, void *dst)
{
	DWORD size;
	int r;

	if (in == NULL || src == NULL || out == NULL || dst == NULL)
		return ICERR_BADPARAM;
	if (!d->begun) {
		LRESULT b = uvcdecBegin(d, in, out);
		if (b != ICERR_OK)
			return b;
	}
	/* a frame the caller will not show, or a repeat of the last one */
	if (flags & (ICDECOMPRESS_HURRYUP | ICDECOMPRESS_NULLFRAME))
		return ICERR_OK;
	size = in->biSizeImage;
	if (size == 0)
		return ICERR_BADFORMAT;
	r = UVCJ_EBAD;
	__try {
		r = uvcjDecode(d->jpeg, (const unsigned char *)src, size,
			d->format, (unsigned char *)dst + d->offset, d->pitch,
			(unsigned int)d->width, (unsigned int)d->height);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		/* a short source buffer: drop the decoder, rebuild on next frame */
		uvcjClose(d->jpeg);
		d->jpeg = NULL;
		d->begun = 0;
		return ICERR_ERROR;
	}
	if (r == UVCJ_OK)
		return ICERR_OK;
	if (r == UVCJ_ESIZE || r == UVCJ_EFORMAT)
		return ICERR_BADFORMAT;
	if (r == UVCJ_EMEM)
		return ICERR_MEMORY;
	return ICERR_ERROR;
}

static LRESULT uvcdecExQuery(const ICDECOMPRESSEX *x, int begin, UVCDEC *d)
{
	LONG ih;

	if (x == NULL)
		return ICERR_BADPARAM;
	if (!uvcdecInputOk(x->lpbiSrc))
		return ICERR_BADFORMAT;
	if (x->lpbiDst == NULL)
		return begin ? ICERR_BADFORMAT : ICERR_OK;
	if (!uvcdecOutputOk(x->lpbiSrc, x->lpbiDst, NULL))
		return ICERR_BADFORMAT;
	/* whole frames only: the rectangles must cover source and output */
	ih = uvcdecAbs(x->lpbiSrc->biHeight);
	if (x->xSrc != 0 || x->ySrc != 0 || x->xDst != 0 || x->yDst != 0)
		return ICERR_BADFORMAT;
	if ((x->dxSrc != 0 || x->dySrc != 0) &&
	    (x->dxSrc != x->lpbiSrc->biWidth || x->dySrc != ih))
		return ICERR_BADFORMAT;
	if ((x->dxDst != 0 || x->dyDst != 0) &&
	    (x->dxDst != x->lpbiDst->biWidth ||
	     x->dyDst != uvcdecAbs(x->lpbiDst->biHeight)))
		return ICERR_BADFORMAT;
	if (begin)
		return uvcdecBegin(d, x->lpbiSrc, x->lpbiDst);
	return ICERR_OK;
}

LRESULT CALLBACK DriverProc(DWORD id, HDRVR hdrvr, UINT msg,
	LPARAM lParam1, LPARAM lParam2)
{
	UVCDEC *d = (UVCDEC *)id;

	(void)hdrvr;
	switch (msg) {
	case DRV_LOAD:
	case DRV_FREE:
	case DRV_ENABLE:
	case DRV_DISABLE:
	case DRV_INSTALL:
	case DRV_REMOVE:
		return 1;
	case DRV_OPEN: {
		ICOPEN *o = (ICOPEN *)lParam2;

		if (o != NULL && o->fccType != ICTYPE_VIDEO)
			return 0;
		d = (UVCDEC *)LocalAlloc(LPTR, sizeof(UVCDEC));
		if (d == NULL) {
			if (o != NULL)
				o->dwError = ICERR_MEMORY;
			return 0;
		}
		d->magic = UVCDEC_MAGIC;
		if (o != NULL)
			o->dwError = ICERR_OK;
		return (LRESULT)d;
	}
	case DRV_CLOSE:
		if (d != NULL && d->magic == UVCDEC_MAGIC) {
			uvcjClose(d->jpeg);
			d->magic = 0;
			LocalFree((HLOCAL)d);
		}
		return 1;
	case DRV_QUERYCONFIGURE:
	case DRV_CONFIGURE:
		return 0;
	}

	if (d == NULL || d->magic != UVCDEC_MAGIC)
		return msg < DRV_USER ? 0 : ICERR_BADHANDLE;

	switch (msg) {
	case ICM_GETINFO:
		return uvcdecGetInfo((ICINFO *)lParam1, (DWORD)lParam2);
	case ICM_GETSTATE:
	case ICM_SETSTATE:
		return 0;
	case ICM_CONFIGURE:
	case ICM_ABOUT:
		return ICERR_UNSUPPORTED;
	case ICM_DECOMPRESS_QUERY:
		return uvcdecQuery((const BITMAPINFOHEADER *)lParam1,
			(const BITMAPINFOHEADER *)lParam2);
	case ICM_DECOMPRESS_GET_FORMAT:
		return uvcdecGetFormat((const BITMAPINFOHEADER *)lParam1,
			(BITMAPINFOHEADER *)lParam2);
	case ICM_DECOMPRESS_GET_PALETTE:
		return ICERR_BADFORMAT;
	case ICM_DECOMPRESS_BEGIN:
		return uvcdecBegin(d, (const BITMAPINFOHEADER *)lParam1,
			(const BITMAPINFOHEADER *)lParam2);
	case ICM_DECOMPRESS: {
		const ICDECOMPRESS *x = (const ICDECOMPRESS *)lParam1;

		if (x == NULL)
			return ICERR_BADPARAM;
		return uvcdecFrame(d, x->dwFlags, x->lpbiInput, x->lpInput,
			x->lpbiOutput, x->lpOutput);
	}
	case ICM_DECOMPRESS_END:
	case ICM_DECOMPRESSEX_END:
		d->begun = 0;
		return ICERR_OK;
	case ICM_DECOMPRESSEX_QUERY:
		return uvcdecExQuery((const ICDECOMPRESSEX *)lParam1, 0, d);
	case ICM_DECOMPRESSEX_BEGIN:
		return uvcdecExQuery((const ICDECOMPRESSEX *)lParam1, 1, d);
	case ICM_DECOMPRESSEX: {
		const ICDECOMPRESSEX *x = (const ICDECOMPRESSEX *)lParam1;

		if (x == NULL)
			return ICERR_BADPARAM;
		return uvcdecFrame(d, x->dwFlags, x->lpbiSrc, x->lpSrc,
			x->lpbiDst, x->lpDst);
	}
	case ICM_COMPRESS_QUERY:
	case ICM_DRAW_QUERY:
		return ICERR_BADFORMAT;
	}
	return msg < DRV_USER ? 0 : ICERR_UNSUPPORTED;
}
