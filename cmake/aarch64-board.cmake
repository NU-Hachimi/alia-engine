# Cross-compile for the Alia Pad board (Arduino UNO Q, Cortex-A53, Debian aarch64) from the PC with clang and lld,
# against the board's sysroot (its headers and libraries, from the Alia Pad devkit):
#   cmake -S . -B build-board -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-board.cmake -DALIA_SYSROOT=/path/to/sysroot
# (or ALIA_SYSROOT in the environment). docs/architecture.md §5.1.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT ALIA_SYSROOT)
  set(ALIA_SYSROOT "$ENV{ALIA_SYSROOT}")
endif()
if(NOT ALIA_SYSROOT OR NOT EXISTS "${ALIA_SYSROOT}/usr/include")
  message(FATAL_ERROR "the board's sysroot: -DALIA_SYSROOT=<dir> (or ALIA_SYSROOT), a directory with usr/include and usr/lib")
endif()
set(ALIA_SYSROOT "${ALIA_SYSROOT}" CACHE PATH "the board's sysroot")
set(CMAKE_SYSROOT "${ALIA_SYSROOT}")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")  # the Python module is a MODULE library
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53")

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${ALIA_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${ALIA_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${ALIA_SYSROOT}/usr/share/pkgconfig")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
