/*
 * jinclude.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1994, Thomas G. Lane.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2022-2023, D. R. Commander.
 * uvcdec modifications (src/video/uvcdec/README.md):
 * Copyright (C) 2026, Yeo Kheng Meng: SNPRINTF for MSVC 6.0 (no variadic macros); JUINT64, as MSVC 6.0 has no long long.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file exists to provide a single place to fix any problems with
 * including the wrong system include files.  (Common problems are taken
 * care of by the standard jconfig symbols, but on really weird systems
 * you may have to edit this file.)
 *
 * NOTE: this file is NOT intended to be included by applications using the
 * JPEG library.  Most applications need only include jpeglib.h.
 */

#ifndef __JINCLUDE_H__
#define __JINCLUDE_H__

/* Include auto-config file to find out which system include files we need. */

#include "jconfig.h"            /* auto configuration options */
#include "jconfigint.h"
#define JCONFIG_INCLUDED        /* so that jpeglib.h doesn't do it again */

/*
 * Note that the core JPEG library does not require <stdio.h>;
 * only the default error handler and data source/destination modules do.
 * But we must pull it in because of the references to FILE in jpeglib.h.
 * You can remove those references if you want to compile without <stdio.h>.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* uvcdec: MSVC 6.0 has no long long */
#if defined(_MSC_VER) && _MSC_VER < 1400
typedef unsigned __int64 JUINT64;
#else
typedef unsigned long long JUINT64;
#endif

/*
 * These macros/inline functions facilitate using Microsoft's "safe string"
 * functions with Visual Studio builds without the need to scatter #ifdefs
 * throughout the code base.
 */


#if defined(_MSC_VER) && _MSC_VER >= 1400

#define SNPRINTF(str, n, format, ...) \
  _snprintf_s(str, n, _TRUNCATE, format, ##__VA_ARGS__)

#elif defined(_MSC_VER)  /* uvcdec: MSVC 6.0 has no variadic macros */

#define SNPRINTF  _snprintf

#else

#define SNPRINTF  snprintf

#endif


#ifndef NO_GETENV

#ifdef _MSC_VER

static INLINE int GETENV_S(char *buffer, size_t buffer_size, const char *name)
{
  size_t required_size;

  return (int)getenv_s(&required_size, buffer, buffer_size, name);
}

#else /* _MSC_VER */

#include <errno.h>

/* This provides a similar interface to the Microsoft/C11 getenv_s() function,
 * but other than parameter validation, it has no advantages over getenv().
 */

static INLINE int GETENV_S(char *buffer, size_t buffer_size, const char *name)
{
  char *env;

  if (!buffer) {
    if (buffer_size == 0)
      return 0;
    else
      return (errno = EINVAL);
  }
  if (buffer_size == 0)
    return (errno = EINVAL);
  if (!name) {
    *buffer = 0;
    return 0;
  }

  env = getenv(name);
  if (!env)
  {
    *buffer = 0;
    return 0;
  }

  if (strlen(env) + 1 > buffer_size) {
    *buffer = 0;
    return ERANGE;
  }

  strncpy(buffer, env, buffer_size);

  return 0;
}

#endif /* _MSC_VER */

#endif /* NO_GETENV */


#ifndef NO_PUTENV

#ifdef _WIN32

#define PUTENV_S(name, value)  _putenv_s(name, value)

#else

#include <errno.h>

/* This provides a similar interface to the Microsoft _putenv_s() function, but
 * other than parameter validation, it has no advantages over setenv().
 */

static INLINE int PUTENV_S(const char *name, const char *value)
{
  if (!name || !value)
    return (errno = EINVAL);

  setenv(name, value, 1);

  return errno;
}

#endif /* _WIN32 */

#endif /* NO_PUTENV */


#endif /* JINCLUDE_H */
