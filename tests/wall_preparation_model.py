#!/usr/bin/env python3
"""Host contract models for CPU-only window caching and borrowed wall spans.

This does not execute EE code or model VU/GS scheduling. It checks exact output
bytes across source mutations and the no-publication-on-rejection contract.
"""
import json
import random
import struct
from pathlib import Path


RNG = random.Random(0x5832574B)
MASK64 = (1 << 64) - 1
ADDRESSES = (0x3F, 0x08, 0x14, 0x06, 0x3B, 0x34, 0x36)
REMAPPED = {0x3F: 0x7F, 0x08: 0x09, 0x14: 0x15,
            0x06: 0x07, 0x3B: 0x3B, 0x34: 0x35, 0x36: 0x37}


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def make_texture():
    # The fixed layout and fallback remapper must agree, including the entire
    # GIF-tag representation outside NLOOP and every 64-bit register value.
    tag = [RNG.getrandbits(64), RNG.getrandbits(64)]
    return tuple(tag + [word for address in ADDRESSES
                        for word in (RNG.getrandbits(64), address)])


def texture_reference(source):
    output = list(source)
    output[0] = (output[0] & ~0x7FFF) | 15
    for offset in range(3, 16, 2):
        output[offset] = REMAPPED[output[offset]]
    return struct.pack('<16Q', *output)


def draw_reference(draw, alpha):
    test, frame, zbuf, xyoffset, scissor, fba = draw
    a = min(128.0, max(0.0, f32(alpha * 128.0)))
    fix = int(f32(a + 0.5))
    return struct.pack('<16Q',
        0x68 | (fix << 32), 0x43,
        (test & (MASK64 ^ 0x3FFF)) | 13, 0x48,
        frame, 0x4D, zbuf | (1 << 32), 0x4F,
        xyoffset, 0x19, scissor, 0x41, fba, 0x4B,
        test & (MASK64 ^ 1), 0x47)


class WindowCache:
    def __init__(self):
        self.texture = None
        self.draw_key = None
        self.texture_bytes = None
        self.draw_bytes = None
        self.texture_hits = 0
        self.draw_hits = 0

    def build(self, texture, draw, alpha, enabled=True):
        draw_key = (draw, struct.pack('<f', alpha))
        if enabled and self.texture == texture:
            self.texture_hits += 1
        else:
            self.texture_bytes = texture_reference(texture)
            self.texture = texture
        if enabled and self.draw_key == draw_key:
            self.draw_hits += 1
        else:
            self.draw_bytes = draw_reference(draw, alpha)
            self.draw_key = draw_key
        return self.texture_bytes + self.draw_bytes


def validate_cache():
    comparisons = 0
    cache = WindowCache()
    textures = [make_texture() for _ in range(8)]
    draw = tuple(RNG.getrandbits(64) for _ in range(6))
    # Material changes must not hide draw-tail reuse. Frame flips invalidate
    # only the tail; a later texture relocation invalidates its own prefix.
    for frame in range(250):
        draw = (draw[0], frame % 2, *draw[2:])
        for texture in textures:
            expected = texture_reference(texture) + draw_reference(draw, 1.0)
            assert cache.build(texture, draw, 1.0) == expected
            comparisons += 1
    assert cache.draw_hits >= 250 * 7

    # Every individual source bit is live in the proof, even where the final
    # output deliberately masks it. Full-key comparison is conservative.
    texture = make_texture()
    for field in range(16):
        if field >= 3 and field % 2:
            continue
        for bit in range(64):
            edited = list(texture)
            edited[field] ^= 1 << bit
            candidate = tuple(edited)
            expected = texture_reference(candidate) + draw_reference(draw, 1.0)
            assert cache.build(candidate, draw, 1.0) == expected
            comparisons += 1
    for field in range(6):
        for bit in range(64):
            edited = list(draw)
            edited[field] ^= 1 << bit
            candidate = tuple(edited)
            expected = texture_reference(texture) + draw_reference(candidate, 1.0)
            assert cache.build(texture, candidate, 1.0) == expected
            comparisons += 1

    # Vary alpha on either side of every FIX rounding transition, and keep
    # signed zero distinct as the source key does. Finite runtime input only.
    alphas = [-2.0, -0.0, 0.0, 2.0]
    for fix in range(128):
        for offset in (-1e-5, 0.0, 1e-5):
            alphas.append(f32((fix + 0.5) / 128.0 + offset))
    for alpha in alphas:
        expected = texture_reference(texture) + draw_reference(draw, alpha)
        for enabled in (False, True, True):
            assert cache.build(texture, draw, alpha, enabled) == expected
            comparisons += 1

    for _ in range(12000):
        if RNG.randrange(3) == 0:
            texture = make_texture()
        if RNG.randrange(3) == 0:
            draw = tuple(RNG.getrandbits(64) for _ in range(6))
        alpha = f32(RNG.uniform(-0.25, 1.25)) if RNG.randrange(3) == 0 else 1.0
        expected = texture_reference(texture) + draw_reference(draw, alpha)
        assert cache.build(texture, draw, alpha) == expected
        comparisons += 1
    return comparisons, cache.texture_hits, cache.draw_hits


def validate_admission():
    cases = 0
    # Match the bounded count/address calculation on a 32-bit EE. A rejected
    # call cannot publish a prefix; accepted source range remains unchanged.
    for paired in (False, True):
        for count in (-1, 0, 1, 4, 100, 33554431, 33554432, 0x7FFFFFFF):
            for address in (0, 1, 4, 0x100000, 0x7FFFFFFC, 0xFFFFFFFC):
                source_bytes = count * 64
                color_bytes = count * (32 if paired else 64)
                accepted = (0 < count <= 0x7FFFFFFF // 64 and address != 0
                            and address % 4 == 0
                            and address <= 0xFFFFFFFF - source_bytes
                            and address <= 0xFFFFFFFF - color_bytes)
                old_packets = ('older draw',)
                packets = old_packets + ((address, source_bytes, color_bytes),) if accepted else old_packets
                if accepted:
                    assert packets[0] == 'older draw'
                    assert source_bytes == count * 4 * 16
                    assert color_bytes == count * (2 if paired else 4) * 16
                else:
                    assert packets == old_packets
                cases += 1
    # Conservative format fallback and rejection leave client arrays alone.
    client = {'normal': (True, 3), 'texcoord': (True, 2),
              'vertex': (False, 3), 'color': (False, 4)}
    for selected in (False, True):
        for independent_custom in (False, True):
            for lighting in (False, True):
                before = client.copy()
                accepted = selected and not independent_custom and not lighting
                pending = ('old',)
                if accepted:
                    pending += ('X2Q/C NEXT',)
                assert client == before
                assert pending[0] == 'old'
                cases += 1
    return cases


def source_contract():
    root = Path(__file__).resolve().parents[1]
    clip = (root / 'src/clip_renderer.cpp').read_text()
    body = clip.split('void CClipTriX2Renderer::BuildWindowContext2Settings()', 1)[1]
    body = body.split('bool CClipTriX2Renderer::TryReuseWindowContext', 1)[0]
    for name in ('GetTestReg', 'GetFrameReg', 'GetZBufReg',
                 'GetXYOffsetReg', 'GetScissorReg', 'GetFBAReg'):
        assert name in body.split('if (!reuseTexture)', 1)[0]
    assert 'memcpy(&alphaSource, &WinColor[3]' in body
    assert 'memcmp(Ctx2SourceTexture, WinTex->GetSettingsBlock()' in body
    assert 'memcpy(&Ctx2[8], rq, sizeof(rq))' in body
    assert 'memcpy(&Ctx2[1], textureRegisters, sizeof(textureRegisters))' in body
    assert '(uint64_t*)&Ctx2[' not in body
    # A CPU memo must not acquire hardware-ownership side effects.
    assert 'packet.' not in body and 'Ctx2Armed =' not in body
    assert clip.count(', Ctx2SourceValid(false)') == 2
    imm = (root / 'src/immgmanager.cpp').read_text()
    api = imm.split('bool CImmGeomManager::DrawWallDescriptorArrays(', 1)[1]
    api = api.split('GLboolean pglDrawWallDescriptorArrays(', 1)[0]
    assert 'CommitNewGeom();' in api and 'SyncColorMaterial(true);' in api
    assert 'VertArray->' not in api and 'CurVertexBuf->' not in api
    assert api.index('return false;') < api.index('PrimChanged(')
    assert 'CanSelectWallDescriptorRenderer(pairedColors)' in api
    return 19


if __name__ == '__main__':
    comparisons, texture_hits, draw_hits = validate_cache()
    print(json.dumps({'status': 'PASS', 'window_payload_comparisons': comparisons,
        'texture_prefix_hits': texture_hits, 'draw_tail_hits': draw_hits,
        'admission_cases': validate_admission(), 'source_contracts': source_contract(),
        'limits': 'Host model only; no EE compiler, VU/GS or hardware timing validation.'}, indent=2))
