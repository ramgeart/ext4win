cmake_minimum_required(VERSION 3.16)
project(ext4fs C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── User-configurable paths ──────────────────────────────────────────────────

set(E2FSPROGS_SOURCE_DIR ""
    CACHE PATH
    "Path to the e2fsprogs source tree (must already contain generated \
headers: ext2_types.h, ext2_err.h, com_err.h, config.h).")

set(WINFSP_INC_DIR ""
    CACHE PATH
    "Path to the WinFsp include directory (must contain fuse/fuse.h).")

set(WINFSP_LIB_DIR ""
    CACHE PATH
    "Path to the WinFsp library directory (must contain winfsp-x64.lib).")

# ── Validate paths ───────────────────────────────────────────────────────────

if(NOT E2FSPROGS_SOURCE_DIR OR NOT IS_DIRECTORY "${E2FSPROGS_SOURCE_DIR}")
    message(FATAL_ERROR
        "E2FSPROGS_SOURCE_DIR must point to an e2fsprogs source tree.\n"
        "Run ./configure on Linux/WSL first to produce the generated headers,\n"
        "then set -DE2FSPROGS_SOURCE_DIR=<path> when invoking cmake.")
endif()

if(NOT WINFSP_INC_DIR OR NOT EXISTS "${WINFSP_INC_DIR}/fuse.h")
    message(FATAL_ERROR
        "WINFSP_INC_DIR must point to WinFsp include directory with fuse.h.\n"
        "Set -DWINFSP_INC_DIR=<path> when invoking cmake.")
endif()

set(WINFSP_LIB "${WINFSP_LIB_DIR}/winfsp-x64.lib")
if(NOT WINFSP_LIB_DIR OR NOT EXISTS "${WINFSP_LIB}")
    message(FATAL_ERROR
        "WINFSP_LIB_DIR must point to directory containing winfsp-x64.lib.\n"
        "Set -DWINFSP_LIB_DIR=<path> when invoking cmake.")
endif()

# ── Helper directories ───────────────────────────────────────────────────────

set(E2FS_DIR "${E2FSPROGS_SOURCE_DIR}/lib/ext2fs")
set(ET_DIR   "${E2FSPROGS_SOURCE_DIR}/lib/et")
set(SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ── Compile-time definitions ─────────────────────────────────────────────────

add_compile_definitions(
    # Let Windows headers target Vista+ (required for some APIs)
    _WIN32_WINNT=0x0600
    # Tell e2fsprogs that we have a config.h
    HAVE_CONFIG_H
    # Suppress MSVC "unsafe function" warnings
    _CRT_SECURE_NO_WARNINGS
    # FUSE API version expected by fuse2fs
    FUSE_USE_VERSION=25
)

# ── libext2fs — minimum set of sources needed by fuse2fs ────────────────────

set(LIBEXT2FS_SOURCES
    ${E2FS_DIR}/alloc.c
    ${E2FS_DIR}/alloc_sb.c
    ${E2FS_DIR}/alloc_stats.c
    ${E2FS_DIR}/alloc_tables.c
    ${E2FS_DIR}/badblocks.c
    ${E2FS_DIR}/bb_inode.c
    ${E2FS_DIR}/bitmaps.c
    ${E2FS_DIR}/bitops.c
    ${E2FS_DIR}/block.c
    ${E2FS_DIR}/bmap.c
    ${E2FS_DIR}/closefs.c
    ${E2FS_DIR}/crc16.c
    ${E2FS_DIR}/crc32c.c
    ${E2FS_DIR}/csum.c
    ${E2FS_DIR}/dir_iterate.c
    ${E2FS_DIR}/dirblock.c
    ${E2FS_DIR}/dirhash.c
    ${E2FS_DIR}/expanddir.c
    ${E2FS_DIR}/ext_attr.c
    ${E2FS_DIR}/extent.c
    ${E2FS_DIR}/fallocate.c
    ${E2FS_DIR}/fileio.c
    ${E2FS_DIR}/freefs.c
    ${E2FS_DIR}/gen_bitmap.c
    ${E2FS_DIR}/gen_bitmap64.c
    ${E2FS_DIR}/get_num_dirs.c
    ${E2FS_DIR}/get_pathname.c
    ${E2FS_DIR}/getsectsize.c
    ${E2FS_DIR}/i_block.c
    ${E2FS_DIR}/icount.c
    ${E2FS_DIR}/ind_block.c
    ${E2FS_DIR}/initialize.c
    ${E2FS_DIR}/inline.c
    ${E2FS_DIR}/inline_data.c
    ${E2FS_DIR}/inode.c
    ${E2FS_DIR}/io_manager.c
    ${E2FS_DIR}/ismounted.c
    ${E2FS_DIR}/link.c
    ${E2FS_DIR}/lookup.c
    ${E2FS_DIR}/mkdir.c
    ${E2FS_DIR}/mkjournal.c
    ${E2FS_DIR}/mmp.c
    ${E2FS_DIR}/namei.c
    ${E2FS_DIR}/newdir.c
    ${E2FS_DIR}/openfs.c
    ${E2FS_DIR}/punch.c
    ${E2FS_DIR}/rbtree.c
    ${E2FS_DIR}/read_bb.c
    ${E2FS_DIR}/read_bb_file.c
    ${E2FS_DIR}/res_gdt.c
    ${E2FS_DIR}/rw_bitmaps.c
    ${E2FS_DIR}/sha256.c
    ${E2FS_DIR}/sha512.c
    ${E2FS_DIR}/sparse.c
    ${E2FS_DIR}/symlink.c
    ${E2FS_DIR}/undo_io.c
    ${E2FS_DIR}/unlink.c
    ${E2FS_DIR}/valid_blk.c
    ${E2FS_DIR}/version.c
    # Our Windows I/O back-end
    ${SRC_DIR}/windows_io.c
    # com_err (error reporting) from lib/et
    ${ET_DIR}/com_err.c
    ${ET_DIR}/error_message.c
    ${ET_DIR}/et_name.c
)

# ── libext2fs static library ─────────────────────────────────────────────────

add_library(ext2fs STATIC ${LIBEXT2FS_SOURCES})

target_include_directories(ext2fs PUBLIC
    # e2fsprogs top-level (for config.h)
    "${E2FSPROGS_SOURCE_DIR}"
    # lib/ (for ext2fs/*.h, et/*.h)
    "${E2FSPROGS_SOURCE_DIR}/lib"
    # Our Windows compatibility header
    "${SRC_DIR}"
    # WinFsp FUSE includes (mmp.c pulls in fuse.h indirectly on some builds)
    "${WINFSP_INC_DIR}"
)

if(MSVC)
    target_compile_options(ext2fs PRIVATE /W4)
else()
    target_compile_options(ext2fs PRIVATE
        -Wall
        -Wno-unused-parameter
        -Wno-sign-compare
    )
endif()

# ── ext4fs.exe ───────────────────────────────────────────────────────────────

add_executable(ext4fs
    "${E2FSPROGS_SOURCE_DIR}/misc/fuse2fs.c"
)

target_include_directories(ext4fs PRIVATE
    "${E2FSPROGS_SOURCE_DIR}"
    "${E2FSPROGS_SOURCE_DIR}/lib"
    "${SRC_DIR}"
    "${WINFSP_INC_DIR}"
)

target_link_libraries(ext4fs PRIVATE
    ext2fs
    "${WINFSP_LIB}"
)

if(MSVC)
    target_compile_options(ext4fs PRIVATE /W4)
else()
    target_compile_options(ext4fs PRIVATE
        -Wall
        -Wno-unused-parameter
        -Wno-sign-compare
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────────

install(TARGETS ext4fs RUNTIME DESTINATION bin)
