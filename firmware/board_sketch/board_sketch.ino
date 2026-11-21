/*
   Duino-Coin DUCO-S1a miner for an Arduino board.

   One sketch for every AVR board the rig supports. A board with 32 KB of flash
   or more (Mega 1280/2560, Due) gets the fully unrolled assembly core; a 328P
   board (Nano, Uno, Pro Mini, clone) gets the looped core, which is the one
   that fits in 30 KB. The looped core is about 3% slower: 1,759 H/s on a Nano
   against 1,815 H/s for the unrolled core on the same board.

   Two ways to feed it a job:

     serial  - the plain AVR miner protocol, one line in, one line out
     I2C     - the DuinoCoinI2C rig protocol (ricaun/DuinoCoinI2C), so the board
               can sit on the same bus as the rig master

   Both paths share one hash core, so the numbers agree whichever way it is
   driven.

   ---------------------------------------------------------------------------
   USER SETTINGS - the only lines you need to change

   SLAVE_ADDRESS  this board's number on the I2C rig, 1 to 126. Every board on a
                  rig needs its own: the pool counts a board by its address, so
                  two boards with the same number behave as one miner. Set 0 for
                  a board that is not wired to a bus, which then mines over
                  serial only.
   RIG_BUS_KHZ    the rig bus speed in kHz. 100 is fine for short leads; use 50
                  when the wires are long or replies start getting dropped.

   The on-board LED blinks while the board is searching and double-flashes every
   time it finds a share.

   Nothing has to be passed on the command line: flash the sketch the way you
   flash any other and set the two numbers above for each board.
   --------------------------------------------------------------------------- */

#define SLAVE_ADDRESS 1
#define RIG_BUS_KHZ   100

/* --------------------------------------------------------------------------- */

#include "duco_hash.h"
#include "i2c_slave.h"
#include "uniqueID.h"

#pragma GCC optimize ("-Ofast")

#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

/* The on-board LED is pin 13 on every board this sketch is built for, but it
   is not the same port bit everywhere: on the ATmega328P and ATmega168 pin 13
   is bit 5 of port B, on the ATmega1280/2560 it is bit 7. One direct write to
   the right bit keeps the blink going without going through digitalWrite in the
   middle of a search. */
#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328__) || \
    defined(__AVR_ATmega168P__) || defined(__AVR_ATmega168__)
  #define LED_PORT PORTB
  #define LED_BIT  (1 << 5)
  #define LED_DIRECT 1
#elif defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  #define LED_PORT PORTB
  #define LED_BIT  (1 << 7)
  #define LED_DIRECT 1
#endif

/* Timer 1 ticks the blink. The search itself is one long blocking call, so an
   interrupt is the only way the LED can keep moving while a board is working.
   Timer 0 is what millis() runs on and Timer 2 is what tone() uses, so Timer 1
   is free. One blink every 200 ms is a hundred instructions a second, which is
   nothing next to a SHA-1. */
#if defined(TCCR1B) && defined(OCIE1A) && defined(WGM12) && defined(TIMER1_COMPA_vect)
  #define LED_TIMER 1
  static volatile uint8_t ledSearching = 0;

  /* The timer is set to clear itself on compare match, so it never reaches the
     overflow that TIMER1_OVF_vect answers; the compare-match vector is the one
     that actually fires. */
  ISR(TIMER1_COMPA_vect) {
    if (ledSearching) {
#ifdef LED_DIRECT
      LED_PORT ^= LED_BIT;
#else
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
#endif
    }
  }

  static void ledTimerStart(void) {
    TCCR1A = 0;
    /* Bit 3 of TCCR1B is Timer 1's compare-match-clear mode; the header does not
       name it, so the bit is written directly. CS12 and CS10 together are the
       /1024 prescaler, which makes OCR1A a plain number of milliseconds. */
    TCCR1B = (1 << 3) | (1 << CS12) | (1 << CS10);
    OCR1A = (F_CPU / 1024UL) / 5UL;      /* 200 ms at any clock */
    TCNT1 = 0;
    TIMSK1 |= (1 << OCIE1A);
  }
#endif

static void ledOn(void) {
#ifdef LED_DIRECT
  LED_PORT |= LED_BIT;
#else
  digitalWrite(LED_BUILTIN, HIGH);
#endif
}

static void ledOff(void) {
#ifdef LED_DIRECT
  LED_PORT &= ~LED_BIT;
#else
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

/* Two flashes for a share this board has just found. */
static void ledFlash(uint8_t times) {
#ifdef LED_TIMER
  ledSearching = 0;
#endif
  for (uint8_t i = 0; i < times; i++) {
    ledOn();
    delay(70);
    ledOff();
    delay(70);
  }
}

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

/* The serial protocol of the AVR miner sends the nonce and the search time in
   binary - that is what the desktop miners parse. The I2C rig sends decimal,
   because the rig master and the pool read the forwarded line as numbers. */
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

/* The board's name on the pool is the sixteen hex digits of the eight byte
   device id in the boot signature row. Clone boards frequently ship with that
   row blank, so several of them report the same name and the pool counts them
   as one miner. The bus address the board claimed goes in the last byte, which
   keeps the id the same length and makes every board on a rig its own. */
static void build_ducoid(uint8_t address) {
  ducoid[0] = 'D'; ducoid[1] = 'U'; ducoid[2] = 'C'; ducoid[3] = 'O';
  ducoid[4] = 'I'; ducoid[5] = 'D';
  for (uint8_t i = 0; i < 8; i++) {
    ducoid[6 + 2 * i] = hexChar((uint8_t)UniqueID8[i] >> 4);
    ducoid[7 + 2 * i] = hexChar((uint8_t)UniqueID8[i] & 0x0f);
  }
  if (address) {
    ducoid[20] = hexChar(address >> 4);
    ducoid[21] = hexChar(address & 0x0f);
  }
  ducoid[22] = 0;
}

static uint32_t search(uint32_t difficulty, uint32_t *elapsed) {
  /* Above 655 the nonce range no longer fits in the search the protocol allows. */
  if (difficulty > 655) {
    *elapsed = 0;
    return 0;
  }

  hexToBytes(jobTargetHash, targetDigest);

#ifdef LED_TIMER
  ledSearching = 1;
#else
  ledOn();
#endif

  uint32_t started = micros();
  uint32_t nonce = duco_mine(jobPrevHash, targetDigest, difficulty * 100u + 1u);
  *elapsed = micros() - started;
  return nonce;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  Serial.begin(115200);
  Serial.setTimeout(10000);
  while (!Serial)
    ;
  Serial.flush();

#ifdef LED_TIMER
  ledTimerStart();
#endif

  uint8_t address = i2c_begin();
  if (address) {
    Serial.print("I2C slave address: ");
    Serial.println(address);
  } else if (SLAVE_ADDRESS) {
    Serial.print("I2C: address ");
    Serial.print(SLAVE_ADDRESS);
    Serial.println(" is taken and every other address is too - serial only");
  }

  /* A board that never joined a bus has no address to fold into its name, so it
     keeps the id its signature row gives it. */
  build_ducoid(address);
}

void loop() {
  /* A job handed over by the rig master is answered through the bus. */
  uint32_t difficulty;
  if (i2c_take_job(jobPrevHash, jobTargetHash, &difficulty)) {
    uint32_t elapsed;
    uint32_t nonce = search(difficulty, &elapsed);
    i2c_submit(nonce, elapsed, ducoid);
    ledFlash(2);
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
    ledFlash(2);

    uint8_t pos = appendBinary(response, 0, nonce);
    response[pos++] = ',';
    pos = appendBinary(response, pos, elapsed);
    response[pos++] = ',';
    for (uint8_t i = 0; i < 22; i++) response[pos++] = ducoid[i];
    response[pos++] = '\n';
    Serial.write(response, pos);
  }
}
