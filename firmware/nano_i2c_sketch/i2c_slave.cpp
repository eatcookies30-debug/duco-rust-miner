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

/* Two 40 character hashes, a difficulty and the separators come to 86 bytes.
   Room for two of them matters: a master that pushes a new job while the board
   is still searching the previous one needs somewhere to put it. The buffer is
   indexed by wrapping 8 bit positions, so its size is a power of two. */
#define JOB_LIMIT 256
/* "nonce,elapsed,DUCOID\n" - the longest value the protocol can produce. */
#define RESP_LIMIT 48
#define FIRST_ADDRESS 1
#define LAST_ADDRESS 127

/* Set SLAVE_ADDRESS to a number 1-126 to give a board a permanent name on the
   rig. Left alone, a board walks the range and takes the first address nobody
   answers to, which is how boards get their numbers when they are connected one
   at a time. The numbers move whenever a board resets, so on a rig where the
   address is how a share is attributed, build each board with
   -DSLAVE_ADDRESS=<n>. A fixed address that is already taken is not traded for
   another one: the board reports that it has no address and mines over serial
   until the bus clears, rather than quietly ending up with a number nobody
   expected. */
#ifndef SLAVE_ADDRESS
#define SLAVE_ADDRESS 0
#endif

static char jobIn[JOB_LIMIT];
static volatile uint8_t jobHead;    /* where the interrupt writes next */
static volatile uint8_t jobTail;    /* first byte the main loop has not taken yet */
static volatile uint8_t jobOverflow;

static char reply[RESP_LIMIT];
static volatile uint8_t replyLen;
static volatile uint8_t replyNext;

void receiveEvent(int count) {
  while (count-- > 0) {
    char c = Wire.read();
    if ((uint8_t)(jobHead - jobTail) >= JOB_LIMIT - 1) {
      /* The buffer filled before the main loop could take a job out of it. A
         half stored job would splice onto the bytes that follow and be parsed as
         a job with the wrong hashes, so nothing in it is read until the main
         loop clears it, and the next newline starts clean. */
      jobOverflow = 1;
      continue;
    }
    jobIn[jobHead++] = c;
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

/* Copy one comma separated field into dest, stopping at the comma or at the end
   of the job, and step *pos past the separators. The job is read out of the
   ring buffer, so a field is addressed as base + offset. Returns how many
   characters the field held, even if it was longer than dest. */
static uint8_t readField(char *dest, uint8_t limit, const char *job, uint8_t base,
                         uint8_t len, uint8_t *pos) {
  uint8_t n = 0, total = 0;
  while (*pos < len && job[(uint8_t)(base + *pos)] != ',') {
    if (n < limit) dest[n++] = job[(uint8_t)(base + *pos)];
    total++;
    (*pos)++;
  }
  while (*pos < len && job[(uint8_t)(base + *pos)] == ',') (*pos)++;
  return total;
}

/* A start condition needs both lines high. Probing while another device is mid
   transaction does nothing useful, and on the AVR a START that never completes
   leaves the module asking for the start condition and holding SDA low, which
   jams the bus for every board on it, this one included. Wire does not report
   that, so the lines are read first. Returns 1 when the address is free. */
static bool addressFree(uint8_t candidate) {
  if (!(digitalRead(TWI_SDA_PIN) && digitalRead(TWI_SCL_PIN))) return false;
  Wire.beginTransmission(candidate);
  return Wire.endTransmission() != 0;
}

uint8_t i2c_begin(void) {
  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);
  Wire.begin();

  uint8_t address = 0;

  /* A fixed address is tried on its own until it comes free. Nothing is
     substituted for it, so a board keeps the number the rig knows it by. */
  if (SLAVE_ADDRESS >= FIRST_ADDRESS && SLAVE_ADDRESS < LAST_ADDRESS) {
    for (uint8_t pass = 0; pass < 6 && !address; pass++) {
      if (addressFree(SLAVE_ADDRESS)) address = SLAVE_ADDRESS;
      else delay(200);
    }
  }

  /* Otherwise a transmission to a free address gets no acknowledge, so the
     first address that does not answer is the one this board takes. The bus may
     simply be busy at the instant the board comes up, which is not the same as
     every address being taken, so the range is walked a few times. */
  for (uint8_t pass = 0; !address && pass < 3; pass++) {
    for (uint8_t candidate = FIRST_ADDRESS; candidate < LAST_ADDRESS; candidate++) {
      if (addressFree(candidate)) {
        address = candidate;
        break;
      }
    }
    if (!address) delay(200);
  }

  Wire.end();

  if (!address) return 0;

  Wire.begin(address);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  return address;
}

bool i2c_take_job(char *prevHash, char *targetHash, uint32_t *difficulty) {
  if (jobOverflow) {
    jobOverflow = 0;
    jobTail = jobHead;
  }

  /* Snapshot how much of the job the interrupt has handed over. It keeps
     appending while this scans, and comparing the scan position against a length
     that has grown since the scan started is what makes a job still in transit
     look like a whole job: the scan runs off the end of the snapshot, the length
     moves on, and the incomplete bytes get read as a job whose hashes are short.
     */
  uint8_t tail = jobTail;
  uint8_t head = jobHead;
  uint8_t end = tail;
  while (end != head && jobIn[end] != '\n') end++;
  if (end == head) return false;

  uint8_t len = (uint8_t)(end - tail);
  uint8_t pos = 0;
  uint8_t prevLen = readField(prevHash, 40, jobIn, tail, len, &pos);
  uint8_t targetLen = readField(targetHash, 40, jobIn, tail, len, &pos);

  uint32_t d = 0;
  uint8_t digits = 0;
  while (pos < len && jobIn[(uint8_t)(tail + pos)] >= '0' &&
         jobIn[(uint8_t)(tail + pos)] <= '9') {
    d = d * 10u + (uint32_t)(jobIn[(uint8_t)(tail + pos)] - '0');
    digits++;
    pos++;
  }

  /* Only this job is consumed; anything the master sent after it stays in the
     buffer for the next pass. */
  jobTail = (uint8_t)(end + 1);

  if (prevLen != 40 || targetLen != 40 || digits == 0) return false;
  prevHash[40] = 0;
  targetHash[40] = 0;
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
