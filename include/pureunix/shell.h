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

#endif
