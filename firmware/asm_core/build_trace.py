#!/usr/bin/env python3
"""Build the round-by-round trace sketch: the asm core dumps the SHA-1 state
after every one of its 70 rounds (plus the state as loaded at entry) into
TRACE[], and the sketch compares that with a plain C SHA-1 stepped round by
round."""
import os
import gen_asm

gen_asm.TRACE = True
core = gen_asm.body()
asm = "".join('        "%s\\n"\n' % l for l in core)
clobbers = ", ".join('"r%d"' % n for n in list(range(2, 28)) + [30, 31])

rot = """/* generated trace core - do not edit directly */
extern "C" {
  uint8_t TRACE[71 * 20] __attribute__((used));
  __attribute__((naked, used)) void duco_rounds_trace(void) {
    __asm__ volatile (
""" + asm + """    );
  }
}
static inline void duco_rounds_trace_call(void) {
  __asm__ volatile ("call duco_rounds_trace"
      ::: """ + clobbers + """);
}
"""

sketch = """#include <Arduino.h>
#include <stdint.h>

extern "C" {
  uint32_t MSG[80] __attribute__((used));
  uint32_t KN[4] __attribute__((used)) = {0x5a827999u, 0x6ed9eba1u, 0x8f1bbcdcu, 0xca62c1d6u};
  uint32_t HA __attribute__((used));
  uint32_t HB __attribute__((used));
  uint32_t HC __attribute__((used));
  uint32_t HD __attribute__((used));
  uint32_t HE __attribute__((used));
}

@CORE@

static const char k_prev[41] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";

static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void set_nonce(uint32_t v) {
  char dig[11];
  int len = 0;
  uint32_t x = v;
  do { dig[len++] = char('0' + x % 10); x /= 10; } while (x);
  for (int i = 0, j = len - 1; i < j; i++, j--) { char t = dig[i]; dig[i] = dig[j]; dig[j] = t; }
  MSG[10] = MSG[11] = MSG[12] = MSG[13] = MSG[14] = MSG[15] = 0;
  for (int i = 0; i < len; i++)
    MSG[10 + (i >> 2)] |= (uint32_t)(uint8_t)dig[i] << (24 - 8 * (i & 3));
  MSG[10 + (len >> 2)] |= (uint32_t)(uint8_t)0x80 << (24 - 8 * (len & 3));
  MSG[15] = (uint32_t)(((40 + len) >> 5) << 8) | (uint32_t)(((40 + len) * 8) & 0xff);
}

static uint32_t word_at(int slot, int w) {
  const uint8_t *p = TRACE + slot * 20 + w * 4;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void show(int slot, const uint32_t ref[5]) {
  Serial.print("slot "); Serial.print(slot);
  for (int j = 0; j < 5; j++) {
    Serial.print(' ');
    Serial.print(word_at(slot, j), HEX);
    if (word_at(slot, j) != ref[j]) Serial.print('*');
  }
  Serial.print("  ref");
  for (int j = 0; j < 5; j++) { Serial.print(' '); Serial.print(ref[j], HEX); }
  Serial.println();
}

static void run_trace(uint32_t nonce) {
  for (int i = 0; i < 10; i++)
    MSG[i] = ((uint32_t)(uint8_t)k_prev[4 * i] << 24) | ((uint32_t)(uint8_t)k_prev[4 * i + 1] << 16)
           | ((uint32_t)(uint8_t)k_prev[4 * i + 2] << 8) | (uint8_t)k_prev[4 * i + 3];
  set_nonce(nonce);

  uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476, e = 0xc3d2e1f0;
  for (int i = 0; i < 10; i++) {
    uint32_t t = rotl(a, 5) + e + MSG[i] + 0x5a827999u + (d ^ (b & (c ^ d)));
    e = d; d = c; c = rotl(b, 30); b = a; a = t;
  }
  HA = a; HB = b; HC = c; HD = d; HE = e;
  uint32_t f16 = rotl(MSG[13] ^ MSG[8] ^ MSG[2] ^ MSG[0], 1);
  uint32_t f17 = rotl(MSG[14] ^ MSG[9] ^ MSG[3] ^ MSG[1], 1);
  MSG[16] = f16;
  MSG[17] = f17;
  MSG[20] = rotl(f17 ^ MSG[12] ^ MSG[6] ^ MSG[4], 1);

  duco_rounds_trace_call();

  uint32_t w[80];
  for (int i = 0; i < 16; i++) w[i] = MSG[i];
  for (int i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  uint32_t ref[5] = {HA, HB, HC, HD, HE};
  show(70, ref);
  a = HA; b = HB; c = HC; d = HD; e = HE;
  int bad = 0;
  for (int i = 10; i < 80; i++) {
    int ph = i / 20;
    uint32_t k = ph == 0 ? 0x5a827999u : ph == 1 ? 0x6ed9eba1u : ph == 2 ? 0x8f1bbcdcu : 0xca62c1d6u;
    uint32_t f;
    if (ph == 0) f = d ^ (b & (c ^ d));
    else if (ph == 1) f = b ^ c ^ d;
    else if (ph == 2) f = (b & c) | (d & (b | c));
    else f = b ^ c ^ d;
    uint32_t t = rotl(a, 5) + e + w[i] + k + f;
    e = d; d = c; c = rotl(b, 30); b = a; a = t;
    ref[0] = a; ref[1] = b; ref[2] = c; ref[3] = d; ref[4] = e;
    if (i < 22 || word_at(i - 10, 0) != a) {
      show(i - 10, ref);
      if (word_at(i - 10, 0) != a) bad++;
    }
  }
  Serial.print("bad rounds: "); Serial.println(bad);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  run_trace(1234567890u);
}

void loop() { delay(1000); }
""".replace("@CORE@", rot)

os.makedirs("trace_test", exist_ok=True)
open("trace_test/trace_test.ino", "w").write(sketch)
print("core instructions:", len(core))
