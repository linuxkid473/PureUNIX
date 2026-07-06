/* Override for a header glibc/uClibc/musl provide but newlib doesn't (see
 * user/newlib_compat/byteswap.h's header comment for why). i686 is always
 * little-endian, so this is unconditional — no other arch needs to be
 * represented here. */
#ifndef PUREUNIX_NEWLIB_COMPAT_ENDIAN_H
#define PUREUNIX_NEWLIB_COMPAT_ENDIAN_H

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN

#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#endif
