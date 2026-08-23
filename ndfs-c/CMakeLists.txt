cmake_minimum_required(VERSION 3.10)
project(ndfs VERSION 0.0.7 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Library ──────────────────────────────────────────────────────────

# Host-file (stdio) block backend: PC-only convenience opener ndfs_open_file().
# ON for host builds; an embedded/freestanding target turns this OFF and
# supplies its own backend through ndfs_open_block().
option(NDFS_WITH_STDIO_BACKEND "Build the host-file (stdio) block backend" ON)

set(NDFS_SOURCES
    src/endian_util.c
    src/nd_time.c
    src/ndfs_name.c
    src/block_pointer.c
    src/master_block.c
    src/bit_file.c
    src/user_friend.c
    src/user_entry.c
    src/object_entry.c
    src/filesystem.c
    src/backend_stdio.c
    src/image_creator.c
    src/boot_loader.c
    src/sintran.c
    src/xat.c
    src/parity.c
    src/wildmatch.c
)

add_library(ndfs STATIC ${NDFS_SOURCES})

# Gate the stdio backend. When ON, the definition reaches both the library
# (backend_stdio.c compiles its body) and consumers (ndfs_open_file is declared
# in block_io.h) -- hence PUBLIC.
if(NDFS_WITH_STDIO_BACKEND)
    target_compile_definitions(ndfs PUBLIC NDFS_WITH_STDIO_BACKEND)
endif()

target_include_directories(ndfs
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Strict warnings
if(MSVC)
    target_compile_options(ndfs PRIVATE /W4)
else()
    target_compile_options(ndfs PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ── Tests ────────────────────────────────────────────────────────────

enable_testing()

set(TEST_SOURCES
    tests/test_main.c
    tests/test_endian.c
    tests/test_nd_time.c
    tests/test_block_pointer.c
    tests/test_master_block.c
    tests/test_bit_file.c
    tests/test_user_entry.c
    tests/test_object_entry.c
    tests/test_object_blocks.c
    tests/test_filesystem.c
    tests/test_image_creator.c
    tests/test_boot_loader.c
    tests/test_sintran.c
    tests/test_write_comprehensive.c
    tests/test_fsck.c
    tests/test_contiguous_files.c
    tests/test_xat.c
    tests/test_xat_copy.c
    tests/test_parity.c
    tests/test_wildmatch.c
    tests/test_golden.c
    tests/test_backend_stdio.c
    tests/test_damaged_media.c
)

add_executable(ndfs_tests ${TEST_SOURCES})
target_link_libraries(ndfs_tests PRIVATE ndfs)
target_include_directories(ndfs_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

if(NOT MSVC)
    target_compile_options(ndfs_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME ndfs_tests COMMAND ndfs_tests)

# ── ndtool CLI ──────────────────────────────────────────────────────

add_executable(ndtool
    tools/ndtool/ndtool.c
    tools/ndtool/cmd_list.c
    tools/ndtool/cmd_extract.c
    tools/ndtool/cmd_put.c
    tools/ndtool/cmd_delete.c
    tools/ndtool/cmd_user.c
    tools/ndtool/cmd_info.c
    tools/ndtool/cmd_create.c
    tools/ndtool/cmd_stat.c
    tools/ndtool/cmd_chmod.c
    tools/ndtool/cmd_shell.c
    tools/ndtool/parity.c
)
target_link_libraries(ndtool ndfs)
target_include_directories(ndtool PRIVATE ${CMAKE_SOURCE_DIR}/include tools/ndtool)

if(NOT MSVC)
    target_compile_options(ndtool PRIVATE -Wall -Wextra)
endif()
