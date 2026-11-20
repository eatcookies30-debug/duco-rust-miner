# The small C hash core

`tiny_hash.c` is the SHA-1 nonce search for boards with too little flash for
the assembly core: 1,946 B of code and 168 B of RAM on an ATmega328P, 1,940 B
on an ATmega8, 1,964 B on an ATtiny85. It is plain, portable C with no
Arduino libraries in it, and it is the search used by `firmware/tiny85`.

It is written to be small rather than fast:

* the schedule is a **16-word rolling window** rather than 80 words. `W[i]` is
  read back at rounds `i+3`, `i+8`, `i+14` and `i+16`, and the round that
  overwrites slot `i & 15` is round `i+16`, the last round that word is needed
  in, so 64 bytes of RAM is enough instead of 320;
* rounds 0-9 depend only on the previous hash, not on the nonce, so they are
  computed **once per job** and the search starts from that state;
* the nonce is walked as **decimal digits** rather than formatted every
  attempt, so the search loop contains no division;
* the state is ordinary local variables, so the compiler decides where it
  lives. On a 328P it stays in registers; on an ATtiny85, which has eight
  registers in total, it spills to RAM and the hash still runs.

`check_tiny_core.py` builds the test vectors with `hashlib`, runs them through
the core, and reports what the core costs on each AVR chip:

```bash
python3 check_tiny_core.py
```

```
tiny core: 408 cases checked, 0 failures
cost of the core on AVR chips (-Os):
  atmega328p   1946 B of code,  168 B of RAM  (of 32768 B flash, 2048 B RAM on chip)
  atmega168p   1946 B of code,  168 B of RAM  (of 16384 B flash, 1024 B RAM on chip)
  atmega8      1940 B of code,  168 B of RAM  (of 8192 B flash, 1024 B RAM on chip)
  atmega16     1946 B of code,  168 B of RAM  (of 16384 B flash, 1024 B RAM on chip)
  attiny85     1964 B of code,  168 B of RAM  (of 8192 B flash, 512 B RAM on chip)
```

`tiny_test.c` is the C half of that check: the script writes the vectors it
derived from `hashlib` into `build/vectors.h`, the test prints what the core
finds, and the script compares it. It is also the quickest way to see the core
behaviour on a desktop, since nothing in it is AVR specific.

One thing to keep in mind if you change it: on AVR `int` is 16 bits wide, so a
literal shifted by 24 has to be written as `(uint32_t)0x80u`. Without the cast
the SHA-1 padding byte disappears from the message, and the core finds
two- and three-digit nonces while missing one- and four-digit ones. A desktop
build cannot show that bug, because the same expression is well defined there.
