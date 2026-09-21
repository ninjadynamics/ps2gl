#!/usr/bin/env python3
"""Reject unsafe generated decal images before linking; never rewrite microcode.

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
        if line == "vsmGeneralClipDecalX2E_CodeStart:":
            active = True
        if not active or not line:
            continue
        if line.endswith(":"):
            labels[line[:-1]] = len(code)
            if line == "vsmGeneralClipDecalX2E_CodeEnd:":
                break
        elif not line.startswith("."):
            code.append(line)
    if not code or "vsmGeneralClipDecalX2E_CodeEnd" not in labels:
        raise ValueError("missing decal image boundaries")
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
            # This module writes its GIF tag in the same straight-line block.
            # VU manual SQ latency is four issue pairs; the raw useful setup
            # instructions preserve that minimum independently of scheduling.
            stores = [i for i in range(pc) if re.search(r"\bsq(?:\.[xyzw]+)?\s", code[i], re.I)]
            if not stores or pc - stores[-1] < 4:
                raise ValueError(f"PC{pc}: latest SQ is too close to XGKICK")
            # The optional prefix immediately precedes this kick's vertex
            # tag. Other polygon writers also use negative SQ offsets.
            bases = []
            for store_pc, offset in zip(stores[-3:], (-2, -1, 0)):
                store = re.search(r'\bsq\s+VF\d+,' + str(offset) + r'\((VI\d+)\)',
                                  code[store_pc], re.I)
                if not store:
                    raise ValueError(f"PC{pc}: missing bounded regional prefix/tag stores")
                bases.append(store.group(1))
            if len(bases) != 3 or len(set(bases)) != 1:
                raise ValueError(f"PC{pc}: regional prefix and vertex tag bases differ")
            kicks += 1
    if stops != 2 or kicks != 3:
        raise ValueError(f"review changed entry/continuation or output structure: E={stops}, XGKICK={kicks}")
    # The third kick is the optional material-boundary path. All three use
    # the same bounded prefix/geometry writer, ahead of the unchanged E exit.
    for prefix in ('e_output_begin_lid', 'e_output_new_material_lid',
                   'e_descriptor_material_store_lid'):
        if prefix not in labels:
            raise ValueError(f"missing material control boundary {prefix}")
    return len(code), padded, branches


def main():
    try:
        count, padded, branches = check(Path(sys.argv[1]).read_text())
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f"x2e microcode guard: FAIL: {error}")
    print(f"x2e microcode guard: PASS ({count} pairs, {padded} padded, {branches} bounded branches)")


if __name__ == "__main__":
    main()
