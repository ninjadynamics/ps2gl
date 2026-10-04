#!/usr/bin/env python3
"""Verify the per-vertex haze decoder for raw X2 triangles.

The decoder may only rewrite each input vertex's color alpha; the scheduled
image is executed with the X2V guard's executor and compared with the law.
"""
import argparse
import hashlib
import math
import random
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import x2d_microcode_guard as legacy
import x2q_microcode_guard as copies
import x2v_microcode_guard as haze

REVIEWED_SHA256 = "73811850463c154a29a5ce78a03fa2e177d0f7de321adc4ed1395d80820494d0"


def model(pairs, labels, top, count, rng):
    vf = [[rng.getrandbits(32) for _ in range(4)] for _ in range(32)]
    vf[0] = [0, 0, 0, 0x3f800000]
    vi = [0] * 16
    mem = [[rng.getrandbits(32) for _ in range(4)] for _ in range(1024)]
    mem[top][0] = count
    cx, cz = rng.uniform(-2000.0, 2000.0), rng.uniform(-2000.0, 2000.0)
    radius, height = rng.uniform(300.0, 3000.0), rng.uniform(40.0, 300.0)
    ground = rng.uniform(-20.0, 20.0)
    slope = 0.0 if rng.random() < 0.125 else -1.0 / height
    mem[top + 1] = [haze.to_bits(cx), 0, haze.to_bits(cz), 0]
    mem[top + 2] = [haze.to_bits(1.0 / (radius * radius)),
                    haze.float_bits(rng, 0.1, 1.0),
                    haze.to_bits(1.0 - ground * slope), haze.to_bits(slope)]
    cx, cz = haze.to_float(mem[top + 1][0]), haze.to_float(mem[top + 1][2])
    inv_r2, alpha, t0, slope = (haze.to_float(v) for v in mem[top + 2])
    expected = {}
    for i in range(count):
        base = top + 5 + i * 4
        span = radius * rng.choice((0.2, 1.0, 1.6))
        y = ground if i == 1 else rng.uniform(ground - 30.0, ground + height * 1.3)
        mem[base][:3] = [haze.to_bits(cx + rng.uniform(-span, span)),
                         haze.to_bits(y),
                         haze.to_bits(cz + rng.uniform(-span, span))]
        x, y, z = (haze.to_float(v) for v in mem[base][:3])
        dx, dz = x - cx, z - cz
        at = min((dx * dx + dz * dz) * inv_r2, 1.0)
        q = alpha * at * at
        t = min(max(t0 + y * slope, 0.0), 1.0)
        v = t * math.sqrt(t)
        expected[base + 3] = (mem[base + 3][:3], 1.0 - q * (q + v * (1.0 - q)))
    original = [v[:] for v in mem]
    saved_vf = [v[:] for v in vf]
    writes = haze.execute(pairs, labels, top, mem, vf, vi)
    if writes != set(expected):
        legacy.fail("X2S: the decoder must store exactly the vertex colors")
    worst = 0.0
    for a, (rgb, keep) in expected.items():
        if mem[a][:3] != rgb:
            legacy.fail("X2S: scheduled decoder changes RGB at qword " + str(a))
        got = haze.to_float(mem[a][3])
        if not abs(got - keep) <= haze.FOG_TOLERANCE:
            legacy.fail(f"X2S: fog {got!r} differs from the law {keep!r}")
        worst = max(worst, abs(got - keep))
    for a in range(1024):
        if a not in writes and mem[a] != original[a]:
            legacy.fail("X2S: position/STQ/header/context was overwritten")
    if vf[:26] != saved_vf[:26]:
        legacy.fail("X2S: escaped the reserved VF26..VF31 partition")
    return worst


def verify(base, old, decoder, generated=False):
    legacy.verify(base, old)
    data = decoder.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != REVIEWED_SHA256:
        legacy.fail("X2S: image differs from its reviewed schedule: " + digest)
    if legacy.context_reads(text) or re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2S: decoder escaped the reserved register/context contract")
    count = legacy.instruction_count(text, "X2S")
    if ((count + 1) & ~1) + legacy.X2_INSTRUCTIONS > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2S: combined image exceeds VU1 instruction memory")
    if "[E]" in text:
        legacy.fail("X2S: decoder terminates before entering X2")
    pairs, labels = copies.code_pairs(text)
    rng = random.Random(0x583253)
    worst = 0.0
    cases = 0
    for _ in range(24):
        for top in (79, 551):
            for n in (0, 3, 6, 15, 30):
                worst = max(worst, model(pairs, labels, top, n, rng))
                cases += 1
    print(f"x2s microcode guard: PASS ({count} instructions, {cases} scheduled cases, "
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
