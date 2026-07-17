/* Override for glibc's <sys/un.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack (not even local/Unix-domain sockets). Declarations only;
 * nothing in the applets currently enabled in .config is network-related.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_SYS_UN_H
#define PUREUNIX_NEWLIB_COMPAT_SYS_UN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[108];
};


#ifdef __cplusplus
}
#endif
#endif
