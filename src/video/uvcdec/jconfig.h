/* jconfig.h for uvcdec: libjpeg-turbo 3.2.0's src/jconfig.h.in, filled in by
 * hand as its CMake build would fill it for a 32-bit Windows (MSVC) build
 * with the defaults (libjpeg v6b API, arithmetic coding, SIMD). There is no
 * CMake step: build.ps1 compiles the files itself (README.md, "libjpeg-turbo").
 *
 * This file is part of libjpeg-turbo (src/jconfig.h.in); see turbo/README.ijg
 * and turbo/LICENSE.md.
 */

/* Version ID for the JPEG library. */
#define JPEG_LIB_VERSION  62

/* libjpeg-turbo version */
#define LIBJPEG_TURBO_VERSION  3.2.0

/* libjpeg-turbo version in integer form */
#define LIBJPEG_TURBO_VERSION_NUMBER  3002000

/* Support arithmetic encoding when using 8-bit samples */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding when using 8-bit samples */
#define D_ARITH_CODING_SUPPORTED 1

/* Support in-memory source/destination managers */
#define MEM_SRCDST_SUPPORTED  1

/* Use accelerated SIMD routines when using 8-bit samples */
#define WITH_SIMD 1

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8
#endif

#ifdef _WIN32

#undef RIGHT_SHIFT_IS_UNSIGNED

/* Define "boolean" as unsigned char, not int, per Windows custom */
#ifndef __RPCNDR_H__            /* don't conflict if rpcndr.h already read */
typedef unsigned char boolean;
#endif
#define HAVE_BOOLEAN            /* prevent jmorecfg.h from redefining it */

/* Define "INT32" as int, not long, per Windows custom */
#if !(defined(_BASETSD_H_) || defined(_BASETSD_H))   /* don't conflict if basetsd.h already read */
typedef short INT16;
typedef signed int INT32;
#endif
#define XMD_H                   /* prevent jmorecfg.h from redefining it */

#endif
