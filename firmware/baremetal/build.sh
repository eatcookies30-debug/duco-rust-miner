#!/bin/sh
#
# Build the bare-metal miners: no Arduino core, no Wire, no Serial, just the
# registers.
#
#   ./build.sh                      every chip that has a core for it
#   ./build.sh atmega328p           a Nano, Uno or Pro Mini
#   ./build.sh attiny45             an ATtiny45 hanging off a rig master
#
# Chips with a UART and a TWI module (ATmega8, ATmega16, ATmega168/328) are
# driven by duco_tiny.c and the search in tiny_hash.c. The ATtinies have
# neither, so they use duco_attiny.c and attiny_hash.c, which bit-bangs the
# bus from the pin change interrupt.
#
# The ATtiny25 is not in the default list: it has 2 KB of flash and 128 bytes
# of RAM, and this miner needs 3,886 bytes of flash and about 184 bytes of RAM.
# SHA-1 alone needs 104 bytes of RAM - a 16 word schedule window, the previous
# hash, the target digest - and the search loop spills another 60 or so onto
# the stack, so no ATtiny with 128 bytes of RAM can run a DUCO-S1a job. The
# ATtiny45 is the smallest chip that can.
#
# The clock is 8 MHz for the ATtiny, the fuse setting most ATtiny boards are
# actually running at, and 16 MHz for the ATmega. Pass a different one if your
# board is somewhere else:
#
#   F_CPU=1000000 ./build.sh attiny45
#
# avr-gcc comes from the Arduino install if it is not on the path.

set -e

DIR=$(dirname "$0")
ARDUINO_GCC="$HOME/.arduino15/packages/arduino/tools/avr-gcc/7.3.0-atmel3.6.1-arduino7/bin"

if command -v avr-gcc >/dev/null 2>&1; then
  CC=avr-gcc
elif [ -x "$ARDUINO_GCC/avr-gcc" ]; then
  CC="$ARDUINO_GCC/avr-gcc"
else
  echo "avr-gcc not found. Install the Arduino IDE, or:"
  echo "  arduino-cli core install arduino:avr"
  exit 1
fi

if command -v avr-objcopy >/dev/null 2>&1; then
  OBJCOPY=avr-objcopy
else
  OBJCOPY="$ARDUINO_GCC/avr-objcopy"
fi
if command -v avr-size >/dev/null 2>&1; then
  SIZE=avr-size
else
  SIZE="$ARDUINO_GCC/avr-size"
fi

CHIPS=${*:-atmega328p atmega168p atmega8 attiny45 attiny85}

for chip in $CHIPS; do
  if [ "$chip" = attiny25 ] || [ "$chip" = attiny45 ] || [ "$chip" = attiny85 ]; then
    source="$DIR/duco_attiny.c"
    core="$DIR/attiny_hash.c"
    cpu=${F_CPU:-8000000}
  else
    source="$DIR/duco_tiny.c"
    core="$DIR/tiny_hash.c"
    cpu=${F_CPU:-16000000}
  fi

  out="$DIR/duco_${chip}"
  echo "== $chip"
  $CC -Os -Wall -std=c99 -mmcu=$chip -DF_CPU=$cpu \
      -o "$out.elf" "$source" "$core"
  $OBJCOPY -O ihex "$out.elf" "$out.hex"
  $SIZE --format=avr --mcu=$chip "$out.elf"
  echo
done

echo "Flash with, for example:"
echo "  avrdude -c usbtiny -p attiny45 -P /dev/ttyUSB0 -U flash:w:duco_attiny45.hex"
echo "  avrdude -c arduino -p atmega328p -P /dev/ttyUSB0 -U flash:w:duco_atmega328p.hex"
