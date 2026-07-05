/*
 * kernel/crypto.c — CoreCrypto: SHA-256, HMAC-SHA256, and a PBKDF2-HMAC-SHA256
 * password KDF.
 *
 * This is what kernel/users.c calls into so every account's password (root
 * included) is verified cryptographically at login, instead of the old
 * hand-rolled FNV-1a mix. See docs/crypto.md.
 *
 * The RNG behind crypto_random_bytes() (salts only) is a splitmix64 stream
 * seeded from rdtsc + the PIT tick count — there's no hardware entropy
 * source on this platform, so it's the best available, not a CSPRNG. Real
 * secrets are never generated with it; only per-account salts.
 */
#include <pureunix/arch.h>
#include <pureunix/crypto.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define SHA256_BLOCK_LEN 64
#define CRYPTO_SALT_LEN 16
#define CRYPTO_ITERATIONS 10000u

/* ---------------- SHA-256 (FIPS 180-4) ---------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[SHA256_BLOCK_LEN];
    size_t buflen;
} sha256_ctx_t;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[SHA256_BLOCK_LEN])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[ctx->buflen++] = data[i];
        if (ctx->buflen == SHA256_BLOCK_LEN) {
            sha256_transform(ctx, ctx->buffer);
            ctx->bitlen += SHA256_BLOCK_LEN * 8;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[CRYPTO_SHA256_DIGEST_LEN])
{
    size_t i = ctx->buflen;
    ctx->bitlen += (uint64_t)ctx->buflen * 8;

    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        while (i < SHA256_BLOCK_LEN) {
            ctx->buffer[i++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        i = 0;
    }
    while (i < 56) {
        ctx->buffer[i++] = 0;
    }
    for (int j = 7; j >= 0; j--) {
        ctx->buffer[i++] = (uint8_t)(ctx->bitlen >> (j * 8));
    }
    sha256_transform(ctx, ctx->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void crypto_sha256(const void *data, size_t len, uint8_t out[CRYPTO_SHA256_DIGEST_LEN])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)data, len);
    sha256_final(&ctx, out);
}

/* ---------------- HMAC-SHA256 (RFC 2104) ---------------- */

void crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                         const uint8_t *msg, size_t msg_len,
                         uint8_t out[CRYPTO_SHA256_DIGEST_LEN])
{
    uint8_t key_block[SHA256_BLOCK_LEN];
    memset(key_block, 0, sizeof(key_block));
    if (key_len > SHA256_BLOCK_LEN) {
        crypto_sha256(key, key_len, key_block);
    } else {
        memcpy(key_block, key, key_len);
    }

    uint8_t ipad[SHA256_BLOCK_LEN], opad[SHA256_BLOCK_LEN];
    for (size_t i = 0; i < SHA256_BLOCK_LEN; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    uint8_t inner[CRYPTO_SHA256_DIGEST_LEN];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);
}

/* ---------------- RNG (salts only; see file header) ---------------- */

static uint64_t rng_state;
static bool crypto_is_ready = false;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t splitmix64_next(void)
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void crypto_random_bytes(uint8_t *out, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint64_t r = splitmix64_next();
        for (int b = 0; b < 8 && i < len; b++, i++) {
            out[i] = (uint8_t)(r >> (b * 8));
        }
    }
}

/* ---------------- self-test + init ---------------- */

static bool sha256_self_test(void)
{
    static const uint8_t expected[CRYPTO_SHA256_DIGEST_LEN] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t digest[CRYPTO_SHA256_DIGEST_LEN];
    crypto_sha256("abc", 3, digest);
    return memcmp(digest, expected, sizeof(expected)) == 0;
}

void crypto_init(void)
{
    rng_state = rdtsc() ^ ((uint64_t)pit_ticks() << 17) ^ 0xA5A5A5A5A5A5A5A5ULL;
    (void)splitmix64_next();
    (void)splitmix64_next();

    crypto_is_ready = sha256_self_test();
}

bool crypto_ready(void)
{
    return crypto_is_ready;
}

/* ---------------- hex encode/decode ---------------- */

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ---------------- PBKDF2-HMAC-SHA256 (RFC 8018), single block ----------------
 *
 * Only ever asked for a 32-byte key (== SHA-256's output length), so the
 * general multi-block PBKDF2 construction collapses to computing just its
 * first block: U1 = HMAC(password, salt || BE32(1)), Ui = HMAC(password,
 * U(i-1)), output = U1 xor U2 xor ... xor Uc.
 */
static void pbkdf2_hmac_sha256_32(const char *password, size_t password_len,
                                   const uint8_t *salt, size_t salt_len,
                                   uint32_t iterations, uint8_t out[CRYPTO_SHA256_DIGEST_LEN])
{
    uint8_t block[SHA256_BLOCK_LEN];
    memcpy(block, salt, salt_len);
    block[salt_len + 0] = 0;
    block[salt_len + 1] = 0;
    block[salt_len + 2] = 0;
    block[salt_len + 3] = 1;

    uint8_t u[CRYPTO_SHA256_DIGEST_LEN];
    crypto_hmac_sha256((const uint8_t *)password, password_len, block, salt_len + 4, u);
    memcpy(out, u, CRYPTO_SHA256_DIGEST_LEN);

    for (uint32_t i = 1; i < iterations; i++) {
        uint8_t next[CRYPTO_SHA256_DIGEST_LEN];
        crypto_hmac_sha256((const uint8_t *)password, password_len, u, sizeof(u), next);
        memcpy(u, next, sizeof(u));
        for (size_t j = 0; j < CRYPTO_SHA256_DIGEST_LEN; j++) {
            out[j] ^= u[j];
        }
    }
}

/* ---------------- public password hashing API ---------------- */

#define HASH_PREFIX "pbkdf2-sha256"

bool crypto_hash_password(const char *password, char *out, size_t out_max)
{
    uint8_t salt[CRYPTO_SALT_LEN];
    crypto_random_bytes(salt, sizeof(salt));

    uint8_t digest[CRYPTO_SHA256_DIGEST_LEN];
    pbkdf2_hmac_sha256_32(password, strlen(password), salt, sizeof(salt), CRYPTO_ITERATIONS, digest);

    char salt_hex[CRYPTO_SALT_LEN * 2 + 1];
    char digest_hex[CRYPTO_SHA256_DIGEST_LEN * 2 + 1];
    hex_encode(salt, sizeof(salt), salt_hex);
    salt_hex[sizeof(salt_hex) - 1] = '\0';
    hex_encode(digest, sizeof(digest), digest_hex);
    digest_hex[sizeof(digest_hex) - 1] = '\0';

    int n = snprintf(out, out_max, "%s$%u$%s$%s", HASH_PREFIX, CRYPTO_ITERATIONS, salt_hex, digest_hex);
    return n > 0 && (size_t)n < out_max;
}

/* Not data-dependent on early exit alone: still walks every byte regardless
 * of where the first mismatch is, so the comparison time doesn't leak which
 * byte differed. */
static bool consttime_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool crypto_verify_password(const char *password, const char *stored)
{
    if (!password || !stored) {
        return false;
    }

    size_t prefix_len = strlen(HASH_PREFIX);
    if (strncmp(stored, HASH_PREFIX, prefix_len) != 0 || stored[prefix_len] != '$') {
        return false;
    }
    const char *p = stored + prefix_len + 1;

    uint32_t iterations = 0;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        iterations = iterations * 10 + (uint32_t)(*p - '0');
        p++;
    }
    if (*p != '$') {
        return false;
    }
    p++;

    const char *salt_hex = p;
    size_t salt_hex_len = 0;
    while (p[salt_hex_len] && p[salt_hex_len] != '$') {
        salt_hex_len++;
    }
    if (p[salt_hex_len] != '$' || salt_hex_len != CRYPTO_SALT_LEN * 2) {
        return false;
    }
    p += salt_hex_len + 1;

    const char *digest_hex = p;
    size_t digest_hex_len = strlen(digest_hex);
    if (digest_hex_len != CRYPTO_SHA256_DIGEST_LEN * 2) {
        return false;
    }

    uint8_t salt[CRYPTO_SALT_LEN];
    uint8_t want[CRYPTO_SHA256_DIGEST_LEN];
    if (!hex_decode(salt_hex, salt, sizeof(salt)) || !hex_decode(digest_hex, want, sizeof(want))) {
        return false;
    }

    uint8_t got[CRYPTO_SHA256_DIGEST_LEN];
    pbkdf2_hmac_sha256_32(password, strlen(password), salt, sizeof(salt), iterations, got);

    return consttime_equal(got, want, sizeof(want));
}
