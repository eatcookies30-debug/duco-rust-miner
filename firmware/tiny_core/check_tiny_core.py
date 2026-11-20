#!/usr/bin/env python3
"""Check the small core against hashlib, and report what it costs on an AVR.

    python3 check_tiny_core.py

The vectors are generated with hashlib itself: for every case the core is
handed a target digest it has to find the nonce that produces it, so a core
that rotates the state wrongly or pads the message wrongly fails here rather
than on a board.

Nothing is written to the board: the AVR part is a compile-only measurement of
flash and RAM, which is the whole point of a core sized for an 8 KB chip.
"""

import hashlib
import os
import random
import re
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")

def find_avr_gcc():
    """avr-gcc from PATH, or the one arduino-cli installed for the AVR core."""
    import glob
    if shutil.which("avr-gcc"):
        return shutil.which("avr-gcc")
    homes = [os.path.expanduser("~"), os.environ.get("ARDUINO_DIR", "")]
    for home in homes:
        hits = sorted(glob.glob(os.path.join(home, ".arduino15/packages/*/tools/"
                                            "avr-gcc/*/bin/avr-gcc")))
        if hits:
            return hits[-1]
    return None

PREV = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d"
KNOWN = [(50, 10), (9999, 100), (42424, 655), (65500, 655)]
CHIPS = ["atmega328p", "atmega168p", "atmega8", "atmega16", "attiny85"]
FLASH = {"atmega328p": 32768, "atmega168p": 16384, "atmega8": 8192,
         "atmega16": 16384, "attiny85": 8192}
RAM = {"atmega328p": 2048, "atmega168p": 1024, "atmega8": 1024,
       "atmega16": 1024, "attiny85": 512}


def target_of(nonce, prevHash):
    return hashlib.sha1((prevHash + str(nonce)).encode()).hexdigest()


def vectors():
    cases = []
    for nonce, diff in KNOWN:
        cases.append((PREV, nonce, diff * 100 + 1, target_of(nonce, PREV)))

    rng = random.Random(1234)
    for _ in range(400):
        diff = rng.choice([10, 100, 655])
        nonce = rng.randrange(1, diff * 100)
        prevHash = "%040x" % rng.randrange(1 << 160)
        cases.append((prevHash, nonce, diff * 100 + 1, target_of(nonce, prevHash)))

    # Targets whose nonce is outside the range searched: the core must answer
    # "no match" rather than find something by accident.
    for nonce, diff in KNOWN:
        cases.append((PREV, 0, nonce, target_of(nonce, PREV)))
    return cases


def write_vectors(path, cases):
    with open(path, "w") as f:
        for prevHash, nonce, maxNonce, target in cases:
            f.write('  {"%s", %d, %d, "%s"},\n' % (prevHash, nonce, maxNonce, target))


def run_host_test(cases):
    os.makedirs(BUILD, exist_ok=True)
    write_vectors(os.path.join(BUILD, "vectors.h"), cases)
    exe = os.path.join(BUILD, "tiny_test")
    subprocess.check_call(["gcc", "-O2", "-Wall", "-Wextra", "-I", BUILD,
                           "-o", exe, os.path.join(HERE, "tiny_test.c"),
                           os.path.join(HERE, "tiny_hash.c")])
    return subprocess.run([exe], capture_output=True, text=True).stdout.strip()


def avr_sizes(gcc):
    if not gcc:
        return "avr-gcc not found - the size measurement was skipped"
    size = os.path.join(os.path.dirname(gcc), "avr-size")
    out = []
    for chip in CHIPS:
        obj = os.path.join(BUILD, "tiny_hash_%s.o" % chip)
        subprocess.check_call([gcc, "-Os", "-mmcu=" + chip, "-c",
                               os.path.join(HERE, "tiny_hash.c"), "-o", obj])
        report = subprocess.run([size, "--format=sysv", obj],
                                capture_output=True, text=True).stdout
        # One line per section. Flash holds .text, .rodata and the initial
        # values of .data; RAM is .data plus .bss.
        code = ram = 0
        for line in report.splitlines():
            fields = line.split()
            if len(fields) < 2 or not fields[1].isdigit():
                continue
            name, value = fields[0], int(fields[1])
            if name in (".text", ".rodata", ".data"):
                code += value
            if name in (".data", ".bss", ".noinit"):
                ram += value
        out.append("  %-11s %5d B of code, %4d B of RAM  (of %d B flash, %d B RAM on chip)"
                   % (chip, code, ram, FLASH[chip], RAM[chip]))
    return "\n".join(out)


def main():
    cases = vectors()
    print(run_host_test(cases))
    print("cost of the core on AVR chips (-Os):")
    print(avr_sizes(find_avr_gcc()))


if __name__ == "__main__":
    main()
