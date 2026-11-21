/*
  DUCO-S1a nonce search for the ATtiny45 and ATtiny85.

  These chips have 256 and 512 bytes of RAM, so the usual SHA-1 layout does not
  fit: the 80 word message schedule is held in a 16 word rolling window (the
  round that overwrites slot i & 15 is the last round that word is needed in),
  the previous hash is kept as the 20 raw bytes the pool sent rather than the 40
  characters it is written as, and the target digest is five words. That is 104
  bytes; every byte left over is the bus state machine and the stack. On the
  ATtiny25, with 128 bytes in total, those 104 bytes plus the stack the search
  loop needs do not fit, which is why that chip is not a target.

  Rounds 0-9 read only the previous hash, so they are the same for every nonce
  in a job and are run once before the search begins. The words they consumed
  are overwritten by the rolling window as the search goes on, so they are
  rebuilt from the raw previous hash before each attempt.

  The pool's hashes are lowercase, and the reconstruction below writes
  lowercase, so the string the chip hashes is byte for byte the string the pool
  hashed.
*/

#include <stdint.h>

#include "attiny_hash.h"

/* AVR's int is 16 bit, so a literal that ends up in a 32 bit shift has to be
   spelled out as unsigned long, and a rotate has to be built out of shifts
   that stay inside the type they are written in. */

static inline uint32_t rotl5(uint32_t x) {
  return (x << 5) | (uint32_t)((uint8_t)(x >> 24) >> 3);
}

static inline uint32_t rotl30(uint32_t x) {
  return (x >> 2) | ((uint32_t)(x & 3u) << 30);
}

static inline uint32_t rotl1(uint32_t x) { return (x << 1) | (x >> 31); }

/* Nothing here is kept in a table. A const array on the AVR is copied into RAM
   at startup, and on these chips RAM is the thing that runs out first, so the
   initial state is written out in full and the hex digits are made by hand. */
#define INIT0 0x67452301u
#define INIT1 0xefcdab89u
#define INIT2 0x98badcfeu
#define INIT3 0x10325476u
#define INIT4 0xc3d2e1f0u

uint8_t attiny_prev[20];    /* the previous hash, as the pool sent it       */
uint32_t attiny_target[5];  /* the digest the nonce has to produce          */
uint32_t attiny_window[16]; /* rolling schedule window                      */

char attiny_hex_char(uint8_t nibble) {
  nibble &= 0x0fu;
  return (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
}

/* One byte of the previous hash becomes the two hex characters SHA-1 hashes,
   packed into the high and low nibbles of a 16 bit value. */
static uint16_t hexPair(uint8_t byte) {
  uint16_t high = (uint16_t)(byte >> 4);
  uint16_t low = (uint16_t)(byte & 0x0fu);
  return (high < 10 ? high + 48 : high + 87) * 256u +
         (low  < 10 ? low  + 48 : low  + 87);
}

/* The rounds below shift the state one place per round, so the digest is the
   state after round 79 plus the initial state - the same convention the
   assembly cores and the ATmega bare-metal core use. Taking the initial state
   out of the target once per job means the compare in the search adds nothing
   to every attempt. */
void attiny_finish_target(void) {
  attiny_target[0] -= INIT0;
  attiny_target[1] -= INIT1;
  attiny_target[2] -= INIT2;
  attiny_target[3] -= INIT3;
  attiny_target[4] -= INIT4;
}

/* The ten words the previous hash turns into, the way SHA-1 wants them. */
static void load_prev_words(void) {
  for (uint8_t i = 0; i < 10; i++) {
    attiny_window[i] = ((uint32_t)hexPair(attiny_prev[2 * i]) << 16) |
                       hexPair(attiny_prev[2 * i + 1]);
  }
}

/* Rounds 0-9. The nonce is not in them, so they run once per job. */
static void prefix_rounds(uint32_t *a, uint32_t *b, uint32_t *c,
                          uint32_t *d, uint32_t *e) {
  load_prev_words();
  *a = INIT0; *b = INIT1; *c = INIT2; *d = INIT3; *e = INIT4;
  for (uint8_t i = 0; i < 10; i++) {
    uint32_t temp = rotl5(*a) + *e + attiny_window[i] + 0x5a827999u +
                    (*d ^ (*b & (*c ^ *d)));
    *e = *d; *d = *c; *c = rotl30(*b); *b = *a; *a = temp;
  }
}

/* The back half of the message: the nonce's decimal digits, the 0x80 that
   closes it, the zeros, and the bit length in the last word. The nonce never
   passes 65,500 in DUCO-S1a, so the message is always one 64 byte block and
   the length never reaches the high byte of the length field. */
static void build_tail(uint16_t nonce) {
  attiny_window[10] = 0; attiny_window[11] = 0; attiny_window[12] = 0;
  attiny_window[13] = 0; attiny_window[14] = 0; attiny_window[15] = 0;

  uint8_t count = 1;
  uint16_t limit = 10;
  while (count < 5 && nonce >= limit) { count++; limit *= 10; }

  /* Digits come out of the division least significant first, so they go in
     from the right hand end of the digit string. */
  for (uint8_t i = 0; i < count; i++) {
    uint8_t slot = count - 1 - i;
    attiny_window[10 + (slot >> 2)] |=
      (uint32_t)(uint8_t)('0' + nonce % 10) << (24 - 8 * (slot & 3));
    nonce /= 10;
  }

  attiny_window[10 + (count >> 2)] |= (uint32_t)0x80u << (24 - 8 * (count & 3));
  /* The bit length is between 328 and 360, so it sits in the low two bytes of
     the 64 bit length field and the two bytes above it are zero. */
  attiny_window[15] = (uint32_t)((40u + count) * 8u);
}

/* True when the message in the window hashes to the target digest. Rounds
   16-79 fold the new word back into the window, which is why it has 16 slots. */
static int compress(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
  for (uint8_t i = 10; i < 80; i++) {
    uint32_t word;
    if (i < 16) {
      word = attiny_window[i];
    } else {
      word = rotl1(attiny_window[(i - 3) & 15] ^ attiny_window[(i - 8) & 15] ^
                   attiny_window[(i - 14) & 15] ^ attiny_window[(i - 16) & 15]);
      attiny_window[i & 15] = word;
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

    uint32_t temp = rotl5(a) + e + word + f + k;
    e = d; d = c; c = rotl30(b); b = a; a = temp;
  }

  return a == attiny_target[0] && b == attiny_target[1] &&
         c == attiny_target[2] && d == attiny_target[3] &&
         e == attiny_target[4];
}

uint32_t attiny_mine(uint16_t maxNonce) {
  uint32_t a, b, c, d, e;
  prefix_rounds(&a, &b, &c, &d, &e);

  for (uint16_t nonce = 0; nonce < maxNonce; nonce++) {
    load_prev_words();
    build_tail(nonce);
    if (compress(a, b, c, d, e)) return nonce;
  }
  return 0;
}
