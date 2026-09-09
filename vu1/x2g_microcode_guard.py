#!/usr/bin/env python3
"""Guard the separate compact-glow decoder and the unchanged X2 dependency.

The scheduled interpreter checks operand/memory semantics and both nearest and
toward-zero float32 rounding models. It is not a cycle-accurate VU simulator:
FMAC extended mantissas, underflow, NaN handling and GS raster rules are outside
this guard. The model intentionally uses finite normal source geometry.
"""
from __future__ import annotations

import argparse
import hashlib
import random
import re
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import x2d_microcode_guard as legacy
from x2q_microcode_guard import code_pairs

X2G_SHA256 = "92f36d375b0940fd0f7d2d377e9ecd4fb8eb4bbd274bc000d00e83c84e365641"


def word(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


def real(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def rounded(value, truncate):
    w = word(value)
    if truncate and abs(real(w)) > abs(value):
        w -= 1
    return w


def model(pairs, labels, top, count, rng, truncate):
    vf = [[rng.getrandbits(32) for _ in range(4)] for _ in range(32)]
    vf[0] = [0, 0, 0, word(1.0)]
    saved_vf = [v[:] for v in vf]
    vi = [0] * 16
    mem = [[rng.getrandbits(32) for _ in range(4)] for _ in range(1024)]
    mem[top][0] = count * 6
    axes = [[word(rng.uniform(-1, 1)) for _ in range(3)] + [0] for _ in range(2)]
    if rng.randrange(3) == 0:
        axes[0][rng.randrange(3)] = rng.choice((0, 0x80000000))
    mem[58:60] = [a[:] for a in axes]
    expected = {}

    def op(a, b, operator):
        x, y = real(a), real(b)
        return rounded(x + y if operator == "+" else
                       x - y if operator == "-" else x * y, truncate)

    for d in range(count):
        scale = rng.choice((1.0, 32.0, 4096.0, 65536.0))
        center = [word(rng.uniform(-scale, scale)) for _ in range(3)]
        half = word(rng.uniform(0.001, 8.0))
        uv = [word(rng.uniform(-8, 8)) for _ in range(4)]
        uv[rng.randrange(4)] = rng.choice((0, 0x80000000, word(1.0)))
        color = [word(rng.randrange(256) * (1.0 / 255.0)) for _ in range(4)]
        mem[top+101+d*2:top+103+d*2] = [center + [half], uv]
        mem[top+109+d*2:top+111+d*2] = [color, [0] * 4]
        scaled = [[op(a[k], half, "*") for k in range(3)] for a in axes]
        corners = [
            [op(op(center[k], scaled[0][k], u), scaled[1][k], v) for k in range(3)]
            for u, v in (("-", "-"), ("+", "-"), ("+", "+"), ("-", "+"))
        ]
        for k, corner in enumerate((0, 1, 2, 0, 2, 3)):
            dst = top + 5 + d*24 + k*4
            expected[dst] = corners[corner] + [word(1.0)]
            expected[dst+2] = [uv[0 if corner in (0, 3) else 2],
                               uv[1 if corner < 2 else 3], word(1.0), 0]
            expected[dst+3] = color[:]
    original = [v[:] for v in mem]
    writes = set()
    pc, pending = 0, None

    def reg(s):
        # VCL prints a redundant component suffix on broadcast operands.
        return int(re.fullmatch(r"V[FI](\d+)[xyzw]?", s, re.I)[1])

    def address(s):
        m = re.fullmatch(r"(-?(?:0x[0-9a-f]+|\d+))\((VI\d+)\)", s, re.I)
        if not m:
            legacy.fail("X2G: unexpected memory address " + s)
        a = int(m[1], 0) + vi[reg(m[2])]
        if not 0 <= a < len(mem):
            legacy.fail("X2G: out-of-range memory access")
        return a

    for _ in range(5000):
        if not 0 <= pc < len(pairs):
            legacy.fail("X2G: decoder escaped before its external tail")
        before = [v[:] for v in vf]
        jump_after, pending = pending, None
        for instruction in pairs[pc]:
            m = re.fullmatch(r"(\w+)(?:\.([xyzw]+))?\s*(.*)", instruction, re.I)
            name, mask, argtext = m.groups()
            name = name.lower()
            args = [a.strip() for a in argtext.split(",")] if argtext else []
            lanes = ["xyzw".index(c) for c in mask] if mask else range(4)
            base = name[:3]
            if name == "nop":
                continue
            if name in ("move", "mr32", "max"):
                dst, src = reg(args[0]), reg(args[1])
                if name == "max" and args[1] != args[2]:
                    legacy.fail("X2G: MAX is no longer an exact source copy")
                for k in lanes:
                    vf[dst][k] = before[src][(k+1)%4] if name == "mr32" else before[src][k]
            elif base in ("add", "sub", "mul"):
                dst, a, b = map(reg, args)
                lane = "xyzw".index(name[3]) if len(name) == 4 else None
                for k in lanes:
                    vf[dst][k] = op(before[a][k], before[b][k if lane is None else lane],
                                    {"add": "+", "sub": "-", "mul": "*"}[base])
            elif name == "xtop": vi[reg(args[0])] = top
            elif name == "ilw": vi[reg(args[0])] = mem[address(args[1])][next(iter(lanes))]
            elif name == "lq":
                value = mem[address(args[1])]
                for k in lanes: vf[reg(args[0])][k] = value[k]
            elif name == "sq":
                dst = address(args[1])
                for k in lanes: mem[dst][k] = before[reg(args[0])][k]
                writes.add(dst)
            elif name in ("iadd", "isub"):
                a, b = vi[reg(args[1])], vi[reg(args[2])]
                vi[reg(args[0])] = (a+b if name == "iadd" else a-b) & 0xffff
            elif name == "iaddiu":
                vi[reg(args[0])] = (vi[reg(args[1])] + int(args[2], 0)) & 0xffff
            elif name == "iblez":
                v = vi[reg(args[0])]
                if v == 0 or v & 0x8000: pending = labels[args[1]]
            elif name == "b": pending = labels[args[0]]
            elif name == "jr":
                if vi[reg(args[0])] != legacy.X2_MAIN_PC:
                    legacy.fail("X2G: wrong external X2 entry")
                pending = -1
            else:
                legacy.fail("X2G: unmodeled scheduled instruction " + instruction)
        if jump_after == -1:
            break
        pc = jump_after if jump_after is not None else pc+1
    else:
        legacy.fail("X2G: decoder did not terminate")
    if writes != set(expected):
        legacy.fail("X2G: output write set differs from the 24-vertex layout")
    for a, value in expected.items():
        if mem[a] != value:
            legacy.fail(f"X2G: changed scheduled corner/UV/color at qword {a}: {mem[a]} != {value}")
    for a in range(len(mem)):
        if a not in writes and mem[a] != original[a]:
            legacy.fail("X2G: descriptor/context/earlier output was overwritten")
    if vf[:26] != saved_vf[:26]:
        legacy.fail("X2G: escaped VF26..VF31 partition")


def verify(x2, old_decoder, decoder, generated=False):
    legacy.verify(x2, old_decoder)
    data = decoder.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != X2G_SHA256:
        legacy.fail("X2G: generated image changed since review: " + digest)
    if legacy.context_reads(text) != {58, 59}:
        legacy.fail("X2G: shared context reads differ from the two owned axes")
    if re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2G: escaped its reserved register partition")
    count = legacy.instruction_count(text, "X2G")
    if ((count+1)&~1) + legacy.X2_INSTRUCTIONS > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2G: decoder plus X2 exceeds instruction memory")
    if "[E]" in text:
        legacy.fail("X2G: decoder ends before its X2 tail call")
    pairs, labels = code_pairs(text)
    rng = random.Random(0x583247)
    tests = 0
    for truncate in (False, True):
        for _ in range(32):
            for top in (79, 551):
                for n in range(5):
                    model(pairs, labels, top, n, rng, truncate)
                    tests += 1
    print(f"x2g microcode guard: PASS ({count} instructions; {tests} scheduled cases; sha256 {digest})")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--fix-decoder", action="store_true")
    p.add_argument("x2", type=Path)
    p.add_argument("legacy", type=Path)
    p.add_argument("decoder", type=Path)
    a = p.parse_args()
    if a.fix_decoder:
        legacy.fix_decoder_tail(a.decoder)
    verify(a.x2, a.legacy, a.decoder, a.fix_decoder)


if __name__ == "__main__":
    main()
