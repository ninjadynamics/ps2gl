#!/usr/bin/env python3
"""Guard the exact-corner decoder without regenerating either legacy image."""
from __future__ import annotations

import argparse
import hashlib
import random
import re
import sys
from pathlib import Path

# The archive guard must not leave generated Python files in the source tree.
sys.dont_write_bytecode = True
import x2d_microcode_guard as legacy

X2Q_SHA256 = "7e4a6b8f116e91e15670a4a32bbb46c4eb74c2b34f1529dd9b294cfd4832ca3c"


def code_pairs(text):
    pairs, labels = [], {}
    active = False
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.endswith("_CodeStart:"):
            active = True
            continue
        if not active or not line or line.startswith("."):
            continue
        if line.endswith(":"):
            labels[line[:-1]] = len(pairs)
            continue
        tokens = line.split()
        upper_words = 1 if tokens[0].lower() == "nop" else 2
        parts = [" ".join(tokens[:upper_words]), " ".join(tokens[upper_words:])]
        if not parts[1]:
            legacy.fail("X2Q: cannot split scheduled instruction pair: " + line)
        pairs.append(parts)
    return pairs, labels


def model(pairs, labels, top, count, rng):
    """Execute the scheduled copy-only image, including branch delay slots.

    Registers/memory are integer bit patterns. This tests dataflow and stores,
    not VU latency or arithmetic; identical-input MAX is VCL's MOVE spelling.
    """
    vf = [[rng.getrandbits(32) for _ in range(4)] for _ in range(32)]
    vf[0] = [0, 0, 0, 0x3f800000]
    vi = [0] * 16
    mem = [[rng.getrandbits(32) for _ in range(4)] for _ in range(1024)]
    mem[top][0] = count * 6
    expected = {}
    for d in range(count):
        # Include signed-zero words; other bit patterns stress pure copies.
        geo = [[rng.getrandbits(32) for _ in range(4)] for _ in range(4)]
        geo[d & 3][(d + 1) & 3] = 0x80000000
        col = [[rng.getrandbits(32) for _ in range(4)] for _ in range(4)]
        mem[top+101+d*4:top+105+d*4] = [v[:] for v in geo]
        mem[top+117+d*4:top+121+d*4] = [v[:] for v in col]
        ax, az, bx, bz = geo[0]
        ay, by, cy, dy = geo[1]
        positions = ((ax,ay,az), (bx,by,bz), (bx,cy,bz), (ax,dy,az))
        uv = (geo[2][:2], geo[2][2:], geo[3][:2], geo[3][2:])
        for k, corner in enumerate((0,1,2,0,2,3)):
            dst = top + 5 + d*24 + k*4
            expected[dst] = list(positions[corner]) + [0x3f800000]
            expected[dst+2] = list(uv[corner]) + [0x3f800000, 0]
            expected[dst+3] = col[corner]
    original = [v[:] for v in mem]
    saved_vf = [v[:] for v in vf]
    writes = set()
    pc, pending = 0, None

    def reg(s):
        return int(s[2:])

    def number(s):
        return int(s, 0)

    def address(s):
        m = re.fullmatch(r"(-?(?:0x[0-9a-f]+|\d+))\((VI\d+)\)", s, re.I)
        if not m:
            legacy.fail("X2Q: unexpected address " + s)
        return number(m[1]) + vi[reg(m[2])]

    for steps in range(5000):
        if not 0 <= pc < len(pairs):
            legacy.fail("X2Q: decoder escaped before JR")
        before = [v[:] for v in vf]
        jump_after = pending
        pending = None
        for inst in pairs[pc]:
            m = re.fullmatch(r"(\w+)(?:\.([xyzw]+))?\s*(.*)", inst, re.I)
            op, mask, argtext = m.groups()
            op = op.lower()
            args = [a.strip() for a in argtext.split(",")] if argtext else []
            lanes = ["xyzw".index(c) for c in mask] if mask else range(4)
            if op == "nop":
                continue
            if op in ("move", "mr32", "max", "sub"):
                dst, src = reg(args[0]), reg(args[1])
                if op == "max" and args[1] != args[2]:
                    legacy.fail("X2Q: arithmetic MAX replaced a bit copy")
                if op == "sub" and args[1:] != ["VF00", "VF00"]:
                    legacy.fail("X2Q: unexpected floating-point arithmetic")
                for k in lanes:
                    vf[dst][k] = (0 if op == "sub" else
                        before[src][(k+1)%4] if op == "mr32" else before[src][k])
            elif op == "xtop": vi[reg(args[0])] = top
            elif op == "ilw": vi[reg(args[0])] = mem[address(args[1])][next(iter(lanes))]
            elif op == "lq": vf[reg(args[0])] = mem[address(args[1])][:]
            elif op == "sq":
                dst = address(args[1])
                mem[dst] = before[reg(args[0])][:]
                writes.add(dst)
            elif op in ("iadd", "isub"):
                a,b = vi[reg(args[1])],vi[reg(args[2])]
                vi[reg(args[0])] = (a+b if op == "iadd" else a-b) & 0xffff
            elif op == "iaddiu": vi[reg(args[0])] = (vi[reg(args[1])] + number(args[2])) & 0xffff
            elif op == "iblez":
                v = vi[reg(args[0])]
                if v == 0 or v & 0x8000: pending = labels[args[1]]
            elif op == "b": pending = labels[args[0]]
            elif op == "jr":
                if vi[reg(args[0])] != legacy.X2_MAIN_PC:
                    legacy.fail("X2Q: wrong external entry")
                pending = -1
            else: legacy.fail("X2Q: unmodeled instruction " + inst)
        if jump_after == -1:
            break
        pc = jump_after if jump_after is not None else pc+1
    else:
        legacy.fail("X2Q: decoder did not terminate")
    if writes != set(expected):
        legacy.fail("X2Q: output write set differs from the exact expanded layout")
    for a, value in expected.items():
        if mem[a] != value:
            legacy.fail("X2Q: scheduled decoder changes attributes at qword " + str(a))
    for a in range(1024):
        if a not in writes and mem[a] != original[a]:
            legacy.fail("X2Q: descriptor/context/earlier output was overwritten")
    if vf[:26] != saved_vf[:26]:
        legacy.fail("X2Q: escaped the reserved VF26..VF31 partition")


def verify(x2_path, old_path, new_path, generated=False):
    legacy.verify(x2_path, old_path)
    data = new_path.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != X2Q_SHA256:
        legacy.fail("X2Q: generated image changed since its scheduled-copy review: " + digest)
    if legacy.context_reads(text):
        legacy.fail("X2Q: decoder unexpectedly reads shared context")
    if re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2Q: decoder escaped its register partition")
    count = legacy.instruction_count(text, "X2Q")
    if ((count+1)&~1) + legacy.X2_INSTRUCTIONS > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2Q: combined image exceeds VU1 instruction memory")
    if "[E]" in text:
        legacy.fail("X2Q: decoder ends before tail-calling X2")
    pairs, labels = code_pairs(text)
    rng = random.Random(0x583251)
    for _ in range(32):
        for top in (80, 552):
            for descriptors in range(5):
                model(pairs, labels, top, descriptors, rng)
    print(f"x2q microcode guard: PASS ({count} instructions; 320 scheduled-copy cases; sha256 {digest})")


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
