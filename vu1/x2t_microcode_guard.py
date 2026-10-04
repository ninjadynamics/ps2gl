#!/usr/bin/env python3
"""Verify the per-corner haze decoder in front of the X2F source-quad program.

The decoder may only rewrite each input corner's color alpha; the scheduled
image is executed with the X2V guard's executor and compared with the law.
X2F itself is only read: its entry PC, its register use and its size.
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

REVIEWED_SHA256 = "4023e0ab57b481d7214d7cc204c487198528b4119a672c643b255231e0280f1e"


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
    s, m = rng.choice(((1.0, 0.0), (0.0, 1.0)))
    mem[top + 1] = [haze.to_bits(cx), haze.to_bits(s), haze.to_bits(cz), haze.to_bits(m)]
    mem[top + 2] = [haze.to_bits(1.0 / (radius * radius)),
                    haze.float_bits(rng, 0.1, 1.0),
                    haze.to_bits(1.0 - ground * slope), haze.to_bits(slope)]
    cx, cz = haze.to_float(mem[top + 1][0]), haze.to_float(mem[top + 1][2])
    inv_r2, alpha, t0, slope = (haze.to_float(v) for v in mem[top + 2])
    expected = {}
    for i in range(count):
        base = top + 5 + i * 3
        span = radius * rng.choice((0.2, 1.0, 1.6))
        y = ground if i == 1 else rng.uniform(ground - 30.0, ground + height * 1.3)
        mem[base][:3] = [haze.to_bits(cx + rng.uniform(-span, span)),
                         haze.to_bits(y),
                         haze.to_bits(cz + rng.uniform(-span, span))]
        mem[base + 2][3] = haze.float_bits(rng, 0.0, 1.0)
        x, y, z = (haze.to_float(v) for v in mem[base][:3])
        source = haze.to_float(mem[base + 2][3])
        dx, dz = x - cx, z - cz
        at = min((dx * dx + dz * dz) * inv_r2, 1.0)
        q = alpha * at * at
        t = min(max(t0 + y * slope, 0.0), 1.0)
        v = t * math.sqrt(t)
        keep = 1.0 - q * (q + v * (1.0 - q))
        expected[base + 2] = (mem[base + 2][:3], keep * (s + m * source))
    original = [v[:] for v in mem]
    saved_vf = [v[:] for v in vf]
    writes = haze.execute(pairs, labels, top, mem, vf, vi)
    if writes != set(expected):
        legacy.fail("X2T: the decoder must store exactly the corner colors")
    worst = 0.0
    for a, (rgb, value) in expected.items():
        if mem[a][:3] != rgb:
            legacy.fail("X2T: scheduled decoder changes RGB at qword " + str(a))
        got = haze.to_float(mem[a][3])
        if not abs(got - value) <= haze.FOG_TOLERANCE:
            legacy.fail(f"X2T: alpha {got!r} differs from the law {value!r}")
        worst = max(worst, abs(got - value))
    for a in range(1024):
        if a not in writes and mem[a] != original[a]:
            legacy.fail("X2T: position/STQ/header/context was overwritten")
    if vf[:26] != saved_vf[:26]:
        legacy.fail("X2T: escaped the reserved VF26..VF31 partition")
    return worst


def verify(program, decoder, generated=False):
    base = program.read_text(encoding="utf-8")
    if legacy.label_pc(base, "main_loop_lid") != legacy.X2_MAIN_PC:
        legacy.fail("X2T: X2F main_loop_lid moved from the decoder's entry PC")
    if re.search(r"\bVF(?:2[6-9]|3[01])\b", base):
        legacy.fail("X2T: X2F now uses VF26..VF31 reserved for the decoder")
    base_count = legacy.instruction_count(base, "X2F")
    data = decoder.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != REVIEWED_SHA256:
        legacy.fail("X2T: image differs from its reviewed schedule: " + digest)
    if legacy.context_reads(text) or re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2T: decoder escaped the reserved register/context contract")
    count = legacy.instruction_count(text, "X2T")
    if ((count + 1) & ~1) + ((base_count + 1) & ~1) > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2T: combined image exceeds VU1 instruction memory")
    if "[E]" in text:
        legacy.fail("X2T: decoder terminates before entering X2F")
    pairs, labels = copies.code_pairs(text)
    rng = random.Random(0x583254)
    worst = 0.0
    cases = 0
    for _ in range(24):
        for top in (79, 551):
            for n in (0, 4, 8, 20, 40):
                worst = max(worst, model(pairs, labels, top, n, rng))
                cases += 1
    print(f"x2t microcode guard: PASS ({count} instructions after X2F's {base_count}, "
          f"{cases} scheduled cases, alpha within {worst:.2e} of the law, {digest})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--fix-decoder", action="store_true")
    parser.add_argument("program", type=Path)
    parser.add_argument("decoder", type=Path)
    args = parser.parse_args()
    if args.fix_decoder:
        legacy.fix_decoder_tail(args.decoder)
    verify(args.program, args.decoder, args.fix_decoder)
