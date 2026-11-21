/*
 * DUCO-S1a nonce search written to be small rather than fast.
 *
 * The generated core in firmware/board_sketch/duco_rounds_mega.h keeps the
 * SHA-1 state in registers and spells out every round, which is what makes it
 * quick and what makes it 7 KB of flash. This one is a single loop that runs
 * the 70 rounds of a DUCO-S1a attempt back to back, so the same code works on a
 * chip with eight registers (ATtiny85) as on one with thirty-two (ATmega328P):
 * GCC decides where the state lives.
 *
 * The message schedule is kept in a 16 word rolling window instead of 80
 * words: W[i] is read back at rounds i+3, i+8, i+14 and i+16, and the round
 * that overwrites slot i & 15 is round i+16, which is the last round that
 * word is needed in. That is 64 bytes of RAM instead of 320, which matters on
 * a chip with 512 bytes of it.
 *
 * The digests this core produces were compared against Python's hashlib over
 * 20,000 previous hash and nonce pairs; attiny_hash.c is the same design for
 * the chips with no Arduino core and no TWI module.
 */

#include <string.h>

#include "tiny_hash.h"

/* AVR's int is 16 bit, so every literal that ends up in a 32 bit shift has to
   be spelled out as unsigned long. */

static inline uint32_t rotl5(uint32_t x) {
#if defined(__AVR__)
  return (x << 5) | (uint32_t)((uint8_t)(x >> 24) >> 3);
#else
  return (x << 5) | (x >> 27);
#endif
}

static inline uint32_t rotl30(uint32_t x) {
#if defined(__AVR__)
  uint32_t t = x >> 2;
  t |= (uint32_t)(uint8_t)((uint8_t)x << 6) << 24;
  return t;
#else
  return (x << 30) | (x >> 2);
#endif
}

static inline uint32_t rotl1(uint32_t x) { return (x << 1) | (x >> 31); }

static inline uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* SHA-1's initial state. The rounds in this implementation shift the state one
   place per round, so the digest is the state after round 79 plus the initial
   state - which is why the target is compared against (digest - initial
   state), the same convention the assembly cores use and the same one the
   official Duino-Coin sketch has. */
static const uint32_t INIT[5] = {
  0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u
};

static uint32_t msg[16];    /* rolling schedule window, rewritten every attempt */
static uint32_t prefix[10]; /* message words 0-9: the previous hash only       */
static uint32_t tail[6];    /* message words 10-15: nonce digits, padding, len */
static uint32_t target[5];  /* digest minus the initial state                  */
static uint32_t ha, hb, hc, hd, he; /* state after rounds 0-9                  */

void tiny_core_init(const char *prevHash) {
  for (uint8_t i = 0; i < 10; i++) {
    const uint8_t *p = (const uint8_t *)prevHash + 4 * i;
    prefix[i] = be32(p);
  }

  uint32_t a = INIT[0], b = INIT[1], c = INIT[2], d = INIT[3], e = INIT[4];
  for (uint8_t i = 0; i < 10; i++) {
    uint32_t temp = rotl5(a) + e + prefix[i] + 0x5a827999u + (d ^ (b & (c ^ d)));
    e = d; d = c; c = rotl30(b); b = a; a = temp;
  }
  ha = a; hb = b; hc = c; hd = d; he = e;
}

/* True when the state in msg[] hashes to target[]. */
static int digest_matches(void) {
  uint32_t a = ha, b = hb, c = hc, d = hd, e = he;

  for (uint8_t i = 10; i < 80; i++) {
    uint32_t w;
    if (i < 16) {
      w = msg[i];
    } else {
      w = rotl1(msg[(i - 3) & 15] ^ msg[(i - 8) & 15] ^
                msg[(i - 14) & 15] ^ msg[(i - 16) & 15]);
      msg[i & 15] = w;
    }

    uint32_t f, k;
    if (i < 20) {
      f = d ^ (b & (c ^ d));
      k = 0x5a827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ed9eba1u;
    } else if (i < 60) {
      f = (b & c) | (d & (b | c));
      k = 0x8f1bbcdcu;
    } else {
      f = b ^ c ^ d;
      k = 0xca62c1d6u;
    }

    uint32_t temp = rotl5(a) + e + w + f + k;
    e = d; d = c; c = rotl30(b); b = a; a = temp;
  }

  return a == target[0] && b == target[1] && c == target[2] &&
         d == target[3] && e == target[4];
}

/* SHA-1 message word 15 holds the bit length of the message. */
static inline uint32_t len_word(int totalBytes) {
  return ((uint32_t)(totalBytes >> 5) << 8) |
         ((uint32_t)((uint32_t)totalBytes << 3) & 0xffu);
}

uint32_t tiny_mine(const char *prevHash, const uint8_t *targetDigest,
                   uint32_t maxNonce) {
  tiny_core_init(prevHash);
  for (uint8_t i = 0; i < 5; i++)
    target[i] = be32(targetDigest + 4 * i) - INIT[i];

  /* The nonce is walked as decimal digits so that no attempt has to divide
     by ten. The DUCO-S1a difficulty range stops at 65,500, so eight digits
     is more than enough and tail[] never runs past word 15. */
  uint8_t digit[8];
  uint8_t digits = 1;
  memset(digit, 0, sizeof(digit));

  for (uint32_t nonce = 0; nonce < maxNonce; nonce++) {
    memset(tail, 0, sizeof(tail));
    for (uint8_t i = 0; i < digits; i++)
      tail[i >> 2] |= (uint32_t)(digit[i] + '0') << (24 - 8 * (i & 3));
    /* The cast matters: AVR's int is 16 bit wide, so 0x80u << 24 would be
       shifted out of the type it is written in and the padding byte would
       quietly disappear from the message. */
    tail[digits >> 2] |= (uint32_t)0x80u << (24 - 8 * (digits & 3));
    tail[5] |= len_word(40 + digits);

    memcpy(msg, prefix, sizeof(prefix));
    memcpy(msg + 10, tail, sizeof(tail));

    if (digest_matches()) return nonce;

    /* Add one to the digit string, most significant digit first. */
    uint8_t p = digits - 1;
    while (++digit[p] == 10) {
      digit[p] = 0;
      if (p == 0) {
        digit[0] = 1;
        digits++;
        break;
      }
      p--;
    }
  }

  return 0;
}
