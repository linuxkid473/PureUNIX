/* Override for a header glibc/uClibc/musl provide but newlib (vendored,
 * third_party/newlib) doesn't — newlib was built for a bare i686-elf
 * target with no OS/libc identity, so it has no <byteswap.h> at all. Found
 * first via -isystem user/newlib_compat (see the Makefile's NEWLIB_CFLAGS
 * and user/newlib_compat/dirent.h's header comment for the same trick).
 * Minimal: just the bswap_16/32/64 macros BusyBox's include/platform.h
 * actually uses. */
#ifndef PUREUNIX_NEWLIB_COMPAT_BYTESWAP_H
#define PUREUNIX_NEWLIB_COMPAT_BYTESWAP_H

#ifdef __cplusplus
extern "C" {
#endif

#define bswap_16(x) \
    ((unsigned short)(((x) << 8) | ((x) >> 8)))

#define bswap_32(x) \
    ((((x) & 0x000000ffU) << 24) | \
     (((x) & 0x0000ff00U) << 8)  | \
     (((x) & 0x00ff0000U) >> 8)  | \
     (((x) & 0xff000000U) >> 24))

#define bswap_64(x) \
    ((((unsigned long long)bswap_32((unsigned int)(x))) << 32) | \
     (unsigned int)bswap_32((unsigned int)((x) >> 32)))


#ifdef __cplusplus
}
#endif
#endif
