/*
  I2C (TWI) slave side of the DuinoCoinI2C rig protocol:
  https://github.com/ricaun/DuinoCoinI2C

  A master (another Arduino, an ESP32, an ESP8266, a Raspberry Pi) drives the
  bus. It pushes a job to the board as "prevHash,targetHash,difficulty" one
  byte at a time, then keeps asking for one byte at a time until it gets a
  finished share. This buffers the job, hands it to the sketch's search and
  serves the answer back a byte per request.

  Everything this needs from the user is SLAVE_ADDRESS, which the sketch sets
  at the top of the .ino file. Set it to 0 and the board never touches the bus.
*/

#ifndef DUCO_I2C_SLAVE_H
#define DUCO_I2C_SLAVE_H

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <stdint.h>

/* The TWI hardware sits on A4/A5 on an ATmega328P and on digital pins 20 and
   21 on an ATmega1280/2560. Wire talks to the hardware either way, but the
   pull-ups have to be enabled on the pins the bus is actually on: a 328P board
   has no external resistors on those lines, a Mega 2560 has them onboard. */
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  #define TWI_SDA_PIN 20
  #define TWI_SCL_PIN 21
#else
  #define TWI_SDA_PIN A4
  #define TWI_SCL_PIN A5
#endif

/* Two 40 character hashes, a difficulty and the separators come to 86 bytes.
   The ring is one job wide plus room for the bytes of the next job that are
   already on their way while this board is still working. */
#define JOB_LIMIT 128
/* "nonce,elapsed,DUCOID\n" - the longest line the protocol can produce. */
#define RESP_LIMIT 48

#define FIRST_ADDRESS 1
#define LAST_ADDRESS 127

static char jobIn[JOB_LIMIT];
static volatile uint8_t jobHead;    /* where the interrupt writes next */
static volatile uint8_t jobTail;    /* first byte the main loop has not taken */
static volatile uint8_t jobOverflow;

static char reply[RESP_LIMIT];
static volatile uint8_t replyLen;
static volatile uint8_t replyNext;

/* The master asks for a reply it could not read by sending a '?' to the board:
   the reply is still in this buffer, so pointing the read pointer back at the
   start makes the board serve the same line again. A job never contains a '?',
   so the two never get confused. */
static void rigReceive(int count) {
  while (count-- > 0) {
    char c = (char)Wire.read();
    if (c == '?') {
      replyNext = 0;
      continue;
    }
    if ((uint8_t)(jobHead - jobTail) >= JOB_LIMIT - 1) {
      /* The buffer filled before the main loop could take a job out of it. A
         half stored job would splice onto the bytes that follow and be parsed
         as a job with the wrong hashes, so nothing in it is read until the main
         loop clears it, and the next newline starts clean. */
      jobOverflow = 1;
      continue;
    }
    jobIn[jobHead++] = c;
  }
}

static void rigRequest() {
  if (replyNext < replyLen) Wire.write(reply[replyNext++]);
  else Wire.write('\n');
}

static uint8_t appendDecimal(char *out, uint8_t pos, uint32_t value) {
  static char digits[10];
  uint8_t n = 0;
  do {
    digits[n++] = (char)('0' + value % 10u);
    value /= 10u;
  } while (value);
  while (n) out[pos++] = digits[--n];
  return pos;
}

/* Copy one comma separated field into dest, stopping at the comma or at the end
   of the job, and step *pos past the separators. The job is read out of the
   ring, so a field is addressed as base + offset. Returns how many characters
   the field held, even if it was longer than dest. */
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

/* A start condition needs both lines high. Probing a bus that another device
   is mid-transaction on does nothing useful, and on the AVR a START that never
   completes leaves the module asking for the start condition and holding SDA
   low, which blocks every board on the bus, this one included. Wire does not
   report that, so the lines are read first. Returns 1 when the address is free. */
static bool addressFree(uint8_t candidate) {
  if (!(digitalRead(TWI_SDA_PIN) && digitalRead(TWI_SCL_PIN))) return false;
  Wire.beginTransmission(candidate);
  uint8_t result = Wire.endTransmission();
  if (result == 4) {
    /* A start that never completed leaves this board's own TWI module driving
       the lines low, which blocks the whole bus. Switching TWI off and back on
       is the only way for a board to let go of them. */
    Wire.end();
    delay(20);
    Wire.begin();
    Wire.setClock((uint32_t)RIG_BUS_KHZ * 1000UL);
  }
  return result != 0;
}

/* Picks this board's address on the rig.
     SLAVE_ADDRESS 1-126  - the board uses that number. While another board is
                            already answering to it the board waits and tries
                            again; if the number never comes free it walks the
                            range and takes the first free one so it keeps mining
                            instead of sitting idle.
     SLAVE_ADDRESS 0      - the board is not on a rig and never touches the bus.
   Returns 0 when no address could be claimed, in which case the board mines
   over serial only. */
uint8_t i2c_begin(void) {
  if (SLAVE_ADDRESS == 0) return 0;

  pinMode(TWI_SDA_PIN, INPUT_PULLUP);
  pinMode(TWI_SCL_PIN, INPUT_PULLUP);
  Wire.begin();
  /* Only matters for the probes this board makes while it is looking for a
     free address; as a slave it never generates the clock itself. */
  Wire.setClock((uint32_t)RIG_BUS_KHZ * 1000UL);

  /* Boards do not look for an address at the same instant. Two boards trying to
     start a probe at the same moment is the common way a bus gets jammed, so
     each board waits a little longer the higher its number is. */
  delay(250 + (uint16_t)SLAVE_ADDRESS * 120);

  uint8_t address = 0;

  for (uint8_t pass = 0; pass < 6 && !address; pass++) {
    if (addressFree(SLAVE_ADDRESS)) address = SLAVE_ADDRESS;
    else delay(200);
  }

  /* The wanted address is taken by another board and has not come free. A bus
     that is only busy for a moment is not the same as every address being used,
     so the range is walked twice before the board gives up. */
  for (uint8_t pass = 0; !address && pass < 2; pass++) {
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
  Wire.onReceive(rigReceive);
  Wire.onRequest(rigRequest);
  return address;
}

/* Returns true when a whole job has arrived, filling prevHash and targetHash
   (41 bytes each, NUL terminated) and difficulty. */
bool i2c_take_job(char *prevHash, char *targetHash, uint32_t *difficulty) {
  if (jobOverflow) {
    jobOverflow = 0;
    jobTail = jobHead;
  }

  /* Snapshot how much of the job the interrupt has handed over. It keeps
     appending while this scans, and comparing the scan position against a
     length that has grown since the scan started is what makes a job still in
     transit look like a whole one: the scan runs off the end of the snapshot,
     the length moves on, and the incomplete bytes get read as a job whose
     hashes are short. */
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
     ring for the next pass. */
  jobTail = (uint8_t)(end + 1);

  if (prevLen != 40 || targetLen != 40 || digits == 0) return false;
  prevHash[40] = 0;
  targetHash[40] = 0;
  *difficulty = d;
  return true;
}

/* Queues "nonce,elapsed,DUCOID\n" for the master to read one byte at a time. */
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

#endif /* DUCO_I2C_SLAVE_H */
