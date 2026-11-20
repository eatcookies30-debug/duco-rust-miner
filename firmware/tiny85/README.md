# Bare-metal firmware for boards with 8 KB of flash

`duco_tiny.c` is the whole miner for a chip too small to carry an Arduino
sketch: no Arduino core, no Wire, no Serial. It drives the TWI and UART
registers itself and searches with the small C core in `firmware/tiny_core`.
It speaks the same protocol as `firmware/asm_sketch` and
`firmware/nano_i2c_sketch`, so the Rust miner over USB serial and an I2C rig
master both drive it unchanged.

An ATmega8 or an ATtiny85 has 8,192 B of program flash, and the standard
Arduino AVR core has no board definition for either chip, so there is nothing
for arduino-cli to build. The whole miner here is 4,448 B; a sketch skeleton
that only does the search, takes a job over Serial and answers on the Wire bus
is 5,514 B on a 328P before the board id, the timing and the reply line are
added. On a chip that small the firmware has to be written against the
registers, and this is it.

## Build

```bash
./build.sh                       # atmega328p - Nano, Uno, Pro Mini
./build.sh atmega8
./build.sh atmega16
./build.sh atmega168p
./build.sh attiny85
```

`build.sh` uses `avr-gcc` from PATH, or the toolchain arduino-cli installed,
writes `duco_tiny_<chip>.hex` next to the source, and prints what that chip's
build costs in flash and RAM. The second argument is passed straight to the
compiler:

```bash
./build.sh atmega328p "-DDUCO_DEBUG"     # prints every job the board parsed
./build.sh atmega328p "-DDUCO_NO_I2C"    # serial only, bus driver left out
```

`F_CPU` defaults to 16 MHz, which is what a Nano, an Uno and an ATmega8 board
run at. A board at a different clock needs `-DF_CPU=8000000UL`.

## Flash

There is no sketch, so arduino-cli has nothing to build. avrdude uploads it:

```bash
avrdude -c arduino -p atmega328p -P /dev/ttyUSB0 -U flash:w:duco_tiny_atmega328p.hex
```

Add `-C <path to avrdude.conf>` if avrdude cannot find a config on its own.
The programmer is `arduino`, the protocol the AVR bootloader speaks; `-c avr`
is not in the config that ships with the Arduino toolchain. Only the
application flash pages are written, so the bootloader is left alone and an
Arduino sketch can be uploaded afterwards with arduino-cli as usual.

## What a build costs

Measured with avr-gcc 7.3.0 (the version arduino-cli installs) at `-Os`:

| chip | flash | RAM | of the chip |
|---|---|---|---|
| ATmega328P (Nano, Uno, Pro Mini) | 4,448 B | 713 B | 14% of 32,768 B, 35% of 2,048 B |
| ATmega168P | 4,448 B | 713 B | 27% of 16,384 B, 70% of 1,024 B |
| ATmega8 | 4,200 B | 729 B | 51% of 8,192 B, 71% of 1,024 B |
| ATmega16 | 4,330 B | 729 B | 26% of 16,384 B, 71% of 1,024 B |
| ATtiny85 | 2,236 B | 237 B | 27% of 8,192 B, 46% of 512 B |

The ATtiny85 line is the same source with the bus driver and the UART compiled
out, because that chip has neither - see the last section. The other builds are
the full miner, serial and I2C both present.

The hash core by itself, without the bus, the serial port or the job parser, is
under 2 KB on every chip: `python3 ../tiny_core/check_tiny_core.py` prints that
number too.

## What it measures on a real board

All of this is the atmega328p build on a Nano 328P clone, run through
`miner --bench`, which sends nonces 50, 9,999, 42,424 and 65,500 and reports
what the board says it found and how long it says it took:

```
/dev/ttyUSB0: DUCOID0123456789ABCDEF01 - test nonce 50 in 65489 us -> 763.49 H/s
  diff 100: found nonce 9999 in 13.009329 s -> 768.60 H/s OK
  diff 655: found nonce 42424 in 55.619646 s -> 762.75 H/s OK
  diff 655: found nonce 65500 in 85.944262 s -> 762.12 H/s OK
```

Driven over the I2C bus by `firmware/i2c_bench` on a Mega 2560 the same board
runs at 727 H/s, with every job byte acknowledged and every nonce the bench
expected. That is about 43% of the assembly core on the same board - the looped
C round body needs roughly 2.3x the cycles of the unrolled assembly one, which
is what fitting in a fifth of the flash costs.

## Which pins are the bus

| chip | SDA | SCL |
|---|---|---|
| ATmega8, ATmega168P/328P (Nano, Uno, Pro Mini) | A4 / PC4 | A5 / PC5 |
| ATmega1280/2560 (Mega) | D20 / PD1 | D21 / PD0 |
| ATmega16 | PC1 | PC0 |

SDA to SDA, SCL to SCL, ground to ground. The driver sets those two pins to
input with the internal pull-ups on, because a 328P board has no external
resistors on the lines; a Mega 2560 has them onboard already. An ATmega16's bus
is on PC0/PC1, not on the RXD/TXD pair that shares port D with it.

The board walks addresses 1 to 126 at boot and takes the first one nobody
acknowledges, then prints the address it chose on the serial port. It retries
every five seconds if the bus is busy at the moment it comes up.

## Timing and baud

Timer 1 times a search. On chips with a 16-bit Timer 1 it counts every cycle,
so one overflow interrupt every 4,096 us at 16 MHz and an exact elapsed time;
the ATtiny85's 8-bit Timer 1 uses a prescaler of 8 so the interrupt comes every
128 us instead of every 64 us. The tick count is read with the overflow
interrupt briefly disabled: reading a 16-bit TCNT1 is two byte loads, and an
overflow landing between them would put the count off by up to 65,536 ticks.

The UART divisor is rounded, not truncated: at 16 MHz that is UBRR 8, 111,111
baud, 3.5% away from the 115,200 a PC asks for, which both ends tolerate.
Truncating gives UBRR 7, 125,000 baud, 8.5% error, and the replies arrive as
garbage.

## The board's id

The 168/328 class chips carry an eight byte device id in the boot signature row,
which is what the rigs use as the miner's identity; the board reads it and
reports it as `DUCOID` plus those sixteen hex digits. Clone boards sometimes ship
with that row blank, and ATmega8, ATmega16 and ATtiny85 have no such row at all,
so several boards can end up reporting the same id - which the pool counts as one
miner. Whatever the signature row holds, the address the board claimed on the bus
is written into the last two hex digits of the id, which is enough to tell the
boards on a rig apart:

```
address 1  ->  DUCOID0000000000000001
address 2  ->  DUCOID0000000000000002
```

A board with no address - one built with `-DDUCO_NO_I2C`, or an ATtiny85, which
has no TWI to claim an address with - keeps the plain id its signature row gives
it.

## One board, one master

The job ring is 128 bytes, a little over one job, and while the board is
searching it ignores the bytes that arrive on either path. That is deliberate:
a job pushed by a second master during a search would splice onto the bytes the
board has already stored and be read as a valid job with the wrong hashes, and
the board would then hunt for a nonce that does not exist. So a board already
mining for one master simply does not take a second job until it answers the
first.

## ATtiny85

The ATtiny85 has no TWI module - `iotnx5.h` defines no `TW*` registers - and no
UART, so there is nothing for the bus driver or the serial driver to talk to.
Serving the bus there means driving it through USI in two-wire mode, which is a
different driver and another 1-2 KB. `build.sh attiny85` compiles with both
compiled out: it is a fit and instruction-set check showing that the core and
the framing fit in 2,236 B of flash and 237 B of RAM, not a board you can hand
jobs to. Nothing here was run on ATtiny85 hardware.
