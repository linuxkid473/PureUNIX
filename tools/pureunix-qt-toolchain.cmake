# CMake cross-compilation toolchain file for PureUnix (i686), used to build
# qtbase against the repo's cross toolchain (i686-elf-gcc/g++) + vendored
# newlib (third_party/newlib) + vendored libstdc++ (third_party/libstdcxx).
#
# This is the first CMake invocation anywhere in the PureUnix repo (see
# docs/qt-port.md section 1 — every other third-party port hand-lists
# source files in the top-level Makefile or vendors prebuilt autotools
# output; Qt is far too large for either, so its own real CMake build
# system is used, cross-compiling, the same way tools/build-newlib.sh and
# tools/build-libstdcxx.sh run real upstream configure/make once, offline).

# CMAKE_SYSTEM_NAME "Generic" (bare-metal/no-OS signal) was tried first and
# rejected: it makes CMake's own UNIX variable false, which excludes
# qtbase's real POSIX source files (io/qfilesystemengine_unix.cpp,
# kernel/qeventdispatcher_unix.cpp, kernel/qcore_unix.cpp,
# thread/qthread_unix.cpp, ... - all gated `CONDITION UNIX` in
# src/corelib/CMakeLists.txt) from the build entirely, while *other* Qt
# source still #includes their headers unconditionally based on the C++
# level Q_OS_UNIX macro (which our qsystemdetection.h patch does define) -
# a mismatch between the CMake-level and preprocessor-level "is this Unix"
# signals. PureUnix genuinely provides the POSIX shape those source files
# assume (real fork/open/read/write/select-ish semantics - see
# docs/qt-port.md), so "Linux" is used here as a pragmatic proxy purely to
# make CMake's own UNIX variable true, not a claim of glibc/Linux-syscall
# compatibility: nothing in the C++ preprocessor thinks it's Linux (no
# Q_OS_LINUX is ever defined - only Q_OS_PUREUNIX/Q_OS_UNIX, from that same
# patch), and the one real qtbase CMakeLists.txt condition keyed on plain
# `LINUX` (not `UNIX`) requires QT_BUILD_SHARED_LIBS, which this static-only
# build never sets anyway.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER i686-elf-gcc)
set(CMAKE_CXX_COMPILER i686-elf-g++)
set(CMAKE_ASM_COMPILER i686-elf-gcc)

# i686-elf-gcc has no start files (crt0.o/crti.o/crtn.o — it was built
# --without-headers, see docs/qt-port.md section 1), so it cannot link a
# normal executable without PureUnix's own crt0/linker script/syscalls
# glue. CMake's own initial "does this compiler work" check and every
# ordinary try_compile() Qt's feature detection performs would otherwise
# hit exactly the same problem tools/build-libstdcxx.sh had to work around
# for autoconf (see that script's fix #1/#2 comments). CMake has a real,
# sanctioned mechanism for this exact bare-metal-toolchain scenario:
# restricting try_compile() to only build a static library, never link a
# full executable — nothing in this build actually needs a linked
# executable at all (the deliverable is libQt6Core.a/libQt6Gui.a/
# libQt6Widgets.a; real executables come later, in Phase 7, using
# PureUnix's own Makefile + real crt0/linker.ld like every other port).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(NEWLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/newlib/i686-elf")
set(LIBSTDCXX_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/libstdcxx/i686-elf")
set(COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/../user/newlib_compat")

# -Wno-incompatible-pointer-types: this newlib target's <stdint.h> defines
# int32_t/uint32_t as long/unsigned long, not plain int/unsigned int (both
# are 4 bytes on i686 - a pure strict-typedef mismatch, not a real bug).
# GCC 14+ escalated -Wincompatible-pointer-types from a warning to a hard
# C-language error by default; bundled 3rdparty C sources written assuming
# glibc's int32_t==int (e.g. src/3rdparty/pcre2) fail to compile without
# this, unmodified. Affects only C (this is a hard error for C specifically,
# not C++, since GCC 14 didn't change C++'s stricter-already type rules).
# -DPCRE2_DISABLE_JIT: bundled PCRE2's JIT backend (sljit) hard-requires
# pthread_mutex_t (no threading exists on PureUnix at all, see
# docs/qt-port.md section 3 - not built yet at this point in the port) and
# generates+executes code at runtime, which is also just not something
# worth chasing here (no real W^X/self-modifying-code story on this
# target). QNX/UIKIT already get this exact same define for their own
# unrelated reasons (src/3rdparty/pcre2/CMakeLists.txt) - passed globally
# via a compiler define here instead of editing that upstream Qt file, one
# less-modified-upstream-file this port needs to track.
# -D__PUREUNIX__: i686-elf-gcc has no built-in macro identifying PureUnix
# (it's a from-scratch OS - no compiler ships knowing about it). Qt's own
# src/corelib/global/qsystemdetection.h has been given a new
# `#elif defined(__PUREUNIX__) -> Q_OS_PUREUNIX` branch (a genuine, minimal
# upstream Qt patch - see docs/qt-port.md's patch list) following the exact
# same "set from the mkspec, not autodetected" convention that file's
# existing Q_OS_VXWORKS branch already documents. This only reaches Qt's
# CMake-based build of Qt itself through our own C/CXX flags here, not
# mkspecs/pureunix-g++/qmake.conf (which qmake-based *application* builds
# against this Qt will read instead, so it defines the same macro too).
# -DHB_NO_MT: bundled HarfBuzz otherwise falls to its std::mutex-based
# mutex implementation, which needs real gthreads support libstdc++ here
# doesn't have (single-threaded gthr-single backend - see docs/qt-port.md
# section 3, no threading exists on PureUnix at all). HarfBuzz has its own
# built-in no-op single-threaded fallback specifically for exactly this
# case (hb-mutex.hh's #else branch), gated on this one define.
# -U__FLT16_MAX__: Qt's qfloat16 (src/corelib/global/qfloat16.h) uses GCC's
# native _Float16 type whenever __FLT16_MAX__ is predefined, which x86 GCC
# always does regardless of -march (it's a pure type-capability macro, not
# an "SSE2 is enabled" indicator). Actually using _Float16 arithmetic
# requires -msse2, which this repo's kernel context-switch code
# (kernel/task.c, arch/i386/*) does NOT save/restore at all (confirmed:
# zero fxsave/fxrstor/xsave anywhere) - not even for x87 FPU state, let
# alone SSE/XMM. Enabling SSE2 codegen for any userspace code (Qt or
# otherwise) before that's fixed would silently corrupt XMM register state
# across any context switch that lands mid-computation - a real,
# separate, unfixed architectural gap (see docs/qt-port.md), well beyond
# this port's scope. Undefining this one builtin macro forces qfloat16
# into its portable (non-native, plain float-based) fallback path instead,
# sidestepping the SSE2 requirement entirely without touching the kernel.
# -D_GNU_SOURCE: newlib's sys/features.h only exposes BSD/POSIX-2008
# declarations (sigaction() the *function* - the struct is always visible,
# getentropy(), etc.) when nothing more restrictive got defined first
# (see that file's "none of the above -> _DEFAULT_SOURCE" fallback) - some
# libstdc++ header ends up defining a narrower _POSIX_C_SOURCE/
# _XOPEN_SOURCE value before Qt's own C++ sources reach <signal.h>/
# <unistd.h>, silently hiding those declarations (the plain C build of
# libctest.c etc. never hits this, only this C++ build does). _GNU_SOURCE
# is the strongest, first-checked knob in that file and forces maximal
# visibility unconditionally, regardless of what else got defined first.
set(PUREUNIX_TARGET_FLAGS "-O2 -fno-stack-protector -fno-pic -fno-pie -m32 -march=i686 -Wno-incompatible-pointer-types -DPCRE2_DISABLE_JIT=1 -DHB_NO_MT=1 -D__PUREUNIX__ -U__FLT16_MAX__ -D_GNU_SOURCE")
set(PUREUNIX_C_INCLUDES "-isystem ${COMPAT_DIR} -isystem ${NEWLIB_DIR}/include")
# libstdc++'s own include dirs must come before newlib's — <cstdlib> etc
# do #include_next <stdlib.h>, which needs newlib's real header still
# ahead in the search path, not already consumed by this same directory
# (see docs/qt-port.md's Phase 2 writeup / the Makefile's LIBSTDCXX_CFLAGS
# comment for the exact same fact already hit once building cxxtest).
set(PUREUNIX_CXX_INCLUDES "-isystem ${LIBSTDCXX_DIR}/include/c++/16.1.0 -isystem ${LIBSTDCXX_DIR}/include/c++/16.1.0/i686-elf ${PUREUNIX_C_INCLUDES}")

set(CMAKE_C_FLAGS_INIT "${PUREUNIX_TARGET_FLAGS} ${PUREUNIX_C_INCLUDES}")
# -fpermissive: the C++ analog of -Wno-incompatible-pointer-types above
# (GCC's own error message points at this flag directly) - same root cause
# (int32_t/uint32_t is long/unsigned long on this newlib target, not plain
# int/unsigned int - both 4 bytes on i686, a pure strict-typedef mismatch),
# just showing up in C++ code (e.g. bundled HarfBuzz's qharfbuzzng.cpp
# passing an hb_codepoint_t* where Qt's own glyph_t* is expected) where the
# C-only warning flag has no effect.
set(CMAKE_CXX_FLAGS_INIT "-std=gnu++17 -fpermissive ${PUREUNIX_TARGET_FLAGS} ${PUREUNIX_CXX_INCLUDES}")
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY (above) covers CMake's own
# and most of Qt's try_compile() feature checks, but at least one real Qt
# config test (config.tests/arch — it inspects a linked binary's bytes for
# embedded architecture-marker strings, so it genuinely needs a real
# executable, not just an object file) drives its own manual
# `cmake --build` of a whole mini CMakeLists.txt project, bypassing that
# setting entirely. Same fix as tools/build-libstdcxx.sh's fix #1/#2:
# -nostartfiles plus linking in the real newlib_crt0/newlib_syscalls
# objects every actual PureUnix program uses. Built once by
# tools/build-qt.sh into build/qt-linkstub/ (not vendored - transient,
# rebuilt from the same repo sources every time).
set(LINKSTUB_DIR "${CMAKE_CURRENT_LIST_DIR}/../build/qt-linkstub")
set(PUREUNIX_LINKER_LD "${CMAKE_CURRENT_LIST_DIR}/../user/linker.ld")
# -T user/linker.ld: newlib_crt0.o references __ctors_start/__eh_frame_start/
# etc, symbols only user/linker.ld's own SECTIONS command defines - without
# it those are just undefined references at link time.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostartfiles -T ${PUREUNIX_LINKER_LD} -L${NEWLIB_DIR}/lib -L${LIBSTDCXX_DIR}/lib ${LINKSTUB_DIR}/newlib_crt0_asm.o ${LINKSTUB_DIR}/newlib_crt0.o ${LINKSTUB_DIR}/newlib_syscalls.o")

set(CMAKE_FIND_ROOT_PATH "${NEWLIB_DIR}" "${LIBSTDCXX_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# No dynamic linking exists on PureUnix at all (docs/qt-port.md section 2)
# — only static libraries make sense as a build product here.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
