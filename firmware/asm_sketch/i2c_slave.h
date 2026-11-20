/*
  I2C (TWI) slave side of the DuinoCoinI2C rig protocol:
  https://github.com/ricaun/DuinoCoinI2C

  A master (ESP32, ESP8266, Raspberry Pi, or another Arduino) drives the bus.
  It pushes a job to the board as "prevHash,targetHash,difficulty" one byte at
  a time, then keeps asking for one byte at a time until it gets a finished
  share. This module buffers the job, hands it to the sketch's search, and
  serves the answer back a byte per request.
*/

#ifndef DUCO_I2C_SLAVE_H
#define DUCO_I2C_SLAVE_H

#include <stdint.h>

/* Picks a bus address and attaches the board as a slave. With SLAVE_ADDRESS set
   to a number 1-126 (see i2c_slave.cpp) the board uses that one, retrying while
   the bus is busy; otherwise it walks 1 to 126 and takes the first address nobody
   acknowledges. Returns 0 when the fixed address is taken or every address is in
   use, in which case the board stays on serial only. */
uint8_t i2c_begin(void);

/* Returns true when a complete job has arrived, filling prevHash and
   targetHash (41 bytes each, NUL terminated) and difficulty. The buffers are
   the caller's, so the search path can keep them wherever it likes. */
bool i2c_take_job(char *prevHash, char *targetHash, uint32_t *difficulty);

/* Queues "nonce,elapsed,DUCOID\n" for the master to read one byte at a time. */
void i2c_submit(uint32_t nonce, uint32_t elapsed, const char *ducoid);

#endif
