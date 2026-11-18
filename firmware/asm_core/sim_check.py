#!/usr/bin/env python3
"""Tiny AVR simulator for the generated DUCO hash cores.

Runs the emitted instruction list and checks the "digest matches target" flag
against real SHA-1 from hashlib. It understands both the fully unrolled core
and the compact one (Z-pointer schedule loads, the pointer as the loop counter,
long jmp).

    python3 sim_check.py                 # full core
    python3 sim_check.py --compact       # looped core for the Nano
"""
import hashlib
import random
import sys
from gen_asm import body

BASE = {"MSG": 0, "TGT": 320, "FIX": 360, "HA": 372, "HB": 376, "HC": 380,
        "HD": 384, "HE": 388, "KN": 404, "HIT": 420, "CNT": 440}
INIT = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]

MEMORY_OPS = ("lds", "sts", "ldd", "std", "ld")


def simulate(lines, mem, max_steps=100000):
    r = [0] * 32
    stack = []
    carry = 0
    zero = 0
    steps = 0
    cycles = 0
    labels = {}
    for i, l in enumerate(lines):
        if l.endswith(":"):
            labels[l[:-1]] = i

    def reg(tok):
        return int(tok[1:])

    def addr(tok):
        parts = tok.split("+")
        return BASE[parts[0]] + sum(int(p) for p in parts[1:])

    def zreg():
        return r[31] * 256 + r[30]

    i = 0
    while True:
        line = lines[i].strip()
        i += 1
        steps += 1
        if steps > max_steps:
            raise SystemExit("loop did not terminate")
        if not line or line.endswith(":"):
            continue
        cycles += 1
        op, _, rest = line.partition(" ")
        if op == "ret":
            break
        if op in ("push", "pop"):
            cycles += 2
            if op == "push":
                stack.append(r[reg(rest)])
            else:
                r[reg(rest)] = stack.pop()
            continue
        args = [x.strip() for x in rest.split(",")]

        if op == "ldi":
            value = args[1]
            if value.startswith("lo8(") or value.startswith("hi8("):
                a = addr(value[4:-1])
                r[reg(args[0])] = (a & 0xff) if value.startswith("lo8") else (a >> 8) & 0xff
            else:
                r[reg(args[0])] = int(value) & 0xff
        elif op == "mov":
            r[reg(args[0])] = r[reg(args[1])]
        elif op == "movw":
            dd, ss = reg(args[0]), reg(args[1])
            r[dd] = r[ss]
            r[dd + 1] = r[ss + 1]
        elif op == "lds":
            r[reg(args[0])] = mem[addr(args[1])]
        elif op == "sts":
            mem[addr(args[0])] = r[reg(args[1])]
        elif op == "ld":
            if args[1] in ("Z", "z"):
                r[reg(args[0])] = mem[zreg()]
            elif args[1] in ("Z+", "z+"):
                r[reg(args[0])] = mem[zreg()]
                r[30] = (r[30] + 1) & 0xff
                if r[30] == 0:
                    r[31] = (r[31] + 1) & 0xff
            else:
                raise SystemExit("unhandled: " + line)
        elif op == "ldd":
            pointer, disp = args[1].split("+")
            r[reg(args[0])] = mem[zreg() + int(disp)]
        elif op == "std":
            pointer, disp = args[0].split("+")
            mem[zreg() + int(disp)] = r[reg(args[1])]
        elif op == "st":
            if args[0] in ("Z+", "z+"):
                mem[zreg()] = r[reg(args[1])]
                r[30] = (r[30] + 1) & 0xff
                if r[30] == 0:
                    r[31] = (r[31] + 1) & 0xff
            else:
                raise SystemExit("unhandled: " + line)
        elif op in ("add", "adc", "sub", "sbc", "subi", "sbci"):
            dd = reg(args[0])
            v = int(args[1]) & 0xff if op in ("subi", "sbci") else r[reg(args[1])]
            c = carry if op in ("adc", "sbc", "sbci") else 0
            if op in ("add", "adc"):
                tot = r[dd] + v + c
                carry = 1 if tot > 255 else 0
            else:
                tot = r[dd] - v - c
                carry = 1 if tot < 0 else 0
            r[dd] = tot & 0xff
            zero = 1 if r[dd] == 0 else 0
        elif op in ("adiw", "subiw", "sbiw"):
            lo, hi = reg(args[0]), reg(args[0]) + 1
            v = int(args[1])
            if op == "adiw":
                tot = r[lo] + r[hi] * 256 + v
            else:
                tot = r[lo] + r[hi] * 256 - v
            carry = 1 if tot < 0 or tot > 65535 else 0
            tot &= 0xffff
            r[lo], r[hi] = tot & 0xff, tot >> 8
            zero = 1 if tot == 0 else 0
        elif op in ("dec", "inc"):
            dd = reg(args[0])
            r[dd] = (r[dd] - 1) & 0xff if op == "dec" else (r[dd] + 1) & 0xff
            zero = 1 if r[dd] == 0 else 0
        elif op in ("lsl", "lsr", "rol", "ror"):
            dd = reg(args[0])
            v = r[dd]
            if op in ("lsl", "rol"):
                out = (v >> 7) & 1
                r[dd] = ((v << 1) | (carry if op == "rol" else 0)) & 0xff
            else:
                out = v & 1
                r[dd] = (v >> 1) | ((carry << 7) if op == "ror" else 0)
            carry = out
            zero = 1 if r[dd] == 0 else 0
        elif op in ("or", "and", "eor"):
            dd = reg(args[0])
            if op == "or":
                r[dd] |= r[reg(args[1])]
            elif op == "and":
                r[dd] &= r[reg(args[1])]
            else:
                r[dd] ^= r[reg(args[1])]
            zero = 1 if r[dd] == 0 else 0
        elif op in ("cp", "cpc"):
            dd, ss = reg(args[0]), reg(args[1])
            v = r[ss] + (carry if op == "cpc" else 0)
            tot = r[dd] - v
            carry = 1 if tot < 0 else 0
            zero = 1 if (tot & 0xff) == 0 else 0
        elif op == "cpi":
            value = args[1]
            v = addr(value[4:-1]) & 0xff if value.startswith("lo8(") else int(value) & 0xff
            tot = r[reg(args[0])] - v
            carry = 1 if tot < 0 else 0
            zero = 1 if (tot & 0xff) == 0 else 0
        elif op == "brne":
            if zero == 0:
                i = labels[args[0]]
        elif op == "breq":
            if zero == 1:
                i = labels[args[0]]
        elif op == "jmp":
            i = labels[args[0]]
        else:
            raise SystemExit("unhandled: " + line)
    return r[24], cycles, r, mem


def setup_mem(prev_hash, nonce):
    raw = prev_hash.encode()
    msg = [0] * 80
    for i in range(10):
        msg[i] = int.from_bytes(raw[4 * i:4 * i + 4], "big")
    dig = str(nonce).encode()
    for i, ch in enumerate(dig):
        msg[10 + (i >> 2)] |= ch << (24 - 8 * (i & 3))
    total = 40 + len(dig)
    msg[total >> 2] |= 0x80 << (24 - 8 * (total & 3))
    msg[15] |= (total * 8) & 0xffffffff

    digest = hashlib.sha1(raw + dig).digest()
    tgt = [(int.from_bytes(digest[4 * i:4 * i + 4], "big") - INIT[i]) & 0xffffffff
           for i in range(5)]

    def r5(x): return ((x << 5) | (x >> 27)) & 0xffffffff
    def r1(x): return ((x << 1) | (x >> 31)) & 0xffffffff

    a, b, c, d, e = INIT
    for i in range(10):
        tt = (r5(a) + e + msg[i] + 0x5a827999 + (d ^ (b & (c ^ d)))) & 0xffffffff
        e, d, c, b, a = d, c, ((b << 30) | (b >> 2)) & 0xffffffff, a, tt

    fix = [0, 0, 0]
    fix[0] = r1(msg[13] ^ msg[8] ^ msg[2] ^ msg[0])
    fix[1] = r1(msg[14] ^ msg[9] ^ msg[3] ^ msg[1])
    fix[2] = r1(fix[1] ^ msg[12] ^ msg[6] ^ msg[4])

    # The core no longer copies FIX into MSG: the C side writes schedule words
    # 16, 17 and 20 into MSG once per job.
    msg[16], msg[17], msg[20] = fix[0], fix[1], fix[2]

    mem = bytearray(512)
    kn = [0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6]
    for sym, words in (("MSG", msg), ("TGT", tgt), ("KN", kn),
                       ("HA", [a]), ("HB", [b]), ("HC", [c]), ("HD", [d]),
                       ("HE", [e])):
        for i, w in enumerate(words):
            for k in range(4):
                mem[BASE[sym] + i * 4 + k] = (w >> (8 * k)) & 0xff
    return mem, msg


def report(compact):
    lines = body(compact)
    cyc = sum(2 if l.split()[0] in MEMORY_OPS else 1 for l in lines)
    print("%s core: %d instructions, %d cycles" %
          ("compact" if compact else "full", len(lines), cyc))
    prev = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d"
    bad = 0
    nonces = list(range(0, 200)) + [9999, 42424, 65500, 100000, 999999]
    for n in nonces:
        mem, _ = setup_mem(prev, n)
        ok, _, _, _ = simulate(lines, mem)
        if ok != 1:
            bad += 1
            if bad < 5:
                print("FAIL nonce %d (cycles %d)" % (n, cyc))
    print("  checked %d nonces, failures: %d" % (len(nonces), bad))

    bad2 = 0
    for _ in range(400):
        h = random.getrandbits(160).to_bytes(20, "big").hex()
        n = random.randrange(0, 1000000)
        mem, _ = setup_mem(h, n)
        ok, _, _, _ = simulate(lines, mem)
        if ok != 1:
            bad2 += 1
    print("  random hash/nonce failures: %d" % bad2)

    # A miss must return 0: run the same nonce set where the target is wrong.
    bad3 = 0
    for n in range(0, 50):
        mem, _ = setup_mem(prev, n)
        mem[BASE["TGT"]] ^= 0xff
        ok, _, _, _ = simulate(lines, mem)
        if ok != 0:
            bad3 += 1
    print("  false positives: %d" % bad3)
    return bad + bad2 + bad3


if __name__ == "__main__":
    compact = "--compact" in sys.argv[1:]
    sys.exit(1 if report(compact) else 0)
