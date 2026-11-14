#pragma once

#include <Arduino.h>

#define SHA1_BLOCK_LEN 64
#define SHA1_HASH_LEN 20

/* Search nonces 0..maxNonce-1 for one whose DUCO-S1a (SHA-1) digest equals
   `target`. Returns the nonce, or 0 when there is no match. */
uint32_t duco_mine(const char *prevHash, const uint8_t *target, uint32_t maxNonce);
