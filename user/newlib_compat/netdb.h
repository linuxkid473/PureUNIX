/* Override for glibc's <netdb.h>, which newlib doesn't have (see
 * user/newlib_compat/byteswap.h's header comment) — PureUNIX has no
 * network stack at all yet. include/libbb.h includes this unconditionally
 * (every applet's translation unit sees it, even ones with nothing to do
 * with networking), and libbb/xconnect.c (always compiled into libbb —
 * same "always-on shared library infrastructure" situation as
 * archival/libarchive/common.o, see user/newlib_compat/sys/stat.h's
 * comment) uses the full getaddrinfo()/getnameinfo() API, not just
 * gethostbyname(). This only needs to satisfy declarations — not provide
 * working name resolution — for the applets currently enabled in .config,
 * none of which are network-related themselves; extend further if a
 * network-adjacent applet is enabled later.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_NETDB_H
#define PUREUNIX_NEWLIB_COMPAT_NETDB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};

struct servent {
    char  *s_name;
    char **s_aliases;
    int    s_port;
    char  *s_proto;
};

struct addrinfo {
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    struct sockaddr *ai_addr;
    char            *ai_canonname;
    struct addrinfo *ai_next;
};

#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400

#define NI_NUMERICHOST 0x01
#define NI_NUMERICSERV 0x02
#define NI_NOFQDN      0x04
#define NI_NAMEREQD    0x08
#define NI_DGRAM       0x10
#define NI_NUMERICSCOPE 0x100

#define EAI_BADFLAGS  -1
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY    -10
#define EAI_SYSTEM    -11
#define EAI_OVERFLOW  -12

struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);
struct servent *getservbyport(int port, const char *proto);

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);
int getnameinfo(const struct sockaddr *addr, socklen_t addrlen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags);

/* h_errno / hstrerror(): gethostbyname()'s errno-equivalent. Declared as a
 * plain global here (real glibc makes it thread-local via a macro to a
 * function call, but PureUNIX has no threads, so a plain int is enough). */
extern int h_errno;
const char *hstrerror(int err);


#ifdef __cplusplus
}
#endif
#endif
