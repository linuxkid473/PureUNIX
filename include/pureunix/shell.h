#ifndef PUREUNIX_SHELL_H
#define PUREUNIX_SHELL_H

void shell_run(void);
int shell_execute_line(const char *line);

/* Shared with kernel/users.c: applied by the login flow once a
 * username/password pair is verified, so the shell starts with the right
 * USER/HOME/SHELL and current directory. */
const char *shell_getenv(const char *key);
int shell_setenv(const char *key, const char *value);
void shell_set_home_cwd(const char *home);

/* Snapshots the whole shell env table as a NULL-terminated "KEY=VALUE"
 * array (same static-buffer contract as shell/sh.c's own callers: valid
 * until the next call, not thread/reentrant-safe — fine here, there's only
 * ever one env table and one caller at a time). kernel/main.c uses this to
 * launch the real login shell (see docs/shell.md's "Login Shell Exec"). */
char *const *shell_build_envp(void);

#endif
