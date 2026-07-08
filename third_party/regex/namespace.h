/*
 * PureUNIX-specific stub: NetBSD's real namespace.h renames every libc
 * symbol here to a reserved-namespace alias (__weak_alias tricks) so this
 * regex engine can coexist with an application's own regcomp() etc. inside
 * NetBSD's own libc. Nothing here needs that - regcomp/regexec/regerror/
 * regfree are linked straight into BusyBox as ordinary global symbols - so
 * this just satisfies the unconditional #include "namespace.h" the vendored
 * sources expect, plus the couple of NetBSD internal macros
 * (_DIAGASSERT/__UNCONST) that newlib's sys/cdefs.h doesn't define.
 */
#ifndef _PUREUNIX_REGEX_NAMESPACE_H_
#define _PUREUNIX_REGEX_NAMESPACE_H_

#define _DIAGASSERT(x) /* nothing */

#ifndef __UNCONST
#define __UNCONST(a) ((void *)(unsigned long)(const void *)(a))
#endif

#ifndef __arraycount
#define __arraycount(a) (sizeof(a) / sizeof((a)[0]))
#endif

#endif
