#!/usr/bin/env python3
"""Talk to a board directly over serial and time each search.

    python3 board_probe.py /dev/ttyACM0              # the six known jobs
    python3 board_probe.py /dev/ttyACM0 --random 25  # plus 25 random jobs

Each job is a hash whose nonce is known in advance, so a board answers with the
nonce it found and the probe compares it.
"""
import hashlib
import random
import serial
import sys
import time

PREV = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d"
KNOWN = [(50, 10), (9999, 100), (42424, 655), (65500, 655), (9999, 100), (42424, 655)]


def target(nonce):
    return hashlib.sha1((PREV + str(nonce)).encode()).hexdigest()


def jobs(argv):
    count = 0
    for i, arg in enumerate(argv):
        if arg == "--random":
            count = int(argv[i + 1])
    out = list(KNOWN)
    rng = random.Random(1234)
    for _ in range(count):
        diff = rng.choice([10, 100, 655])
        out.append((rng.randrange(1, diff * 100), diff))
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: board_probe.py <port> [--random N]", file=sys.stderr)
        return 2
    port = sys.argv[1]
    ser = serial.Serial(port, 115200, timeout=200)
    time.sleep(2.5)
    ser.reset_input_buffer()
    for nonce, diff in jobs(sys.argv[2:]):
        job = "%s,%s,%d,\n" % (PREV, target(nonce), diff)
        t0 = time.time()
        ser.write(job.encode())
        ser.flush()
        line = ser.readline()
        # Boards that scan for an I2C address print a banner first; skip it.
        while line and b"," not in line:
            line = ser.readline()
        dt = time.time() - t0
        if not line:
            print("%s: no reply (wall %.1f s)" % (port, dt))
            continue
        fields = line.decode().strip().split(",")
        found = int(fields[0], 2)
        elapsed = int(fields[1], 2)
        ok = "OK" if found == nonce else "MISMATCH"
        print("%s nonce %6d diff %3d -> found %6d elapsed %8d us wall %6.2f s rate %.2f H/s %s"
              % (port, nonce, diff, found, elapsed, dt, found / (elapsed / 1e6), ok))
    ser.close()


if __name__ == "__main__":
    main()
