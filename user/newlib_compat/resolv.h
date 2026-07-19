/* Override for <resolv.h>, which newlib doesn't have at all — see
 * user/newlib_compat/arpa/nameser.h's own comment (same reasoning: GLib's
 * gio/meson.build, docs/pcmanfm-port.md phase 3/6, requires this to link
 * a real res_query() for its GResolver DNS-record-lookup backend).
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_RESOLV_H
#define PUREUNIX_NEWLIB_COMPAT_RESOLV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Real (user/newlib_syscalls.c) — see that file's own comment for why
 * it correctly reports "no answer" rather than fabricating a response:
 * PureUnix has no real DNS resolver client at all. Parameter named
 * `dnsclass`, not `class` (a real BIND resolv.h would also avoid this —
 * `class` is a C++ keyword, and this header must be includable from C++
 * translation units too). */
int res_query(const char *dname, int dnsclass, int type,
               unsigned char *answer, int anslen);

#ifdef __cplusplus
}
#endif
#endif
