#!/usr/bin/env python3
"""Verify the compact color decoder, keeping the reviewed X2 body unchanged."""
import argparse
import hashlib
import random
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
import x2d_microcode_guard as legacy
import x2q_microcode_guard as copies

REVIEWED_SHA256 = "e4121d5eeb0b84864bc9e3a3225ef0e4a19f126a71fb96eab2cfbe2866be20a6"


def verify(base, old, decoder, generated=False):
    legacy.verify(base, old)
    data = decoder.read_bytes()
    text = data.decode("utf-8")
    digest = hashlib.sha256(data).hexdigest()
    if not generated and digest != REVIEWED_SHA256:
        legacy.fail("X2C: image differs from scheduled-copy review: " + digest)
    if legacy.context_reads(text) or re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", text):
        legacy.fail("X2C: decoder escaped the reserved register/context contract")
    count = legacy.instruction_count(text, "X2C")
    if ((count + 1) & ~1) + legacy.X2_INSTRUCTIONS > legacy.VU1_MICRO_CAPACITY:
        legacy.fail("X2C: combined image exceeds VU1 instruction memory")
    if "[E]" in text:
        legacy.fail("X2C: decoder terminates before entering X2")
    pairs, labels = copies.code_pairs(text)
    rng = random.Random(0x583243)
    for _ in range(64):
        for top in (79, 551):
            for n in range(5):
                copies.model(pairs, labels, top, n, rng, compact=True)
    print(f"x2c microcode guard: PASS ({count} instructions, 640 scheduled-copy cases, {digest})")


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
