#include <Arduino.h>
#include <stdint.h>
#include "duco_hash.h"

extern "C" {
  
  extern uint32_t MSG[80];
  extern uint32_t TGT[5];
  extern uint32_t HA, HB, HC, HD, HE;
}

static const char kPrev[41] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";

static void hex2bytes(const char *hex, uint8_t *out) {
  for (int i = 0; i < 20; i++) {
    int hi = hex[2 * i], lo = hex[2 * i + 1];
    if (hi <= '9') hi -= '0'; else hi -= 'a' - 10;
    if (lo <= '9') lo -= '0'; else lo -= 'a' - 10;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  const char *hexes[2] = {
    "d0beba883d7e8cd119ea2b0e09b78f60f29e0968",  /* sha1(kPrev + "50") */
    "0123456789abcdef0123456789abcdef01234567",  /* deliberately wrong  */
  };
  for (int i = 0; i < 2; i++) {
    uint8_t target[20];
    hex2bytes(hexes[i], target);
    uint32_t start = micros();
    uint32_t found = duco_mine(kPrev, target, 1001);
    uint32_t took = micros() - start;
    Serial.print("target "); Serial.println(hexes[i]);
    Serial.print("  duco_mine -> "); Serial.print(found);
    Serial.print("   in "); Serial.print(took); Serial.println(" us");

    Serial.print("  TGT0 "); Serial.println(TGT[0], HEX);
  }
}

void loop() { delay(1000); }
