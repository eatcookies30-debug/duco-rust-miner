/*

   ____  __  __  ____  _  _  _____       ___  _____  ____  _  _ 
  (  _ \(  )(  )(_  _)( \( )(  _  )___  / __)(  _  )(_  _)( \( )
   )(_) ))(__)(  _)(_  )  (  )(_)((___)( (__  )(_)(  _)(_  )  ( 
  (____/(______)(____)(_)\_)(_____)     \___)(_____)(____)(_)\_)
  Official code for Arduino boards (and relatives)   version 4.3
  
  Duino-Coin Team & Community 2019-2026 © MIT Licensed
  https://duinocoin.com
  https://github.com/revoxhere/duino-coin
  If you don't know where to start, visit official website and navigate to
  the Getting Started page. Have fun mining!
*/

/* For microcontrollers with low memory change that to -Os in all files,
for default settings use -O0. -O may be a good tradeoff between both */
#pragma GCC optimize ("-Ofast")
/* For microcontrollers with custom LED pins, adjust the line below */
#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif
#define SEP_TOKEN ","
#define END_TOKEN "\n"
/* For 8-bit microcontrollers we should use 16 bit variables since the
difficulty is low, for all the other cases should be 32 bits. */
#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_MEGAAVR)
typedef uint32_t uintDiff;
#else
typedef uint32_t uintDiff;
#endif
// Arduino identifier library - https://github.com/ricaun
#include "uniqueID.h"

#include "duco_hash.h"
#include "i2c_slave.h"

/* The bus address the board claimed goes in the last byte of the id. Clone
   boards often have a blank signature row, and several of them then report the
   same name, which the pool counts as one miner. Folding the address in keeps
   the id the length the protocol expects and makes every board on a rig its
   own; a board that never joined a bus keeps the id its signature row gives it. */
String get_DUCOID(uint8_t address) {
  String ID = "DUCOID";
  char buff[4];
  for (size_t i = 0; i < 8; i++) {
    sprintf(buff, "%02X", (uint8_t)UniqueID8[i]);
    ID += buff;
  }
  if (address) {
    sprintf(buff, "%02X", address);
    ID.setCharAt(20, buff[0]);
    ID.setCharAt(21, buff[1]);
  }
  return ID;
}

String DUCOID = "";

/* Job buffers for the rig path; the serial path reads its own line. */
static char wirePrevHash[41];
static char wireTargetHash[41];

void setup() {
  // Prepare built-in led pin as output
  pinMode(LED_BUILTIN, OUTPUT);
  // Open serial port
  Serial.begin(115200);
  Serial.setTimeout(10000);
  while (!Serial)
    ;  // For Arduino Leonardo or any board with the ATmega32U4
  Serial.flush();

  // Join an I2C mining rig if one is on the bus (see i2c_slave.h)
  uint8_t address = i2c_begin();
  if (address) {
    Serial.print("I2C slave address: ");
    Serial.println(address);
  } else {
    Serial.println("I2C: no address available (bus busy, SLAVE_ADDRESS taken, or every address used) - serial only");
  }

  DUCOID = get_DUCOID(address);
}

void lowercase_hex_to_bytes(char const * hexDigest, uint8_t * rawDigest) {
  for (uint8_t i = 0, j = 0; j < SHA1_HASH_LEN; i += 2, j += 1) {
    uint8_t x = hexDigest[i];
    uint8_t b = x >> 6;
    uint8_t r = ((x & 0xf) | (b << 3)) + b;

    x = hexDigest[i + 1];
    b = x >> 6;

    rawDigest[j] = (r << 4) | (((x & 0xf) | (b << 3)) + b);
  }
}

// DUCO-S1A hasher
uintDiff ducos1a(char const * prevBlockHash, char const * targetBlockHash, uintDiff difficulty) {
  #if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_MEGAAVR)
    // If the difficulty is too high for AVR architecture then return 0
    if (difficulty > 655) return 0;
  #endif

  uint8_t target[SHA1_HASH_LEN];
  lowercase_hex_to_bytes(targetBlockHash, target);

  uintDiff const maxNonce = difficulty * 100 + 1;
  return duco_mine(prevBlockHash, target, maxNonce);
}

void loop() {
  // A job pushed by the rig master over I2C is answered over the same bus.
  uint32_t wireDifficulty;
  if (i2c_take_job(wirePrevHash, wireTargetHash, &wireDifficulty)) {
    uint32_t wireStart = micros();
    uint32_t wireNonce = ducos1a(wirePrevHash, wireTargetHash, wireDifficulty);
    i2c_submit(wireNonce, micros() - wireStart, DUCOID.c_str());
  }

  // Wait for serial data
  if (Serial.available() <= 0) {
    return;
  }

  // Reserve 1 extra byte for comma separator (and later zero)
  char lastBlockHash[40 + 1];
  char newBlockHash[40 + 1];

  // Read last block hash
  if (Serial.readBytesUntil(',', lastBlockHash, 41) != 40) {
    return;
  }
  lastBlockHash[40] = 0;

  // Read expected hash
  if (Serial.readBytesUntil(',', newBlockHash, 41) != 40) {
    return;
  }
  newBlockHash[40] = 0;

  // Read difficulty
  uintDiff difficulty = strtoul(Serial.readStringUntil(',').c_str(), NULL, 10);
  // Clearing the receive buffer reading one job.
  while (Serial.available()) Serial.read();
  // Turn off the built-in led
  #if defined(ARDUINO_ARCH_AVR)
      PORTB = PORTB | B00100000;
  #else
      digitalWrite(LED_BUILTIN, LOW);
  #endif

  // Start time measurement
  uint32_t startTime = micros();

  // Call DUCO-S1A hasher
  uintDiff ducos1result = ducos1a(lastBlockHash, newBlockHash, difficulty);

  // Calculate elapsed time
  uint32_t elapsedTime = micros() - startTime;

  // Turn on the built-in led
  #if defined(ARDUINO_ARCH_AVR)
      PORTB = PORTB & B11011111;
  #else
      digitalWrite(LED_BUILTIN, HIGH);
  #endif

  // Clearing the receive buffer before sending the result.
  while (Serial.available()) Serial.read();

  // Send result back to the program with share time
  Serial.print(String(ducos1result, 2) 
               + SEP_TOKEN
               + String(elapsedTime, 2) 
               + SEP_TOKEN
               + String(DUCOID) 
               + END_TOKEN);
}
