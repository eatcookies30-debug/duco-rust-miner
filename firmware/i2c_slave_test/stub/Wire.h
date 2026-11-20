/* Stand-in for Wire.h that models the AVR TWI the way the driver behaves: a
   master transmission to an addressed slave calls that slave's onReceive
   handler with the byte count, and a requestFrom calls onRequest once and then
   hands the master back whatever the slave wrote. One object plays both ends of
   the bus, which is enough to drive the slave glue through a real exchange. */
#ifndef DUCO_STUB_WIRE_H
#define DUCO_STUB_WIRE_H

#include <stdint.h>
#include <string.h>
#include <functional>

struct FakeWire {
  bool isSlave = false;
  uint8_t slaveAddress = 0;
  /* Addresses some other board is holding, so a probe on them is acknowledged.
     Lets the address claim code be exercised against a bus that is not empty. */
  bool taken[128] = {};
  std::function<void(int)> onReceiveHandler;
  std::function<void()> onRequestHandler;

  void takeOver(uint8_t address) { taken[address] = true; }

  bool transmitting = false;
  uint8_t target = 0;
  uint8_t buffer[64];
  int length = 0;
  int position = 0;

  void begin() {}
  void begin(uint8_t address) {
    isSlave = true;
    slaveAddress = address;
  }
  void end() { isSlave = false; }
  void setClock(unsigned long) {}
  void onReceive(std::function<void(int)> handler) { onReceiveHandler = handler; }
  void onRequest(std::function<void()> handler) { onRequestHandler = handler; }

  void beginTransmission(uint8_t address) {
    transmitting = true;
    target = address;
    length = 0;
    position = 0;
  }

  int endTransmission() {
    transmitting = false;
    if (!isSlave || target != slaveAddress)
      return taken[target] ? 0 : 1; /* nothing acknowledges unless held */
    position = 0;
    if (onReceiveHandler) onReceiveHandler(length);
    return 0;
  }

  /* A master's write builds the transaction; a slave writing from its
     onRequest handler fills the bytes the master then reads. */
  void write(char c) {
    if (length < 64) buffer[length++] = (uint8_t)c;
  }

  uint8_t requestFrom(uint8_t address, uint8_t count) {
    if (!isSlave || address != slaveAddress) return 0;
    length = 0;
    position = 0;
    if (onRequestHandler) onRequestHandler();
    if (length > (int)count) length = (int)count;
    position = 0;
    return (uint8_t)length;
  }

  int available() { return length - position; }
  char read() { return position < length ? (char)buffer[position++] : (char)0; }
};

extern FakeWire Wire;

#endif
