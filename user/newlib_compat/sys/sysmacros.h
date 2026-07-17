/* Override for glibc's <sys/sysmacros.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no device
 * nodes (no /dev, no mknod), so these encode/decode a device number
 * format nothing on this kernel ever actually produces or consumes; they
 * exist purely so code referencing major()/minor()/makedev() compiles.
 * Standard glibc bit layout (12-bit major, 20-bit minor) in case anything
 * ever does exercise these round-trip.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_SYSMACROS_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_SYSMACROS_H

#ifdef __cplusplus
extern "C" {
#endif

#define major(dev) (((unsigned int)(dev) >> 8) & 0xfff)
#define minor(dev) ((unsigned int)(dev) & 0xff)
#define makedev(maj, min) ((((unsigned int)(maj) & 0xfff) << 8) | ((unsigned int)(min) & 0xff))


#ifdef __cplusplus
}
#endif
#endif
