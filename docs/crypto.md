# CoreCrypto

## Overview

CoreCrypto is the kernel's cryptographic primitives module. It exists so every account's password — root's included — is hashed and verified with a real cryptographic construction instead of a hand-rolled mix, a plaintext compare, or anything else that would fall over under an offline attack on `/etc/shadow`.

| File | Purpose |
|---|---|
| `kernel/crypto.c` | SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, the salt RNG, and the self-test |
| `include/pureunix/crypto.h` | Public API (`crypto_init`, `crypto_ready`, `crypto_sha256`, `crypto_hmac_sha256`, `crypto_random_bytes`, `crypto_hash_password`, `crypto_verify_password`) |

`kernel/users.c` (see `docs/users.md`) is CoreCrypto's only caller: it hashes a new password with `crypto_hash_password()` in the first-boot wizard, `adduser`, and `passwd`, and checks a typed password against the stored `/etc/shadow` entry with `crypto_verify_password()` in `users_login()`.

---

## Boot sequence and the "Crypto OK" message

`kernel_main` (`kernel/main.c`) initializes CoreCrypto right after enabling interrupts, before the first-boot wizard or the login prompt ever run:

```c
arch_enable_interrupts();

crypto_init();
if (crypto_ready()) {
    printf("Crypto OK\n");
} else {
    panic("CoreCrypto self-test failed; refusing to start login.");
}
```

`crypto_init()` seeds the salt RNG (see below) and runs a self-test: it computes SHA-256 of the known test vector `"abc"` and checks the digest against the FIPS 180-4 reference value. If that doesn't match, `crypto_ready()` returns `false` and the kernel panics rather than ever showing a `login:` prompt — there is no safe way to verify a password with a SHA-256 implementation that just failed its own known-answer test, so refusing to boot into login is the only sound option on a single-user machine.

Every successful boot therefore prints `Crypto OK` right before the first-boot wizard / login prompt.

---

## Primitives

- **`crypto_sha256(data, len, out[32])`** — SHA-256 per FIPS 180-4. Single-buffer streaming implementation (`sha256_init`/`sha256_update`/`sha256_final`, internal to `kernel/crypto.c`); no dynamic allocation.
- **`crypto_hmac_sha256(key, key_len, msg, msg_len, out[32])`** — HMAC per RFC 2104, built on `crypto_sha256`.
- **`crypto_random_bytes(out, len)`** — a splitmix64 PRNG seeded once, at `crypto_init()` time, from `rdtsc()` xored with the PIT tick count (`pit_ticks()`, `arch/i386/pit.c`). This platform has no hardware entropy source, so this is the best available source of unpredictability, **not** a CSPRNG — it is used only to generate per-account password salts, never to key anything long-lived.

---

## Password hashing (PBKDF2-HMAC-SHA256)

`crypto_hash_password(password, out, out_max)`:

1. Draws a fresh 16-byte salt from `crypto_random_bytes()`.
2. Runs PBKDF2-HMAC-SHA256 for 10,000 iterations over `password` with that salt. Since the derived key length (32 bytes) equals SHA-256's output length, the general multi-block PBKDF2 construction collapses to just its first block — see the comment above `pbkdf2_hmac_sha256_32()` in `kernel/crypto.c`.
3. Formats the result as a self-describing, hex-encoded string:

   ```
   pbkdf2-sha256$<iterations>$<salt-hex>$<digest-hex>
   ```

   e.g. `pbkdf2-sha256$10000$3f9a...$8bce...` — this is exactly what gets written into `/etc/shadow`.

`crypto_verify_password(password, stored)` parses that string (rejecting anything that doesn't start with `pbkdf2-sha256$`, has a malformed iteration count, or has the wrong salt/digest length), recomputes the digest from `password` with the parsed salt and iteration count, and compares it to the stored digest with a constant-time comparison (`consttime_equal()` — XORs every byte and only checks the accumulator at the end, so the compare doesn't take a data-dependent shortcut on the first mismatching byte).

---

## Limitations

- `crypto_random_bytes()` is not a CSPRNG (see above) — acceptable for salts, which only need to be unique per account, not unpredictable to a strong adversary.
- Changing the shadow format (as happened when CoreCrypto replaced the previous FNV-1a mix) invalidates every existing `/etc/shadow` entry on disk images provisioned before the change; see `docs/users.md`'s Limitations section.
- No password aging, pepper, or hardware-backed key storage — appropriate for a hobby OS, not a production security boundary.
