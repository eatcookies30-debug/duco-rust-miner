/*
 * DUCO-S1a nonce search, rewritten for speed.
 *
 * Differences from the 4.3 reference implementation:
 *   * the 10 rounds over the constant previous-hash prefix are computed once
 *     per job instead of once per nonce attempt
 *   * the 70 remaining rounds are unrolled into the four SHA-1 phases, so no
 *     per-round branch chain and no loop-carried index math
 *   * the nonce is walked as decimal digits, so there is no ultoa() and no
 *     64-byte buffer refill for every attempt
 *   * message words 0..9 (the previous hash) are restored from a saved copy
 *     instead of being re-read from a byte buffer
 *   * constant rotations are written so that the AVR code generator does not
 *     emit a shift loop: on AVR (x<<5)|(x>>27) costs ~170 instructions because
 *     the >>27 becomes a 27-iteration loop, while the decomposed form below
 *     costs ~25
 *   * the digest is compared as five 32-bit words with early exit
 */

#include "duco_hash.h"

#pragma GCC optimize ("-Ofast")

/* AVR: int is 16 bit, so every literal used in a 32-bit shift must be
   explicitly 32 bit. */
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

/* SHA-1 initial state - the digest is (final state + initial state), so the
   target is compared against (target - initial state). */
static const uint32_t INIT[5] = {
  0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u
};

struct duco_core_t {
  uint32_t a, b, c, d, e;   // state after the 10 constant rounds
  uint32_t base[10];        // message words 0..9 - the previous hash
  uint32_t w[16];           // scratch, clobbered by the round schedule
  uint32_t t[5];            // target digest minus the initial state, so the
                           // final compare needs no extra adds
};

static void core_init(duco_core_t &h, const char *prevHash) {
  for (uint8_t i = 0; i < 10; i++) {
    h.base[i] = be32((const uint8_t *)prevHash + 4 * i);
    h.w[i] = h.base[i];
  }
  h.w[10] = h.w[11] = h.w[12] = h.w[13] = h.w[14] = h.w[15] = 0;

  uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476, e = 0xc3d2e1f0;
  for (uint8_t i = 0; i < 10; i++) {
    uint32_t temp = rotl5(a) + e + h.w[i] + 0x5a827999u + (d ^ (b & (c ^ d)));
    e = d; d = c; c = rotl30(b); b = a; a = temp;
  }
  h.a = a; h.b = b; h.c = c; h.d = d; h.e = e;
}

#define SCH(i) w[i & 15] = rotl1(w[(i-3) & 15] ^ w[(i-8) & 15] ^ w[(i-14) & 15] ^ w[(i-16) & 15])
/* (b&c)+(~b&d) == d^(b&(c^d)): the two products are bit-disjoint, so the
   three-op form is the same value with one fewer 32-bit operation. */
#define P1(i) { uint32_t tt = rotl5(a) + e + w[i & 15] + (d ^ (b & (c ^ d))) + 0x5a827999u; e = d; d = c; c = rotl30(b); b = a; a = tt; }
#define P2(i) { uint32_t tt = rotl5(a) + e + w[i & 15] + (b ^ c ^ d) + 0x6ed9eba1u; e = d; d = c; c = rotl30(b); b = a; a = tt; }
/* (b&c)|(b&d)|(c&d) == (b&c)|(d&(b|c)): same value, one less 32-bit operation */
#define P3(i) { uint32_t tt = rotl5(a) + e + w[i & 15] + ((b & c) | (d & (b | c))) + 0x8f1bbcdcu; e = d; d = c; c = rotl30(b); b = a; a = tt; }
#define P4(i) { uint32_t tt = rotl5(a) + e + w[i & 15] + (b ^ c ^ d) + 0xca62c1d6u; e = d; d = c; c = rotl30(b); b = a; a = tt; }

static bool rounds(uint32_t *w, uint32_t a, uint32_t b, uint32_t c,
                   uint32_t d, uint32_t e, const uint32_t *t) {
  P1(10); P1(11); P1(12); P1(13); P1(14); P1(15);
  SCH(16); P1(16); SCH(17); P1(17); SCH(18); P1(18); SCH(19); P1(19);
  SCH(20); P2(20); SCH(21); P2(21); SCH(22); P2(22); SCH(23); P2(23);
  SCH(24); P2(24); SCH(25); P2(25); SCH(26); P2(26); SCH(27); P2(27);
  SCH(28); P2(28); SCH(29); P2(29); SCH(30); P2(30); SCH(31); P2(31);
  SCH(32); P2(32); SCH(33); P2(33); SCH(34); P2(34); SCH(35); P2(35);
  SCH(36); P2(36); SCH(37); P2(37); SCH(38); P2(38); SCH(39); P2(39);
  SCH(40); P3(40); SCH(41); P3(41); SCH(42); P3(42); SCH(43); P3(43);
  SCH(44); P3(44); SCH(45); P3(45); SCH(46); P3(46); SCH(47); P3(47);
  SCH(48); P3(48); SCH(49); P3(49); SCH(50); P3(50); SCH(51); P3(51);
  SCH(52); P3(52); SCH(53); P3(53); SCH(54); P3(54); SCH(55); P3(55);
  SCH(56); P3(56); SCH(57); P3(57); SCH(58); P3(58); SCH(59); P3(59);
  SCH(60); P4(60); SCH(61); P4(61); SCH(62); P4(62); SCH(63); P4(63);
  SCH(64); P4(64); SCH(65); P4(65); SCH(66); P4(66); SCH(67); P4(67);
  SCH(68); P4(68); SCH(69); P4(69); SCH(70); P4(70); SCH(71); P4(71);
  SCH(72); P4(72); SCH(73); P4(73); SCH(74); P4(74); SCH(75); P4(75);
  SCH(76); P4(76); SCH(77); P4(77); SCH(78); P4(78); SCH(79); P4(79);

  return (a == t[0]) && (b == t[1]) && (c == t[2]) && (d == t[3]) && (e == t[4]);
}

static inline bool try_nonce(duco_core_t &h, uint32_t w10, uint32_t w11, uint32_t w15) {
  uint32_t *w = h.w;
  w[0]  = h.base[0]; w[1]  = h.base[1]; w[2]  = h.base[2]; w[3]  = h.base[3];
  w[4]  = h.base[4]; w[5]  = h.base[5]; w[6]  = h.base[6]; w[7]  = h.base[7];
  w[8]  = h.base[8]; w[9]  = h.base[9];
  w[10] = w10; w[11] = w11; w[12] = 0; w[13] = 0; w[14] = 0; w[15] = w15;
  return rounds(w, h.a, h.b, h.c, h.d, h.e, h.t);
}

#define TRY(w10, w11, w15, nonceVal)                                    \
  do {                                                                 \
    if (try_nonce(h, (w10), (w11), (w15))) return (nonceVal);          \
  } while (0)

/* SHA-1 message word 15 = bit length of the message. */
static inline uint32_t len_word(int totalBytes) {
  return ((uint32_t)(totalBytes >> 5) << 8) | (uint32_t)(((uint32_t)totalBytes << 3) & 0xff);
}

uint32_t duco_mine(const char *prevHash, const uint8_t *target, uint32_t maxNonce) {
  static duco_core_t h;
  core_init(h, prevHash);
  for (uint8_t i = 0; i < 5; i++)
    h.t[i] = be32(target + 4 * i) - INIT[i];

  /* int is 16 bit on AVR - keep the digit/padding constants 32 bit */
  const uint32_t Z = (uint32_t)'0';
  const uint32_t P = (uint32_t)0x80;

  /* 1 digit: 0..9 */
  if (maxNonce > 0) {
    uint32_t w15 = len_word(41);
    for (uint32_t a = 0; a < 10 && a < maxNonce; a++)
      TRY(((Z + a) << 24) | (P << 16), 0, w15, a);
  }
  /* 2 digits: 10..99 */
  if (maxNonce > 10) {
    uint32_t w15 = len_word(42);
    for (uint32_t a = 1; a < 10; a++)
      for (uint32_t b = 0; b < 10; b++) {
        uint32_t v = a * 10 + b;
        if (v >= maxNonce) return 0;
        TRY(((Z + a) << 24) | ((Z + b) << 16) | (P << 8), 0, w15, v);
      }
  }
  /* 3 digits: 100..999 */
  if (maxNonce > 100) {
    uint32_t w15 = len_word(43);
    for (uint32_t a = 1; a < 10; a++)
      for (uint32_t b = 0; b < 10; b++)
        for (uint32_t c = 0; c < 10; c++) {
          uint32_t v = a * 100 + b * 10 + c;
          if (v >= maxNonce) return 0;
          TRY(((Z + a) << 24) | ((Z + b) << 16) | ((Z + c) << 8) | P, 0, w15, v);
        }
  }
  /* 4 digits: 1000..9999 */
  if (maxNonce > 1000) {
    uint32_t w15 = len_word(44);
    for (uint32_t a = 1; a < 10; a++)
      for (uint32_t b = 0; b < 10; b++)
        for (uint32_t c = 0; c < 10; c++)
          for (uint32_t d = 0; d < 10; d++) {
            uint32_t v = a * 1000 + b * 100 + c * 10 + d;
            if (v >= maxNonce) return 0;
            TRY(((Z + a) << 24) | ((Z + b) << 16) | ((Z + c) << 8) | (Z + d),
                P << 24, w15, v);
          }
  }
  /* 5 digits: 10000..99999 (the highest difficulty the protocol allows) */
  if (maxNonce > 10000) {
    uint32_t w15 = len_word(45);
    for (uint32_t a = 1; a < 10; a++)
      for (uint32_t b = 0; b < 10; b++)
        for (uint32_t c = 0; c < 10; c++)
          for (uint32_t d = 0; d < 10; d++)
            for (uint32_t e = 0; e < 10; e++) {
              uint32_t v = a * 10000 + b * 1000 + c * 100 + d * 10 + e;
              if (v >= maxNonce) return 0;
              TRY(((Z + a) << 24) | ((Z + b) << 16) | ((Z + c) << 8) | (Z + d),
                  ((Z + e) << 24) | (P << 16), w15, v);
            }
  }

  /* Longer nonces cannot occur with the DUCO-S1a difficulty range, but the
     generic path keeps the routine correct for any input. */
  if (maxNonce > 100000) {
    for (uint32_t v = 100000; v < maxNonce; v++) {
      char dig[11];
      int len = 0;
      uint32_t x = v;
      char tmp[11];
      int n = 0;
      while (x) { tmp[n++] = char('0' + x % 10); x /= 10; }
      while (n) dig[len++] = tmp[--n];
      dig[len] = 0;

      uint32_t ww[6] = {0, 0, 0, 0, 0, 0};   /* message words 10..15 */
      for (int i = 0; i < len; i++)
        ww[i >> 2] |= (uint32_t)(uint8_t)dig[i] << (24 - 8 * (i & 3));
      ww[len >> 2] |= (uint32_t)(uint8_t)0x80 << (24 - 8 * (len & 3));
      ww[5] = len_word(40 + len);

      uint32_t *w = h.w;
      w[0]  = h.base[0]; w[1]  = h.base[1]; w[2]  = h.base[2]; w[3]  = h.base[3];
      w[4]  = h.base[4]; w[5]  = h.base[5]; w[6]  = h.base[6]; w[7]  = h.base[7];
      w[8]  = h.base[8]; w[9]  = h.base[9];
      w[10] = ww[0]; w[11] = ww[1]; w[12] = ww[2]; w[13] = ww[3];
      w[14] = ww[4]; w[15] = ww[5];
      if (rounds(w, h.a, h.b, h.c, h.d, h.e, h.t)) return v;
    }
  }

  return 0;
}
