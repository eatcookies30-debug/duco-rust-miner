#!/usr/bin/env python3
"""Generate the hand-written AVR assembly DUCO-S1a hash core.

The SHA-1 state lives in six 32-bit register groups that rotate through the
five roles (a,b,c,d,e) one group per round: the round accumulates the new 'a'
in the group the previous round's 'e' vacated, so the a->b->c->d->e->a shift is
a change of which group the next round's code names, not eight movw copies.

    G0 r16-19   G1 r20-23   G2 r2-5   G3 r6-9   G4 r10-13
    G5 r14,r15,r30,r31      scratch: r24-27

r24-r27 are deliberately reserved for the scratch value (the schedule word and
then f(b,c,d)): every byte of them can take an immediate, so the phase
constants are added with subi/sbci instead of four LDS + ADD from memory. GAS
on AVR has no "add reg, imm", so a constant is added as a subtraction of its
two's complement.
"""

GROUPS = [[16, 17, 18, 19], [20, 21, 22, 23], [2, 3, 4, 5], [6, 7, 8, 9],
          [10, 11, 12, 13], [14, 15, 30, 31]]
SCRATCH = ["r24", "r25", "r26", "r27"]   # the only group r16 and up
K = {1: 0x5a827999, 2: 0x6ed9eba1, 3: 0x8f1bbcdc, 4: 0xca62c1d6}


def grp(j):
    return ["r%d" % r for r in GROUPS[j]]


def load4(regs, base):
    """AVR LDS/STS are single-byte instructions: a 32-bit value needs four of
    each. (Assuming lds loads two bytes is the classic mistake here - it is
    not, and it silently leaves every odd register holding stale garbage.)"""
    return ["lds %s, %s+%d" % (regs[i], base, i) for i in range(4)]


def store4(regs, base):
    return ["sts %s+%d, %s" % (base, i, regs[i]) for i in range(4)]


def add_const(regs, value):
    """regs += value. AVR has no 32-bit immediate add; subi/sbci with the
    two's complement do it in four instructions. Only r16-r31 accept an
    immediate, which is why the scratch group is pinned to r24-r27 - the
    constant is added to f(b,c,d) there, not to the accumulator, so every
    round pays four instructions instead of four LDS plus four ADD from
    memory. GAS prints negative immediates as the low byte."""
    n = (0x100000000 - value) & 0xffffffff
    out = ["subi %s, %d" % (regs[0], n & 0xff)]
    for i in range(1, 4):
        b = (n >> (8 * i)) & 0xff
        out.append("sbci %s, %d" % (regs[i], b if b <= 127 else b - 256))
    return out


def load(regs, word):
    """regs = MSG[word] (little-endian 32-bit in memory)."""
    return load4(regs, "MSG+%d" % (word * 4))


def copy(dst, src):
    return ["movw %s, %s" % (dst[0], src[0]), "movw %s, %s" % (dst[2], src[2])]


def add32(dst, src):
    return ["add %s, %s" % (dst[0], src[0])] + \
           ["adc %s, %s" % (dst[i], src[i]) for i in range(1, 4)]


def xor32(dst, src):
    return ["eor %s, %s" % (dst[i], src[i]) for i in range(4)]


def or32(dst, src):
    return ["or %s, %s" % (dst[i], src[i]) for i in range(4)]


def and32(dst, src):
    return ["and %s, %s" % (dst[i], src[i]) for i in range(4)]


def rotl5(r):
    """r = rotl32(r, 5): five shifts through the group, the five bits that
    fall off the top come back from the saved top byte."""
    out = ["mov r0, %s" % r[3]]
    for _ in range(5):
        out += ["lsl %s" % r[0], "rol %s" % r[1], "rol %s" % r[2], "rol %s" % r[3]]
    return out + ["lsr r0", "lsr r0", "lsr r0", "or %s, r0" % r[0]]


def rotl30(r):
    """r = rotl32(r, 30) == r >> 2 with the two low bits wrapped to the top.
    A right shift walks the group from the most significant byte down."""
    out = ["mov r0, %s" % r[0]]
    for _ in range(2):
        out += ["lsr %s" % r[3], "ror %s" % r[2], "ror %s" % r[1], "ror %s" % r[0]]
    return out + ["lsl r0"] * 6 + ["or %s, r0" % r[3]]


def rotl1(r):
    return ["lsl %s" % r[0], "rol %s" % r[1], "rol %s" % r[2], "rol %s" % r[3],
            "adc %s, r1" % r[0]]


def schedule(i, work, tmp):
    """work = rotl1(MSG[i-3] ^ MSG[i-8] ^ MSG[i-14] ^ MSG[i-16])"""
    out = load(work, i - 3)
    for w in (i - 8, i - 14, i - 16):
        out += load(tmp, w)
        out += xor32(work, tmp)
    out += rotl1(work)
    # A word is only ever read back by rounds i+3, i+8, i+14 and i+16, so the
    # last three rounds (77-79) can skip the write entirely.
    if i <= 76:
        out += store4(work, "MSG+%d" % (i * 4))
    return out


def combine(phase, b, c, d, t1, t2):
    """f(b, c, d) left in t1."""
    if phase in (2, 4):                     # b ^ c ^ d
        return copy(t1, b) + xor32(t1, c) + xor32(t1, d)
    if phase == 1:                          # d ^ (b & (c ^ d))
        return copy(t1, c) + xor32(t1, d) + and32(t1, b) + xor32(t1, d)
    # phase 3: majority == (b & c) | (d & (b | c))
    return copy(t1, b) + or32(t1, c) + and32(t1, d) + copy(t2, b) + and32(t2, c) + or32(t1, t2)


def plain_body(i, phase, roles, new, has_word):
    a, b, c, d, e = (grp(j) for j in roles)
    new = grp(new)
    out = copy(new, a)                      # the old a becomes the next round's b
    out += rotl5(new)
    out += add32(new, e)
    if has_word:
        out += load(SCRATCH, i)
        out += add32(new, SCRATCH)          # + the message word
    out += combine(phase, b, c, d, SCRATCH, e)   # e is dead: it is already in new
    out += add_const(SCRATCH, K[phase])
    out += add32(new, SCRATCH)
    out += rotl30(b)                        # in place: this group is next round's c
    return out


def body_with_schedule(i, phase, roles, new):
    """Schedule word computed into scratch, then added straight from registers."""
    new = grp(new)
    out = schedule(i, SCRATCH, new)
    a, b, c, d, e = (grp(j) for j in roles)
    out += copy(new, a)
    out += rotl5(new)
    out += add32(new, e)
    out += add32(new, SCRATCH)
    out += combine(phase, b, c, d, SCRATCH, e)
    out += add_const(SCRATCH, K[phase])
    out += add32(new, SCRATCH)
    out += rotl30(b)
    return out


FINAL_ROLES = None
DEBUG = False
TRACE = False


def dump_state(lines, roles, base):
    """Store a,b,c,d,e byte by byte into a flat global at `base` (16 bytes)."""
    for word, regs in enumerate(grp(j) for j in roles):
        for i in range(4):
            lines.append("sts %s+%d, %s" % (base, word * 4 + i, regs[i]))


def body():
    global FINAL_ROLES
    lines = []
    for j, name in enumerate(("HA", "HB", "HC", "HD", "HE")):
        lines += load4(grp(j), name)
    # Schedule words 16, 17 and 20 do not depend on the nonce (for every nonce
    # length the pool sends, the digits never reach MSG[12] or MSG[13]), so the
    # C side writes them straight into MSG[] once per job. Copying them here
    # would cost 24 memory instructions on every single attempt.

    roles = [0, 1, 2, 3, 4]
    new = 5
    if TRACE:
        dump_state(lines, roles, "TRACE+1400")
    for i in range(10, 80):
        phase = 1 if i < 20 else 2 if i < 40 else 3 if i < 60 else 4
        has_sch = i >= 16 and i not in (16, 17, 20)
        if has_sch:
            lines += body_with_schedule(i, phase, roles, new)
        else:
            lines += plain_body(i, phase, roles, new, True)
        vacated = roles[4]
        roles = [new, roles[0], roles[1], roles[2], roles[3]]
        new = vacated
        if TRACE:
            dump_state(lines, roles, "TRACE+%d" % ((i - 10) * 20))

    tgt = SCRATCH
    if TRACE:
        lines.append("ret")
        return lines
    if DEBUG:
        for word, regs in enumerate(grp(g) for g in roles):
            for i in range(4):
                lines.append("sts DBG+%d, %s" % (word * 4 + i, regs[i]))
        lines.append("ret")
        return lines

    def compare(word, regs):
        # Each cpc carries the borrow from the more significant byte, so an
        # unequal byte anywhere in the word leaves Z clear and exits. The
        # target's most significant byte is checked first: almost every nonce
        # fails there, so the whole compare is eight instructions. AVR
        # conditional branches only reach 63 words, so the failure handler is
        # inlined per word and the "byte equal" case branches past it.
        fail = "dc_fail_%d" % word
        nxt = "dc_word_%d" % (word + 1)
        base = "TGT+%d" % (word * 4)
        out = ["lds %s, %s+3" % (tgt[3], base),
               "cp %s, %s" % (regs[3], tgt[3]), "brne %s" % fail,
               "lds %s, %s+2" % (tgt[2], base),
               "cpc %s, %s" % (regs[2], tgt[2]), "brne %s" % fail,
               "lds %s, %s+1" % (tgt[1], base),
               "cpc %s, %s" % (regs[1], tgt[1]), "brne %s" % fail,
               "lds %s, %s+0" % (tgt[0], base),
               "cpc %s, %s" % (regs[0], tgt[0]), "breq %s" % nxt,
               fail + ":", "ldi r24, 0", "ret",
               nxt + ":"]
        return out

    for word, regs in enumerate((grp(j) for j in roles)):
        lines += compare(word, regs)
    # The hit flag is returned in r24: the wrapper takes it as an asm output
    # register. A plain global is not enough - without a memory clobber GCC
    # decides the flag cannot change between calls and hoists the load out of
    # the unrolled nonce loop, so every attempt looks like a miss.
    lines.append("ldi r24, 1")
    lines.append("ret")
    return lines


def cycles(lines):
    mem = ("lds", "sts")
    return sum(2 if l.split()[0] in mem else 1 for l in lines)


def emit(path):
    lines = body()
    asm = "".join('        "%s\\n"\n' % l for l in lines)
    # r24 carries the hit flag out of the core, so it is the asm statement's
    # output rather than a clobber. HIT as a plain global is not enough: with
    # no memory clobber GCC decides the flag cannot change between calls and
    # hoists the load out of the unrolled nonce loop, so every attempt looks
    # like a miss.
    clobbers = ", ".join('"r%d"' % n for n in list(range(2, 28)) + [30, 31]
                         if n != 24)
    src = "/* generated by gen_asm.py - do not edit by hand */\n" \
          "extern \"C\" {\n" \
          "  __attribute__((naked, used)) void duco_rounds_core(void) {\n" \
          "    __asm__ volatile (\n%s    );\n" \
          "  }\n" \
          "}\n" \
          "/* The core clobbers every general register. Calling it through an\n" \
          "   asm statement with an explicit clobber list makes GCC spill its\n" \
          "   live loop state into the stack frame around it; a plain call lets\n" \
          "   the compiler keep counters in registers the core overwrites. */\n" \
          "static inline int duco_rounds_asm(void) {\n" \
          "  register uint8_t hit asm(\"r24\");\n" \
          "  __asm__ volatile (\"call duco_rounds_core\"\n" \
          "      : \"=r\"(hit)\n" \
          "      : : %s);\n" \
          "  return hit;\n" \
          "}\n" % (asm, clobbers)
    open(path, "w").write(src)
    print("instructions: %d  cycles: %d" % (len(lines), cycles(lines)))


if __name__ == "__main__":
    import sys
    # Default is a scratch file; pass the sketch's header to regenerate the core
    # that actually gets flashed:
    #   python3 gen_asm.py ../asm_sketch/duco_rounds.h
    emit(sys.argv[1] if len(sys.argv) > 1 else "rounds_body.inc")
