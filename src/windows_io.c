/*
 * windows_io.c — Windows Win32 I/O back-end for libext2fs
 *
 * Implements the ext2fs io_manager interface using Win32 CreateFile /
 * ReadFile / WriteFile instead of the POSIX open / read / write used by
 * unix_io.c.  This file is compiled only on _WIN32.
 *
 * Block-device paths (\\.\PhysicalDriveN, \\.\X:, etc.) are opened with
 * FILE_FLAG_NO_BUFFERING so that direct, sector-aligned I/O is possible.
 * Image files are opened with normal buffered I/O.
 *
 * The io_manager is exported as:
 *   io_manager windows_io_manager;
 *
 * Use it in place of unix_io_manager when initialising ext2fs on Windows.
 */

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Pull in the ext2fs types and io_manager definitions. */
#include "ext2fs/ext2_err.h"
#include "ext2fs/ext2fs.h"

/* ── Private per-channel state ─────────────────────────────────────────── */

#define EXT2_ET_MAGIC_WINDOWS_IO_CHANNEL 0x10eda001UL

struct windows_private_data {
    errcode_t   magic;          /* EXT2_ET_MAGIC_WINDOWS_IO_CHANNEL        */
    HANDLE      hFile;          /* Win32 file/device handle                 */
    int         flags;          /* IO_FLAG_* as passed to open()            */
    int         block_size;     /* current logical block size (bytes)       */
    int         is_block_device;/* non-zero when path is a \\.\xxx device   */
    LONGLONG    file_size;      /* total size in bytes (best-effort)        */
};

/* ── Forward declarations ───────────────────────────────────────────────── */

static errcode_t windows_open(const char *name, int flags,
                               io_channel *channel);
static errcode_t windows_close(io_channel channel);
static errcode_t windows_set_blksize(io_channel channel, int blksize);
static errcode_t windows_read_blk(io_channel channel, unsigned long block,
                                   int count, void *data);
static errcode_t windows_write_blk(io_channel channel, unsigned long block,
                                    int count, const void *data);
static errcode_t windows_flush(io_channel channel);
static errcode_t windows_write_byte(io_channel channel, unsigned long offset,
                                     int count, const void *data);
static errcode_t windows_set_option(io_channel channel, const char *option,
                                     const char *arg);
static errcode_t windows_get_stats(io_channel channel, io_stats *stats);
static errcode_t windows_read_blk64(io_channel channel,
                                     unsigned long long block,
                                     int count, void *data);
static errcode_t windows_write_blk64(io_channel channel,
                                      unsigned long long block,
                                      int count, const void *data);
static errcode_t windows_discard(io_channel channel,
                                  unsigned long long block,
                                  unsigned long long count);
static errcode_t windows_cache_readahead(io_channel channel,
                                          unsigned long long block,
                                          unsigned long long count);
static errcode_t windows_zeroout(io_channel channel,
                                  unsigned long long block,
                                  unsigned long long count);

/* ── io_manager descriptor ─────────────────────────────────────────────── */

static struct struct_io_manager struct_windows_manager = {
    EXT2_ET_MAGIC_IO_MANAGER,
    "Windows I/O Manager",
    windows_open,
    windows_close,
    windows_set_blksize,
    windows_read_blk,
    windows_write_blk,
    windows_flush,
    windows_write_byte,
    windows_set_option,
    windows_get_stats,
    windows_read_blk64,
    windows_write_blk64,
    windows_discard,
    windows_cache_readahead,
    windows_zeroout,
};

io_manager windows_io_manager = &struct_windows_manager;

/* ── Helpers ────────────────────────────────────────────────────────────── */

/*
 * Return non-zero when `name` looks like a Win32 device path (\\.\...).
 */
static int is_block_device_path(const char *name)
{
    return (name != NULL &&
            name[0] == '\\' && name[1] == '\\' &&
            name[2] == '.'  && name[3] == '\\');
}

/*
 * Convert a Win32 error code to a coarse errno equivalent so that
 * callers that inspect errno get a recognisable value.
 */
static int win32_error_to_errno(DWORD err)
{
    switch (err) {
    case ERROR_ACCESS_DENIED:        return EACCES;
    case ERROR_FILE_NOT_FOUND:       return ENOENT;
    case ERROR_PATH_NOT_FOUND:       return ENOENT;
    case ERROR_SHARING_VIOLATION:    return EBUSY;
    case ERROR_LOCK_VIOLATION:       return EBUSY;
    case ERROR_INVALID_PARAMETER:    return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:    return ENOMEM;
    case ERROR_HANDLE_EOF:           return 0;      /* not really an error */
    default:                         return EIO;
    }
}

/*
 * Seek to `byte_offset` then read `byte_count` bytes into `buf`.
 * Returns 0 on success or an ext2 error code on failure.
 */
static errcode_t raw_read(struct windows_private_data *data,
                           LONGLONG byte_offset, DWORD byte_count,
                           void *buf)
{
    LARGE_INTEGER li;
    DWORD bytes_read;

    li.QuadPart = byte_offset;
    if (!SetFilePointerEx(data->hFile, li, NULL, FILE_BEGIN)) {
        errno = win32_error_to_errno(GetLastError());
        return EXT2_ET_LLSEEK_FAILED;
    }

    if (!ReadFile(data->hFile, buf, byte_count, &bytes_read, NULL)) {
        errno = win32_error_to_errno(GetLastError());
        return EXT2_ET_SHORT_READ;
    }

    if (bytes_read != byte_count)
        return EXT2_ET_SHORT_READ;

    return 0;
}

/*
 * Seek to `byte_offset` then write `byte_count` bytes from `buf`.
 * Returns 0 on success or an ext2 error code on failure.
 */
static errcode_t raw_write(struct windows_private_data *data,
                            LONGLONG byte_offset, DWORD byte_count,
                            const void *buf)
{
    LARGE_INTEGER li;
    DWORD bytes_written;

    li.QuadPart = byte_offset;
    if (!SetFilePointerEx(data->hFile, li, NULL, FILE_BEGIN)) {
        errno = win32_error_to_errno(GetLastError());
        return EXT2_ET_LLSEEK_FAILED;
    }

    if (!WriteFile(data->hFile, buf, byte_count, &bytes_written, NULL)) {
        errno = win32_error_to_errno(GetLastError());
        return EXT2_ET_SHORT_WRITE;
    }

    if (bytes_written != byte_count)
        return EXT2_ET_SHORT_WRITE;

    return 0;
}

/* ── io_manager callbacks ───────────────────────────────────────────────── */

static errcode_t windows_open(const char *name, int flags,
                               io_channel *channel)
{
    io_channel  io   = NULL;
    struct windows_private_data *data = NULL;
    HANDLE      hFile;
    DWORD       dwAccess, dwShare, dwFlags;
    int         is_blkdev;
    LARGE_INTEGER size;
    errcode_t   retval;

    *channel = NULL;

    /* Allocate and zero-initialise the channel struct. */
    io = (io_channel) calloc(1, sizeof(struct struct_io_channel));
    if (!io)
        return EXT2_ET_NO_MEMORY;

    io->magic      = EXT2_ET_MAGIC_IO_CHANNEL;
    io->manager    = windows_io_manager;
    io->block_size = 1024;
    io->refcount   = 1;

    io->name = strdup(name);
    if (!io->name) {
        retval = EXT2_ET_NO_MEMORY;
        goto fail;
    }

    data = (struct windows_private_data *) calloc(1, sizeof(*data));
    if (!data) {
        retval = EXT2_ET_NO_MEMORY;
        goto fail;
    }
    data->magic      = EXT2_ET_MAGIC_WINDOWS_IO_CHANNEL;
    data->block_size = 1024;

    is_blkdev              = is_block_device_path(name);
    data->is_block_device  = is_blkdev;

    /* Access mode: read-only or read-write. */
    if (flags & IO_FLAG_RW)
        dwAccess = GENERIC_READ | GENERIC_WRITE;
    else
        dwAccess = GENERIC_READ;

    /* Allow concurrent readers; deny concurrent writers. */
    dwShare = FILE_SHARE_READ;

    /*
     * Block devices: FILE_FLAG_NO_BUFFERING enables direct sector-level
     * access without the page cache.  libext2fs already works in
     * block-aligned chunks so alignment is satisfied.
     * Image files: normal buffered I/O.
     */
    dwFlags = is_blkdev ? FILE_FLAG_NO_BUFFERING : FILE_ATTRIBUTE_NORMAL;

    hFile = CreateFileA(name,
                        dwAccess,
                        dwShare,
                        NULL,               /* default security */
                        OPEN_EXISTING,
                        dwFlags,
                        NULL);              /* no template */

    if (hFile == INVALID_HANDLE_VALUE) {
        errno  = win32_error_to_errno(GetLastError());
        retval = EXT2_ET_IO_CHANNEL_NOT_SUPPORTED;
        goto fail;
    }

    data->hFile = hFile;
    data->flags = flags;

    /* Best-effort size determination. */
    if (is_blkdev) {
        GET_LENGTH_INFORMATION gli;
        DWORD br;
        if (DeviceIoControl(hFile, IOCTL_DISK_GET_LENGTH_INFO,
                            NULL, 0, &gli, sizeof(gli), &br, NULL))
            data->file_size = gli.Length.QuadPart;
    } else {
        if (GetFileSizeEx(hFile, &size))
            data->file_size = size.QuadPart;
    }

    io->private_data = data;
    *channel = io;
    return 0;

fail:
    if (io) {
        free(io->name);
        free(io);
    }
    free(data);
    return retval;
}

static errcode_t windows_close(io_channel channel)
{
    struct windows_private_data *data;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    if (--channel->refcount > 0)
        return 0;

    /* Flush buffered writes before closing. */
    windows_flush(channel);

    if (data->hFile != INVALID_HANDLE_VALUE)
        CloseHandle(data->hFile);

    free(data);
    free(channel->name);
    free(channel);
    return 0;
}

static errcode_t windows_set_blksize(io_channel channel, int blksize)
{
    struct windows_private_data *data;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    channel->block_size = blksize;
    data->block_size    = blksize;
    return 0;
}

static errcode_t windows_read_blk(io_channel channel, unsigned long block,
                                   int count, void *buf)
{
    struct windows_private_data *data;
    LONGLONG byte_offset;
    DWORD    byte_count;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    /* Negative count means byte count (not block count). */
    byte_count  = (count < 0) ? (DWORD)(-(count))
                               : (DWORD)((unsigned long)count *
                                         channel->block_size);
    byte_offset = (LONGLONG)block * channel->block_size;
    return raw_read(data, byte_offset, byte_count, buf);
}

static errcode_t windows_write_blk(io_channel channel, unsigned long block,
                                    int count, const void *buf)
{
    struct windows_private_data *data;
    LONGLONG byte_offset;
    DWORD    byte_count;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    byte_count  = (count < 0) ? (DWORD)(-(count))
                               : (DWORD)((unsigned long)count *
                                         channel->block_size);
    byte_offset = (LONGLONG)block * channel->block_size;
    return raw_write(data, byte_offset, byte_count, buf);
}

static errcode_t windows_flush(io_channel channel)
{
    struct windows_private_data *data;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    if (channel->flags & IO_FLAG_RW)
        FlushFileBuffers(data->hFile);

    return 0;
}

static errcode_t windows_write_byte(io_channel channel,
                                     unsigned long offset,
                                     int count, const void *buf)
{
    struct windows_private_data *data;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    return raw_write(data, (LONGLONG)offset, (DWORD)count, buf);
}

static errcode_t windows_set_option(io_channel channel,
                                     const char *option,
                                     const char *arg)
{
    (void)channel; (void)option; (void)arg;
    return EXT2_ET_INVALID_ARGUMENT;
}

static errcode_t windows_get_stats(io_channel channel, io_stats *stats)
{
    (void)channel;
    if (stats)
        *stats = NULL;
    return 0;
}

static errcode_t windows_read_blk64(io_channel channel,
                                     unsigned long long block,
                                     int count, void *buf)
{
    struct windows_private_data *data;
    LONGLONG byte_offset;
    DWORD    byte_count;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    byte_count  = (count < 0) ? (DWORD)(-(count))
                               : (DWORD)((unsigned long long)(unsigned int)count *
                                          (unsigned long long)channel->block_size);
    byte_offset = (LONGLONG)(block * (unsigned long long)channel->block_size);
    return raw_read(data, byte_offset, byte_count, buf);
}

static errcode_t windows_write_blk64(io_channel channel,
                                      unsigned long long block,
                                      int count, const void *buf)
{
    struct windows_private_data *data;
    LONGLONG byte_offset;
    DWORD    byte_count;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    byte_count  = (count < 0) ? (DWORD)(-(count))
                               : (DWORD)((unsigned long long)(unsigned int)count *
                                          (unsigned long long)channel->block_size);
    byte_offset = (LONGLONG)(block * (unsigned long long)channel->block_size);
    return raw_write(data, byte_offset, byte_count, buf);
}

static errcode_t windows_discard(io_channel channel,
                                  unsigned long long block,
                                  unsigned long long count)
{
    (void)channel; (void)block; (void)count;
    /* TRIM/discard not implemented for Windows */
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t windows_cache_readahead(io_channel channel,
                                          unsigned long long block,
                                          unsigned long long count)
{
    (void)channel; (void)block; (void)count;
    return EXT2_ET_UNIMPLEMENTED;
}

static errcode_t windows_zeroout(io_channel channel,
                                  unsigned long long block,
                                  unsigned long long count)
{
    struct windows_private_data *data;
    void      *zero_buf;
    LONGLONG   byte_offset;
    LONGLONG   total_bytes;
    DWORD      chunk_size = 65536; /* 64 KiB chunks */
    errcode_t  retval = 0;

    EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
    data = (struct windows_private_data *) channel->private_data;

    total_bytes = (LONGLONG)(count * (unsigned long long)channel->block_size);
    byte_offset = (LONGLONG)(block * (unsigned long long)channel->block_size);

    zero_buf = calloc(1, chunk_size);
    if (!zero_buf)
        return EXT2_ET_NO_MEMORY;

    while (total_bytes > 0) {
        DWORD to_write = (total_bytes > (LONGLONG)chunk_size)
                         ? chunk_size : (DWORD)total_bytes;
        retval = raw_write(data, byte_offset, to_write, zero_buf);
        if (retval)
            break;
        byte_offset += to_write;
        total_bytes -= to_write;
    }

    free(zero_buf);
    return retval;
}

#endif /* _WIN32 */
