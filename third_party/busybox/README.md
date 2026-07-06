# BusyBox (vendored)

Prebuilt [BusyBox](https://busybox.net/) 1.36.1, cross-compiled for the bare-metal
`i686-elf` target with the same `i686-elf-gcc` used to build the rest of PureUNIX,
linked against the vendored newlib (`third_party/newlib/`) and PureUNIX's own
syscall glue (`user/newlib_syscalls.c`, `user/newlib_compat/`). Vendored as a
single prebuilt ELF (rather than BusyBox's own ~4400-file source tree) so the
build stays network-free, reproducible, and doesn't bloat this repository —
the same choice `third_party/newlib/` makes for the same reasons.

- `busybox.elf` — the multi-call binary itself. `tools/mkext2.py` installs it
  at `/bin/busybox.elf` on the EXT2 root filesystem, plus a symlink for every
  enabled applet (`/bin/ls -> busybox.elf`, `/bin/cat -> busybox.elf`, ...) —
  BusyBox dispatches on `argv[0]`'s basename (`applets/applets.c`), exactly
  like a real BusyBox install's `/bin/ls -> busybox`. FAT16 has no symlinks,
  so BusyBox isn't installed there — see `docs/userland.md`.
- `pureunix.config` — the exact BusyBox `.config` this binary was built from
  (which applets are enabled, and every other Kconfig option).
- `LICENSE` — BusyBox's own license (GPLv2). Vendoring a GPLv2 binary means
  this repository must also make BusyBox's *source* available or point to
  where it can be obtained — `tools/build-busybox.sh` and this file's own
  version/URL above are that pointer (upstream tarballs are permanent at
  `https://busybox.net/downloads/busybox-1.36.1.tar.bz2`).

## What's enabled and why

`pureunix.config` enables a deliberately small, non-forking applet set —
`[`, `basename`, `cat`, `cp`, `dirname`, `echo`, `false`, `ln`, `ls`, `mkdir`,
`mv`, `pwd`, `rm`, `rmdir`, `sleep`, `test`, `touch`, `true`, `wc`, `yes` —
chosen because none of them depend on `pipe()`/`dup2()`/real signal delivery,
which PureUNIX doesn't have yet (see `docs/syscalls.md`'s "Unimplemented
Syscalls"). BusyBox's own shell (`ash`/`hush`) and anything that forks a
pipeline is deliberately deferred to a later phase, once those syscalls
exist — see the project's own tracked plan for the phased rollout.

Both `CONFIG_SH_IS_ASH` and `CONFIG_SHELL_ASH` had to be explicitly disabled
(not just leaving the `ash` applet itself off) — otherwise BusyBox's Kbuild
still pulls in ash's shared runtime code as the default `/bin/sh` provider,
even with no shell applet selected.

## Platform gaps this build needed (see `user/newlib_compat/`)

BusyBox assumes a fairly complete POSIX host. Building it against newlib
(which has no OS identity of its own — see `third_party/newlib/README.md`)
surfaced a long list of headers/functions newlib doesn't provide out of the
box, all worked around in `user/newlib_compat/` (shadow headers searched
before newlib's own — see the Makefile's `NEWLIB_CFLAGS`) rather than by
patching BusyBox itself, per this project's own porting philosophy:

- **Missing headers newlib never shipped for a generic target** (only
  recognizes specific OSes like `__CYGWIN__`/`__rtems__`): `byteswap.h`,
  `endian.h`, `features.h`, `mntent.h`, `poll.h`, `sys/ioctl.h`,
  `sys/mman.h`, `sys/socket.h`, `sys/statfs.h`, `sys/sysmacros.h`,
  `sys/termios.h` (full Linux-compatible bit layout), `sys/utsname.h`,
  `sys/un.h`, `net/if.h`, `netdb.h`, `netinet/in.h`, `arpa/inet.h` — mostly
  declarations-only stubs for networking/device-node concepts PureUNIX
  doesn't have at all; `sys/ioctl.h` and `sys/termios.h` are real (backed by
  PureUNIX's actual `SYS_IOCTL`/termios syscalls).
- **Declared by newlib only for specific targets, needed generically**:
  `lstat()`/`mknod()` (`sys/stat.h`), `nanosleep()` (`time.h`), `getline()`
  (`stdio.h`, aliased to newlib's `__getline`), `SA_RESTART` and friends
  (`signal.h`), `UTIME_NOW`/`UTIME_OMIT` (`sys/stat.h`).
  `itoa()`/`utoa()` collide: newlib declares its own 3-arg extensions with
  those exact names, clashing with BusyBox's unrelated 1-arg internal
  helpers of the same name — `user/newlib_compat/stdlib.h` renames newlib's
  versions out of the way via a macro before `#include_next`ing the real
  header.
- **New PureUNIX syscalls added specifically to make this build link
  correctly**, not just header shims (see `docs/syscalls.md`):
  `SYS_GETUID`/`SYS_GETGID` (32/33), `SYS_UTIME` (34, real on EXT2 —
  `fs/ext2/mount.c`'s `ext2_utime()`), `SYS_GETTIMEOFDAY` (35). Plus
  `SYS_EXEC`'s argv/envp extension, `SYS_CHDIR`/`SYS_GETCWD`, real
  `SYS_CHMOD`/`SYS_CHOWN`, and the `DIR*` stream API — all added in earlier,
  separately-tracked phases of this same porting effort, not specifically
  for BusyBox, but all load-bearing for it.
- **Real functions newlib's libc.a doesn't implement despite declaring the
  prototype**: `dirname()` (standard in-place POSIX algorithm,
  `user/newlib_syscalls.c`).
- **Userspace-only conveniences with no kernel state to back them**:
  `umask()` (a plain static, since PureUNIX's own `mkdir`/`open` don't
  consult mode bits on creation yet), `getgroups()` (always reports zero
  supplementary groups — PureUNIX has none), `lchown()` (aliased straight
  to `chown()` — PureUNIX's `SYS_CHOWN` has no "don't follow the final
  symlink" variant).

## Rebuilding

Run `tools/build-busybox.sh` from the repo root (after `make` has built
`build/user/newlib_crt0_asm.o`/`newlib_crt0.o`/`newlib_syscalls.o` at least
once). It fetches BusyBox 1.36.1, applies `pureunix.config`, runs BusyBox's
own Kbuild to produce every applet object file, then performs the final
link itself (BusyBox's own link step assumes hosted-target crt0 files this
freestanding cross toolchain doesn't have).
