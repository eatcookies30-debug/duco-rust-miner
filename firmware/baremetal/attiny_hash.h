/*
  The search an ATtiny25/45/85 runs, and the little it shares with the driver.

  These chips have between 128 and 512 bytes of RAM, and the ATtiny25 is at the
  bottom of that range, so the job is never held as a string: the bytes coming
  off the bus are written straight into attiny_prev and attiny_target, which is
  the whole 86 character job in 40 bytes.

  attiny_window is the schedule. It is empty between searches, and a share line
  is shorter than it, so the driver writes its answer into it. That is why it is
  visible here rather than private to the search.
*/

#ifndef ATTINY_HASH_H
#define ATTINY_HASH_H

#include <stdint.h>

extern uint8_t attiny_prev[20];     /* previous hash, raw               */
extern uint32_t attiny_target[5];   /* target digest, raw               */
extern uint32_t attiny_window[16];  /* schedule window, free between searches */

void attiny_finish_target(void);
uint32_t attiny_mine(uint16_t maxNonce);
char attiny_hex_char(uint8_t nibble);

#endif
