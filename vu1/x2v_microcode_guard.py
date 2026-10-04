#!/usr/bin/env python3
"""Verify the haze-evaluating wall decoder, keeping the reviewed X2 body unchanged.

Geometry, UV and RGB are checked as exact bit copies, like the other decoders.
The fog lane is arithmetic: the scheduled image is executed with float32
operations and compared with the closed-form law. Q has no hardware interlock,
so every MULQ must read a SQRT that is its own (one consumer per SQRT) and has
completed (seven instruction slots, or a WAITQ).
"""
import argparse
import hashlib
import math
import random
import re
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import x2d_microcode_guard as legacy
import x2q_microcode_guard as copies

REVIEWED_SHA256 = "09580ffc5883a9c48f9d2e5ba86791f307b8d0070804defa4d710c1ef7b8074b"
FOG_TOLERANCE = 2.0e-5
SQRT_LATENCY = 7


def to_float(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xffffffff))[0]


def to_bits(value):
    try:
        return struct.unpack("<I", struct.pack("<f", value))[0]
    except OverflowError:
        return 0x7f800000 if value > 0 else 0xff800000


def float_bits(rng, low, high):
    return to_bits(rng.uniform(low, high))


def model(pairs, labels, top, count, rng):
    """Execute the scheduled image, including branch delay slots."""
    vf = [[rng.getrandbits(32) for _ in range(4)] for _ in range(32)]
    vf[0] = [0, 0, 0, 0x3f800000]
    vi = [0] * 16
    mem = [[rng.getrandbits(32) for _ in range(4)] for _ in range(1024)]
    mem[top][0] = count * 6
    cx, cz = rng.uniform(-2000.0, 2000.0), rng.uniform(-2000.0, 2000.0)
    radius, height = rng.uniform(300.0, 3000.0), rng.uniform(40.0, 300.0)
    ground = rng.uniform(-20.0, 20.0)
    # A zero height slope is the EE's "no height falloff" encoding (v = 1).
    slope = 0.0 if rng.random() < 0.125 else -1.0 / height
    haze0 = [to_bits(cx), to_bits(cz), to_bits(cx), to_bits(cz)]
    haze1 = [to_bits(1.0 / (radius * radius)), float_bits(rng, 0.1, 1.0),
             to_bits(1.0 - ground * slope), to_bits(slope)]
    mem[top + 1], mem[top + 2] = haze0[:], haze1[:]
    cx, cz = to_float(haze0[0]), to_float(haze0[1])
    inv_r2, alpha, t0, slope = (to_float(v) for v in haze1)

    exact, fog = {}, {}
    for d in range(count):
        geo = [[rng.getrandbits(32) for _ in range(4)] for _ in range(4)]
        span = radius * rng.choice((0.2, 1.0, 1.6))
        geo[0] = [to_bits(cx + rng.uniform(-span, span)),
                  to_bits(cz + rng.uniform(-span, span)),
                  to_bits(cx + rng.uniform(-span, span)),
                  to_bits(cz + rng.uniform(-span, span))]
        geo[1] = [float_bits(rng, ground - 30.0, ground + height * 1.3)
                  for _ in range(4)]
        if d == 1:
            geo[1][0] = to_bits(ground)
        # Signed zeros in the copied UV lanes, as in the copy-only guards.
        geo[2 + (d & 1)][(d + 1) & 3] = 0x80000000
        col = [[rng.getrandbits(32) for _ in range(4)] for _ in range(2)]
        mem[top + 101 + d * 4:top + 105 + d * 4] = [v[:] for v in geo]
        mem[top + 117 + d * 2:top + 119 + d * 2] = [v[:] for v in col]
        ax, az, bx, bz = geo[0]
        ay, by, cy, dy = geo[1]
        positions = ((ax, ay, az), (bx, by, bz), (bx, cy, bz), (ax, dy, az))
        uv = (geo[2][:2], geo[2][2:], geo[3][:2], geo[3][2:])
        rgb = (col[0][:3], col[0][:3], col[1][:3], col[1][:3])
        keep = []
        for x, y, z in positions:
            dx, dz = to_float(x) - cx, to_float(z) - cz
            at = min((dx * dx + dz * dz) * inv_r2, 1.0)
            q = alpha * at * at
            t = min(max(t0 + to_float(y) * slope, 0.0), 1.0)
            v = t * math.sqrt(t)
            keep.append(1.0 - q * (q + v * (1.0 - q)))
        for k, corner in enumerate((0, 1, 2, 0, 2, 3)):
            dst = top + 5 + d * 24 + k * 4
            exact[dst] = list(positions[corner]) + [0x3f800000]
            exact[dst + 2] = list(uv[corner]) + [0x3f800000, 0]
            exact[dst + 3] = rgb[corner]
            fog[dst + 3] = keep[corner]
    original = [v[:] for v in mem]
    saved_vf = [v[:] for v in vf]
    writes = set()
    pc, pending, cycle = 0, None, 0
    q_bits, q_start, q_waited, q_consumed = None, None, False, True

    def reg(s):
        return int(re.match(r"VF?I?(\d+)", s, re.I)[1])

    def lane(s):
        return "xyzw".index(s[-1].lower())

    def address(s):
        m = re.fullmatch(r"(-?(?:0x[0-9a-f]+|\d+))\((VI\d+)\)", s, re.I)
        if not m:
            legacy.fail("X2V: unexpected address " + s)
        return (int(m[1], 0) + vi[reg(m[2])]) & 0xffff

    arithmetic = {
        "add": lambda a, b: a + b, "sub": lambda a, b: a - b,
        "mul": lambda a, b: a * b, "max": max, "mini": min,
    }
    for _ in range(8000):
        if not 0 <= pc < len(pairs):
            legacy.fail("X2V: decoder escaped before JR")
        before = [v[:] for v in vf]
        jump_after = pending
        pending = None
        if pairs[pc][1].strip().lower() == "waitq":
            q_waited = True
        for inst in pairs[pc]:
            m = re.fullmatch(r"(\w+)(?:\.([xyzw]+))?\s*(.*)", inst, re.I)
            op, mask, argtext = m.groups()
            op = op.lower()
            args = [a.strip() for a in argtext.split(",")] if argtext else []
            lanes = ["xyzw".index(c) for c in mask.lower()] if mask else range(4)
            if op in ("nop", "waitq"):
                continue
            if op == "move" or (op == "max" and args[1] == args[2]):
                for k in lanes:
                    vf[reg(args[0])][k] = before[reg(args[1])][k]
            elif op == "mr32":
                for k in lanes:
                    vf[reg(args[0])][k] = before[reg(args[1])][(k + 1) % 4]
            elif op == "sub" and args[1:] == ["VF00", "VF00"]:
                for k in lanes:
                    vf[reg(args[0])][k] = 0
            elif op in arithmetic:
                a, b = before[reg(args[1])], before[reg(args[2])]
                for k in lanes:
                    vf[reg(args[0])][k] = to_bits(
                        arithmetic[op](to_float(a[k]), to_float(b[k])))
            elif op[:-1] in arithmetic and op[-1] in "xyzw":
                a = before[reg(args[1])]
                b = to_float(before[reg(args[2][:-1])][lane(args[2])])
                for k in lanes:
                    vf[reg(args[0])][k] = to_bits(
                        arithmetic[op[:-1]](to_float(a[k]), b))
            elif op == "sqrt":
                if not q_consumed:
                    legacy.fail("X2V: SQRT restarted before its MULQ")
                value = to_float(before[reg(args[1][:-1])][lane(args[1])])
                if value < 0.0:
                    legacy.fail("X2V: SQRT of a negative height term")
                q_bits = to_bits(math.sqrt(value))
                q_start, q_waited, q_consumed = cycle, False, False
            elif op == "mulq":
                if q_consumed or not (q_waited or cycle - q_start >= SQRT_LATENCY):
                    legacy.fail("X2V: MULQ does not read its own completed SQRT")
                q_consumed = True
                a = before[reg(args[1])]
                for k in lanes:
                    vf[reg(args[0])][k] = to_bits(to_float(a[k]) * to_float(q_bits))
            elif op == "xtop":
                vi[reg(args[0])] = top
            elif op == "ilw":
                vi[reg(args[0])] = mem[address(args[1])][next(iter(lanes))] & 0xffff
            elif op == "lq":
                source = mem[address(args[1])]
                for k in lanes:
                    vf[reg(args[0])][k] = source[k]
            elif op == "sq":
                dst = address(args[1])
                for k in lanes:
                    mem[dst][k] = before[reg(args[0])][k]
                writes.add(dst)
            elif op in ("iadd", "isub"):
                a, b = vi[reg(args[1])], vi[reg(args[2])]
                vi[reg(args[0])] = (a + b if op == "iadd" else a - b) & 0xffff
            elif op == "iaddiu":
                vi[reg(args[0])] = (vi[reg(args[1])] + int(args[2], 0)) & 0xffff
            elif op == "iblez":
                v = vi[reg(args[0])]
                if v == 0 or v & 0x8000:
                    pending = labels[args[1]]
            elif op == "b":
                pending = labels[args[0]]
            elif op == "jr":
                if vi[reg(args[0])] != legacy.X2_MAIN_PC:
                    legacy.fail("X2V: wrong external entry")
                pending = -1
            else:
                legacy.fail("X2V: unmodeled instruction " + inst)
        cycle += 1
        if jump_after == -1:
            break
        pc = jump_after if jump_after is not None else pc + 1
    else:
        legacy.fail("X2V: decoder did not terminate")
    if not q_consumed:
        legacy.fail("X2V: a SQRT result is never consumed")
    if writes != set(exact):
        legacy.fail("X2V: output write set differs from the exact expanded layout")
    worst = 0.0
    for a, value in exact.items():
        if mem[a][:len(value)] != value:
            legacy.fail("X2V: scheduled decoder changes attributes at qword " + str(a))
        if a in fog:
            got = to_float(mem[a][3])
            if not abs(got - fog[a]) <= FOG_TOLERANCE:
                legacy.fail(f"X2V: fog {got!r} differs from the law {fog[a]!r}")
            worst = max(worst, abs(got - fog[a]))
    for a in range(1024):
        if a not in writes and mem[a] != original[a]:
            legacy.fail("X2V: descriptor/header/context/earlier output was overwritten")
    if vf[:26] != saved_vf[:26]:
        legacy.fail("X2V: escaped the reserved VF26..VF31 partition")
    return worst


def verify(base, old, decoder, generated=False):
    legacy.verify(base, old)
    data = decoder.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != REVIEWED_SHA256:
        legacy.fail("X2V: image differs from its reviewed schedule: " + digest)
    if legacy.context_reads(text) or re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2V: decoder escaped the reserved register/context contract")
    count = legacy.instruction_count(text, "X2V")
    if ((count + 1) & ~1) + legacy.X2_INSTRUCTIONS > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2V: combined image exceeds VU1 instruction memory")
    if "[E]" in text:
        legacy.fail("X2V: decoder terminates before entering X2")
    pairs, labels = copies.code_pairs(text)
    rng = random.Random(0x583256)
    worst = 0.0
    for _ in range(64):
        for top in (79, 551):
            for n in range(5):
                worst = max(worst, model(pairs, labels, top, n, rng))
    print(f"x2v microcode guard: PASS ({count} instructions, 640 scheduled cases, "
          f"fog within {worst:.2e} of the law, {digest})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fix-decoder", action="store_true")
    parser.add_argument("base", type=Path)
    parser.add_argument("old", type=Path)
    parser.add_argument("decoder", type=Path)
    args = parser.parse_args()
    if args.fix_decoder:
        legacy.fix_decoder_tail(args.decoder)
    verify(args.base, args.old, args.decoder, args.fix_decoder)
