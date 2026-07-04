/*
 * kernel/users.c — accounts, login, and the first-boot setup wizard.
 *
 * Storage is two flat files, classic-Unix style:
 *   /etc/passwd  name:x:uid:gid:gecos:home:shell     (already seeded by
 *                                                      tools/mkext2.py)
 *   /etc/shadow  name:hash                            (created here, on
 *                                                      first boot)
 *
 * /etc/shadow existing at all is what "first boot" means (see
 * users_first_boot()): a fresh disk image ships /etc/passwd but no shadow
 * file, so the very first boot always runs the setup wizard.
 *
 * There is no crypto library in this freestanding kernel, so passwords are
 * hashed with a salted FNV-1a mix (hash_password() below) — enough to keep
 * /etc/shadow from holding plaintext, not a defense against an offline
 * attack. Good enough for a hobby OS; not a real KDF.
 */
#include <pureunix/config.h>
#include <pureunix/keyboard.h>
#include <pureunix/memory.h>
#include <pureunix/shell.h>
#include <pureunix/stdio.h>
#include <pureunix/stdlib.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/users.h>
#include <pureunix/vfs.h>

#define PASSWD_PATH "/etc/passwd"
#define SHADOW_PATH "/etc/shadow"
#define HOME_BASE   "/home"
#define SHADOW_MAX_FILE 4096
#define LINE_SCRATCH_MAX 192

/* ---- raw line input (login/adduser/passwd run before/outside the shell's
 * own line editor, so this is a small standalone reader: no history, no tab
 * completion, just backspace + enter, with optional '*' masking). ---- */
static size_t read_line_raw(char *buf, size_t max, bool mask)
{
    size_t len = 0;
    for (;;) {
        int key = keyboard_getkey();
        if (key == KEY_ENTER) {
            putchar('\n');
            buf[len] = '\0';
            return len;
        }
        if (key == KEY_CTRL_C) {
            buf[0] = '\0';
            putchar('\n');
            return 0;
        }
        if (key == KEY_BACKSPACE) {
            if (len > 0) {
                len--;
                printf("\b \b");
            }
            continue;
        }
        if (key >= 32 && key < 127 && len + 1 < max) {
            buf[len++] = (char)key;
            putchar(mask ? '*' : (char)key);
        }
    }
}

/* Not a cryptographic hash (no crypto library exists in this kernel) — see
 * the file header. Salts with the username so two accounts sharing a
 * password don't share a hash. */
static void put_hex32(char *buf, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }
}

static void hash_password(const char *username, const char *password, char out[17])
{
    uint32_t h1 = 2166136261u;
    for (const char *s = username; *s; ++s) { h1 ^= (uint8_t)*s; h1 *= 16777619u; }
    h1 ^= 0x5Au; h1 *= 16777619u;
    for (const char *s = password; *s; ++s) { h1 ^= (uint8_t)*s; h1 *= 16777619u; }

    uint32_t h2 = h1 ^ 0x9E3779B9u;
    for (const char *s = password; *s; ++s) { h2 ^= (uint8_t)*s; h2 *= 16777619u; }
    for (const char *s = username; *s; ++s) { h2 ^= (uint8_t)*s; h2 *= 16777619u; }

    put_hex32(out, h1);
    put_hex32(out + 8, h2);
    out[16] = '\0';
}

/* kmalloc'd NUL-terminated copy of a file's contents, or NULL if it doesn't
 * exist. Caller kfree()s it. */
static char *slurp(const char *path)
{
    uint8_t *data;
    size_t size;
    if (vfs_read_file(path, &data, &size) != 0) {
        return NULL;
    }
    char *buf = kmalloc(size + 1);
    if (!buf) {
        kfree(data);
        return NULL;
    }
    memcpy(buf, data, size);
    buf[size] = '\0';
    kfree(data);
    return buf;
}

int users_lookup(const char *name, user_record_t *out)
{
    if (!name || !*name) {
        return -1;
    }
    char *buf = slurp(PASSWD_PATH);
    if (!buf) {
        return -1;
    }

    int found = -1;
    char *saveline;
    char *line = strtok_r(buf, "\n", &saveline);
    while (line) {
        char linecopy[LINE_SCRATCH_MAX];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *savefield;
        char *uname = strtok_r(linecopy, ":", &savefield);
        if (uname && strcmp(uname, name) == 0) {
            strtok_r(NULL, ":", &savefield); /* password placeholder ("x") */
            char *uid_s  = strtok_r(NULL, ":", &savefield);
            char *gid_s  = strtok_r(NULL, ":", &savefield);
            strtok_r(NULL, ":", &savefield); /* gecos */
            char *home_s  = strtok_r(NULL, ":", &savefield);
            char *shell_s = strtok_r(NULL, ":", &savefield);

            strncpy(out->name, name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            out->uid = uid_s ? (uid_t)atoi(uid_s) : 0;
            out->gid = gid_s ? (gid_t)atoi(gid_s) : 0;
            strncpy(out->home, home_s ? home_s : "/", sizeof(out->home) - 1);
            out->home[sizeof(out->home) - 1] = '\0';
            strncpy(out->shell, shell_s ? shell_s : "/bin/sh", sizeof(out->shell) - 1);
            out->shell[sizeof(out->shell) - 1] = '\0';
            found = 0;
            break;
        }
        line = strtok_r(NULL, "\n", &saveline);
    }
    kfree(buf);
    return found;
}

static uid_t next_free_uid(void)
{
    char *buf = slurp(PASSWD_PATH);
    uid_t max_uid = 999;
    if (buf) {
        char *saveline;
        char *line = strtok_r(buf, "\n", &saveline);
        while (line) {
            char linecopy[LINE_SCRATCH_MAX];
            strncpy(linecopy, line, sizeof(linecopy) - 1);
            linecopy[sizeof(linecopy) - 1] = '\0';

            char *savefield;
            strtok_r(linecopy, ":", &savefield);      /* name */
            strtok_r(NULL, ":", &savefield);          /* password placeholder */
            char *uid_s = strtok_r(NULL, ":", &savefield);
            if (uid_s) {
                uid_t u = (uid_t)atoi(uid_s);
                if (u > max_uid) {
                    max_uid = u;
                }
            }
            line = strtok_r(NULL, "\n", &saveline);
        }
        kfree(buf);
    }
    return max_uid + 1;
}

static int shadow_get_hash(const char *name, char *hash_out, size_t max)
{
    char *buf = slurp(SHADOW_PATH);
    if (!buf) {
        return -1;
    }

    int found = -1;
    char *saveline;
    char *line = strtok_r(buf, "\n", &saveline);
    while (line) {
        char linecopy[LINE_SCRATCH_MAX];
        strncpy(linecopy, line, sizeof(linecopy) - 1);
        linecopy[sizeof(linecopy) - 1] = '\0';

        char *savefield;
        char *uname = strtok_r(linecopy, ":", &savefield);
        char *hash  = strtok_r(NULL, ":", &savefield);
        if (uname && hash && strcmp(uname, name) == 0) {
            strncpy(hash_out, hash, max - 1);
            hash_out[max - 1] = '\0';
            found = 0;
            break;
        }
        line = strtok_r(NULL, "\n", &saveline);
    }
    kfree(buf);
    return found;
}

/* Rewrites /etc/shadow with `name`'s hash set to `hash`, replacing an
 * existing entry or appending a new one. The whole file is small (one line
 * per account) so a full read-modify-rewrite is simplest and matches how
 * every other builtin here mutates small config files. */
static int shadow_set_hash(const char *name, const char *hash)
{
    char *newbuf = kmalloc(SHADOW_MAX_FILE);
    if (!newbuf) {
        return -1;
    }
    size_t pos = 0;
    bool replaced = false;

    char *buf = slurp(SHADOW_PATH);
    if (buf) {
        char *saveline;
        char *line = strtok_r(buf, "\n", &saveline);
        while (line) {
            char linecopy[LINE_SCRATCH_MAX];
            strncpy(linecopy, line, sizeof(linecopy) - 1);
            linecopy[sizeof(linecopy) - 1] = '\0';

            char *savefield;
            char *uname = strtok_r(linecopy, ":", &savefield);
            bool is_match = uname && strcmp(uname, name) == 0;

            int n = is_match
                ? snprintf(newbuf + pos, SHADOW_MAX_FILE - pos, "%s:%s\n", name, hash)
                : snprintf(newbuf + pos, SHADOW_MAX_FILE - pos, "%s\n", line);
            if (n > 0) {
                pos += (size_t)n;
            }
            if (is_match) {
                replaced = true;
            }
            line = strtok_r(NULL, "\n", &saveline);
        }
        kfree(buf);
    }

    if (!replaced) {
        int n = snprintf(newbuf + pos, SHADOW_MAX_FILE - pos, "%s:%s\n", name, hash);
        if (n > 0) {
            pos += (size_t)n;
        }
    }

    int rc = vfs_write_file(SHADOW_PATH, (const uint8_t *)newbuf, pos, VFS_O_TRUNC);
    kfree(newbuf);
    return rc;
}

bool users_first_boot(void)
{
    vfs_stat_t st;
    return vfs_stat(SHADOW_PATH, &st) != 0;
}

static bool prompt_new_password(const char *who, char *out, size_t max)
{
    char confirm[64];
    for (;;) {
        printf("New password for %s: ", who);
        read_line_raw(out, max, true);
        if (!out[0]) {
            printf("Password cannot be empty.\n");
            continue;
        }
        printf("Retype new password: ");
        read_line_raw(confirm, sizeof(confirm), true);
        if (strcmp(out, confirm) != 0) {
            printf("Passwords do not match, try again.\n");
            continue;
        }
        return true;
    }
}

static void print_setup_guide(void)
{
    printf("\n=======================================================\n");
    printf(" %s first-boot setup\n", PUREUNIX_NAME);
    printf("=======================================================\n");
    printf("This is the first time %s has booted from this disk.\n", PUREUNIX_NAME);
    printf("Set a password for the 'root' account to continue.\n\n");
}

static void print_welcome_guide(void)
{
    printf("\n--- Basic setup guide ---\n");
    printf("You will be asked to log in every time the system boots.\n");
    printf("  whoami            show the logged-in user\n");
    printf("  adduser <name>    create a new user account\n");
    printf("  passwd [name]     change a password (root can change any)\n");
    printf("  ls, cd, cat, vim  explore and edit files\n");
    printf("  help              list every shell command\n");
    printf("Accounts live in /etc/passwd; hashed passwords in /etc/shadow.\n");
    printf("--------------------------\n");
}

void users_first_boot_setup(void)
{
    print_setup_guide();

    char password[64];
    prompt_new_password("root", password, sizeof(password));

    char hash[17];
    hash_password("root", password, hash);
    shadow_set_hash("root", hash);

    /* /etc/passwd already ships a root entry (tools/mkext2.py); this only
     * fills one in if a stripped-down disk image somehow lacks it. */
    user_record_t rec;
    if (users_lookup("root", &rec) != 0) {
        static const char line[] = "root:x:0:0:root:/root:/bin/sh\n";
        vfs_write_file(PASSWD_PATH, (const uint8_t *)line, sizeof(line) - 1, VFS_O_APPEND);
    }

    printf("Root password set.\n");
    print_welcome_guide();
}

void users_login(void)
{
    char username[USERS_MAX_NAME];
    char password[64];

    for (;;) {
        printf("\n%s login: ", PUREUNIX_NAME);
        read_line_raw(username, sizeof(username), false);
        if (!username[0]) {
            continue;
        }

        printf("Password: ");
        read_line_raw(password, sizeof(password), true);

        user_record_t rec;
        char want[17], got[17];
        if (users_lookup(username, &rec) == 0 &&
            shadow_get_hash(username, want, sizeof(want)) == 0) {
            hash_password(username, password, got);
            if (strcmp(got, want) == 0) {
                task_set_creds(rec.uid, rec.gid);
                shell_setenv("USER", rec.name);
                shell_setenv("HOME", rec.home);
                shell_setenv("SHELL", rec.shell);
                shell_set_home_cwd(rec.home);
                printf("\nWelcome, %s.\n", rec.name);
                return;
            }
        }
        printf("Login incorrect\n");
    }
}

static bool valid_username(const char *name)
{
    if (!name || !*name) {
        return false;
    }
    size_t len = strlen(name);
    if (len >= USERS_MAX_NAME) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

int users_adduser(const char *name)
{
    if (!valid_username(name)) {
        printf("adduser: invalid username '%s'\n", name ? name : "");
        return -1;
    }
    user_record_t existing;
    if (users_lookup(name, &existing) == 0) {
        printf("adduser: user '%s' already exists\n", name);
        return -1;
    }

    uid_t uid = next_free_uid();
    gid_t gid = uid;
    char home[PUREUNIX_MAX_PATH];
    snprintf(home, sizeof(home), "%s/%s", HOME_BASE, name);

    char password[64];
    prompt_new_password(name, password, sizeof(password));

    char hash[17];
    hash_password(name, password, hash);

    char line[LINE_SCRATCH_MAX];
    int n = snprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:/bin/sh\n", name, uid, gid, name, home);
    if (n <= 0 || vfs_write_file(PASSWD_PATH, (const uint8_t *)line, (size_t)n, VFS_O_APPEND) != 0) {
        printf("adduser: failed to update /etc/passwd\n");
        return -1;
    }
    if (shadow_set_hash(name, hash) != 0) {
        printf("adduser: failed to update /etc/shadow\n");
        return -1;
    }

    vfs_mkdir(HOME_BASE);  /* ignore EEXIST: shared by every account */
    vfs_mkdir(home);

    printf("User '%s' added (uid=%u, home=%s).\n", name, uid, home);
    return 0;
}

int users_passwd(const char *name)
{
    user_record_t rec;
    if (users_lookup(name, &rec) != 0) {
        printf("passwd: no such user '%s'\n", name);
        return -1;
    }

    char password[64];
    prompt_new_password(rec.name, password, sizeof(password));

    char hash[17];
    hash_password(rec.name, password, hash);
    if (shadow_set_hash(rec.name, hash) != 0) {
        printf("passwd: failed to update /etc/shadow\n");
        return -1;
    }
    return 0;
}
