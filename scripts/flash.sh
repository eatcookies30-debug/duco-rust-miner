#!/usr/bin/env bash
# Flash a sketch folder onto one or more boards.
#
#   ./scripts/flash.sh firmware/asm_sketch                     # every board arduino-cli finds
#   ./scripts/flash.sh firmware/asm_sketch /dev/ttyACM0 /dev/ttyACM1
#   ./scripts/flash.sh firmware/c_core /dev/ttyACM0
#
# The sketch folder name must match its .ino file (arduino-cli requirement).
# Requires arduino-cli: https://arduino.github.io/arduino-cli/installation/
set -u

if [ $# -lt 1 ]; then
    echo "usage: flash.sh <sketch-dir> [port ...]" >&2
    exit 2
fi

SKETCH="$1"
shift
if [ ! -d "$SKETCH" ]; then
    echo "no such sketch folder: $SKETCH" >&2
    exit 1
fi

# Detect the connected boards when no ports are given.
if [ $# -eq 0 ]; then
    PORTS=$(arduino-cli board list 2>/dev/null | awk '/ttyACM|ttyUSB {print $1}')
    PORTS=${PORTS:-}
    [ -z "$PORTS" ] && { echo "no boards detected - plug one in or pass the ports" >&2; exit 1; }
    echo "detected boards: $PORTS"
else
    PORTS="$*"
fi

for port in $PORTS; do
    fqbn=$(arduino-cli board list 2>/dev/null |
           awk -v p="$port" '$1==p { for (i=1;i<=NF;i++) if ($i ~ /^[A-Za-z0-9_]+:[A-Za-z0-9_]+:[A-Za-z0-9_]+$/) { print $i; break } }')
    [ -z "$fqbn" ] && { echo "== $port: not detected"; continue; }
    echo "== $port -> $fqbn"
    out=$(timeout 240 arduino-cli compile --upload --fqbn "$fqbn" --port "$port" "$SKETCH" 2>&1)
    echo "$out" | grep -E "Sketch uses|Global|error|No device found" || echo "$out" | tail -3
    sleep 3
done
