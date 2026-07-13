/* ziptest -- standalone regression check that real upstream zlib
 * (third_party/zlib/, docs/zlib-port.md) actually compresses and
 * decompresses correctly on PureUNIX, independent of anything libpng/
 * imgview-related. Complements imgview's own decode-correctness proof
 * (every real PNG it decodes is itself DEFLATE-compressed, so a correct
 * rendered image already proves inflate() works on real compressed data --
 * see docs/zlib-port.md's Testing section) with a direct compress()+
 * uncompress() roundtrip over multiple payload shapes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

static int roundtrip(const char *label, const unsigned char *src, uLong srclen)
{
    uLong destlen = compressBound(srclen);
    unsigned char *comp = malloc(destlen);
    if (!comp) {
        printf("%s: FAIL (out of memory)\n", label);
        return 1;
    }

    int rc = compress(comp, &destlen, src, srclen);
    if (rc != Z_OK) {
        printf("%s: FAIL (compress() returned %d)\n", label, rc);
        free(comp);
        return 1;
    }

    uLong outlen = srclen;
    unsigned char *out = malloc(outlen ? outlen : 1);
    if (!out) {
        printf("%s: FAIL (out of memory)\n", label);
        free(comp);
        return 1;
    }

    rc = uncompress(out, &outlen, comp, destlen);
    if (rc != Z_OK) {
        printf("%s: FAIL (uncompress() returned %d)\n", label, rc);
        free(comp);
        free(out);
        return 1;
    }

    int ok = (outlen == srclen) && (srclen == 0 || memcmp(out, src, srclen) == 0);
    printf("%s: %s (%lu -> %lu -> %lu bytes)\n", label, ok ? "PASS" : "FAIL",
           (unsigned long)srclen, (unsigned long)destlen, (unsigned long)outlen);

    free(comp);
    free(out);
    return ok ? 0 : 1;
}

int main(void)
{
    printf("zlib version: %s\n", zlibVersion());

    int failures = 0;

    const char *text = "Hello, PureUNIX! Hello, PureUNIX! Hello, PureUNIX! "
                        "The quick brown fox jumps over the lazy dog. "
                        "The quick brown fox jumps over the lazy dog.";
    failures += roundtrip("repetitive text", (const unsigned char *)text, (uLong)strlen(text));

    unsigned char empty[1] = {0};
    failures += roundtrip("empty buffer", empty, 0);

    /* Pseudo-random, effectively-incompressible data -- exercises the
     * stored/near-1:1 path in deflate(), not just the easy repetitive case
     * above. */
    static unsigned char rnd[8192];
    unsigned int seed = 12345;
    for (size_t i = 0; i < sizeof(rnd); i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = (unsigned char)(seed >> 16);
    }
    failures += roundtrip("8KiB pseudo-random data", rnd, sizeof(rnd));

    /* CRC32/Adler32 sanity -- both used internally by zlib itself (gzip
     * vs. zlib stream trailers) and exposed as public API real callers
     * (e.g. libpng's own CRC checks) depend on. */
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const unsigned char *)text, (uInt)strlen(text));
    printf("crc32(text) = 0x%08lx\n", (unsigned long)crc);

    uLong adler = adler32(0L, Z_NULL, 0);
    adler = adler32(adler, (const unsigned char *)text, (uInt)strlen(text));
    printf("adler32(text) = 0x%08lx\n", (unsigned long)adler);

    if (failures == 0)
        printf("ziptest: ALL PASS\n");
    else
        printf("ziptest: %d FAILURE(S)\n", failures);

    return failures ? 1 : 0;
}
