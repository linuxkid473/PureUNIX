#ifndef HEADER_PureUnixProcessField
#define HEADER_PureUnixProcessField
/*
htop - pureunix/ProcessField.h
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.

No PureUNIX-specific columns beyond the standard set every platform
shares (PID/USER/PRIORITY/NICE/M_VIRT/M_RESIDENT/STATE/PERCENT_CPU/
PERCENT_MEM/TIME/Command — see Platform_defaultScreens in Platform.c) —
everything PureUNIX's procfs (fs/procfs.c) genuinely exposes maps onto
one of those already. See docs/htop-port.md.
*/

#define PLATFORM_PROCESS_FIELDS  \
   DUMMY_BUMP_FIELD = CWD,       \
   // End of list

#endif /* HEADER_PureUnixProcessField */
