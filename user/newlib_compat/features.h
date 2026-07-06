/* Override for glibc's <features.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment). BusyBox's
 * include/platform.h includes this whenever _NEWLIB_VERSION is defined
 * (which it always is, building against this vendored newlib) purely to
 * pull in glibc feature-test macros (__GLIBC_PREREQ, __USE_GNU, ...) that
 * nothing in BusyBox's own source actually references directly — an empty
 * stub is enough to satisfy the #include. */
#ifndef PUREUNIX_NEWLIB_COMPAT_FEATURES_H
#define PUREUNIX_NEWLIB_COMPAT_FEATURES_H
#endif
