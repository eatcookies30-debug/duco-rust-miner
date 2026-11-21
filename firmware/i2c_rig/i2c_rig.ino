/*
  Rig master for a Duino-Coin I2C mining rig.

  This is the board in the middle of the rig: it talks to the miner over USB
  serial and to every mining board on the I2C bus, giving each board its own
  job and collecting each board's share back. SDA to SDA, SCL to SCL, GND to
  GND:

      ATmega328P (Uno, Nano, Pro Mini)  SDA = A4,  SCL = A5
      ATmega1280/2560 (Mega)            SDA = D20, SCL = D21

  The TWI hardware is on different pins on the two families, so a Mega wired to
  its analog A4/A5 is connected to nothing.

  Every board on the bus needs its own address, and the slave sketches take it
  from the SLAVE_ADDRESS line at the top of their own .ino file.

  ------------------------------------------------------------
  USER SETTINGS - the only lines you need to change

  MAX_SLAVES       how many boards this master will drive. Each one costs a
                   48 byte reply buffer plus a little bookkeeping: comfortable
                   on a Mega, tight on a 328P board acting as the master.
  RIG_BUS_KHZ      the bus speed in kHz. 100 is fine for short leads; use 50 if
                   the wires are long, many boards are hanging off them, or
                   replies start getting dropped.
  FIRST_ADDRESS / LAST_ADDRESS
                   which board numbers the master looks for. Boards find their
                   own number, so a rig numbered 1 to 6 only needs 1 to 6.
                   A wide range means the master probes addresses that are not
                   there, which is bus traffic that mines nothing.
  RESCAN_SECONDS    how often the master walks the bus again, so a board plugged
                   in later joins without restarting anything.

  The on-board LED double-flashes for every share the rig brings back, from any
  board on the bus.
  ------------------------------------------------------------ */

#define MAX_SLAVES         6
#define RIG_BUS_KHZ        100
#define FIRST_ADDRESS      1
#define LAST_ADDRESS       40
#define RESCAN_SECONDS     60
#define SILENCE_TIMEOUT_MS 20000

/* ---------------------------------------------------------------- */

#include <Wire.h>

#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
#define TWI_SDA_PIN 20
#define TWI_SCL_PIN 21
#else
#define TWI_SDA_PIN A4
#define TWI_SCL_PIN A5
#endif

/* The on-board LED is pin 13 on both families, but pin 13 is bit 5 of port B on
   a 328P and bit 7 of port B on a 1280/2560, so the bit has to be picked per
   chip. Writing the port directly is what lets the LED blink while a search is
   running: digitalWrite() cannot be called from the timer interrupt that drives
   the blink. */
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

/* A board that sends nothing at all - not even the newline a searching board
   sends - has not got the job. The master sends it again, and after this many
   tries gives up on that board. */
#define JOB_ATTEMPTS 3

/* Two ways to run the rig:

  1. With the miner. Start `miner/duco-miner --rig /dev/ttyACM1` and the rig
     master takes its jobs from the pool through the miner and answers with
     shares. The lines on the serial port are:

         to the master:  MODE rig                    switch off the self-test
                        GIVE <addr> <prev>,<target>,<diff>
                        PING
         from the master: RIG <addr> <addr> ...     the boards on the bus
                         SHARE <addr> <nonce>,<elapsed>,<DUCOID>
                         BUSY <addr>                that board is still searching
                         MISS <addr> <reason>       that board never answered

     Shares are plain decimal numbers over the bus, not the base-2 the serial
     protocol of an AVR board uses.

  2. By itself. Open a serial monitor at 115200 and the master invents its own
     job for every board it finds, prints each share and the running totals,
     and tells you whether the wiring is good. That is the self-test; it stops
     as soon as the miner starts sending jobs.

  The bus protocol is the one from https://github.com/ricaun/DuinoCoinI2C: the
  job goes out one byte per transmission, the answer comes back one byte per
  poll, and a newline means "the line is finished". A board that is still
  searching answers a poll with a bare newline, which is how the master knows to
  keep asking.

  MAX_SLAVES is the limit; raise it for a bigger rig, keeping in mind that the
  master carries a 48 byte reply buffer for each slave.
*/

struct Slave {
  uint8_t address;
  bool searching;              /* a job is in flight and no line has come back */
  char reply[48];
  uint8_t replyLen;
  char job[88];                /* the job text, newline included */
  uint8_t jobLen;
  bool jobFromPool;            /* the job came from the miner, not the self-test */
  unsigned long lastReplyAt;   /* the last byte any poll returned */
  uint8_t attempts;
  uint8_t retries;             /* times this reply line was asked for again */
  uint32_t jobs, ok, bad;
  uint32_t lastHashrate;       /* that board's own figure from its last share */
  bool alive;                  /* cleared when it stops answering altogether */
};

static Slave slaves[MAX_SLAVES];
static uint8_t slaveCount = 0;
static bool poolDriven = false;

static void readSerialLines(void);   /* the miner's lines are handled in the loop */

static uint32_t jobsSent = 0;
static uint32_t repliesOk = 0;
static uint32_t repliesBad = 0;
static uint32_t bytesNacked = 0;
static uint32_t sends = 0;

/* The self-test job is SHA-1("ba29a15896fd2d792d5c4b60668bf2b9feebc51d" +
   "9999"), so a correct board answers with 9999. */
static const char SELF_PREV[] = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d";
static const char SELF_TARGET[] = "3169de686edf3ec107cc21cb0d9cfce0837f34a3";
static const uint32_t SELF_NONCE = 9999;
static const uint32_t SELF_DIFF = 100;

static void ledOn() {
#ifdef LED_DIRECT
  LED_PORT |= LED_BIT;
#else
  digitalWrite(LED_BUILTIN, HIGH);
#endif
}

static void ledOff() {
#ifdef LED_DIRECT
  LED_PORT &= ~LED_BIT;
#else
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

static void ledToggle() {
#ifdef LED_DIRECT
  LED_PORT ^= LED_BIT;
#else
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
#endif
}

/* Two short flashes for every share the rig brings back. The flashing is done
   in small slices, and each slice is a chance to take in what the miner is
   writing: a whole pause of a quarter of a second would overflow the board's
   64 byte serial buffer with a job line that arrived in the middle of it. */
static void pauseMs(unsigned long ms) {
  while (ms) {
    unsigned long step = ms > 5 ? 5 : ms;
    delay(step);
    readSerialLines();
    ms -= step;
  }
}

static void ledFlash(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
    ledOn();
    pauseMs(70);
    ledOff();
    pauseMs(70);
  }
}

/* A start condition needs both lines high. Probing while some other device is
   mid-transaction accomplishes nothing, and on the AVR a START that never
   completes leaves the module asking for the condition and holding SDA low,
   which jams the bus for everybody on it. Wire does not report that, so the
   lines are checked before anything is attempted. */
static bool busIdle(void) {
  return digitalRead(TWI_SDA_PIN) && digitalRead(TWI_SCL_PIN);
}

static void waitForIdleBus(void) {
  /* The pause is spent taking in whatever the miner is writing: the board's
     serial buffer is 64 bytes and a job line is 92, so bytes that arrive while
     the master is waiting for the bus have to be read as they come. */
  for (uint8_t i = 0; i < 20; i++) {
    readSerialLines();
    if (busIdle()) break;
    delay(5);
  }
}

static bool slaveExists(uint8_t address) {
  waitForIdleBus();
  /* Forcing a START while another device is still mid-transaction is how an
     AVR TWI module ends up holding the lines low forever, so the master only
     ever probes when both lines are high. */
  if (!busIdle()) return false;
  Wire.beginTransmission(address);
  uint8_t result = Wire.endTransmission();
  if (result == 4) {
    /* A failed start can leave the module driving the lines. Switching TWI off
       and back on lets go of them, so a jam here does not take the rig down. */
    Wire.end();
    pauseMs(20);
    Wire.begin();
    Wire.setClock((uint32_t)RIG_BUS_KHZ * 1000UL);
  }
  return result == 0;
}

static void printSlaveList(void) {
  Serial.print("RIG");
  for (uint8_t i = 0; i < slaveCount; i++) {
    Serial.print(' ');
    Serial.print(slaves[i].address);
  }
  Serial.println();
}

/* Walks the address range and adds anything that answers. Boards already known
   are left alone, so their counts do not reset when the bus is rescanned. */
static void discover(void) {
  if (slaveCount >= MAX_SLAVES) return;   /* the rig is full, nothing to look for */
  bool added = false;
  for (uint8_t address = FIRST_ADDRESS; address < LAST_ADDRESS; address++) {
    readSerialLines();   /* a job sent while the master is scanning must not be lost */
    bool known = false;
    for (uint8_t i = 0; i < slaveCount; i++)
      if (slaves[i].address == address) { known = true; break; }
    if (known) continue;
    if (!slaveExists(address)) continue;
    if (slaveCount == MAX_SLAVES) {
      Serial.print("MISS 0 more than MAX_SLAVES boards on the bus, raise MAX_SLAVES for address ");
      Serial.println(address);
      return;
    }
    Slave &s = slaves[slaveCount++];
    s.address = address;
    s.searching = false;
    s.replyLen = 0;
    s.jobLen = 0;
    s.jobFromPool = false;
    s.attempts = 0;
    s.jobs = s.ok = s.bad = 0;
    s.lastHashrate = 0;
    s.alive = true;
    added = true;
  }
  if (added) printSlaveList();
}

/* One transmission per byte, exactly like the reference rig masters. A byte the
   slave does not acknowledge is sent again, because a short job is simply
   dropped by the slave and the master would otherwise sit and wait on a job
   that never arrived. */
static void sendJob(uint8_t address, const char *job, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    /* Sending a job takes a hundred trips round the bus while the miner is
       writing the next job to the serial port. The board's receive buffer is
       only 64 bytes, so every byte put on the bus is also a chance to take in
       what has arrived on serial. */
    readSerialLines();
    bool written = false;
    for (uint8_t attempt = 0; attempt < JOB_ATTEMPTS && !written; attempt++) {
      waitForIdleBus();
      if (!busIdle()) continue;
      Wire.beginTransmission(address);
      Wire.write(job[i]);
      written = Wire.endTransmission() == 0;
    }
    if (!written) bytesNacked++;
    sends++;
  }
}

static void dispatch(Slave &s) {
  sendJob(s.address, s.job, s.jobLen);
  /* The job text is spent once it has gone out on the bus. */
  s.jobLen = 0;
  s.searching = true;
  s.replyLen = 0;
  s.attempts = 1;
  s.lastReplyAt = millis();
  s.jobs++;
  jobsSent++;
}

/* A share line is "nonce,elapsed,DUCOID...". On a busy bus a byte or two goes
   missing occasionally, usually at the start of the line, and the number that
   comes out is then nonsense. The board still has the whole line in its reply
   buffer: sending it a '?' puts the read pointer back at the beginning, so the
   same line is handed over again. */
static bool replyLooksWhole(const char *line) {
  uint8_t i = 0;
  while (line[i] >= '0' && line[i] <= '9') i++;
  if (i == 0 || line[i] != ',') return false;
  i++;
  uint8_t digits = 0;
  while (line[i] >= '0' && line[i] <= '9') {
    i++;
    digits++;
  }
  if (digits == 0 || line[i] != ',') return false;
  return line[i + 1] == 'D';
}

static void askAgain(uint8_t address) {
  waitForIdleBus();
  Wire.beginTransmission(address);
  Wire.write('?');
  Wire.endTransmission();
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

static void printHashrate(uint32_t nonce, uint32_t elapsed) {
  if (!elapsed) return;
  /* nonce * 1e6 overflows a 32 bit unsigned long once the nonce is five
     digits, so the multiply is done in 64 bit. */
  Serial.print((unsigned long)((unsigned long long)nonce * 1000000ULL / elapsed));
  Serial.print(" H/s");
}

/* Collect one byte from one board. Returns true when that byte finished a
   share. A bare newline from a board that is still searching is not a line, so
   it is dropped and the master comes back to this board on the next round. The
   poll is deliberately a single request per board per round: the master has to
   move on to the next board, otherwise a board that is still searching would
   hold the bus and every other board on it would sit idle. */
static bool pollSlave(Slave &s) {
  if (!busIdle()) return false;   /* never start a read into a jammed bus */
  Wire.requestFrom((int)s.address, 1);
  if (!Wire.available()) return false;
  s.lastReplyAt = millis();
  char c = (char)Wire.read();
  if (c == '\n') {
    if (s.replyLen == 0) return false;
    s.reply[s.replyLen] = 0;
    return true;
  }
  if (s.replyLen < sizeof(s.reply) - 1) s.reply[s.replyLen++] = c;
  return false;
}

/* A line the miner sent over serial. "GIVE 3 prev,target,diff" queues a job for
   board 3; anything else is ignored so that typing in the serial monitor does
   not break the rig. */
static void handleLine(char *line) {
  if (line[0] == 'M') {                 /* MODE rig */
    poolDriven = true;
    printSlaveList();
    return;
  }
  if (line[0] == 'P') {                 /* PING */
    Serial.println("PONG");
    return;
  }
  if (line[0] != 'G') return;           /* GIVE */

  uint8_t address = (uint8_t)strtoul(line + 5, NULL, 10);
  Slave *s = NULL;
  for (uint8_t i = 0; i < slaveCount; i++)
    if (slaves[i].address == address) { s = &slaves[i]; break; }

  if (!s) {
    Serial.print("MISS ");
    Serial.print(address);
    Serial.println(" not on this bus");
    return;
  }

  const char *job = line + 6;
  while (*job == ' ') job++;
  uint8_t len = 0;
  while (job[len] && len < sizeof(s->job) - 1) len++;
  /* A job is two 40 character hashes, a difficulty and the two commas between
     them: 84 characters. A shorter line is one that got cut off on the way in,
     and the miner is told so it can send the job again rather than waiting for
     a share that will never come. */
  if (len < 84) {
    Serial.print("MISS ");
    Serial.println(address);
    return;
  }
  memcpy(s->job, job, len);
  if (s->job[len - 1] != '\n') s->job[len++] = '\n';
  s->jobLen = len;
  s->jobFromPool = true;
  poolDriven = true;

  /* A board that is still searching cannot take the job yet - its reply line is
     halfway collected and the bus is busy with it. The job is kept here and sent
     the moment the board comes free, so the miner does not have to try again. */
  if (s->searching) {
    Serial.print("BUSY ");
    Serial.println(address);
    return;
  }
  dispatch(*s);
}

static void readSerialLines(void) {
  static char line[120];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        line[len] = 0;
        handleLine(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0;   /* a line longer than the parser can hold is thrown away whole */
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial)
    ;

  /* Weak internal pull-ups hold the lines from floating. Boards wired to the
     same net put theirs in parallel, which only makes the lines stronger. */
  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  ledOff();

  Wire.begin();
  Wire.setClock((uint32_t)RIG_BUS_KHZ * 1000UL);

  discover();
  printSlaveList();
  if (!slaveCount) {
    Serial.println("MISS 0 no board answered. Check SDA to SDA, SCL to SCL and GND to GND.");
    Serial.print("On this board the bus is on pin ");
    Serial.print(TWI_SDA_PIN);
    Serial.print(" (SDA) and pin ");
    Serial.print(TWI_SCL_PIN);
    Serial.println(" (SCL).");
  }
}

void loop() {
  char selfJob[88];
  snprintf(selfJob, sizeof(selfJob), "%s,%s,%u\n", SELF_PREV, SELF_TARGET, SELF_DIFF);

  unsigned long lastRescan = millis();
  unsigned long lastReport = millis();
  uint8_t ledTick = 0;

  while (true) {
    readSerialLines();

    if (millis() - lastRescan > (unsigned long)RESCAN_SECONDS * 1000UL) {
      lastRescan = millis();
      discover();
    }

    bool anySearching = false;

    for (uint8_t i = 0; i < slaveCount; i++) {
      Slave &s = slaves[i];
      if (!s.alive) continue;
      readSerialLines();   /* the miner's next job must not sit unread */

      if (!s.searching) {
        /* In rig mode a board waits for the miner to give it a job; in the
           self-test the master makes one up for it. */
        if (s.jobLen) {
          dispatch(s);
        } else if (!poolDriven) {
          memcpy(s.job, selfJob, sizeof(selfJob));
          s.jobLen = (uint8_t)strlen(selfJob);
          s.jobFromPool = false;
          dispatch(s);
        }
        continue;
      }

      /* One poll per board, then on to the next one. Every board is searching in
         its own main loop while the master works through the list, so all of
         them mine at once. */
      if (pollSlave(s)) {
        if (!replyLooksWhole(s.reply) && s.retries == 0) {
          s.retries = 1;
          s.replyLen = 0;
          askAgain(s.address);
          continue;
        }
        s.retries = 0;
        uint32_t nonce = parseNonce(s.reply);
        uint32_t elapsed = parseElapsed(s.reply);
        /* A job from the pool is checked by the miner itself; the self-test job
           has a known answer, so the master can judge it directly. */
        bool ok = s.jobFromPool || (nonce == SELF_NONCE);
        if (ok) repliesOk++; else repliesBad++;
        s.ok += ok;
        s.bad += !ok;
        s.searching = false;
        /* jobLen is left alone on purpose: if the miner handed over another job
           while this board was searching, that job is still queued and goes out
           on the next round. */
        s.lastHashrate = elapsed ? (uint32_t)((unsigned long long)nonce * 1000000ULL / elapsed) : 0;

        Serial.print("SHARE ");
        Serial.print(s.address);
        Serial.print(' ');
        Serial.println(s.reply);
        ledFlash(2);

        if (!poolDriven) {
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
        }
        continue;
      }

      /* A board that answers with bare newlines is still searching and doing
         fine. Only a board that has gone completely silent gets the job sent
         again, and then gets dropped. The clock is read here rather than using
         the value captured at the top of the round: the poll above just moved
         lastReplyAt forward, and subtracting a time that is newer than `now`
         wraps an unsigned long into a huge number and makes every board look
         dead. */
      if (millis() - s.lastReplyAt > SILENCE_TIMEOUT_MS) {
        s.searching = false;
        s.replyLen = 0;
        if (s.attempts >= JOB_ATTEMPTS) {
          Serial.print("MISS ");
          Serial.print(s.address);
          Serial.println(" silent");
          repliesBad += 1;
          s.bad++;
          s.alive = false;
          if (!poolDriven) {
            Serial.print("slave ");
            Serial.print(s.address);
            Serial.print(": no answer after ");
            Serial.print(JOB_ATTEMPTS);
            Serial.print(" attempts, dropping it");
            Serial.println(s.jobs ? " (it answered before, check its wiring)" : "");
          }
        } else {
          s.attempts++;
          if (!poolDriven) {
            Serial.print("slave ");
            Serial.print(s.address);
            Serial.print(": no answer in ");
            Serial.print(SILENCE_TIMEOUT_MS / 1000);
            Serial.println(" s, sending the job again");
          }
        }
      }
    }

    /* The LED blinks while the rig is working. */
    for (uint8_t i = 0; i < slaveCount; i++)
      if (slaves[i].alive && slaves[i].searching) { anySearching = true; break; }
    if (anySearching && (++ledTick % 4) == 0) ledToggle();

    if (!poolDriven && millis() - lastReport >= 5000UL) {
      lastReport = millis();
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

    /* The pause between rounds drains the serial rather than just sleeping. A
       job line is 92 characters and the board's receive buffer is 64 bytes, so
       a line that arrives while the master is busy on the bus loses its tail if
       nobody reads it promptly. */
    pauseMs(50);
  }
}
