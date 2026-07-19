/* Override for <arpa/nameser_compat.h> — see arpa/nameser.h's own
 * comment in this same directory. Real, standard BIND-style aliases
 * (glibc/BSD have carried these exact names for decades), needed by
 * gio/gthreadedresolver.c's real source. */
#ifndef PUREUNIX_NEWLIB_COMPAT_ARPA_NAMESER_COMPAT_H
#define PUREUNIX_NEWLIB_COMPAT_ARPA_NAMESER_COMPAT_H

#include <arpa/nameser.h>

#define T_A ns_t_a
#define T_NS ns_t_ns
#define T_CNAME ns_t_cname
#define T_SOA ns_t_soa
#define T_MX ns_t_mx
#define T_TXT ns_t_txt
#define T_AAAA ns_t_aaaa
#define T_SRV ns_t_srv
#define T_NAPTR ns_t_naptr

#endif
