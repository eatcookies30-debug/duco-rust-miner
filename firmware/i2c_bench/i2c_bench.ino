/*
  I2C bench master.

  Drives one or more mining slaves on the A4/A5 bus the way an ESP rig master
  does, so a board's I2C path can be checked end to end without a PC-side rig:

      master A4 -- slave A4     (SDA)
      master A5 -- slave A5     (SCL)
      master GND -- slave GND

  Run it on any Arduino (a Mega 2560 or an Uno is fine). Open the serial
  monitor at 115200: it lists every slave that answers, sends each one a job
  whose nonce is known in advance, and prints the share the slave returns.

  The protocol is the one from https://github.com/ricaun/DuinoCoinI2C: the job
  goes out one byte per transmission, the answer comes back one byte per poll,
  and a newline means "the line is finished".
*/

#include <Wire.h>

#define BUS_SCAN_LIMIT 32
#define POLL_TIMEOUT_MS 60000

/* The job below is SHA-1("ba29a15896fd2d792d5c4b60668bf2b9feebc51d" + "9999"),
   so a correct slave answers with 9999. */
static const char PREV_HASH[] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";
static const char TARGET_HASH[] = "06c4b35738a4611b85aff1250fd22f669c27747a";
static const uint32_t EXPECTED_NONCE = 9999;
static const uint32_t DIFFICULTY = 100;

static bool slave_exists(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

static void send_job(uint8_t address, const char *job) {
  /* One transmission per byte, exactly like the rig masters. */
  for (uint8_t i = 0; job[i]; i++) {
    Wire.beginTransmission(address);
    Wire.write(job[i]);
    Wire.endTransmission();
  }
}

static void read_line(uint8_t address, char *out, uint8_t limit) {
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
      break;
    }
    if (n < limit - 1) out[n++] = c;
  }
  out[n] = 0;
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  Wire.begin();
  Wire.setClock(100000);

  char job[128];
  snprintf(job, sizeof(job), "%s,%s,%u\n", PREV_HASH, TARGET_HASH, DIFFICULTY);

  Serial.println("Scanning the bus for slaves...");
  uint8_t found = 0;
  for (uint8_t address = 1; address < BUS_SCAN_LIMIT; address++) {
    if (!slave_exists(address)) continue;
    found++;

    Serial.print("Slave at ");
    Serial.print(address);
    Serial.print(": job ");
    Serial.println(job);

    send_job(address, job);

    char reply[64];
    read_line(address, reply, sizeof(reply));

    Serial.print("  reply: ");
    Serial.println(reply);

    /* "nonce,elapsed,DUCOID" */
    uint32_t nonce = 0, elapsed = 0;
    const char *p = reply;
    while (*p >= '0' && *p <= '9') nonce = nonce * 10u + (uint32_t)(*p++ - '0');
    if (*p == ',') {
      p++;
      while (*p >= '0' && *p <= '9') elapsed = elapsed * 10u + (uint32_t)(*p++ - '0');
    }
    if (elapsed) {
      Serial.print("  hashrate: ");
      Serial.print((unsigned long)((unsigned long)nonce * 1000000UL / elapsed));
      Serial.println(" H/s");
    }
    Serial.print("  nonce: ");
    Serial.print(nonce);
    Serial.println(nonce == EXPECTED_NONCE ? " OK" : " MISMATCH");
  }

  if (!found) Serial.println("No slave answered. Check the A4/A5/GND wiring.");
}

void loop() {}
