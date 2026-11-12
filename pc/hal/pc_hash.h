/* pc_hash.h -- SHA-1 and SHA-256 for the first-run asset setup
 * (pc/hal/asset_setup.c).
 *
 * Written for this port from FIPS 180-4; no third-party code.  Dedicated to
 * the public domain (CC0) like any small reference implementation: copy it
 * freely.  Streaming: init, update any number of times, final.
 */
#ifndef PC_HASH_H
#define PC_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} PcSha1;

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} PcSha256;

void pc_sha1_init(PcSha1 *c);
void pc_sha1_update(PcSha1 *c, const void *data, size_t len);
void pc_sha1_final(PcSha1 *c, uint8_t out[20]);

void pc_sha256_init(PcSha256 *c);
void pc_sha256_update(PcSha256 *c, const void *data, size_t len);
void pc_sha256_final(PcSha256 *c, uint8_t out[32]);

/* Lower-case hex of `n` bytes into out (2n + 1 bytes). */
void pc_hash_hex(const uint8_t *digest, size_t n, char *out);

#ifdef __cplusplus
}
#endif
#endif
