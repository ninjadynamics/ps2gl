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


X2_MAIN_PC = 6
VU1_MICRO_CAPACITY = 2048
# The X2-only header selects independent reviewed experiments. Hash canonical LF
# text without trailing whitespace so regeneration and Windows checkout agree.
_header = Path(__file__).with_name('x2_window_copy_gate.h').read_text()


def read_gate(name: str) -> int:
    match = re.search(r'^#define ' + name + r' ([01])$', _header, re.M)
    if not match:
        raise SystemExit('x2d microcode guard: missing or invalid ' + name)
    return int(match.group(1))


X2_WINDOW_COPY_TRIANGLES = read_gate('PGL_X2_WINDOW_COPY_TRIANGLES')
X2_SKIP_IDENTITY_NEAR = read_gate('PGL_X2_SKIP_IDENTITY_NEAR')
_reviewed_images = {
    (0, 0): (806, 'ce3c1209d0943428ed23964c5263cef33ca5b5b51148de02764715a83052f4fb'),
    (1, 0): (836, 'b8c515204d661ca638cecbac5d64c69c93ad528366391c857206fde15632e0c5'),
    (0, 1): (828, 'ba5565ef6ed5a542e42375310e57b7526c5ae66e90e2abee5e28d5247353c2a3'),
}
_selection = (X2_WINDOW_COPY_TRIANGLES, X2_SKIP_IDENTITY_NEAR)
if _selection not in _reviewed_images:
    raise SystemExit('x2d microcode guard: unreviewed X2 gate combination '
                     + str(_selection))
X2_INSTRUCTIONS, X2_VSM_SHA256 = _reviewed_images[_selection]
X2D_DECODER_VSM_SHA256 = "3ba063fc6baa758450aec971c0b57a44e3df8d216c4837280c036bbc9743a6ad"

# CClipTriX2Renderer::InitContext uploads these absolute context locations.
# Its C++ offset checks retain the full ABI, including the untouched holes and
# guard q78. Changing the generated read set requires reviewing that EE writer.
X2_CONTEXT_READS = {0, 57, 62, 63, 64, 65, 75, 76, 77}


def fail(message: str) -> None:
    raise SystemExit(f"x2d microcode guard: {message}")


def instruction_count(text: str, name: str) -> int:
    match = re.search(r";\s*iCount=(\d+)", text)
    if not match:
        fail(f"{name}: missing iCount")
    return int(match.group(1))


def context_reads(text: str) -> set[int]:
    # The committed X2 image addresses shared context through VI00. Buffer
    # references use XTOP-derived registers and remain guarded by its hash.
    code = "\n".join(line.split(";", 1)[0] for line in text.splitlines())
    return {
        int(address, 0)
        for address in re.findall(
            r"\b(?:lq|ilw)(?:\.[xyzw]+)?\s+\w+,\s*(0x[0-9a-f]+|\d+)\(VI00\)",
            code, re.IGNORECASE,
        )
    }


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


def verify_schedule(text: str) -> int:
    code, labels = [], {}
    active = False
    for raw in text.splitlines():
        line = raw.split(';', 1)[0].strip()
        if line == 'vsmGeneralClipTriX2_CodeStart:':
            active = True
        if not active or not line:
            continue
        if line.endswith(':'):
            labels[line[:-1]] = len(code)
            if line == 'vsmGeneralClipTriX2_CodeEnd:':
                break
        elif not line.startswith('.'):
            code.append(line)
    if len(code) != X2_INSTRUCTIONS:
        fail('X2 emitted instruction count differs from the reviewed image')
    branch = re.compile(r'\b(b|bal|ibeq|ibne|ibgez|ibgtz|iblez|ibltz)\s+([^;]+)$', re.I)
    branches = stops = kicks = 0
    for pc, instruction in enumerate(code):
        match = branch.search(instruction)
        if match:
            target = match.group(2).split(',')[-1].strip()
            if target not in labels:
                fail(f'X2 PC{pc}: unknown branch target {target}')
            displacement = labels[target] - (pc + 1)
            if not -1024 <= displacement <= 1023:
                fail(f'X2 PC{pc}: branch displacement {displacement} is out of range')
            if pc + 1 >= len(code) or branch.search(code[pc + 1]):
                fail(f'X2 PC{pc}: missing or branching delay slot')
            branches += 1
        if '[E]' in instruction.upper():
            if pc + 1 >= len(code) or any(re.search(r'\bxgkick\b', c, re.I)
                    for c in code[pc:pc + 2]):
                fail(f'X2 PC{pc}: invalid E delay or overlapping XGKICK')
            stops += 1
        if re.search(r'\bxgkick\b', instruction, re.I):
            stores = [i for i in range(pc)
                if re.search(r'\bsq(?:\.[xyzw]+)?\s', code[i], re.I)]
            if not stores or pc - stores[-1] < 4:
                fail(f'X2 PC{pc}: latest SQ is too close to XGKICK')
            kicks += 1
    if stops != 2 or kicks != 3:
        fail(f'X2 entry/continuation or output structure changed: E={stops}, XGKICK={kicks}')
    return branches


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

    canonical_x2 = '\n'.join(line.rstrip() for line in x2.splitlines()) + '\n'
    digest = hashlib.sha256(canonical_x2.encode('utf-8')).hexdigest()
    if digest != X2_VSM_SHA256:
        fail(
            "X2 VSM is no longer the guarded generated image "
            f"(sha256 {digest}, expected {X2_VSM_SHA256})"
        )
    decoder_digest = hashlib.sha256(decoder_bytes).hexdigest()
    if decoder_digest != X2D_DECODER_VSM_SHA256:
        fail(
            "decoder VSM changed since its generated store schedule was audited "
            f"(sha256 {decoder_digest}, expected {X2D_DECODER_VSM_SHA256})"
        )

    reads = context_reads(x2)
    if reads != X2_CONTEXT_READS:
        fail(f"X2 sparse EE context read set changed: {sorted(reads)}")
    if context_reads(decoder):
        fail("decoder now reads shared context; review the sparse EE writer")

    x2_count = instruction_count(x2, "X2")
    decoder_count = instruction_count(decoder, "decoder")
    if x2_count != X2_INSTRUCTIONS:
        fail(f"X2 is {x2_count} instructions, expected {X2_INSTRUCTIONS}")
    if label_pc(x2, "main_loop_lid") != X2_MAIN_PC:
        fail(f"X2 main_loop_lid moved from PC {X2_MAIN_PC}")
    branches = verify_schedule(x2)
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
        f"@ PC{x2_upload_count}, tail -> PC{X2_MAIN_PC}, "
        f"copy3={X2_WINDOW_COPY_TRIANGLES}, near-skip={X2_SKIP_IDENTITY_NEAR}, "
        f"{branches} bounded branches)"
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
