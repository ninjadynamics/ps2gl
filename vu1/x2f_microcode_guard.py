#!/usr/bin/env python3
"""Reject unsafe generated four-corner source images before linking; never rewrite microcode.

This checks image size, relative branches and basic E/XGKICK structure. It is
not a VU simulator or proof of register, clipping or DMA lifetime correctness.
"""
import re
import sys
from pathlib import Path


def check(text):
    labels = {}
    code = []
    active = False
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if line == "vsmGeneralClipQuadX2F_CodeStart:":
            active = True
        if not active or not line:
            continue
        if line.endswith(":"):
            labels[line[:-1]] = len(code)
            if line == "vsmGeneralClipQuadX2F_CodeEnd:":
                break
        elif not line.startswith("."):
            code.append(line)
    if not code or "vsmGeneralClipQuadX2F_CodeEnd" not in labels:
        raise ValueError("missing four-corner source image boundaries")
    count = re.search(r";\s*iCount=(\d+)", text)
    if not count or int(count.group(1)) != len(code):
        raise ValueError("instruction count does not match emitted pairs")
    padded = (len(code) + 1) & ~1
    if padded > 2048:
        raise ValueError(f"{padded} padded pairs exceed VU1 instruction memory")
    branch = re.compile(r"\b(b|bal|ibeq|ibne|ibgez|ibgtz|iblez|ibltz)\s+([^;]+)$", re.I)
    branches = 0
    stops = 0
    kicks = 0
    for pc, instruction in enumerate(code):
        match = branch.search(instruction)
        if match:
            target = match.group(2).split(",")[-1].strip()
            if target not in labels:
                raise ValueError(f"PC{pc}: unknown branch target {target}")
            displacement = labels[target] - (pc + 1)
            if not -1024 <= displacement <= 1023:
                raise ValueError(f"PC{pc}: branch to {target} is out of range ({displacement})")
            if pc + 1 >= len(code) or branch.search(code[pc + 1]):
                raise ValueError(f"PC{pc}: missing or branching delay slot")
            branches += 1
        if "[E]" in instruction.upper():
            if pc + 1 >= len(code):
                raise ValueError(f"PC{pc}: missing E delay slot")
            if any(re.search(r"\bxgkick\b", c, re.I) for c in code[pc:pc + 2]):
                raise ValueError(f"PC{pc}: XGKICK overlaps E or its delay")
            stops += 1
        if re.search(r"\bxgkick\b", instruction, re.I):
            # Each compound output kick writes its GIF tag before the block fence.
            # VU manual SQ latency is four issue pairs; the raw useful setup
            # instructions preserve that minimum independently of scheduling.
            stores = [i for i in range(pc) if re.search(r"\bsq(?:\.[xyzw]+)?\s", code[i], re.I)]
            if not stores or pc - stores[-1] < 4:
                raise ValueError(f"PC{pc}: latest SQ is too close to XGKICK")
            kicks += 1
    if stops != 2 or kicks != 3:
        raise ValueError(f"review changed entry/continuation or output structure: E={stops}, XGKICK={kicks}")
    return len(code), padded, branches


def main():
    try:
        count, padded, branches = check(Path(sys.argv[1]).read_text())
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f"x2f microcode guard: FAIL: {error}")
    print(f"x2f microcode guard: PASS ({count} pairs, {padded} padded, {branches} bounded branches)")


if __name__ == "__main__":
    main()
