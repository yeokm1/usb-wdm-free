/* jconfigint.h for uvcdec: libjpeg-turbo 3.2.0's src/jconfigint.h.in, filled
 * in by hand for MSVC 6.0, 32-bit x86 (README.md, "libjpeg-turbo"). Two
 * values differ from what CMake would find on a newer compiler: no
 * <intrin.h> (MSVC 6.0 has none; jpeg_nbits.c then uses its table) and no
 * thread-local storage (only the TurboJPEG API uses it, and that is not
 * built; __declspec(thread) does not work in a DLL that Windows 98 loads
 * with LoadLibrary). SIZE_MAX is supplied because MSVC 6.0 has no <stdint.h>.
 *
 * This file is part of libjpeg-turbo (src/jconfigint.h.in); see
 * turbo/README.ijg and turbo/LICENSE.md.
 */

/* libjpeg-turbo build number */
#define BUILD  "uvcdec"

/* How to hide global symbols. */
#define HIDDEN

/* Compiler's inline keyword */
#undef inline

/* How to obtain function inlining. */
#define INLINE  __inline

/* How to obtain thread-local storage */
#define THREAD_LOCAL

/* Define to the full name of this package. */
#define PACKAGE_NAME  "libjpeg-turbo"

/* Version number of package */
#define VERSION  "3.2.0"

/* The size of `size_t', as computed by sizeof. */
#define SIZEOF_SIZE_T  4

/* MSVC 6.0 knows no __has_attribute */
#define FALLTHROUGH

/* MSVC 6.0 has no <stdint.h> */
#ifndef SIZE_MAX
#define SIZE_MAX  ((size_t)-1)
#endif

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE  8      /* use 8 or 12 */
#endif

#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD

#if BITS_IN_JSAMPLE == 8

/* Support arithmetic encoding */
#define C_ARITH_CODING_SUPPORTED 1

/* Support arithmetic decoding */
#define D_ARITH_CODING_SUPPORTED 1

/* Use accelerated SIMD routines. */
#define WITH_SIMD 1

#define SIMD_ARCHITECTURE  I386

#endif
