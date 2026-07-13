#!/usr/bin/env python3
"""Fix and verify the X2D decoder tail without ever rewriting X2.

VCL 1.4beta7 insists on an E-bit exit and cannot express this decoder's
external absolute tail call.  It does, however, allocate/schedule the decoder
correctly and leaves a two-instruction exit epilogue.  --fix replaces only the
E-bit instruction with JR to the register VCL just loaded with X2 PC 6.  The
normal build runs the same structural checks without modifying either file.
"""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


X2_INSTRUCTIONS = 690
X2_MAIN_PC = 6
VU1_MICRO_CAPACITY = 2048
X2_VSM_SHA256 = "ce499598c5453b99e118cbc86bbb7303e91bd5190b8e7b3e72db113c10e73fd6"
X2D_DECODER_VSM_SHA256 = "3ba063fc6baa758450aec971c0b57a44e3df8d216c4837280c036bbc9743a6ad"


def fail(message: str) -> None:
    raise SystemExit(f"x2d microcode guard: {message}")


def instruction_count(text: str, name: str) -> int:
    match = re.search(r";\s*iCount=(\d+)", text)
    if not match:
        fail(f"{name}: missing iCount")
    return int(match.group(1))


def label_pc(text: str, label: str) -> int:
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.endswith("_CodeStart:"))
        target = lines.index(label + ":", start + 1)
    except (StopIteration, ValueError):
        fail(f"missing label {label}")

    pc = 0
    for line in lines[start + 1 : target]:
        stripped = line.strip()
        if not stripped or stripped.startswith((";", ".")) or stripped.endswith(":"):
            continue
        pc += 1
    return pc


def fix_decoder_tail(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if re.search(r"\bjr\s+VI\d+", text, re.IGNORECASE):
        return

    # With an out_vi binding, VCL commonly pairs E with the target load:
    #   NOP[E]  iaddiu VI15,VI00,6
    #   NOP     NOP
    # Remove E, turn the existing delay pair into JR, and append JR's delay.
    paired = re.search(
        r"^([ \t]*)NOP\[E\]([ \t]+)iaddiu[ \t]+(VI\d+),VI00,0x0*6[ \t]*$",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if paired:
        jump_reg = paired.group(3).upper()
        load_line = (
            f"{paired.group(1)}NOP{paired.group(2)}"
            f"iaddiu        {jump_reg},VI00,0x00000006"
        )
        text = text[: paired.start()] + load_line + text[paired.end() :]

        after_load = paired.start() + len(load_line)
        delay = re.search(
            r"^[ \t]*NOP[ \t]+NOP[ \t]*$",
            text[after_load:],
            re.IGNORECASE | re.MULTILINE,
        )
        if not delay:
            fail("decoder: missing VCL exit delay after the PC-6 load")
        delay_start = after_load + delay.start()
        delay_end = after_load + delay.end()
        jr_and_delay = (
            f"         NOP                                                        jr            {jump_reg}\n"
            "         NOP                                                        NOP"
        )
        text = text[:delay_start] + jr_and_delay + text[delay_end:]

        count = instruction_count(text, "decoder")
        text = re.sub(r"(;\s*iCount=)\d+", rf"\g<1>{count + 1}", text, count=1)
        path.write_text(text, encoding="utf-8", newline="\n")
        return

    # Older VCL layouts put the target load one instruction before E.
    target = re.search(
        r"^[ \t]*NOP[ \t]+iaddiu[ \t]+(VI\d+),VI00,0x0*6[ \t]*$",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if not target:
        fail("decoder: missing VCL-allocated PC-6 register load")
    jump_reg = target.group(1).upper()

    exit_inst = re.compile(
        r"^[ \t]*NOP\[E\][ \t]+nop[ \t]*$", re.IGNORECASE | re.MULTILINE
    )
    matches = list(exit_inst.finditer(text))
    if len(matches) != 1 or matches[0].start() < target.end():
        fail("decoder: expected one E epilogue after the PC-6 load")

    jr_line = f"         NOP                                                        jr            {jump_reg}"
    text = exit_inst.sub(jr_line, text, count=1)
    path.write_text(text, encoding="utf-8", newline="\n")


def verify(x2_path: Path, decoder_path: Path) -> None:
    x2_bytes = x2_path.read_bytes()
    x2 = x2_bytes.decode("utf-8")
    decoder_bytes = decoder_path.read_bytes()
    decoder = decoder_bytes.decode("utf-8")

    digest = hashlib.sha256(x2_bytes).hexdigest()
    if digest != X2_VSM_SHA256:
        fail(
            "X2 VSM is no longer the hardware-green image "
            f"(sha256 {digest}, expected {X2_VSM_SHA256})"
        )
    decoder_digest = hashlib.sha256(decoder_bytes).hexdigest()
    if decoder_digest != X2D_DECODER_VSM_SHA256:
        fail(
            "decoder VSM changed since its generated store schedule was audited "
            f"(sha256 {decoder_digest}, expected {X2D_DECODER_VSM_SHA256})"
        )

    x2_count = instruction_count(x2, "X2")
    decoder_count = instruction_count(decoder, "decoder")
    if x2_count != X2_INSTRUCTIONS:
        fail(f"X2 is {x2_count} instructions, expected {X2_INSTRUCTIONS}")
    if label_pc(x2, "main_loop_lid") != X2_MAIN_PC:
        fail(f"X2 main_loop_lid moved from PC {X2_MAIN_PC}")
    # CodeEnd follows `.align 4`, so an odd logical instruction count uploads
    # one assembler padding instruction as part of the symbol range.
    x2_upload_count = (x2_count + 1) & ~1
    decoder_upload_count = (decoder_count + 1) & ~1
    if x2_upload_count + decoder_upload_count > VU1_MICRO_CAPACITY:
        fail(
            f"combined upload is {x2_upload_count + decoder_upload_count} instructions, "
            f"VU1 holds {VU1_MICRO_CAPACITY}"
        )

    if re.search(r"\bVF(?:2[6-9]|3[01])\b", x2):
        fail("X2 now uses VF26..VF31 reserved for the decoder")
    if re.search(r"\bVF(?:0[1-9]|1\d|2[0-5])\b", decoder):
        fail("decoder escaped its VF26..VF31 register partition")

    target = re.search(
        r"^[ \t]*NOP[ \t]+iaddiu[ \t]+(VI\d+),VI00,0x0*6[ \t]*$",
        decoder,
        re.IGNORECASE | re.MULTILINE,
    )
    jump = re.search(r"\bjr\s+(VI\d+)\b", decoder, re.IGNORECASE)
    if not target or not jump or target.group(1).upper() != jump.group(1).upper():
        fail("decoder does not load and JR through the same PC-6 register")
    if "[E]" in decoder[: jump.start()]:
        fail("decoder E-stops before its X2 tail call")

    print(
        "x2d microcode guard: PASS "
        f"(X2 {x2_upload_count} insn @ PC0, decoder {decoder_upload_count} insn "
        f"@ PC{x2_upload_count}, tail -> PC{X2_MAIN_PC})"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fix-decoder", action="store_true")
    parser.add_argument("x2", type=Path)
    parser.add_argument("decoder", type=Path)
    args = parser.parse_args()

    if args.fix_decoder:
        fix_decoder_tail(args.decoder)
    verify(args.x2, args.decoder)


if __name__ == "__main__":
    main()
