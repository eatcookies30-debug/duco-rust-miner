# The AVR SHA-1 core

These scripts generate and check the assembly core that is flashed from
`../asm_sketch` (Mega 2560) and `../nano_i2c_sketch` (ATmega328P). Nothing here
is needed to mine - the generated files
`../asm_sketch/duco_rounds.h` and `../nano_i2c_sketch/duco_rounds.h` are already
checked in and ready to flash.

Regenerate and re-verify both cores:

```bash
cd firmware/asm_core
python3 gen_asm.py                                       # instruction/cycle counts, full core
python3 gen_asm.py --compact                             # instruction/cycle counts, looped core
python3 gen_asm.py ../asm_sketch/duco_rounds.h           # write the Mega core into the sketch
python3 gen_asm.py --compact ../nano_i2c_sketch/duco_rounds.h
python3 sim_check.py                                     # simulated AVR vs hashlib: 0 failures
python3 sim_check.py --compact
python3 build_trace.py ../asm_sketch/asm_sketch.ino      # round-by-round trace build
../../scripts/flash.sh ../asm_sketch /dev/ttyACM0 /dev/ttyACM1
```

* `gen_asm.py` - emits the 70 SHA-1 rounds as one naked, `call`-able assembly
  block. The SHA-1 state is held in six register groups that rotate through the
  `a,b,c,d,e` roles, so the role shift costs nothing. `--compact` keeps the same
  rotation but replaces each 20-round phase with a six-round body that runs three
  times, with the leftover rounds inline, which is what lets the core fit in a
  328P's 30 KB of flash.
* `sim_check.py` - a small AVR simulator (only the opcodes the cores use) that
  runs the generated core and compares the digest with Python's `hashlib`. This
  is what catches register-aliasing and flag bugs before they reach a board.
* `build_trace.py` + `trace_test/` - builds a sketch that stores the state after
  every round and compares it with a C reference, printing `bad rounds: N`. This
  is the on-hardware equivalent of the simulator.
* `hit_test/` - a standalone sketch with hardcoded targets: confirms the core
  finds nonce 50 and correctly reports "no match" for a wrong target.
* `board_probe.py` - talks to a board over serial directly (no pool, no account),
  useful for timing searches without the miner:
  `python3 board_probe.py /dev/ttyACM0 --random 10`

AVR things the generator has to get right, which are easy to get wrong:

* `lds`/`sts` move **one byte**, not a 32-bit word - a 32-bit value needs four of
  each (the classic "LDS loads two bytes" mistake silently leaves every odd
  register holding garbage);
* `ldi`, `subi`, `sbci` only work on r16-r31, which is why the scratch group is
  pinned to r24-r27;
* conditional branches reach only 63 words, so the compare's failure handlers are
  inlined per word rather than chained to one distant label;
* `r1` is the zero register by ABI - the rotate-by-1 uses `adc reg, r1`;
* the hit flag must come back as an inline-asm output register, not a global:
  without a memory clobber GCC hoists the global's load out of the nonce loop and
  every attempt looks like a miss;
* globals referenced only from asm must be non-`static` and marked
  `__attribute__((used))`, because arduino-cli builds with `-flto` and otherwise
  deletes them;
* `.inc` files are not reliably copied by arduino-cli, so the generated core is
  included as a `.h`.
