/*
 * Host check for the tiny core: compile it for the machine this runs on and
 * compare every nonce it finds with what hashlib says.
 *
 * Run it with check_tiny_core.py, which builds the test vectors first.
 */

#include <stdio.h>
#include <stdint.h>

#include "tiny_hash.h"

static const struct {
  const char *prevHash;
  unsigned long nonce;
  unsigned long maxNonce;
  const char *targetHex;
} cases[] = {
#include "vectors.h"
};

static void hexToBytes(const char *hex, uint8_t *raw) {
  for (int i = 0; i < 20; i++) {
    char a = hex[2 * i], b = hex[2 * i + 1];
    int hi = (a >= '0' && a <= '9') ? a - '0' : (a & 0x0f) + 9;
    int lo = (b >= '0' && b <= '9') ? b - '0' : (b & 0x0f) + 9;
    raw[i] = (uint8_t)((hi << 4) | lo);
  }
}

int main(void) {
  uint8_t digest[20];
  unsigned long checked = 0, failures = 0, falsePositives = 0;

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    hexToBytes(cases[i].targetHex, digest);
    unsigned long found = tiny_mine(cases[i].prevHash, digest,
                                    (uint32_t)cases[i].maxNonce);
    checked++;
    if (found != cases[i].nonce) {
      failures++;
      printf("case %lu: expected nonce %lu, got %lu (target %s)\n",
             (unsigned long)i, cases[i].nonce, found, cases[i].targetHex);
    }
  }

  printf("tiny core: %lu cases checked, %lu failures\n", checked, failures);
  (void)falsePositives;
  return failures ? 1 : 0;
}
