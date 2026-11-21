#!/usr/bin/env python3
"""Generate the AVR assembly DUCO-S1a hash core.

The SHA-1 state lives in six 32-bit register groups that rotate through the
five roles (a,b,c,d,e) one group per round: the round accumulates the new 'a'
in the group the previous round's 'e' vacated, so the a->b->c->d->e->a shift is
a change of which group the next round's code names, not eight movw copies.

    full core (default, for the Mega 2560)
        G0 r16-19   G1 r20-23   G2 r2-5   G3 r6-9   G4 r10-13
        G5 r14,r15,r30,r31      scratch: r24-27

    compact core (--compact, for an ATmega328P Nano, which has 30 KB of flash)
        G0 r16-19   G1 r20-23   G2 r2-5   G3 r6-9   G4 r10-13
        G5 r14,r15,r28,r29      scratch: r24-27   Z r30-31 = schedule pointer

r24-r27 are deliberately reserved for the scratch value (the schedule word and
then f(b,c,d)): every byte of them can take an immediate, so the phase
constants are added with subi/sbci instead of four LDS + ADD from memory. GAS
on AVR has no "add reg, imm", so a constant is added as a subtraction of its
two's complement.

The compact core keeps the same register rotation but stops unrolling every
round. The role shift repeats every six rounds, so each 20-round phase is cut
into a six-round body that runs three times, plus the leftover rounds emitted
inline. Six rounds of code now serve eighteen rounds of hashing, which is what
makes the sketch small enough for a Nano: about 8 KB less code. The pointer work
a looped round needs instead of four absolute LDS/STS costs roughly 3% of the
hashrate - 1,759 H/s against 1,815 H/s for the unrolled core on the same Nano.

The compact core keeps part of the SHA-1 state in r28:r29, which is the pair
GCC uses as its stack frame pointer. The core pushes and pops them around every
call, so it can be called from an ordinary function with local variables.

Inside a loop body Z points at MSG + 4*round - 64, which is why the four
schedule words sit at Z+52, Z+32, Z+8 and Z+0: AVR's LDD displacement is a
6-bit field, so a pointer can reach 63 bytes, and MSG[round] is exactly 64
bytes past MSG[round-16]. The round therefore steps the pointer forward 60
bytes to store, then back 56 with SBIW to land on the next round's base. That
also makes the pointer itself the loop counter: it advances exactly four bytes
a round, so comparing its low byte against the address past the last pass ends
the loop with no counter at all.
"""

import sys

K = {1: 0x5a827999, 2: 0x6ed9eba1, 3: 0x8f1bbcdc, 4: 0xca62c1d6}

GROUPS_FULL = [[16, 17, 18, 19], [20, 21, 22, 23], [2, 3, 4, 5],
               [6, 7, 8, 9], [10, 11, 12, 13], [14, 15, 30, 31]]
GROUPS_COMPACT = [[16, 17, 18, 19], [20, 21, 22, 23], [2, 3, 4, 5],
                  [6, 7, 8, 9], [10, 11, 12, 13], [14, 15, 28, 29]]

SCRATCH = ["r24", "r25", "r26", "r27"]   # the only group fully r16 and up
POINTER = ["r30", "r31"]                 # Z, the schedule pointer (compact only)

DEBUG = False
TRACE = False


def grp(j):
    return ["r%d" % r for r in GROUPS[j]]


def load4(regs, base):
    """AVR LDS/STS move **one byte**, not a 32-bit word: a 32-bit value needs
    four of each. (Assuming lds loads two bytes is the classic mistake here -
    it silently leaves every odd register holding stale garbage.)"""
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


def roles_at(i):
    """Which group plays a,b,c,d,e at round i, and which one is free to write
    the new 'a' into. The rotation has period 6, so a six-round body starts
    with the same role layout every time it runs."""
    shift = (10 - i) % 6
    return [(k + shift) % 6 for k in range(5)], (5 + shift) % 6


def phase_of(i):
    return 1 if i < 20 else 2 if i < 40 else 3 if i < 60 else 4


def raw_word(i, compact):
    """Rounds 10-15 hash the message word itself: SHA-1's schedule only starts
    at word 16. Schedule words 16 and 17 only ever combine the previous hash
    and the zero nonce words, so they are the same for every attempt of a job
    and the C side writes them into MSG[] once. In the compact core word 20 is
    recomputed in the loop body instead, because a body has to work for all of
    its three passes."""
    if i < 16 or i in (16, 17):
        return True
    return i == 20 and not compact


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


def zload4(regs, disp):
    """regs = the four bytes at Z + disp. LDD's displacement is six bits, so
    disp must be 0..63; the byte sitting exactly on the pointer needs the plain
    `ld r, Z` form, the other three use a displacement."""
    out = []
    for i, r in enumerate(regs):
        d = disp + i
        out.append("ld %s, Z" % r if d == 0 else "ldd %s, Z+%d" % (r, d))
    return out


def schedule_z(work, tmp):
    """The same schedule computation, but Z = MSG + 4*round - 64 on entry, so
    the four words the schedule reads sit at Z+52, Z+32, Z+8 and Z+0. The word
    being written is at Z+64, just past LDD's 63-byte reach: ADIW walks the
    pointer up far enough for the four stores to land on it, and SBIW walks it
    back to the base for the next round. A looped round therefore costs four
    cycles and six bytes more than an unrolled one.
    """
    out = zload4(work, 52)        # MSG[i-3]
    out += zload4(tmp, 32)        # MSG[i-8]
    out += xor32(work, tmp)
    out += zload4(tmp, 8)        # MSG[i-14]
    out += xor32(work, tmp)
    out += zload4(tmp, 0)        # MSG[i-16]
    out += xor32(work, tmp)
    out += rotl1(work)
    out += ["adiw %s, 60" % POINTER[0]]
    out += ["std Z+%d, %s" % (4 + i, r) for i, r in enumerate(work)]
    out += ["sbiw %s, 56" % POINTER[0]]
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


def round_z(phase, roles, new):
    """One round inside a loop body, with the schedule reached through Z."""
    new = grp(new)
    a, b, c, d, e = (grp(j) for j in roles)
    out = schedule_z(SCRATCH, new)
    out += copy(new, a)
    out += rotl5(new)
    out += add32(new, e)
    out += add32(new, SCRATCH)
    out += combine(phase, b, c, d, SCRATCH, e)
    out += add_const(SCRATCH, K[phase])
    out += add32(new, SCRATCH)
    out += rotl30(b)
    return out


def dump_state(lines, roles, base):
    """Store a,b,c,d,e byte by byte into a flat global at `base` (20 bytes)."""
    for word, regs in enumerate(grp(j) for j in roles):
        for i in range(4):
            lines.append("sts %s+%d, %s" % (base, word * 4 + i, regs[i]))


def exit_stub(lines, hit):
    """Return with the hit flag in r24. The compact core borrows r28:r29 for
    part of the SHA-1 state, so it puts the caller's frame pointer back before
    every return; the full core never touches it and returns straight away."""
    lines.append("ldi r24, %d" % hit)
    if RESTORE_FP:
        lines.append("pop r29")
        lines.append("pop r28")
    lines.append("ret")


def compare_tail(roles):
    tgt = SCRATCH

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
               fail + ":"]
        exit_stub(out, 0)
        out.append(nxt + ":")
        return out

    lines = []
    for word, regs in enumerate((grp(j) for j in roles)):
        lines += compare(word, regs)
    # The hit flag is returned in r24: the wrapper takes it as an asm output
    # register. A plain global is not enough - without a memory clobber GCC
    # decides the flag cannot change between calls and hoists the load out of
    # the nonce loop, so every attempt looks like a miss.
    exit_stub(lines, 1)
    return lines


def blocks(compact):
    """(first round, one past last round, mode). A loop body must be a whole
    number of six-round role cycles and stay inside one SHA-1 phase, so each
    20-round phase gives three body passes plus two rounds emitted inline."""
    if not compact:
        return [(10, 80, "abs")]
    return [(10, 20, "abs"),
            (20, 38, "loop"), (38, 40, "abs"),
            (40, 58, "loop"), (58, 60, "abs"),
            (60, 78, "loop"), (78, 80, "abs")]


def body(compact=False):
    global GROUPS, RESTORE_FP
    GROUPS = GROUPS_COMPACT if compact else GROUPS_FULL
    RESTORE_FP = compact
    lines = []
    if compact:
        # r28:r29 hold part of the SHA-1 state in the compact core, and GCC uses
        # them as its frame pointer. Saving them lets the core run inside a
        # function that has local variables instead of forcing every caller to
        # be frame-free.
        lines += ["push r28", "push r29"]
    for j, name in enumerate(("HA", "HB", "HC", "HD", "HE")):
        lines += load4(grp(j), name)

    for first, last, mode in blocks(compact):
        if mode == "abs":
            for i in range(first, last):
                roles, new = roles_at(i)
                if raw_word(i, compact):
                    lines += plain_body(i, phase_of(i), roles, new, True)
                else:
                    lines += body_with_schedule(i, phase_of(i), roles, new)
        else:
            top = "dc_loop_%d" % first
            end = "dc_loop_end_%d" % first
            base = 4 * (first - 16)
            lines += ["ldi %s, lo8(MSG+%d)" % (POINTER[0], base),
                      "ldi %s, hi8(MSG+%d)" % (POINTER[1], base),
                      top + ":"]
            for i in range(first, first + 6):
                roles, new = roles_at(i)
                lines += round_z(phase_of(i), roles, new)
            # Every body pass ends with the pointer one round past the rounds it
            # ran, so the loop is over when it reaches the address just after the
            # last pass. The pointer only moves in steps of four across a window
            # of 72 bytes, so that low byte can match exactly once and the loop
            # needs no counter at all. `breq` cannot span the body, so the exit is
            # a stub the branch can reach and the back-edge is a long jmp.
            lines += ["cpi %s, lo8(MSG+%d)" % (POINTER[0], 4 * (last - 16)),
                      "breq " + end,
                      "jmp " + top,
                      end + ":"]

    # Round 80 does not exist: after the last round the roles have already been
    # shifted once more, so the final a..e are the groups round 80 would use.
    roles, _ = roles_at(80)
    if TRACE:
        lines = lines  # trace mode is only built from the full core
    lines += compare_tail(roles)
    return lines


def cycles(lines):
    two_cycle = ("lds", "sts", "ldd", "std", "ld", "push", "pop",
                 "adiw", "sbiw", "jmp")
    return sum(2 if l.split()[0] in two_cycle else 1 for l in lines)


def used_registers(compact=False):
    regs = set()
    for g in GROUPS:
        regs.update(g)
    for r in SCRATCH + POINTER:
        regs.add(int(r[1:]))
    if compact:
        # r28:r29 are used by the core but pushed and popped back, so GCC's
        # frame pointer survives and they do not have to be clobbered.
        regs -= {28, 29}
    return regs


def emit(path, compact=False):
    lines = body(compact)
    asm = "".join('        "%s\\n"\n' % l for l in lines)
    # r24 carries the hit flag out of the core, so it is the asm statement's
    # output rather than a clobber.
    clobbers = ", ".join('"r%d"' % n for n in sorted(used_registers(compact)) if n != 24)
    src = "/* generated by gen_asm.py - do not edit directly */\n" \
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
    args = [a for a in sys.argv[1:] if a != "--compact"]
    compact = "--compact" in sys.argv[1:]
    # Default is a scratch file; pass the sketch's header to regenerate the core
    # that actually gets flashed:
    #   python3 gen_asm.py duco_rounds_mega.h
    #   python3 gen_asm.py --compact duco_rounds_328p.h
    emit(args[0] if args else "rounds_body.inc", compact)
