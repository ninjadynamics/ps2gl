#!/usr/bin/env python3
"""Validate X2A image structure using the shared ground output contract."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from x2r_microcode_guard import check as check_ground_structure


def check(text):
    symbol = "vsmGeneralClipBillboardX2A"
    for suffix in ("_CodeStart:", "_CodeEnd:"):
        if text.count(symbol + suffix) != 1:
            raise ValueError("missing or duplicate billboard alpha image boundary")
    return check_ground_structure(text.replace(symbol, "vsmGeneralClipRoadX2R"))


def main():
    try:
        count, padded, branches = check(Path(sys.argv[1]).read_text())
    except (ValueError, OSError, IndexError) as error:
        raise SystemExit(f"x2a microcode guard: FAIL: {error}")
    print(f"x2a microcode guard: PASS ({count} pairs, {padded} padded, "
          f"{branches} bounded branches)")


if __name__ == "__main__":
    main()
