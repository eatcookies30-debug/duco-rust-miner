#!/usr/bin/env python3
"""Tiny AVR simulator for the generated DUCO hash core.

Runs the emitted instruction list and checks the "digest matches target" flag
against real SHA-1 from hashlib.
"""
import hashlib
import random
from gen_asm import body

BASE = {"MSG": 0, "TGT": 320, "FIX": 360, "HA": 372, "HB": 376, "HC": 380, "HD": 384, "HE": 388, "KN": 404, "HIT": 420}
INIT = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0]


def simulate(lines, mem, stop=None):
    r = [0] * 32
    C = 0
    Z = 0
    labels = {}
    for i, l in enumerate(lines):
        if l.endswith(":"):
            labels[l[:-1]] = i

    def addr(tok):
        parts = tok.split("+")
        return BASE[parts[0]] + sum(int(p) for p in parts[1:])

    i = 0
    limit = len(lines) if stop is None else stop
    cycles = 0
    while i < limit:
        line = lines[i].strip()
        cycles += 1
        i += 1
        if not line or line.endswith(":"):
            continue
        op, _, rest = line.partition(" ")
        if op == "ret":
            break
        args = [x.strip() for x in rest.split(",")]
        if op == "ldi":
            r[int(args[0][1:])] = int(args[1]) & 0xff
        elif op == "mov":
            r[int(args[0][1:])] = r[int(args[1][1:])]
        elif op == "movw":
            dd, ss = int(args[0][1:]), int(args[1][1:])
            r[dd] = r[ss]
            r[dd + 1] = r[ss + 1]
        elif op == "lds":
            dd, a = int(args[0][1:]), addr(args[1])
            r[dd] = mem[a]
        elif op == "sts":
            a = addr(args[0])
            ss = int(args[1][1:])
            mem[a] = r[ss]
        elif op in ("add", "adc", "sub", "sbc", "subi", "sbci"):
            dd = int(args[0][1:])
            v = int(args[1]) & 0xff if op in ("subi", "sbci") else r[int(args[1][1:])]
            carry = C if op in ("adc", "sbc", "sbci") else 0
            if op in ("add", "adc"):
                tot = r[dd] + v + carry
                C = 1 if tot > 255 else 0
            else:
                tot = r[dd] - v - carry
                C = 1 if tot < 0 else 0
            res = tot & 0xff
            Z = 1 if res == 0 else 0
            r[dd] = res
        elif op in ("eor", "and", "or"):
            dd, ss = int(args[0][1:]), int(args[1][1:])
            if op == "eor":
                v = r[dd] ^ r[ss]
            elif op == "and":
                v = r[dd] & r[ss]
            else:
                v = r[dd] | r[ss]
            r[dd] = v
            Z = 1 if v == 0 else 0
        elif op in ("lsl", "rol", "lsr", "ror"):
            dd = int(args[0][1:])
            v = r[dd]
            if op == "lsl":
                C = (v >> 7) & 1
                nv = (v << 1) & 0xff
            elif op == "rol":
                cin = C
                C = (v >> 7) & 1
                nv = ((v << 1) | cin) & 0xff
            elif op == "lsr":
                C = v & 1
                nv = v >> 1
            else:
                cin = C
                C = v & 1
                nv = (v >> 1) | (cin << 7)
            r[dd] = nv
            Z = 1 if nv == 0 else 0
        elif op in ("cp", "cpc"):
            dd, ss = int(args[0][1:]), int(args[1][1:])
            v = r[ss] + (C if op == "cpc" else 0)
            tot = r[dd] - v
            C = 1 if tot < 0 else 0
            Z = 1 if (tot & 0xff) == 0 else 0
        elif op == "brne":
            if Z == 0:
                i = labels[args[0]]
        elif op == "breq":
            if Z == 1:
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
    tgt = [(int.from_bytes(digest[4 * i:4 * i + 4], "big") - INIT[i]) & 0xffffffff for i in range(5)]

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
                       ("HA", [a]), ("HB", [b]), ("HC", [c]), ("HD", [d]), ("HE", [e])):
        for i, w in enumerate(words):
            for k in range(4):
                mem[BASE[sym] + i * 4 + k] = (w >> (8 * k)) & 0xff
    return mem, msg


if __name__ == "__main__":
    lines = body()
    print("instructions: %d  cycles: %d" % (len(lines), sum(2 if l.split()[0] in ("lds", "sts") else 1 for l in lines)))
    prev = "ba29a15896fd2d792d5c4b60668bf2b9feebc51d"
    bad = 0
    for n in list(range(0, 200)) + [9999, 42424, 65500, 100000, 999999]:
        mem, _ = setup_mem(prev, n)
        ok, cyc, _, _ = simulate(lines, mem)
        if ok != 1:
            bad += 1
            if bad < 5:
                print("FAIL nonce %d (cycles %d)" % (n, cyc))
    print("checked %d nonces, failures: %d" % (205, bad))

    # random prevHashes too
    bad2 = 0
    for _ in range(400):
        h = random.getrandbits(160).to_bytes(20, "big").hex()
        n = random.randrange(0, 1000000)
        mem, _ = setup_mem(h, n)
        ok, _, _, _ = simulate(lines, mem)
        if ok != 1:
            bad2 += 1
    print("random hash/nonce failures: %d" % bad2)
