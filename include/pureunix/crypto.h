#ifndef PUREUNIX_CRYPTO_H
#define PUREUNIX_CRYPTO_H

/*
 * CoreCrypto — the kernel's cryptographic primitives.
 *
 * Provides SHA-256 / HMAC-SHA256 and a PBKDF2-HMAC-SHA256 password KDF, used
 * by kernel/users.c so that every account's password (root included) is
 * verified cryptographically at login instead of with the old ad hoc FNV-1a
 * mix. See docs/crypto.md.
 */

#include <pureunix/types.h>

#define CRYPTO_SHA256_DIGEST_LEN 32

/* "pbkdf2-sha256$<iterations>$<salt-hex>$<digest-hex>\0" */
#define CRYPTO_HASH_STRING_MAX 128

/* Seeds CoreCrypto's RNG and runs a self-test (known SHA-256 test vector).
 * Must be called once, after pit_init()/arch_enable_interrupts(), before any
 * password is hashed or verified. crypto_ready() reflects whether the
 * self-test passed. */
void crypto_init(void);
bool crypto_ready(void);

void crypto_sha256(const void *data, size_t len, uint8_t out[CRYPTO_SHA256_DIGEST_LEN]);
void crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                         const uint8_t *msg, size_t msg_len,
                         uint8_t out[CRYPTO_SHA256_DIGEST_LEN]);

/* Fills `len` bytes from a PRNG seeded at crypto_init() time from rdtsc and
 * the PIT tick count. Good enough for password salts on a hobby OS with no
 * hardware entropy source; not a CSPRNG suitable for keying long-lived
 * secrets. */
void crypto_random_bytes(uint8_t *out, size_t len);

/* Hashes `password` with a fresh random salt through PBKDF2-HMAC-SHA256,
 * writing a self-describing "pbkdf2-sha256$<iters>$<salt>$<hash>" string
 * (hex-encoded) into out[out_max]. Returns false if out_max is too small. */
bool crypto_hash_password(const char *password, char *out, size_t out_max);

/* Recomputes the PBKDF2 digest described by `stored` (as produced by
 * crypto_hash_password()) from `password` and compares it in constant time.
 * Returns false on malformed input as well as on mismatch. */
bool crypto_verify_password(const char *password, const char *stored);

#endif
