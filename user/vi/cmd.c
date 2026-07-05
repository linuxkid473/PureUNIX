/* PureUNIX platform glue for Neatvi's external-process layer.
 *
 * Upstream cmd.c shells out via fork()+pipe()+execvp() to run filter
 * commands (":!cmd", "!motion", ":r !cmd", the "\~"/"@:" register-execute
 * commands, and ":make"). PureUNIX has no pipe(), no dup(), no PATH search,
 * and pu_exec() takes a single path with no argv — there's no way to
 * faithfully run an arbitrary shell command line here, so this is a stub:
 * every call reports failure, exactly like a real Neatvi would if execvp()
 * itself failed. Every call site in ex.c/vi.c already checks for that and
 * reports "cannot run" rather than crashing, so this degrades cleanly —
 * it's the one Neatvi feature area PureUNIX doesn't support. */
#include <stddef.h>
#include "vi.h"

char *cmd_pipe(char *cmd, char *ibuf, int oproc)
{
	(void)cmd;
	(void)ibuf;
	(void)oproc;
	return NULL;
}

char *cmd_unix(char *path, char *ibuf)
{
	(void)path;
	(void)ibuf;
	return NULL;
}

int cmd_exec(char *cmd)
{
	(void)cmd;
	return 1;
}
