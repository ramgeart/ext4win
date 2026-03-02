/*
 * windows_compat.h — POSIX compatibility shims for Windows builds
 *
 * Include this header (indirectly via ext2fs.h) to shim over the POSIX
 * primitives that libext2fs and fuse2fs assume but that are absent on
 * Windows / MSVC / MinGW-w64.
 */

#pragma once

#ifdef _WIN32

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ssize_t — defined by MinGW-w64 but not by MSVC */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

/* Symbolic-link test — ext4 has symlinks but Windows stat has no S_ISLNK */
#ifndef S_ISLNK
#  define S_ISLNK(m) 0
#endif

/* O_NOFOLLOW — meaningful only on POSIX; ignored on Windows */
#ifndef O_NOFOLLOW
#  define O_NOFOLLOW 0
#endif

/* O_DIRECT — mapped to FILE_FLAG_NO_BUFFERING in windows_io.c; define
 * the flag to 0 so that any code that tests it compiles cleanly. */
#ifndef O_DIRECT
#  define O_DIRECT 0
#endif

/* sys/ioctl.h does not exist on Windows; callers that include it
 * conditionally via HAVE_SYS_IOCTL_H will be fine once config.h is
 * generated with that symbol unset.  Anything that includes it
 * unconditionally needs the patch in src/patches/. */

/* sys/mount.h / sys/statvfs.h do not exist on Windows either; same
 * story — guard via generated config.h or patches. */

/* GCC __attribute__ works natively; nothing needed for MinGW-w64.
 * For MSVC add:  #define __attribute__(x) */

#endif /* _WIN32 */
