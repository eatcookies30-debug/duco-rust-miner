/* Stand-in for Arduino.h, just enough to compile the I2C slave glue on a
   desktop. It is not part of the firmware; see slave_test.cpp. */
#ifndef DUCO_STUB_ARDUINO_H
#define DUCO_STUB_ARDUINO_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define A4 4
#define A5 5
#define INPUT_PULLUP 2

static inline void pinMode(int, int) {}
static inline unsigned long millis() { return 0; }

#endif
