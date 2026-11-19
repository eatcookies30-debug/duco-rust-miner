/*
  I2C bench master.

  Drives one or more mining slaves on the TWI bus the way an ESP rig master
  does, so a board's I2C path can be checked end to end without a PC-side rig.
  SDA to SDA, SCL to SCL, GND to GND:

      ATmega328P (Uno, Nano, Pro Mini)  SDA = A4,  SCL = A5
      ATmega1280/2560 (Mega)            SDA = D20, SCL = D21

  The TWI hardware is on different pins on the two families, so a Mega wired to
  its analog A4/A5 is connected to nothing.

  Run it on any Arduino. Open the serial monitor at 115200: it lists every slave
  that answers, sends each one a job whose nonce is known in advance, prints the
  share the slave returns, and keeps a running total so a flaky wire shows up as
  a mismatch rate rather than as a single odd line.

  The protocol is the one from https://github.com/ricaun/DuinoCoinI2C: the job
  goes out one byte per transmission, the answer comes back one byte per poll,
  and a newline means "the line is finished".
*/

#include <Wire.h>

#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
#define TWI_SDA_PIN 20
#define TWI_SCL_PIN 21
#else
#define TWI_SDA_PIN A4
#define TWI_SCL_PIN A5
#endif

#define BUS_SCAN_LIMIT 32
#define BUS_CLOCK_HZ 100000
#define REPEAT_SECONDS 20
/* Difficulty 100 caps the search at 10,001 nonces, so a Nano answers in about
   6 s; 20 s is comfortably longer than that without hiding a dead board. */
#define POLL_TIMEOUT_MS 20000
/* A job that gets a byte dropped is refused by the slave, so a slave that never
   answers has probably never seen the job at all. The bench sends it again. */
#define JOB_ATTEMPTS 3

/* The job below is SHA-1("ba29a15896fd2d792d5c4b60668bf2b9feebc51d" + "9999"),
   so a correct slave answers with 9999. */
static const char PREV_HASH[] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";
static const char TARGET_HASH[] = "3169de686edf3ec107cc21cb0d9cfce0837f34a3";
static const uint32_t EXPECTED_NONCE = 9999;
static const uint32_t DIFFICULTY = 100;

static uint32_t jobsSent = 0;
static uint32_t repliesOk = 0;
static uint32_t repliesBad = 0;
static uint32_t bytesNacked = 0;
static uint32_t sends = 0;

static bool slave_exists(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

/* One transmission per byte, exactly like the rig masters. A byte the slave
   does not acknowledge is sent again, because a short job is simply dropped by
   the slave and the master would otherwise sit and wait on a job that never
   arrived. */
static void send_job(uint8_t address, const char *job) {
  for (uint8_t i = 0; job[i]; i++) {
    bool written = false;
    for (uint8_t attempt = 0; attempt < 3 && !written; attempt++) {
      Wire.beginTransmission(address);
      Wire.write(job[i]);
      written = Wire.endTransmission() == 0;
    }
    if (!written) bytesNacked++;
    sends++;
  }
}

/* Returns true when a line came back, false when the slave stayed silent for
   POLL_TIMEOUT_MS. */
static bool read_line(uint8_t address, char *out, uint8_t limit) {
  uint8_t n = 0;
  unsigned long started = millis();
  while (millis() - started < POLL_TIMEOUT_MS) {
    Wire.requestFrom(address, 1);
    if (!Wire.available()) continue;
    char c = (char)Wire.read();
    if (c == '\n') {
      /* A slave that is still searching answers every poll with a bare newline,
         so an empty line means "not done yet" and we keep asking. */
      if (n == 0) continue;
      out[n] = 0;
      return true;
    }
    if (n < limit - 1) out[n++] = c;
  }
  out[0] = 0;
  return false;
}

static void run_test(void) {
  char job[128];
  snprintf(job, sizeof(job), "%s,%s,%u\n", PREV_HASH, TARGET_HASH, DIFFICULTY);

  Serial.println("Scanning the bus for slaves...");
  uint8_t found = 0;
  for (uint8_t address = 1; address < BUS_SCAN_LIMIT; address++) {
    if (!slave_exists(address)) continue;
    found++;
    jobsSent++;

    Serial.print("Slave at ");
    Serial.print(address);
    Serial.print(": job ");
    Serial.println(job);

    char reply[64];
    uint32_t nonce = 0, elapsed = 0;
    bool answered = false;

    for (uint8_t attempt = 1; attempt <= JOB_ATTEMPTS; attempt++) {
      send_job(address, job);
      if (!read_line(address, reply, sizeof(reply))) {
        Serial.print("  no answer in ");
        Serial.print(POLL_TIMEOUT_MS / 1000);
        Serial.println(" s, sending the job again");
        continue;
      }

      /* "nonce,elapsed,DUCOID" */
      const char *p = reply;
      while (*p >= '0' && *p <= '9') nonce = nonce * 10u + (uint32_t)(*p++ - '0');
      if (*p == ',') {
        p++;
        while (*p >= '0' && *p <= '9') elapsed = elapsed * 10u + (uint32_t)(*p++ - '0');
      }
      answered = true;
      break;
    }

    Serial.print("  reply: ");
    Serial.println(reply);

    if (elapsed) {
      /* nonce * 1e6 overflows a 32 bit unsigned long once the nonce is five
         digits, so the multiply is done in 64 bit. */
      Serial.print("  hashrate: ");
      Serial.print((unsigned long)((unsigned long long)nonce * 1000000ULL / elapsed));
      Serial.println(" H/s");
    }

    bool ok = answered && nonce == EXPECTED_NONCE;
    if (ok) repliesOk++;
    else repliesBad++;

    Serial.print("  nonce: ");
    Serial.print(nonce);
    Serial.println(ok ? " OK" : " MISMATCH");
  }

  if (!found) {
    Serial.println("No slave answered. Check SDA to SDA, SCL to SCL and GND to GND.");
    Serial.print("On this board the bus is on pin ");
    Serial.print(TWI_SDA_PIN);
    Serial.print(" (SDA) and pin ");
    Serial.print(TWI_SCL_PIN);
    Serial.println(" (SCL).");
  }

  Serial.print("Totals: ");
  Serial.print(jobsSent);
  Serial.print(" jobs, ");
  Serial.print(repliesOk);
  Serial.print(" correct, ");
  Serial.print(repliesBad);
  Serial.print(" wrong, ");
  Serial.print(bytesNacked);
  Serial.print(" of ");
  Serial.print(sends);
  Serial.println(" job bytes the slave did not acknowledge.");
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  /* Weak internal pull-ups hold the lines from floating when only two boards
     share the bus and there are no external resistors. */
  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(BUS_CLOCK_HZ);
}

void loop() {
  run_test();
  delay(REPEAT_SECONDS * 1000UL);
}
