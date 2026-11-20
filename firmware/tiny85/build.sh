#!/usr/bin/env bash
# Build the bare-metal tiny miner for one AVR chip and report what it costs.
#
#   ./build.sh                      # the Nano/Uno chip, atmega328p
#   ./build.sh atmega8              # an 8 KB flash board
#   ./build.sh attiny85
#
# Writes duco_tiny_<chip>.hex next to this script. Flash it with avrdude:
#
#   avrdude -C <avrdude.conf> -c arduino -p atmega328p -P /dev/ttyUSB0 \
#           -U flash:w:duco_tiny_atmega328p.hex
#
# A board whose TWI lines are not connected at all can be built without the
# bus driver: pass -DDUCO_NO_I2C. -DDUCO_DEBUG prints every job the board
# parses, which is the only way to see what a board with no debugger saw.
#
# F_CPU defaults to 16 MHz, which is what a Nano, an Uno and an ATmega8 board
# run at. A board running at a different clock needs -DF_CPU=<hz>.
set -eu

CHIP="${1:-atmega328p}"
EXTRA_CFLAGS="${2:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CORE="$HERE/../tiny_core"

find_avr_gcc() {
    if command -v avr-gcc >/dev/null 2>&1; then
        command -v avr-gcc
        return
    fi
    for dir in "$HOME/.arduino15/packages" "$HOME/.arduino15"; do
        hit=$(ls "$dir"/*/tools/avr-gcc/*/bin/avr-gcc 2>/dev/null | sort | tail -1)
        if [ -n "$hit" ]; then
            echo "$hit"
            return
        fi
    done
    echo "avr-gcc not found. Install the Arduino AVR toolchain or add avr-gcc to PATH." >&2
    exit 1
}

GCC="$(find_avr_gcc)"
SIZE="${GCC%gcc}size"
OBJCOPY="${GCC%gcc}objcopy"

SUFFIX=""
case "$EXTRA_CFLAGS" in
    *DUCO_NO_I2C*) SUFFIX="${SUFFIX}_no_i2c" ;;
esac
case "$EXTRA_CFLAGS" in
    *DUCO_DEBUG*) SUFFIX="${SUFFIX}_debug" ;;
esac

OUT="$HERE/duco_tiny_${CHIP}${SUFFIX}"

"$GCC" -Os -Wall -Wextra -mmcu="$CHIP" -I "$CORE" $EXTRA_CFLAGS \
       -o "$OUT.elf" "$HERE/duco_tiny.c" "$CORE/tiny_hash.c"
"$OBJCOPY" -O ihex "$OUT.elf" "$OUT.hex"

echo "== $CHIP"
"$SIZE" --format=sysv "$OUT.elf" |
    awk -v chip="$CHIP" '
        $1 == ".text"   || $1 == ".rodata" || $1 == ".data" { flash += $2 }
        $1 == ".data"   || $1 == ".bss"    || $1 == ".noinit" { ram += $2 }
        END { printf "  %d B of flash, %d B of RAM\n", flash, ram }'
