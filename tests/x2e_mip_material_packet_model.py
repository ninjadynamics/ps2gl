#!/usr/bin/env python3
"""X2E material metadata lifetime through encoded DMA/VIF and owned replay.

Independent stream walker verifies the header seen by each alternating TOP,
not merely the constructor's inputs. GS output/arena checks live with the VU
model. Neither model proves EE compilation or on-hardware performance.
"""
import random
from pathlib import Path
from compact_packet_model import Packet, sweep, execute


def packet_cases():
    rng = random.Random(0xE321)
    cases = records_seen = 0
    shapes = [[(0, n)] for n in (1, 7, 8, 9, 15, 16, 17, 32, 512)]
    shapes += [[(1, 768)], [(0, 17), (1, 13), (0, 77)], [(i % 2, 1) for i in range(512)]]
    shapes += [[(rng.randrange(2), rng.randrange(1, 45)) for _ in range(rng.randrange(1, 16))]
               for _ in range(40)]
    for shape in shapes:
        sources = [rng.randbytes(n * 144) for _, n in shape]
        cells = [bytes(rng.randrange(4) for _ in range(n)) for _, n in shape]
        for compact in (False, True):
            for reuse in (False, True):
                for mip_passes in ((True, True), (True, False), (False, True)):
                    packet, owned, expected = Packet(), [], []
                    base_end = 0
                    for material_pass in range(2):
                        packet.tag(1)
                        packet.pad(0)
                        packet.memory += bytes(16 * rng.randrange(1, 32))
                        packet.close()
                        for i, ((fmt, n), source, material) in enumerate(zip(shape, sources, cells)):
                            use_cells = material if mip_passes[material_pass] else None
                            addresses = sweep(packet, source, compact, bool(material_pass and reuse),
                                              owned[i] if material_pass and reuse else None,
                                              format=fmt, materials=use_cells)
                            if material_pass == 0:
                                owned.append(addresses)
                            for at in range(0, n, 16):
                                count = min(n - at, 16)
                                expected.append((source[at * 144:(at + count) * 144], fmt,
                                                 material[at:at + count] if use_cells is not None else None))
                        if material_pass == 0:
                            base_end = len(packet.memory)
                    observed = execute(packet, capture_header=True)
                    assert len(observed) == len(expected)
                    for (count, source, header), (original, fmt, material) in zip(observed, expected):
                        assert source == original
                        assert header[0] == count and (header[1] & 1) == fmt
                        assert bool(header[1] & 2) == (material is not None)
                        assert header[2] <= 65535 and header[3] <= 65535
                        decoded = bytes((header[2 + i // 8] >> (2 * (i % 8))) & 3
                                        for i in range(count))
                        assert decoded == (material if material is not None else bytes(count))
                        records_seen += count
                    assert all(address < base_end for run in owned for address in run)
                    records = sum(n for _, n in shape)
                    batches = sum((n + 15) // 16 for _, n in shape)
                    assert len(packet.memory) <= (2 * (records * 9 + batches * 16 + 512) + 16) * 16
                    cases += 1
    return cases, records_seen


def admission_cases():
    # Byte streams may be unaligned. Pointer table is word-aligned. Cached
    # and uncached aliases must both reject overlap with future packet bytes.
    begin, end = 0x1200000, 0x1210000
    cases = 0
    for mapping in (0, 0x20000000, 0x80000000, 0xA0000000):
        for length in (1, 8, 16, 768, 512 * 4):
            for ptr, expected in ((begin - length, True), (begin - length + 1, False),
                                  (begin, False), (end - 1, False), (end, True), (end + 1, True)):
                normal = (ptr | mapping) & 0x1fffffff
                assert (normal + length <= begin or normal >= end) == expected
                cases += 1
    root = Path(__file__).resolve().parents[1]
    manager = (root / 'src/immgmanager.cpp').read_text().split(
        'bool CImmGeomManager::DrawDecalRunsSampling(', 1)[1].split('\nGLboolean ', 1)[0]
    before, publish = manager.split('for (unsigned int pass = 0;', 1)
    for invariant in ('materials[i][j] > 3u', 'Core::MakePtrNormal(materials)',
                      'Core::MakePtrNormal(materials[i])', 'materialEnd > writeBegin',
                      'pgl_texture_has_mips32', 'GetContext() != GS::kContext1',
                      'CanReserveWords', '(regions ? regionPasses : 0u)'):
        assert invariant in before, invariant
    assert 'return false' not in publish.split('#else', 1)[0]
    assert 'if (i && regions && regionTextures[pass]' in publish
    assert 'materials && regionTextures[pass] ? materials[i] : NULL' in publish
    assert 'if (materials && regionTextures[pass]) GLContext.TextureChanged();' in publish
    writer = (root / 'src/x2e_renderer.cpp').read_text().split(
        'void CClipDecalX2ERenderer::DrawDecalRecords(', 1)[1].split('\nvoid CClipDecal', 1)[0]
    for invariant in ('format | (materials ? 2u : 0u)',
                      'materials[i] << (i * 2)', 'materials[i] << ((i - 8) * 2)',
                      'if (materials) materials += batch;',
                      'PGL_DECAL_HEADER_COMPACT ? 1u : 5u'):
        assert invariant in writer, invariant
    return cases


if __name__ == '__main__':
    cases, records = packet_cases()
    print(f'PASS: {cases} encoded material sweeps, {records} record/header checks, '
          f'{admission_cases()} metadata alias boundaries. Hardware pending.')
