---
name: Bug report
about: Something in the firmware, the miner or a script does not work
title: "[BUG] "
labels: bug
---

<!-- Fill in as much of this as you can. Most reports are solvable only with the
     bench output and the exact error text. -->

**What happened**

Describe what you expected and what actually happened.

**What I ran**

```
exact command line, including the flags
```

**Board and firmware**

- Board: (Mega 2560 / Uno / Nano / other)
- Sketch flashed: `firmware/asm_sketch` / `firmware/c_core` / `firmware/official_sketch` / other
- Flash report from the compile:

```
Sketch uses NNNNN bytes (NN%) of program storage space. Maximum is NNNNNN bytes.
Global variables use NNN bytes (NN%) of dynamic memory.
```

**Toolchain versions**

```
arduino-cli version
rustc --version
```

**Miner output**

Paste the lines the miner printed, in particular anything containing
`rejected:`, `no reply from`, `pool closed`, or `unexpected job reply`.

If the problem is a board that never answers, run the offline benchmark first -
it needs no account and no network:

```bash
./target/release/duco-miner --bench --ports /dev/ttyACM0,/dev/ttyACM1
```

**Serial ports**

```bash
ls -l /dev/ttyACM* /dev/ttyUSB*
```
