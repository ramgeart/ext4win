#!/usr/bin/env python3
"""
apply_patches.py — Apply Windows-compatibility changes to an e2fsprogs source tree.

Usage:
    python3 scripts/apply_patches.py <e2fsprogs-dir>

The script makes minimal, targeted insertions using pattern matching so it
is robust across minor version differences in e2fsprogs.
"""

import sys
import os
import re

def read_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()

def write_file(path, content):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)

def patch_ext2fs_h(src_dir):
    """Insert #include "windows_compat.h" near the top of ext2fs.h."""
    path = os.path.join(src_dir, 'lib', 'ext2fs', 'ext2fs.h')
    content = read_file(path)

    guard = '#ifdef _WIN32\n#include "windows_compat.h"\n#endif\n'
    if 'windows_compat.h' in content:
        print(f'  {path}: already patched, skipping')
        return

    # Insert before the first #ifndef include-guard in the header.
    marker = '#ifndef _EXT2FS_EXT2FS_H'
    if marker not in content:
        print(f'  WARNING: {path}: could not find marker "{marker}", skipping')
        return

    content = content.replace(marker, guard + marker, 1)
    write_file(path, content)
    print(f'  Patched: {path}')


def patch_getsectsize(src_dir):
    """Prepend Windows implementation to getsectsize.c."""
    path = os.path.join(src_dir, 'lib', 'ext2fs', 'getsectsize.c')
    content = read_file(path)

    if 'win_get_sectsize(' in content:
        print(f'  {path}: already patched, skipping')
        return

    win_impl = r"""#ifdef _WIN32
/*
 * Windows implementation: query sector size via DeviceIoControl.
 * Falls back to 512 for image files.
 */
#include "windows_compat.h"
#include <windows.h>
#include <winioctl.h>
#include <string.h>

static int win_get_sectsize(const char *file, int physical)
{
	HANDLE hFile;
	STORAGE_PROPERTY_QUERY query;
	STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment;
	DWORD bytes_returned;
	int result = 512;

	hFile = CreateFileA(file, 0,
	                    FILE_SHARE_READ | FILE_SHARE_WRITE,
	                    NULL, OPEN_EXISTING,
	                    FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return 512;

	memset(&query, 0, sizeof(query));
	query.PropertyId = StorageAccessAlignmentProperty;
	query.QueryType  = PropertyStandardQuery;

	if (DeviceIoControl(hFile,
	                    IOCTL_STORAGE_QUERY_PROPERTY,
	                    &query, sizeof(query),
	                    &alignment, sizeof(alignment),
	                    &bytes_returned, NULL)) {
		result = physical ? (int)alignment.BytesPerPhysicalSector
		                  : (int)alignment.BytesPerLogicalSector;
		if (result <= 0)
			result = 512;
	}
	CloseHandle(hFile);
	return result;
}

int ext2fs_get_device_sectsize(const char *file, int *sectsize)
{
	*sectsize = win_get_sectsize(file, 0);
	return 0;
}

int ext2fs_get_device_phys_sectsize(const char *file, int *sectsize)
{
	*sectsize = win_get_sectsize(file, 1);
	return 0;
}

#else /* !_WIN32 — original POSIX implementation */
"""

    win_endif = '\n#endif /* _WIN32 */\n'

    # Wrap entire original content.
    content = win_impl + content + win_endif
    write_file(path, content)
    print(f'  Patched: {path}')


def patch_ismounted(src_dir):
    """Prepend a Windows stub to ismounted.c."""
    path = os.path.join(src_dir, 'lib', 'ext2fs', 'ismounted.c')
    content = read_file(path)

    if 'Windows stub for ismounted.c' in content:
        print(f'  {path}: already patched, skipping')
        return

    win_stub = r"""#ifdef _WIN32
/*
 * Windows stub for ismounted.c
 *
 * We cannot query the kernel mount table on Windows, so we always report
 * the device as unmounted.  This is safe for read-only image-file access
 * and acceptable for read-write mounts where the operator is aware.
 */
#include "config.h"
#include "windows_compat.h"
#include "ext2fs/ext2fs.h"

errcode_t ext2fs_check_if_mounted(const char *file, int *mount_flags)
{
	(void)file;
	*mount_flags = 0;
	return 0;
}

errcode_t ext2fs_check_mount_point(const char *device, int *mount_flags,
                                    char *mtpt, int mtlen)
{
	(void)device; (void)mtpt; (void)mtlen;
	*mount_flags = 0;
	return 0;
}

#else /* !_WIN32 — original POSIX implementation follows */
"""

    win_endif = '\n#endif /* _WIN32 */\n'
    content = win_stub + content + win_endif
    write_file(path, content)
    print(f'  Patched: {path}')


def patch_fuse2fs(src_dir):
    """Add Windows compatibility guards to misc/fuse2fs.c."""
    path = os.path.join(src_dir, 'misc', 'fuse2fs.c')
    content = read_file(path)

    if 'windows_compat.h' in content:
        print(f'  {path}: already patched, skipping')
        return

    # ── 1. Insert windows_compat.h include before #include "config.h" ──────
    compat_block = (
        '#ifdef _WIN32\n'
        '#include "windows_compat.h"\n'
        '#endif\n\n'
    )
    content = content.replace('#include "config.h"',
                               compat_block + '#include "config.h"', 1)

    # ── 2. Guard POSIX-only headers ─────────────────────────────────────────
    posix_headers = [
        '#include <sys/ioctl.h>',
        '#include <sys/mount.h>',
        '#include <sys/statvfs.h>',
        '#include <sys/wait.h>',
        '#include <pwd.h>',
        '#include <grp.h>',
        '#include <mntent.h>',
        '#include <syslog.h>',
    ]
    for hdr in posix_headers:
        if hdr in content:
            content = content.replace(
                hdr,
                f'#ifndef _WIN32\n{hdr}\n#endif'
            )

    # ── 3. Append Windows syslog/uid stubs after the include block ──────────
    stub_block = (
        '\n#ifdef _WIN32\n'
        '/* uid/gid stubs: on Windows all files appear owned by root (uid=0/gid=0).\n'
        ' * We avoid redefining struct passwd/group (which may be provided by MinGW).\n'
        ' * Instead we guard them with __MINGW_H and provide fallbacks only when\n'
        ' * those types are truly absent. */\n'
        '#ifndef _PWD_H\n'
        '#ifndef _MINGW_PWD_H\n'
        'struct passwd { char *pw_name; unsigned int pw_uid; unsigned int pw_gid; };\n'
        'static inline struct passwd *getpwuid(unsigned int u) { (void)u; return NULL; }\n'
        '#endif\n'
        '#endif\n'
        '#ifndef _GRP_H\n'
        '#ifndef _MINGW_GRP_H\n'
        'struct group  { char *gr_name; unsigned int gr_gid; };\n'
        'static inline struct group  *getgrgid(unsigned int g) { (void)g; return NULL; }\n'
        '#endif\n'
        '#endif\n'
        '#ifndef LOG_ERR\n'
        '#define LOG_ERR 3\n'
        '#endif\n'
        '#define syslog(pri, ...)    fprintf(stderr, __VA_ARGS__)\n'
        '#define openlog(id, o, f)   do {} while (0)\n'
        '#define closelog()          do {} while (0)\n'
        '#endif /* _WIN32 */\n'
    )

    # Insert just before the first function definition (heuristic: first
    # occurrence of "^static " or "^int main(")
    m = re.search(r'^(static |int main\b)', content, re.MULTILINE)
    if m:
        content = content[:m.start()] + stub_block + content[m.start():]
    else:
        content += stub_block

    write_file(path, content)
    print(f'  Patched: {path}')


def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <e2fsprogs-source-dir>', file=sys.stderr)
        sys.exit(1)

    src_dir = sys.argv[1]
    if not os.path.isdir(src_dir):
        print(f'Error: {src_dir} is not a directory', file=sys.stderr)
        sys.exit(1)

    print(f'Applying Windows compatibility patches to: {src_dir}')
    patch_ext2fs_h(src_dir)
    patch_getsectsize(src_dir)
    patch_ismounted(src_dir)
    patch_fuse2fs(src_dir)
    print('Done.')


if __name__ == '__main__':
    main()
