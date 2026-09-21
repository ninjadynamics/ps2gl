#!/usr/bin/env python3
"""Ordered mixed decal payload, activation and transactional admission model.

Reuses the independent encoded DMA/VIF walker, including real CNT/REF/TOP
rules. Does not emulate EE execution, texture uploads or hardware completion.
"""
import json
import random
from pathlib import Path
from compact_packet_model import Packet, sweep, execute


def check_packets():
    rng = random.Random(0x14E2026)
    cases = activations = 0
    shapes = [[(0, 1)], [(1, 1)], [(0, 33), (1, 12), (0, 49)],
              [(0, 16), (1, 16)] * 32, [(i & 1, 1) for i in range(128)],
              [(i & 1, 1) for i in range(512)]]
    for unused in range(120):
        shapes.append([(rng.randrange(2), rng.randrange(1, 65))
                       for i in range(rng.randrange(1, 33))])
    for shape in shapes:
        sources = [rng.randbytes(count * 144) for fmt, count in shape]
        expected = [(min(count - i, 16), src[i * 144:min(count, i + 16) * 144], fmt)
                    for (fmt, count), src in zip(shape, sources)
                    for i in range(0, count, 16)]
        for glow in (False, True):
            for reuse in (False, True):
                for compact in (False, True):
                    packet = Packet()
                    owned = []
                    for (fmt, count), source in zip(shape, sources):
                        owned.append(sweep(packet, source, compact, False, format=fmt))
                    # Arbitrary qword-aligned state packets do not affect owned
                    # offsets. No source-array pointer survives in a REF.
                    packet.tag(1)
                    packet.pad(0)
                    packet.memory += bytes(16 * rng.randrange(20))
                    packet.close()
                    base_end = len(packet.memory)
                    if glow:
                        for (fmt, count), source, payload in zip(shape, sources, owned):
                            sweep(packet, source, compact, reuse,
                                  payload if reuse else None, format=fmt)
                    observed = execute(packet, capture_format=True)
                    assert observed == expected * (2 if glow else 1)
                    assert all(p < base_end for group in owned for p in group)
                    records = sum(count for fmt, count in shape)
                    batches = sum((count + 15) // 16 for fmt, count in shape)
                    bound = ((2 if glow else 1) * (records * 9 + batches * 16 + 512) + 16) * 16
                    assert len(packet.memory) <= bound
                    cases += 1
                    activations += len(observed)
    return cases, activations


def check_admission():
    # Independent interval model for the cached/uncached EE alias boundary.
    write_begin, write_end = 0x01200000, 0x01210000
    def admitted(pointer, length):
        if pointer & 3 or length <= 0 or pointer + length > 0xFFFFFFFF:
            return False
        normal = pointer & 0x1FFFFFFF
        return normal + length <= write_begin or normal >= write_end
    count = 0
    for mapping in (0, 0x20000000, 0x80000000, 0xA0000000):
        for start, length, valid in (
            (write_begin - 144, 144, True), (write_begin - 144, 145, False),
            (write_begin, 144, False), (write_end - 4, 144, False),
            (write_end, 144, True), (write_end + 2, 144, False)):
            assert admitted(start | mapping, length) == valid
            count += 1
    root = Path(__file__).resolve().parents[1]
    manager = (root / 'src/immgmanager.cpp').read_text().split(
        'bool CImmGeomManager::DrawDecalRuns(', 1)[1].split('\nGLboolean pglDrawDecalRuns', 1)[0]
    preflight, publish = manager.split('for (unsigned int pass = 0;', 1)
    assert 'runCount > 512' in preflight
    assert 'CanReserveWords' in preflight
    assert 'Core::MakePtrNormal(runs[i].records)' in preflight
    assert 'Core::MakePtrNormal(runs)' in preflight
    assert 'Core::MakePtrNormal(context)' in preflight
    assert 'run.format != PGL_DECAL_RUN_NDC_TRIANGLES' in preflight
    assert '!PGL_DECAL_PROJECTED_RUNS' in preflight
    assert 'return false' not in publish.split('#else\n    (void)context;', 1)[0]
    assert publish.index('SyncGsContext();') < publish.index('for (int i = 0; i < runCount;')
    assert 'ownedPayloads + batchOffset' in publish
    assert 'batchOffset += ((unsigned int)run.count + 15u) / 16u;' in publish
    writer = (root / 'src/x2e_renderer.cpp').read_text().split(
        'void CClipDecalX2ERenderer::DrawDecalRecords(', 1)[1].split('\nvoid CClipDecal', 1)[0]
    assert 'packet += format | (materials ? 2u : 0u);' in writer
    assert 'source += batch * 36;' in writer
    assert 'Core::MakePtrNormal(ownedPayloads[batchIndex])' in writer
    return count, 14


if __name__ == '__main__':
    cases, activations = check_packets()
    aliases, contracts = check_admission()
    print(json.dumps({'encoded_mixed_sweeps': cases, 'activations': activations,
                      'source_alias_boundaries': aliases, 'source_contracts': contracts,
                      'ordered_full_base_then_glow_and_owned_refs': 'pass',
                      'native_EE_and_hardware': 'pending'}, indent=2))
