/* libFuzzer harness: hashing the input in pieces whose sizes the input
 * chooses gives the same digest as hashing it in one call. Correctness of
 * the digest itself is pinned by the vectors in tests/test_sha256.c; this
 * checks that buffering across arbitrary splits never changes it. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cu.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    CuSha256 h;
    unsigned char whole[32];
    unsigned char pieces[32];
    size_t splits;
    size_t at;
    size_t n;
    size_t i;
    const char *msg;
    size_t len;

    if (size < 1)
        return 0;
    /* The first byte says how many split sizes follow; the rest is the
     * message. Split sizes repeat if the message outlasts them. */
    splits = data[0] % 8 + 1;
    if (size < 1 + splits)
        return 0;
    msg = (const char *)data + 1 + splits;
    len = size - 1 - splits;

    cu_sha256(cu_str_n(msg, len), whole);
    cu_sha256_init(&h);
    for (at = 0, i = 0; at < len; at += n, i++) {
        n = data[1 + i % splits] % 130;
        if (n > len - at)
            n = len - at;
        /* An empty update must change nothing; then take one byte so the
         * loop always advances. */
        if (n == 0) {
            cu_sha256_update(&h, cu_str_empty());
            n = 1;
        }
        cu_sha256_update(&h, cu_str_n(msg + at, n));
    }
    cu_sha256_final(&h, pieces);
    if (memcmp(whole, pieces, 32) != 0)
        __builtin_trap();
    return 0;
}
