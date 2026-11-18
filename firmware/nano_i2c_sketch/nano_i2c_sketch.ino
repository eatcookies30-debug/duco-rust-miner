/*
   Duino-Coin DUCO-S1a miner for ATmega328P boards (Nano, Uno, clones).

   Same search core as the Mega 2560 build, but the round loop is not unrolled:
   the SHA-1 role rotation repeats every six rounds, so one six-round body runs
   three times per phase instead of twenty rounds of code being spelled out.
   That is what keeps the sketch inside a Nano's 30 KB of flash. The pointer
   arithmetic that replaces the unrolled schedule, plus the register spills the
   extra clobber forces, costs about 3% of the hashrate: 1,759 H/s on a Nano
   against 1,815 H/s for the unrolled core on the same board.

   Two ways to feed it a job:

     serial  - the plain AVR miner protocol, one line in, one line out
     I2C     - the DuinoCoinI2C rig protocol (ricaun/DuinoCoinI2C), so the board
               can sit on the same A4/A5 bus as an ESP master

   Both paths share one hash core, so the numbers agree whichever rig drives it.
*/

#include "duco_hash.h"
#include "i2c_slave.h"
#include "uniqueID.h"

#pragma GCC optimize ("-Ofast")

#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

/* Worst case for the serial reply: 16 binary digits for the nonce, 32 for the
   elapsed microseconds, the 22 character DUCOID, two commas and a newline. */
static char response[80];

static char ducoid[23];

/* The job buffers live at file scope instead of as locals: a local 41-byte
   array would have to be copied into a stack frame for every job, and the
   search path only needs them once per job anyway. */
static uint8_t targetDigest[SHA1_HASH_LEN];
static char jobPrevHash[41];
static char jobTargetHash[41];

static char hexNibble(char c) {
  return (c >= '0' && c <= '9') ? (char)(c - '0') : (char)((c & 0x0f) + 9);
}

static char hexChar(uint8_t value) {
  return value < 10 ? char('0' + value) : char('A' + value - 10);
}

static void hexToBytes(const char *hex, uint8_t *raw) {
  for (uint8_t i = 0, j = 0; j < SHA1_HASH_LEN; i += 2, j++)
    raw[j] = (uint8_t)(((uint8_t)hexNibble(hex[i]) << 4) | (uint8_t)hexNibble(hex[i + 1]));
}

static uint8_t appendDecimal(char *out, uint8_t pos, uint32_t value) {
  static char tmp[10];
  uint8_t n = 0;
  do {
    tmp[n++] = char('0' + value % 10u);
    value /= 10u;
  } while (value);
  while (n) out[pos++] = tmp[--n];
  return pos;
}

/* The serial protocol of the AVR miner sends the nonce and the search time in
   binary - that is what the desktop miners parse. The I2C rig sends decimal,
   because the pool reads the line the master forwards as plain numbers. */
static uint8_t appendBinary(char *out, uint8_t pos, uint32_t value) {
  uint8_t bit = 31;
  while (bit && !((value >> bit) & 1u)) bit--;
  for (;;) {
    out[pos++] = char('0' + ((value >> bit) & 1u));
    if (!bit) break;
    bit--;
  }
  return pos;
}

/* prevHash,targetHash,difficulty - a trailing comma or newline is ignored. */
static bool parseJob(const char *job, uint8_t len, uint32_t *difficulty) {
  uint8_t i = 0, k = 0;
  while (i < len && k < 40 && job[i] != ',') jobPrevHash[k++] = job[i++];
  if (k != 40) return false;
  while (i < len && job[i] == ',') i++;
  k = 0;
  while (i < len && k < 40 && job[i] != ',') jobTargetHash[k++] = job[i++];
  if (k != 40) return false;
  while (i < len && job[i] == ',') i++;
  uint32_t d = 0;
  while (i < len && job[i] >= '0' && job[i] <= '9')
    d = d * 10u + (uint32_t)(job[i++] - '0');
  *difficulty = d;
  return true;
}

static uint32_t search(uint32_t difficulty, uint32_t *elapsed) {
  /* Above 655 the nonce range no longer fits in the search the protocol allows. */
  if (difficulty > 655) {
    *elapsed = 0;
    return 0;
  }

  hexToBytes(jobTargetHash, targetDigest);

  PORTB |= B00100000;                 /* led on while the search runs */
  uint32_t started = micros();
  uint32_t nonce = duco_mine(jobPrevHash, targetDigest, difficulty * 100u + 1u);
  *elapsed = micros() - started;
  PORTB &= B11011111;
  return nonce;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  PORTB &= B11011111;

  ducoid[0] = 'D'; ducoid[1] = 'U'; ducoid[2] = 'C'; ducoid[3] = 'O';
  ducoid[4] = 'I'; ducoid[5] = 'D';
  for (uint8_t i = 0; i < 8; i++) {
    ducoid[6 + 2 * i] = hexChar((uint8_t)UniqueID8[i] >> 4);
    ducoid[7 + 2 * i] = hexChar((uint8_t)UniqueID8[i] & 0x0f);
  }
  ducoid[22] = 0;

  Serial.begin(115200);
  Serial.setTimeout(10000);
  while (!Serial)
    ;
  Serial.flush();

  uint8_t address = i2c_begin();
  if (address) {
    Serial.print("I2C slave address: ");
    Serial.println(address);
  } else {
    Serial.println("I2C: every bus address is taken, serial only");
  }
}

void loop() {
  /* A job handed over by the rig master is answered through the bus. */
  uint32_t difficulty;
  if (i2c_take_job(jobPrevHash, jobTargetHash, &difficulty)) {
    uint32_t elapsed;
    uint32_t nonce = search(difficulty, &elapsed);
    i2c_submit(nonce, elapsed, ducoid);
  }

  /* The serial path is the one the AVR miners on a PC drive. */
  static char serialJob[100];
  static uint8_t serialFill;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') {
      if (serialFill < sizeof(serialJob) - 1) serialJob[serialFill++] = c;
      continue;
    }
    uint8_t len = serialFill;
    serialFill = 0;
    if (!parseJob(serialJob, len, &difficulty)) continue;
    uint32_t elapsed;
    uint32_t nonce = search(difficulty, &elapsed);
    uint8_t pos = appendBinary(response, 0, nonce);
    response[pos++] = ',';
    pos = appendBinary(response, pos, elapsed);
    response[pos++] = ',';
    for (uint8_t i = 0; i < 22; i++) response[pos++] = ducoid[i];
    response[pos++] = '\n';
    Serial.write(response, pos);
  }
}
