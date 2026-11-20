/* Desktop check of the I2C slave glue: it drives i2c_slave.cpp through exactly
   the sequence a rig master uses, against the stub Wire in stub/, which models
   the AVR TWI. No board is involved; firmware/i2c_bench is the end-to-end test
   on a real bus.

     cd firmware/i2c_slave_test
     g++ -Wall -pthread -I stub -x c++ slave_test.cpp ../asm_sketch/i2c_slave.cpp -o slave_test
     ./slave_test

   The exchange format is the one from
   https://github.com/ricaun/DuinoCoinI2C: the master pushes
   "prevHash,targetHash,difficulty\n" one byte per transmission and then asks
   for one byte at a time until it gets a finished share. */
#include <stdio.h>
#include <string.h>
#include <thread>
#include <chrono>

#include "Wire.h"
#include "../asm_sketch/i2c_slave.h"

FakeWire Wire;

static int failures = 0;

/* The address this run's board claimed, so the same checks work whether the board
   auto-claimed one or was built with a fixed SLAVE_ADDRESS. */
static uint8_t busAddress = 1;

static void check(bool condition, const char *what) {
  printf("%-5s %s\n", condition ? "ok" : "FAIL", what);
  if (!condition) failures++;
}

/* On a board the receive interrupt keeps handing in bytes while the main loop is
   in the middle of i2c_take_job. The stub cannot fire an interrupt inside a
   function, so a second thread plays the interrupt here: it pushes a job one byte
   at a time while the main loop scans for a finished job as fast as it can. A scan
   that compares its position with a length that has grown since the scan started
   accepts the bytes it happened to have seen as a whole job, which is how a job
   still in transit got read as a truncated one. */
static void race_test(void) {
  const char *const PREV = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *const TARGET = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *const JOB =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,123\n";
  const int ROUNDS = 20;
  volatile int feeding = 1;

  std::thread feeder([&feeding, JOB, ROUNDS]() {
    for (int round = 0; round < ROUNDS; round++) {
      for (int i = 0; JOB[i]; i++) {
        Wire.beginTransmission(busAddress);
        Wire.write(JOB[i]);
        Wire.endTransmission();
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    feeding = 0;
  });

  char prev[41], target[41];
  uint32_t difficulty = 0;
  int accepted = 0, truncated = 0, scans = 0;
  while (feeding) {
    if (i2c_take_job(prev, target, &difficulty)) {
      accepted++;
      if (strcmp(prev, PREV) != 0 || strcmp(target, TARGET) != 0 || difficulty != 123)
        truncated++;
    }
    scans++;
  }
  feeder.join();
  /* Whatever the master had pushed by the time it stopped is still in the buffer. */
  while (i2c_take_job(prev, target, &difficulty)) {
    accepted++;
    if (strcmp(prev, PREV) != 0 || strcmp(target, TARGET) != 0 || difficulty != 123)
      truncated++;
  }

  check(accepted == ROUNDS, "every job pushed while the scan was running was accepted once");
  check(truncated == 0, "no job is accepted before its newline arrives");
  printf("      (%d scans, %d jobs accepted)\n", scans, accepted);
}

/* The master sends one byte per transmission, like the reference masters. */
static void send_job(const char *job) {
  for (int i = 0; job[i]; i++) {
    Wire.beginTransmission(busAddress);
    Wire.write(job[i]);
    Wire.endTransmission();
  }
}

static void read_line(char *out, int limit) {
  int n = 0;
  for (int polls = 0; polls < 200; polls++) {
    Wire.requestFrom(busAddress, 1);
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
#ifdef TEST_ADDRESS_TAKEN
  Wire.takeOver(SLAVE_ADDRESS);
#endif
  uint8_t address = i2c_begin();
  busAddress = address;
#ifdef TEST_ADDRESS_TAKEN
  check(address != 0 && address != SLAVE_ADDRESS,
        "a fixed address already held falls back to one that is free");
#elif defined(SLAVE_ADDRESS)
  check(address == SLAVE_ADDRESS, "a fixed SLAVE_ADDRESS is claimed directly");
#else
  check(address == 1, "slave takes the first address nobody answers to");
#endif

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
  Wire.requestFrom(busAddress, 1);
  check(Wire.available() == 1 && Wire.read() == '\n', "empty reply polls as a newline");

  send_job("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,"
           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,10\n");
  check(i2c_take_job(prev, target, &difficulty), "second job parsed");
  check(strcmp(prev, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0, "second job prev hash");
  check(difficulty == 10, "second job difficulty");
  i2c_submit(7, 8, "DUCOID12345678ABCDEF01");
  read_line(line, sizeof(line));
  check(strcmp(line, "7,8,DUCOID12345678ABCDEF01") == 0, "second reply line");

  /* Two jobs pushed before the slave can answer still fit side by side in the
     job buffer, so the first one is parsed and the remainder of the second is
     kept for later. */
  send_job("cccccccccccccccccccccccccccccccccccccccc,"
           "dddddddddddddddddddddddddddddddddddddddd,655\n");
  send_job("eeeeeeeeeeeeeeee");
  check(i2c_take_job(prev, target, &difficulty), "first of two queued jobs parsed");
  check(strcmp(prev, "cccccccccccccccccccccccccccccccccccccccc") == 0, "queued job prev hash");
  check(difficulty == 655, "queued job difficulty");
  check(!i2c_take_job(prev, target, &difficulty), "partial job waits for its newline");
  send_job("eeeeeeeeeeeeeeee,\n");
  check(!i2c_take_job(prev, target, &difficulty), "job with a short hash refused");

  /* Jobs pushed while the board is still searching can overflow the job buffer.
     The glue discards the buffer rather than keep a half stored job, so nothing
     is ever parsed out of a spliced pair of jobs and the next complete job starts
     clean. Three 86 byte jobs are 258 bytes, which is more than the buffer holds. */
  send_job("1111111111111111111111111111111111111111,2222222222222222222222222222222222222222,655\n");
  send_job("3333333333333333333333333333333333333333,4444444444444444444444444444444444444444,655\n");
  send_job("5555555555555555555555555555555555555555,6666666666666666666666666666666666666666,655\n");
  check(!i2c_take_job(prev, target, &difficulty), "no job is parsed out of an overflowed buffer");
  check(!i2c_take_job(prev, target, &difficulty), "the spliced remainder is dropped");
  send_job("7777777777777777777777777777777777777777,8888888888888888888888888888888888888888,655\n");
  check(i2c_take_job(prev, target, &difficulty), "complete job after an overflow");
  check(strcmp(prev, "7777777777777777777777777777777777777777") == 0, "previous hash after an overflow");
  check(strcmp(target, "8888888888888888888888888888888888888888") == 0, "target hash after an overflow");

  send_job("short,too,10\n");
  check(!i2c_take_job(prev, target, &difficulty), "malformed job refused");
  send_job("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,10\n");
  check(!i2c_take_job(prev, target, &difficulty), "job with a hash longer than 40 refused");
  send_job("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb,\n");
  check(!i2c_take_job(prev, target, &difficulty), "job with no difficulty refused");
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

  race_test();

  printf("\n%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
