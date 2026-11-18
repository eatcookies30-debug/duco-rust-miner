#!/usr/bin/env bash
# Flash a sketch folder onto one or more boards.
#
#   ./scripts/flash.sh firmware/asm_sketch                      # every board arduino-cli finds
#   ./scripts/flash.sh firmware/asm_sketch /dev/ttyACM0 /dev/ttyACM1
#   ./scripts/flash.sh --board arduino:avr:nano firmware/nano_i2c_sketch /dev/ttyUSB0
#
# The sketch folder name must match its .ino file (arduino-cli requirement).
# Boards whose name arduino-cli cannot read (Nano clones on a CH340 chip show up
# as "Unknown") need --board.
# Requires arduino-cli: https://arduino.github.io/arduino-cli/installation/
set -u

BOARD=""
ARGS=()
while [ $# -gt 0 ]; do
    if [ "$1" = "--board" ]; then
        BOARD="$2"
        shift 2
    else
        ARGS+=("$1")
        shift
    fi
done

if [ ${#ARGS[@]} -lt 1 ]; then
    echo "usage: flash.sh [--board FQBN] <sketch-dir> [port ...]" >&2
    exit 2
fi

SKETCH="${ARGS[0]}"
if [ ! -d "$SKETCH" ]; then
    echo "no such sketch folder: $SKETCH" >&2
    exit 1
fi
PORTS=("${ARGS[@]:1}")

# Detect the connected boards when no ports are given.
if [ ${#PORTS[@]} -eq 0 ]; then
    PORTS=$(arduino-cli board list 2>/dev/null | awk '/ttyACM|ttyUSB {print $1}')
    [ -z "$PORTS" ] && { echo "no boards detected - plug one in or pass the ports" >&2; exit 1; }
    echo "detected boards: $PORTS"
fi

for port in $PORTS; do
    fqbn="$BOARD"
    if [ -z "$fqbn" ]; then
        fqbn=$(arduino-cli board list 2>/dev/null |
               awk -v p="$port" '$1==p { for (i=1;i<=NF;i++) if ($i ~ /^[A-Za-z0-9_]+:[A-Za-z0-9_]+:[A-Za-z0-9_]+$/) { print $i; break } }')
    fi
    if [ -z "$fqbn" ]; then
        echo "== $port: board name not recognised (a Nano clone reports as Unknown);"
        echo "   pass it explicitly, e.g. --board arduino:avr:nano"
        continue
    fi
    echo "== $port -> $fqbn"
    out=$(timeout 240 arduino-cli compile --upload --fqbn "$fqbn" --port "$port" "$SKETCH" 2>&1)
    echo "$out" | grep -E "Sketch uses|Global|error|No device found" || echo "$out" | tail -3
    sleep 3
done
