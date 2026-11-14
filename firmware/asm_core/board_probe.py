#!/usr/bin/env python3
"""Talk to a board directly over serial and time each search."""
import serial
import hashlib
import sys
import time

PREV = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d"


def target(nonce):
    return hashlib.sha1((PREV + str(nonce)).encode()).hexdigest()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM2"
    ser = serial.Serial(port, 115200, timeout=200)
    time.sleep(2.5)
    ser.reset_input_buffer()
    for nonce, diff in [(50, 10), (9999, 100), (42424, 655), (65500, 655), (9999, 100), (42424, 655)]:
        job = "%s,%s,%d,\n" % (PREV, target(nonce), diff)
        t0 = time.time()
        ser.write(job.encode())
        ser.flush()
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
