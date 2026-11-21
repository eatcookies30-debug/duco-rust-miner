## What changed

One or two sentences describing the change and why.

## Evidence

Any claim about speed, correctness or reliability needs numbers, not adjectives.

- Which board, and which sketch was flashed
- `./target/release/duco-miner --bench --ports ...` output before and after
- if the assembly core changed:
  ```bash
  cd firmware/board_sketch
  python3 gen_asm.py duco_rounds_mega.h            # instruction and cycle counts
  python3 gen_asm.py --compact duco_rounds_328p.h
  ```
- `cargo test` in `miner/` passes

## Notes for the reviewer

Things that are easy to get wrong on the AVR are listed in the README under
"AVR gotchas the generator has to get right". If your change touches the
register groups, the compare tail or the schedule stores, say explicitly how it
was verified beyond the simulator.
