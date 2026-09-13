#!/usr/bin/env python3
"""Check X2P image structure using the shared X2R packet/continuation contract.

Only the boundary symbol is adapted in memory for the established validator;
no source, instruction, register or generated file is rewritten. Numerical and
GS equivalence require separate source/generated/hardware review.
"""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from x2r_microcode_guard import check as check_ground_structure


def check(text):
    symbol = "vsmGeneralClipPoolX2P"
    for suffix in ("_CodeStart:", "_CodeEnd:"):
        if text.count(symbol + suffix) != 1:
            raise ValueError("missing or duplicate pool image boundary")
    return check_ground_structure(text.replace(symbol, "vsmGeneralClipRoadX2R"))


def main():
    try:
        count, padded, branches = check(Path(sys.argv[1]).read_text())
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f"x2p microcode guard: FAIL: {error}")
    print(f"x2p microcode guard: PASS ({count} pairs, {padded} padded, "
          f"{branches} bounded branches)")


if __name__ == "__main__":
    main()
