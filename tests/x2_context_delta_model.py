#!/usr/bin/env python3
"""Replay encoded VIF context writes and audit the opted-in VU store contract.

This model checks exact consumed bytes, barrier/restart ordering, stale-owner
rejection and the bounded VU scratch arenas. It does not execute EE code or
claim a hardware timing improvement. The retained-memory argument also relies
on the existing reviewed generated-program hashes, checked below.
"""
import hashlib
import random
import re
import struct
import sys
from pathlib import Path

from compact_packet_model import Packet, U32

ROOT = Path(__file__).resolve().parents[1]
RNG = random.Random(0x58324354)
RANGES = ((0, 0, 1), (57, 1, 1), (62, 2, 4), (75, 6, 4))
RESTART = (0x14000000, 0x10000000, 0x0300004F, 0x020001D8)


def emit(inputs, previous=None, retained=False):
    packet = Packet()
    packet.tag(1)
    packet.word(0x01000101)  # STCYCL(1,1)
    packet.word(0x11000000)  # FLUSH, including outstanding PATH1 geometry
    for offset, first, count in RANGES:
        source = inputs[first * 16:(first + count) * 16]
        if not retained or source != previous[first * 16:(first + count) * 16]:
            packet.pad(12)
            packet.word(0x6C000000 | (count << 16) | offset)
            packet.memory += source
    for command in RESTART:
        packet.word(command)
    packet.close()
    return packet.memory


def replay(data, memory):
    # Parse the real CNT/TTE representation, independently of emit's ranges.
    control, address = struct.unpack_from('<2I', data)
    assert control >> 28 == 1 and address == 0
    assert len(data) == 16 * ((control & 65535) + 1)
    stream = data[8:]
    cursor = 0
    commands = []
    writes = []
    while cursor < len(stream):
        command = U32.unpack_from(stream, cursor)[0]
        cursor += 4
        if command == 0:
            continue
        if command >> 24 == 0x6C:
            assert command & 0x8000 == 0  # Absolute context, never TOP-relative.
            count = ((command >> 16) & 255) or 256
            offset = command & 1023
            size = count * 16
            assert offset + count <= 79
            memory[offset * 16:offset * 16 + size] = stream[cursor:cursor + size]
            writes.append((offset, count))
            cursor += size
        else:
            commands.append(command)
    assert cursor == len(stream)
    assert commands == [0x01000101, 0x11000000, *RESTART]
    return writes


def proof_matches(valid, dlist, normal, old, current):
    # Tuple: packet identity, packet base, post-draw cursor, frame, renderer.
    return valid and not dlist and normal and old == current


def packet_cases():
    cases = 0
    previous = RNG.randbytes(160)
    for byte in range(160):
        for bit in range(8):
            changed = bytearray(previous)
            changed[byte] ^= 1 << bit
            initial = bytearray(RNG.randbytes(1024 * 16))
            replay(emit(previous), initial)
            expected = initial.copy()
            candidate = initial.copy()
            replay(emit(changed), expected)
            sent = replay(emit(changed, previous, True), candidate)
            assert expected == candidate
            assert len(sent) == 1
            cases += 1
    # Entire unchanged context still carries the exact original activation and
    # completion sequence. Every byte outside its written ranges is untouched.
    unchanged = bytearray(RNG.randbytes(1024 * 16))
    replay(emit(previous), unchanged)
    original = unchanged.copy()
    assert replay(emit(previous, previous, True), unchanged) == []
    assert unchanged == original
    assert len(emit(previous)) == 256
    assert len(emit(previous, previous, True)) == 32
    # Stale packet/end/frame/renderer and every other admission rejection must
    # overwrite poisoned context in full, even when CPU input bytes match.
    owner = (11, 0x10000, 0x10300, 63, 2)
    rejected = [(False, False, True, owner), (True, True, True, owner),
                (True, False, False, owner)]
    for field in range(len(owner)):
        changed = list(owner)
        changed[field] += 1
        rejected.append((True, False, True, tuple(changed)))
    assert proof_matches(True, False, True, owner, owner)
    for valid, dlist, normal, current in rejected:
        assert not proof_matches(valid, dlist, normal, owner, current)
        expected = bytearray(RNG.randbytes(1024 * 16))
        candidate = expected.copy()
        replay(emit(previous), expected)
        sent = replay(emit(previous, previous, False), candidate)
        assert expected == candidate and len(sent) == 4
        cases += 1
    return cases + 1


def store_contracts():
    # Reuse the standing reviewed-image hashes, so a VU change requires a new
    # explicit proof rather than passing an absence-of-absolute-stores regex.
    sys.path.insert(0, str(ROOT / 'vu1'))
    import x2d_microcode_guard as legacy
    import x2q_microcode_guard as quad
    import x2c_microcode_guard as compact
    files = (
        ('general_clip_tri_x2_vcl.vsm', legacy.X2_VSM_SHA256),
        ('general_clip_tri_x2d_decode_vcl.vsm', legacy.X2D_DECODER_VSM_SHA256),
        ('general_clip_tri_x2q_decode_vcl.vsm', quad.X2Q_SHA256),
        ('general_clip_tri_x2c_decode_vcl.vsm', compact.REVIEWED_SHA256),
    )
    for name, digest in files:
        data = (ROOT / 'vu1' / name).read_bytes()
        assert hashlib.sha256(data).hexdigest() == digest, name
        code = '\n'.join(line.split(';')[0] for line in data.decode().splitlines())
        assert not re.search(r'\b(?:sq|isw)(?:\.[xyzw]+)?\s+\w+,\s*[^\n]*\(VI00\)',
                             code, re.I), name
    # Every audited decoder writes 24*4q beginning at TOP+5. X2's plane,
    # polygon and output stores have these bounded relative address ranges.
    # Windows duplicate only committed vertices into the same output arena;
    # the smaller arena holds one maximum clipped fan (18 vertices).
    checks = 0
    for top in (79, 79 + 472):
        spans = [(5, 100), (125, 129), (130, 153), (154, 177)]
        for tag, capacity in ((180, 30), (362, 18)):
            for count in range(0, capacity + 1, 3):
                # One tag + N*3 wall qwords + one tag + N*3 window qwords.
                spans.append((tag, tag + 1 + 6 * count))
        for first, last in spans:
            assert 79 <= top + first <= top + last <= 1022
            assert last < 472
            checks += 1
    # The PC0 prologue is six instructions, all context reads or register math.
    text = (ROOT / 'vu1/general_clip_tri_x2_vcl.vsm').read_text()
    assert legacy.context_reads(text) == {0, 57, 62, 63, 64, 65, 75, 76, 77}
    assert legacy.label_pc(text, 'main_loop_lid') == 6
    return checks + len(files) + 2


def source_contracts():
    code = (ROOT / 'src/clip_renderer.cpp').read_text()
    context = code.split('void CClipTriX2Renderer::InitRetainedContext()', 1)[1]
    context = context.split('void CClipTriX2Renderer::RememberContextEnd()', 1)[0]
    for guard in ('ContextInputsValid', '!pGLContext->InDListDef()',
                  'pGLContext->UsesNormalFramePacket()', 'ContextPacket == &packet',
                  'ContextPacketBase == packet.GetBase()',
                  'ContextPacketEnd == packet.GetNextPtr()',
                  'ContextFrame == pGLContext->GetFrameNumber()',
                  'GetCurRenderer() == this'):
        assert guard in context, guard
    assert 'pglInvalidateUnlitContextDelta();' in context
    assert 'ContextPacketEnd = NULL;' in context
    for name in ('CClipTriX2Renderer', 'CClipTriX2DRenderer'):
        load = code.split(f'void {name}::Load()', 1)[1].split('\n}', 1)[0]
        assert 'ContextInputsValid = false;' in load
    assert code.count('RememberContextEnd();') == 2
    # The generic protected constructor defaults off. Q/C explicitly opt in;
    # G has a different absolute context ABI and stays excluded.
    assert 'ContextDeltaEligible(false)' in code
    for name in ('x2q_renderer.cpp', 'x2c_renderer.cpp'):
        assert 'ContextDeltaEligible = true;' in (ROOT / 'src' / name).read_text()
    glow = (ROOT / 'src/x2g_renderer.cpp').read_text()
    assert 'ContextDeltaEligible = true;' not in glow
    assert '#if PGL_X2_CONTEXT_DELTA' not in code
    return 18


if __name__ == '__main__':
    print(f'PASS: {packet_cases()} encoded context/ownership cases, '
          f'{store_contracts()} reviewed VU store/ABI checks, '
          f'{source_contracts()} source contracts. EE build and hardware pending.')
