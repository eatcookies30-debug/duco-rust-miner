/* DUCO-S1a nonce search written to be small rather than fast. */

#ifndef TINY_HASH_H
#define TINY_HASH_H

#include <stdint.h>

/* The core is C. Wrapped like this it can also be included from a sketch,
   which is a .cpp file to the Arduino toolchain. */
#ifdef __cplusplus
extern "C" {
#endif

#define TINY_HASH_LEN 20

/* Rounds 0-9 of the hash depend only on the previous hash, so they are
   computed once when a job arrives and reused for every nonce attempt. */
void tiny_core_init(const char *prevHash);

/* Search nonces 0..maxNonce-1 for one whose DUCO-S1a digest equals
   `targetDigest` (20 raw bytes). Returns the nonce, or 0 when there is none. */
uint32_t tiny_mine(const char *prevHash, const uint8_t *targetDigest, uint32_t maxNonce);

#ifdef __cplusplus
}
#endif

#endif
