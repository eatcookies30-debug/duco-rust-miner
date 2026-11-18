/*
  I2C (TWI) slave side of the DuinoCoinI2C rig protocol, following
  https://github.com/ricaun/DuinoCoinI2C

  The master pushes the job as "prevHash,targetHash,difficulty\n", one byte per
  transmission, and then polls for the answer one byte at a time. The slave
  accumulates the job until it sees the newline, runs the search, and serves the
  reply out of a small buffer. While the search is still running the slave
  answers every poll with a bare newline, which is how the reference masters
  tell "not done yet" apart from a finished share.
*/

#include <Arduino.h>
#include <string.h>

#include <Wire.h>

#include "i2c_slave.h"

/* The TWI hardware sits on A4/A5 on an ATmega328P and on digital pins 20 and
   21 on an ATmega1280/2560. Wire talks to the hardware either way, but the
   pull-ups have to be enabled on the pins the bus is actually on: a 328P board
   has no external resistors on those lines, and a Mega 2560 has them onboard. */
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
#define TWI_SDA_PIN 20
#define TWI_SCL_PIN 21
#else
#define TWI_SDA_PIN A4
#define TWI_SCL_PIN A5
#endif

/* Two 40 character hashes, a difficulty and the separators fit in 100 bytes. */
#define JOB_LIMIT 100
/* "nonce,elapsed,DUCOID\n" - the longest value the protocol can produce. */
#define RESP_LIMIT 48
#define FIRST_ADDRESS 1
#define LAST_ADDRESS 127

static char jobIn[JOB_LIMIT];
static volatile uint8_t jobLen;

static char reply[RESP_LIMIT];
static volatile uint8_t replyLen;
static volatile uint8_t replyNext;

void receiveEvent(int count) {
  while (count-- > 0) {
    char c = Wire.read();
    if (jobLen < JOB_LIMIT - 1) jobIn[jobLen++] = c;
  }
}

void requestEvent() {
  if (replyNext < replyLen) Wire.write(reply[replyNext++]);
  else Wire.write('\n');
}

static uint8_t appendDecimal(char *out, uint8_t pos, uint32_t value) {
  static char digits[10];
  uint8_t n = 0;
  do {
    digits[n++] = char('0' + value % 10u);
    value /= 10u;
  } while (value);
  while (n) out[pos++] = digits[--n];
  return pos;
}

/* Copy one comma separated field into dest, stopping at the comma or the end
   of the job, and step *i past the separators. Returns how many characters the
   field held. */
static uint8_t readField(char *dest, uint8_t limit, const char *job, uint8_t len,
                         uint8_t *i) {
  uint8_t n = 0;
  while (*i < len && job[*i] != ',') {
    if (n < limit) dest[n++] = job[*i];
    (*i)++;
  }
  while (*i < len && job[*i] == ',') (*i)++;
  return n;
}

uint8_t i2c_begin(void) {
  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);
  Wire.begin();

  /* A transmission to a free address gets no acknowledge, so the first address
     that does not answer is the one this board takes. */
  uint8_t address = 0;
  for (uint8_t candidate = FIRST_ADDRESS; candidate < LAST_ADDRESS; candidate++) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() != 0) {
      address = candidate;
      break;
    }
  }
  Wire.end();

  if (!address) return 0;

  Wire.begin(address);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  return address;
}

bool i2c_take_job(char *prevHash, char *targetHash, uint32_t *difficulty) {
  uint8_t end = 0;
  while (end < jobLen && jobIn[end] != '\n') end++;
  if (end == jobLen) return false;

  /* Only this job is consumed; anything the master sent after it stays in the
   buffer for the next pass. */
  char job[JOB_LIMIT];
  memcpy(job, jobIn, end);
  memmove(jobIn, jobIn + end + 1, jobLen - end - 1);
  jobLen -= end + 1;

  uint8_t i = 0;
  if (readField(prevHash, 40, job, end, &i) != 40) return false;
  prevHash[40] = 0;
  if (readField(targetHash, 40, job, end, &i) != 40) return false;
  targetHash[40] = 0;

  uint32_t d = 0;
  while (i < end && job[i] >= '0' && job[i] <= '9')
    d = d * 10u + (uint32_t)(job[i++] - '0');
  *difficulty = d;
  return true;
}

void i2c_submit(uint32_t nonce, uint32_t elapsed, const char *ducoid) {
  uint8_t pos = appendDecimal(reply, 0, nonce);
  reply[pos++] = ',';
  pos = appendDecimal(reply, pos, elapsed);
  reply[pos++] = ',';
  for (uint8_t i = 0; ducoid[i] && pos < RESP_LIMIT - 1; i++) reply[pos++] = ducoid[i];
  reply[pos++] = '\n';
  replyLen = pos;
  replyNext = 0;
}
