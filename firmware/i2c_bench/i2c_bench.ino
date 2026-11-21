/*
  I2C rig master / bench for the DuinoCoinI2C protocol.

  Drives every slave on the TWI bus at the same time, so a rig of several boards
  can be checked end to end without a PC-side rig master. SDA to SDA, SCL to
  SCL, GND to GND:

      ATmega328P (Uno, Nano, Pro Mini)  SDA = A4,  SCL = A5
      ATmega1280/2560 (Mega)            SDA = D20, SCL = D21

  The TWI hardware is on different pins on the two families, so a Mega wired to
  its analog A4/A5 is connected to nothing.

  Each slave gets its own job, and every slave is polled in turn while all of
  them are searching, so N boards mine at the same time instead of one at a
  time. Open the serial monitor at 115200: it lists every slave that answers,
  prints each share as it comes back with that board's hashrate, and keeps
  running totals, so a flaky wire shows up as a board's own mismatch rate
  rather than as a single odd line.

  The protocol is the one from https://github.com/ricaun/DuinoCoinI2C: the job
  goes out one byte per transmission, the answer comes back one byte per poll,
  and a newline means "the line is finished". A slave that is still searching
  answers a poll with a bare newline, which is how the master knows to keep
  asking.

  Boards claim their own addresses (see i2c_slave.h). If you want the numbers to
  be stable across resets, build each board with -DSLAVE_ADDRESS=<1-126>.

  MAX_SLAVES is 10 and the master carries a 48 byte reply buffer for each slave:
  comfortable on a Mega, tight on a 328P board acting as the master.
*/

#include <Wire.h>

#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
#define TWI_SDA_PIN 20
#define TWI_SCL_PIN 21
#else
#define TWI_SDA_PIN A4
#define TWI_SCL_PIN A5
#endif

#define MAX_SLAVES 10
/* Addresses 1 to 126, the same range the slaves claim from. */
#define FIRST_ADDRESS 1
#define LAST_ADDRESS 127
#define BUS_CLOCK_HZ 100000
/* How often the bus is walked again, so a board plugged in later is picked up
   without restarting the master. */
#define RESCAN_SECONDS 30
/* Difficulty 100 caps the search at 10,001 nonces, so a Nano answers in about
   6 s; 20 s is comfortably longer than that without hiding a dead board. */
#define POLL_TIMEOUT_MS 20000
/* A job that gets a byte dropped is refused by the slave, so a slave that never
   answers has probably never seen the job at all. The master sends it again. */
#define JOB_ATTEMPTS 3

/* The job below is SHA-1("ba29a15896fd2d792d5c4b60668bf2b9feebc51d" + "9999"),
   so a correct slave answers with 9999. */
static const char PREV_HASH[] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";
static const char TARGET_HASH[] = "3169de686edf3ec107cc21cb0d9cfce0837f34a3";
static const uint32_t EXPECTED_NONCE = 9999;
static const uint32_t DIFFICULTY = 100;

struct Slave {
  uint8_t address;
  bool searching;              /* a job is in flight and no line has come back */
  char reply[48];
  uint8_t replyLen;
  unsigned long jobSentAt;
  uint8_t attempts;
  uint32_t jobs, ok, bad;
  uint32_t lastHashrate;       /* that board's own figure from its last share */
  bool alive;                  /* cleared when it stops answering altogether */
};

static Slave slaves[MAX_SLAVES];
static uint8_t slaveCount = 0;

static uint32_t jobsSent = 0;
static uint32_t repliesOk = 0;
static uint32_t repliesBad = 0;
static uint32_t bytesNacked = 0;
static uint32_t sends = 0;

/* A start condition needs both lines high. Probing while some other device is
   mid-transaction accomplishes nothing, and on the AVR a START that never
   completes leaves the module asking for the condition and holding SDA low,
   which jams the bus for everybody on it. Wire does not report that, so the
   lines are checked before anything is attempted. */
static bool busIdle(void) {
  return digitalRead(TWI_SDA_PIN) && digitalRead(TWI_SCL_PIN);
}

static void waitForIdleBus(void) {
  for (uint8_t i = 0; i < 20 && !busIdle(); i++) delay(5);
}

static bool slave_exists(uint8_t address) {
  waitForIdleBus();
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

/* Walks the address range and adds anything that answers. Boards already known
   are left alone, so their counts do not reset when the bus is rescanned. */
static void discover(void) {
  for (uint8_t address = FIRST_ADDRESS; address < LAST_ADDRESS; address++) {
    bool known = false;
    for (uint8_t i = 0; i < slaveCount; i++)
      if (slaves[i].address == address) { known = true; break; }
    if (known) continue;
    if (!slave_exists(address)) continue;
    if (slaveCount == MAX_SLAVES) {
      Serial.print("More than ");
      Serial.print(MAX_SLAVES);
      Serial.print(" slaves on the bus; raise MAX_SLAVES to address ");
      Serial.println(address);
      return;
    }
    Slave &s = slaves[slaveCount++];
    s.address = address;
    s.searching = false;
    s.replyLen = 0;
    s.attempts = 0;
    s.jobs = s.ok = s.bad = 0;
    s.lastHashrate = 0;
    s.alive = true;
    Serial.print("Slave at address ");
    Serial.println(address);
  }
}

/* One transmission per byte, exactly like the rig masters. A byte the slave
   does not acknowledge is sent again, because a short job is simply dropped by
   the slave and the master would otherwise sit and wait on a job that never
   arrived. */
static void send_job(uint8_t address, const char *job) {
  for (uint8_t i = 0; job[i]; i++) {
    bool written = false;
    for (uint8_t attempt = 0; attempt < JOB_ATTEMPTS && !written; attempt++) {
      Wire.beginTransmission(address);
      Wire.write(job[i]);
      written = Wire.endTransmission() == 0;
    }
    if (!written) bytesNacked++;
    sends++;
  }
}

static void printHashrate(uint32_t nonce, uint32_t elapsed) {
  if (!elapsed) return;
  /* nonce * 1e6 overflows a 32 bit unsigned long once the nonce is five
     digits, so the multiply is done in 64 bit. */
  Serial.print((unsigned long)((unsigned long long)nonce * 1000000ULL / elapsed));
  Serial.print(" H/s");
}

static uint32_t parseNonce(const char *line) {
  uint32_t n = 0;
  const char *p = line;
  while (*p >= '0' && *p <= '9') n = n * 10u + (uint32_t)(*p++ - '0');
  return n;
}

static uint32_t parseElapsed(const char *line) {
  const char *p = line;
  while (*p >= '0' && *p <= '9') p++;
  if (*p != ',') return 0;
  p++;
  uint32_t n = 0;
  while (*p >= '0' && *p <= '9') n = n * 10u + (uint32_t)(*p++ - '0');
  return n;
}

/* Collect one byte from one slave. Returns true when that byte finished a
   share. A bare newline from a slave that is still searching is not a line, so
   it is dropped and the master comes back to this board on the next round. The
   poll is deliberately a single request per board per round: the master must
   move on to the next board, otherwise a slave that is still searching would
   hold the bus and every other board on it would sit idle. */
static bool poll_slave(Slave &s) {
  Wire.requestFrom(s.address, 1);
  if (!Wire.available()) return false;
  char c = (char)Wire.read();
  if (c == '\n') {
    if (s.replyLen == 0) return false;
    s.reply[s.replyLen] = 0;
    return true;
  }
  if (s.replyLen < sizeof(s.reply) - 1) s.reply[s.replyLen++] = c;
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  /* Weak internal pull-ups hold the lines from floating. Boards wired to the
     same net put theirs in parallel, which only makes the lines stronger. */
  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(BUS_CLOCK_HZ);

  discover();
  if (!slaveCount) {
    Serial.println("No slave answered. Check SDA to SDA, SCL to SCL and GND to GND.");
    Serial.print("On this board the bus is on pin ");
    Serial.print(TWI_SDA_PIN);
    Serial.print(" (SDA) and pin ");
    Serial.print(TWI_SCL_PIN);
    Serial.println(" (SCL).");
  }
}

void loop() {
  char job[128];
  snprintf(job, sizeof(job), "%s,%s,%u\n", PREV_HASH, TARGET_HASH, DIFFICULTY);

  unsigned long lastRescan = millis();

  while (true) {
    if (millis() - lastRescan > (unsigned long)RESCAN_SECONDS * 1000UL) {
      lastRescan = millis();
      discover();
    }

    unsigned long now = millis();
    uint32_t roundJobs = 0;

    for (uint8_t i = 0; i < slaveCount; i++) {
      Slave &s = slaves[i];
      if (!s.alive) continue;

      if (!s.searching) {
        send_job(s.address, job);
        s.searching = true;
        s.replyLen = 0;
        s.attempts = 1;
        s.jobSentAt = millis();
        s.jobs++;
        jobsSent++;
        roundJobs++;
        continue;
      }

      /* One poll per board, then on to the next one. Every board is searching
         in its own main loop while the master works through the list, so all of
         them mine at once. */
      if (poll_slave(s)) {
        uint32_t nonce = parseNonce(s.reply);
        uint32_t elapsed = parseElapsed(s.reply);
        bool ok = nonce == EXPECTED_NONCE;
        if (ok) repliesOk++; else repliesBad++;
        s.ok += ok;
        s.bad += !ok;
        s.searching = false;
        s.lastHashrate = elapsed ? (uint32_t)((unsigned long long)nonce * 1000000ULL / elapsed) : 0;

        Serial.print("slave ");
        Serial.print(s.address);
        Serial.print(": nonce ");
        Serial.print(nonce);
        Serial.print(ok ? " OK, " : " MISMATCH, ");
        printHashrate(nonce, elapsed);
        Serial.print("  (");
        Serial.print(s.ok);
        Serial.print("/");
        Serial.print(s.jobs);
        Serial.println(" so far)");
        continue;
      }

      if (now - s.jobSentAt > POLL_TIMEOUT_MS) {
        s.searching = false;
        s.replyLen = 0;
        if (s.attempts >= JOB_ATTEMPTS) {
          Serial.print("slave ");
          Serial.print(s.address);
          Serial.print(": no answer after ");
          Serial.print(JOB_ATTEMPTS);
          Serial.print(" attempts, dropping it");
          Serial.println(s.jobs ? " (it answered before, check its wiring)" : "");
          repliesBad += 1;
          s.bad++;
          s.alive = false;
        } else {
          s.attempts++;
          Serial.print("slave ");
          Serial.print(s.address);
          Serial.print(": no answer in ");
          Serial.print(POLL_TIMEOUT_MS / 1000);
          Serial.println(" s, sending the job again");
        }
      }
    }

    if (roundJobs) {
      /* The bus total is the sum of what each board last reported for itself.
         Every board measures its own elapsed time, so the figure does not
         depend on how long the master took to collect the share. */
      uint32_t busHash = 0;
      uint8_t mining = 0;
      for (uint8_t i = 0; i < slaveCount; i++) {
        if (slaves[i].alive && slaves[i].lastHashrate) {
          busHash += slaves[i].lastHashrate;
          mining++;
        }
      }

      Serial.print("Totals: ");
      Serial.print(slaveCount);
      Serial.print(" slaves, ");
      Serial.print(jobsSent);
      Serial.print(" jobs, ");
      Serial.print(repliesOk);
      Serial.print(" correct, ");
      Serial.print(repliesBad);
      Serial.print(" wrong, ");
      Serial.print(bytesNacked);
      Serial.print(" of ");
      Serial.print(sends);
      Serial.print(" job bytes unacked, ");
      Serial.print(busHash);
      Serial.print(" H/s across the ");
      Serial.print(mining);
      Serial.println(" mining slaves");
    }

    delay(50);
  }
}
