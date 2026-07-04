# Users, Login, and First-Boot Setup

## Overview

Account state and the boot-time login flow live in one file:

| File | Purpose |
|---|---|
| `kernel/users.c` | `/etc/passwd`/`/etc/shadow` parsing, password hashing, first-boot wizard, login prompt, `adduser`/`passwd` logic |
| `include/pureunix/users.h` | Public API (`user_record_t`, `users_first_boot`, `users_first_boot_setup`, `users_login`, `users_lookup`, `users_adduser`, `users_passwd`) |

`kernel_main` (`kernel/main.c`) calls into this module right after the filesystems are mounted and interrupts are enabled, before `shell_run()`:

```c
arch_enable_interrupts();

if (users_first_boot()) {
    users_first_boot_setup();
}
users_login();

shell_run();
```

So every boot either runs the first-boot wizard once, then always ends with a `login:`/`Password:` prompt before the shell starts.

---

## Storage Format

Two flat files, classic-Unix style:

```
/etc/passwd   name:x:uid:gid:gecos:home:shell
/etc/shadow   name:hash
```

`/etc/passwd` is seeded by `tools/mkext2.py` with `root` (uid 0) and a demo `guest` (uid 1000) entry; `/etc/shadow` does not exist on a fresh disk image — its absence is exactly what "first boot" means (`users_first_boot()` is just `vfs_stat("/etc/shadow", &st) != 0`).

The `x` placeholder field in `/etc/passwd` is vestigial (real Unix history — password is never stored there); `users_lookup()` skips over it without interpreting it.

### Password hashing

There is no crypto library in this freestanding kernel, so `hash_password()` mixes the username (as a salt) and password through a chained FNV-1a, producing a 16-hex-character digest stored in `/etc/shadow`. This keeps the shadow file from holding plaintext; it is **not** a cryptographic KDF and would not resist an offline attack. Good enough for a hobby OS, not a security boundary.

---

## First-Boot Setup

`users_first_boot_setup()`:

1. Prints a short banner explaining this is the first boot on this disk.
2. Prompts twice (`New password for root:` / `Retype new password:`, both masked with `*`) until the two entries match and are non-empty.
3. Hashes and writes the root entry into `/etc/shadow` (creating the file).
4. Prints a short "basic setup guide" — the handful of commands (`whoami`, `adduser`, `passwd`, `ls`/`cd`/`cat`/`vim`, `help`) a first-time user needs.

This only ever runs once per disk image — `/etc/shadow` persisting across ATA writes is what suppresses it on every later boot.

---

## Login

`users_login()` loops forever:

```
<PUREUNIX_NAME> login: <username>
Password: <masked>
```

- Looks up `username` in `/etc/passwd` and its hash in `/etc/shadow`.
- Recomputes the hash from the entered password and compares.
- On success: calls `task_set_creds(uid, gid)` (kernel/task.c) to apply that account's credentials to the currently running task, then `shell_setenv()`s `USER`/`HOME`/`SHELL` and `shell_set_home_cwd()`s the shell's starting directory to the account's home.
- On failure: prints `Login incorrect` and loops back to the `login:` prompt. There is no attempt limit or lockout (a single-user local machine has no one to lock out).

Reading raw keystrokes here (`read_line_raw`, static to `kernel/users.c`) is a small standalone reader — no history, no tab completion, just backspace/enter, with optional `*` masking — since this runs before `shell_run()` and doesn't need the shell's full line editor (`shell/line.c`).

---

## `adduser` / `passwd`

Both are shell builtins (`shell/builtins.c`) that call straight into `kernel/users.c`; both take over the terminal directly for interactive password prompts (same pattern as `vim`/`cmd_editor`), so only their final one-line result goes through the buffered `shell_output_t`.

- **`adduser NAME`** — root only (`current_uid() != 0` is rejected in the builtin). Picks the next free uid ≥ 1000 (gid = uid), appends a `/etc/passwd` row and an `/etc/shadow` row, and creates `/home/NAME`.
- **`passwd [NAME]`** — with no argument, changes the caller's own password; with an argument, only root may target another account. Rewrites (or appends) the `/etc/shadow` row for that user.

---

## Limitations

- No password aging/expiry fields (`/etc/shadow` here is just `name:hash`, not the full 9-field format).
- New home directories are owned by whoever ran `adduser` (root), not the new account — `chown`/`chmod` are unimplemented on both filesystem drivers (see `docs/filesystem.md`), so there's no way to fix that after creation.
- No account lockout/attempt limiting on login.
- Hashing is FNV-1a based, not a real KDF — see "Password hashing" above.
