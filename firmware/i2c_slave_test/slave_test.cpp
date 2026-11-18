/* Desktop check of the I2C slave glue: it drives i2c_slave.cpp through exactly
   the sequence a rig master uses, against the stub Wire in stub/, which models
   the AVR TWI. No board is involved; firmware/i2c_bench is the end-to-end test
   on a real bus.

     cd firmware/i2c_slave_test
     g++ -Wall -I stub -x c++ slave_test.cpp ../asm_sketch/i2c_slave.cpp -o slave_test
     ./slave_test

   The exchange format is the one from
   https://github.com/ricaun/DuinoCoinI2C: the master pushes
   "prevHash,targetHash,difficulty\n" one byte per transmission and then asks
   for one byte at a time until it gets a finished share. */
#include <stdio.h>
#include <string.h>

#include "Wire.h"
#include "../asm_sketch/i2c_slave.h"

FakeWire Wire;

static int failures = 0;

static void check(bool condition, const char *what) {
  printf("%-5s %s\n", condition ? "ok" : "FAIL", what);
  if (!condition) failures++;
}

/* The master sends one byte per transmission, like the reference masters. */
static void send_job(const char *job) {
  for (int i = 0; job[i]; i++) {
    Wire.beginTransmission(1);
    Wire.write(job[i]);
    Wire.endTransmission();
  }
}

static void read_line(char *out, int limit) {
  int n = 0;
  for (int polls = 0; polls < 200; polls++) {
    Wire.requestFrom(1, 1);
    if (!Wire.available()) break;
    char c = Wire.read();
    if (c == '\n') {
      if (n == 0) continue;  /* still searching: "nothing yet" */
      break;
    }
    if (n < limit - 1) out[n++] = c;
  }
  out[n] = 0;
}

int main(void) {
  uint8_t address = i2c_begin();
  check(address == 1, "slave takes the first address nobody answers to");

  char prev[41], target[41], line[64];
  uint32_t difficulty = 0;

  check(!i2c_take_job(prev, target, &difficulty), "no job before the newline arrives");

  send_job("ba29a15896fd2d792d5c4b60668bf2b9feebc51d,"
           "06c4b35738a4611b85aff1250fd22f669c27747a,100\n");
  check(i2c_take_job(prev, target, &difficulty), "complete job parsed");
  check(strcmp(prev, "ba29a15896fd2d792d5c4b60668bf2b9feebc51d") == 0, "previous hash");
  check(strcmp(target, "06c4b35738a4611b85aff1250fd22f669c27747a") == 0, "target hash");
  check(difficulty == 100, "difficulty");

  /* Before the search finishes, every poll is a bare newline and the master
     reads an empty line. */
  read_line(line, sizeof(line));
  check(strcmp(line, "") == 0, "a searching slave polls as an empty line");

  i2c_submit(9999, 5482440, "DUCOID12345678ABCDEF01");
  read_line(line, sizeof(line));
  check(strcmp(line, "9999,5482440,DUCOID12345678ABCDEF01") == 0, "reply line");

  /* Once the reply is drained every poll answers with a newline, which is how
     the reference masters read "nothing yet". */
  Wire.requestFrom(1, 1);
  check(Wire.available() == 1 && Wire.read() == '\n', "empty reply polls as a newline");

  send_job("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,"
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,10\n");
  check(i2c_take_job(prev, target, &difficulty), "second job parsed");
  check(strcmp(prev, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0, "second job prev hash");
  check(difficulty == 10, "second job difficulty");
  i2c_submit(7, 8, "DUCOID12345678ABCDEF01");
  read_line(line, sizeof(line));
  check(strcmp(line, "7,8,DUCOID12345678ABCDEF01") == 0, "second reply line");

  /* Two jobs pushed before the slave can answer fill the 100 byte job buffer,
     so the second one arrives incomplete. The first is still parsed correctly,
     the remainder is not accepted as a job, and nothing overruns the buffers. */
  send_job("cccccccccccccccccccccccccccccccccccccccc,"
           "dddddddddddddddddddddddddddddddddddddddd,655\n");
  send_job("eeeeeeeeeeeeeeee");
  check(i2c_take_job(prev, target, &difficulty), "first of two queued jobs parsed");
  check(strcmp(prev, "cccccccccccccccccccccccccccccccccccccccc") == 0, "queued job prev hash");
  check(difficulty == 655, "queued job difficulty");
  check(!i2c_take_job(prev, target, &difficulty), "partial job waits for its newline");
  send_job("eeeeeeeeeeeeeeee,\n");
  check(!i2c_take_job(prev, target, &difficulty), "job with a short hash refused");

  send_job("short,too,10\n");
  check(!i2c_take_job(prev, target, &difficulty), "malformed job refused");
  send_job("ba29a15896fd2d792d5c4b60668bf2b9feebc51d,"
           "06c4b35738a4611b85aff1250fd22f669c27747a,655\n");
  check(i2c_take_job(prev, target, &difficulty), "good job after a bad one");
  check(difficulty == 655, "difficulty after a bad job");

  send_job("ba29a15896fd2d792d5c4b60668bf2b9feebc51d,"
           "06c4b35738a4611b85aff1250fd22f669c27747a,65535\n");
  check(i2c_take_job(prev, target, &difficulty), "large difficulty parsed");
  check(difficulty == 65535, "large difficulty value");

  /* The longest reply the protocol can produce still fits the 48 byte buffer. */
  i2c_submit(4294967295u, 4294967295u, "DUCOIDFFFFFFFFFFFFFFFF");
  read_line(line, sizeof(line));
  check(strcmp(line, "4294967295,4294967295,DUCOIDFFFFFFFFFFFFFFFF") == 0,
        "longest possible reply fits");

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
