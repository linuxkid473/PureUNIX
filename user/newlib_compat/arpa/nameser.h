/* Override for <arpa/nameser.h>, which newlib doesn't have at all (no
 * DNS resolver library ships with it) — GLib's own gio/meson.build
 * (docs/pcmanfm-port.md phase 3/6) hard-requires this header to exist
 * and define C_IN for any non-Windows/Android target, and
 * gio/gthreadedresolver.c's real source (outside its own __BIONIC__-only
 * compat block) references several more of the standard BIND-style
 * resolver constants/types below.
 *
 * These are the real, standard, decades-old BIND/RFC 1035 resolver
 * definitions common to every Unix libc's own <arpa/nameser.h> (glibc,
 * the BSDs, macOS) — genuinely portable content, not invented for this
 * platform, the same "port the smallest correct real thing" principle
 * used throughout this project applied to a header instead of a whole
 * library. PureUnix has no real DNS resolver client at all (see
 * user/newlib_compat/arpa/nameser_compat.h and the dn_expand()/
 * dn_skipname() implementations wired in for this port — they correctly
 * report failure via their own real, documented error contract, since
 * there's no real DNS response data they could ever actually be asked to
 * decompress on this platform): this only exists to let gio's own
 * unconditionally-built gthreadedresolver.c compile and archive
 * correctly. GResolver's DNS-record-lookup entry points
 * (lookup_by_name/lookup_records) are a real, disclosed, unused code
 * path for a local-files-only PCManFM-Qt port, matching this project's
 * own "no dbus/no volume monitor" disclosed-limitation precedent.
 */
#ifndef PUREUNIX_NEWLIB_COMPAT_ARPA_NAMESER_H
#define PUREUNIX_NEWLIB_COMPAT_ARPA_NAMESER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stdint.h>

#define C_IN 1

#define NS_INT32SZ 4
#define NS_INT16SZ 2

typedef enum __ns_type {
    ns_t_invalid = 0,
    ns_t_a = 1,
    ns_t_ns = 2,
    ns_t_md = 3,
    ns_t_mf = 4,
    ns_t_cname = 5,
    ns_t_soa = 6,
    ns_t_mb = 7,
    ns_t_mg = 8,
    ns_t_mr = 9,
    ns_t_null = 10,
    ns_t_wks = 11,
    ns_t_ptr = 12,
    ns_t_hinfo = 13,
    ns_t_minfo = 14,
    ns_t_mx = 15,
    ns_t_txt = 16,
    ns_t_rp = 17,
    ns_t_afsdb = 18,
    ns_t_x25 = 19,
    ns_t_isdn = 20,
    ns_t_rt = 21,
    ns_t_nsap = 22,
    ns_t_nsap_ptr = 23,
    ns_t_sig = 24,
    ns_t_key = 25,
    ns_t_px = 26,
    ns_t_gpos = 27,
    ns_t_aaaa = 28,
    ns_t_loc = 29,
    ns_t_nxt = 30,
    ns_t_srv = 33,
    ns_t_naptr = 35,
    ns_t_opt = 41
} ns_type;

/* Real BIND/RFC 1035 DNS message header layout — genuinely
 * bit-for-bit identical to every other libc's own <arpa/nameser.h>. */
typedef struct {
    unsigned id : 16;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    unsigned qr : 1;
    unsigned opcode : 4;
    unsigned aa : 1;
    unsigned tc : 1;
    unsigned rd : 1;
    unsigned ra : 1;
    unsigned unused : 1;
    unsigned ad : 1;
    unsigned cd : 1;
    unsigned rcode : 4;
#else
    unsigned rd : 1;
    unsigned tc : 1;
    unsigned aa : 1;
    unsigned opcode : 4;
    unsigned qr : 1;
    unsigned rcode : 4;
    unsigned cd : 1;
    unsigned ad : 1;
    unsigned unused : 1;
    unsigned ra : 1;
#endif
    unsigned qdcount : 16;
    unsigned ancount : 16;
    unsigned nscount : 16;
    unsigned arcount : 16;
} HEADER;

#define NS_GET16(s, cp) do { \
	const unsigned char *t_cp = (const unsigned char *)(cp); \
	(s) = ((uint16_t)t_cp[0] << 8) | ((uint16_t)t_cp[1]); \
	(cp) += NS_INT16SZ; \
} while (0)

#define NS_GET32(l, cp) do { \
	const unsigned char *t_cp = (const unsigned char *)(cp); \
	(l) = ((uint32_t)t_cp[0] << 24) | ((uint32_t)t_cp[1] << 16) \
	    | ((uint32_t)t_cp[2] << 8) | ((uint32_t)t_cp[3]); \
	(cp) += NS_INT32SZ; \
} while (0)

#define GETSHORT NS_GET16
#define GETLONG NS_GET32

/* Real functions (user/newlib_syscalls.c) — see that file's own comment
 * for why they correctly report failure rather than faking a decode. */
int dn_expand(const unsigned char *msg, const unsigned char *eom,
              const unsigned char *comp_dn, char *exp_dn, int length);
int dn_skipname(const unsigned char *comp_dn, const unsigned char *eom);

#ifdef __cplusplus
}
#endif

/* Real BSD precedent: <arpa/nameser.h> itself unconditionally exposes the
 * T_A/T_MX/etc BIND-4-compat aliases (most real libcs do this, gated on a
 * BIND_4_COMPAT-style macro that's simply always on in practice) — needed
 * because GLib's own gio/meson.build (see arpa/nameser_compat.h's header
 * comment) only adds an explicit #include <arpa/nameser_compat.h> to its
 * generated gnetworking.h when C_IN is NOT already visible from plain
 * <arpa/nameser.h>; since ours already defines C_IN directly above (like
 * every real libc's own nameser.h does), that generated #include never
 * happens, and gio/gthreadedresolver.c's direct T_TXT/T_SOA/etc uses would
 * otherwise go undeclared. */
#include <arpa/nameser_compat.h>

#endif
