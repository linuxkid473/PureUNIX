#ifndef PUREUNIX_USERS_H
#define PUREUNIX_USERS_H

#include <pureunix/config.h>
#include <pureunix/types.h>

#define USERS_MAX_NAME 32

/* One /etc/passwd row, parsed. Mirrors the classic
 * name:x:uid:gid:gecos:home:shell layout (see tools/mkext2.py's seed
 * /etc/passwd and docs/userland.md). */
typedef struct user_record {
    char name[USERS_MAX_NAME];
    uid_t uid;
    gid_t gid;
    char home[PUREUNIX_MAX_PATH];
    char shell[PUREUNIX_MAX_PATH];
} user_record_t;

/* True until /etc/shadow exists — i.e. no root password has ever been set
 * on this disk image. */
bool users_first_boot(void);

/* Interactive: prompts for and stores a root password, then prints a short
 * first-boot setup guide. Call once, only when users_first_boot() is true. */
void users_first_boot_setup(void);

/* Interactive: loops "login:"/"Password:" prompts until a valid
 * username/password pair is entered, then applies that user's uid/gid to
 * the calling task and sets USER/HOME/SHELL for the shell. Never returns
 * until login succeeds. */
void users_login(void);

/* Look up a /etc/passwd entry by name. Returns 0 and fills *out, or -1 if
 * no such user exists. */
int users_lookup(const char *name, user_record_t *out);

/* Interactive: prompts twice for a new password, creates USERS_MAX_NAME-1
 * uid/gid >= 1000 account `name`, appends it to /etc/passwd and
 * /etc/shadow, and creates its home directory. Returns 0 on success, -1 on
 * failure (already prints the reason). */
int users_adduser(const char *name);

/* Interactive: prompts twice for a new password and rewrites the /etc/shadow
 * entry for the (already-existing) user `name`. Returns 0 on success, -1 on
 * failure (already prints the reason). */
int users_passwd(const char *name);

#endif
