# Users, Login, and First-Boot Setup

## Overview

Account state and the boot-time login flow live in one file:

| File | Purpose |
|---|---|
| `kernel/users.c` | `/etc/passwd`/`/etc/shadow` parsing, password hashing, first-boot wizard, login prompt, `adduser`/`passwd` logic |
| `include/pureunix/users.h` | Public API (`user_record_t`, `users_first_boot`, `users_first_boot_setup`, `users_login`, `users_lookup`, `users_adduser`, `users_passwd`) |
| `kernel/crypto.c` / `include/pureunix/crypto.h` | CoreCrypto — the SHA-256/HMAC/PBKDF2 primitives `kernel/users.c` hashes and verifies every password with. See `docs/crypto.md`. |

`kernel_main` (`kernel/main.c`) calls into this module right after the filesystems are mounted and interrupts are enabled, before `shell_run()`:

```c
arch_enable_interrupts();

crypto_init();
if (crypto_ready()) {
    printf("Crypto OK\n");
} else {
    panic("CoreCrypto self-test failed; refusing to start login.");
}

if (users_first_boot()) {
    users_first_boot_setup();
}
users_login();

shell_run();
```

CoreCrypto is initialized (and self-tested) before either the first-boot wizard or the login prompt runs, since both hash or verify a password — if the self-test fails there is no safe way to check a password, so the kernel panics instead of ever showing a `login:` prompt. So every successful boot prints `Crypto OK`, then either runs the first-boot wizard once, then always ends with a `login:`/`Password:` prompt before the shell starts.

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

Every password (root's included) is hashed and verified through CoreCrypto (`kernel/crypto.c`): `crypto_hash_password()` runs PBKDF2-HMAC-SHA256 (10,000 iterations) over the password with a fresh random 16-byte salt, and stores a self-describing string in `/etc/shadow`:

```
pbkdf2-sha256$<iterations>$<salt-hex>$<digest-hex>
```

`crypto_verify_password()` parses that string, recomputes the digest from the entered password, and compares it in constant time. See `docs/crypto.md` for the primitives themselves.

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
- CoreCrypto's RNG (used only for salts) is seeded from `rdtsc`/PIT ticks, not a hardware entropy source — see `docs/crypto.md`.
- Changing the hash format (as happened when CoreCrypto replaced the old FNV-1a mix) invalidates every existing `/etc/shadow` entry; accounts on disk images provisioned before that change must have their passwords reset via `passwd`/a fresh first-boot setup.
