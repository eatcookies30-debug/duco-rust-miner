#!/usr/bin/env bash
# Start the whole rig with one command: every board on the desk, one process,
# one connection to the pool.
#
#   ./scripts/start-rig.sh                          # reads ./Settings.cfg
#   ./scripts/start-rig.sh /path/to/Settings.cfg    # any official-style config
#   ./scripts/start-rig.sh --rounds 10              # anything else goes to the miner
#
# The boards are not hardwired here. They come from the config file's `avrport`
# (boards plugged into the PC over USB) and `rigport` (the rig master the boards
# are hanging off). If the config has no ports, pass them yourself:
#
#   ./scripts/start-rig.sh --config ./Settings.cfg \
#       --ports /dev/ttyACM0 --rig /dev/ttyACM1
#
# The miner is built if it is not there yet, which needs Rust. If you already
# have the binary, this is just a shortcut for running it.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
MINER="$HERE/../miner/target/release/duco-miner"

if [ ! -x "$MINER" ]; then
    echo "building the miner (needs Rust)..."
    (cd "$HERE/../miner" && cargo build --release) || exit 1
fi

# A settings file may be given as the first argument on its own.
if [ $# -gt 0 ] && [ -f "$1" ]; then
    settings="$1"
    shift
    exec "$MINER" --config "$settings" "$@"
elif [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    # The caller named a settings file that is not there. A desktop shortcut
    # whose Exec line has an unquoted path with spaces lands here, because the
    # path is chopped at the first space.
    echo "no settings file at: $1"
    echo "Quote the path in the shortcut, or keep it free of spaces:"
    echo "  Exec=/path/to/start-rig.sh \"/home/me/Settings.cfg\""
    exit 1
fi

# Use the settings file the caller named; otherwise the one next to the miner,
# otherwise whatever DUCO_SETTINGS points at.
config=""
for arg in "$@"; do
    if [ "$arg" = "--config" ]; then config="given"; break; fi
done
if [ -z "$config" ]; then
    if [ -f "$HERE/../Settings.cfg" ]; then
        config="$HERE/../Settings.cfg"
    elif [ -n "${DUCO_SETTINGS:-}" ]; then
        config="$DUCO_SETTINGS"
    fi
fi

if [ "$config" = "given" ]; then
    exec "$MINER" "$@"
elif [ -n "$config" ]; then
    exec "$MINER" --config "$config" "$@"
else
    echo "no settings file: put Settings.cfg next to the miner, set DUCO_SETTINGS,"
    echo "or pass --config /path/to/Settings.cfg"
    exec "$MINER" "$@"
fi
