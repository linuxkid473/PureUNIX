#ifndef HEADER_PureUnixProcessTable
#define HEADER_PureUnixProcessTable
/*
htop - pureunix/PureUnixProcessTable.h
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "Hashtable.h"
#include "Machine.h"
#include "ProcessTable.h"


typedef struct PureUnixProcessTable_ {
   ProcessTable super;
} PureUnixProcessTable;

#endif
