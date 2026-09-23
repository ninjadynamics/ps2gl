"""Host coverage/packet contracts for the private untextured clear sprite."""
import random
import struct
import unittest
from pathlib import Path


FORK = Path(__file__).resolve().parents[1]
STUFF = FORK.parent / "ps2stuff"
COLOR_FORMATS = (0, 1, 2, 10)
DEPTH_FORMATS = (48, 49, 50, 58)


def admitted(width, height, frame=0, depth=48, gate=True):
    return (gate and 64 < width <= 2048 and width % 64 == 0
            and 0 < height <= 2048 and frame in COLOR_FORMATS
            and depth in DEPTH_FORMATS)


def rectangles(width, height, **kwargs):
    if admitted(width, height, **kwargs):
        return [(x, 0, x + 64, height) for x in range(0, width, 64)]
    return [(0, 0, width, height)]


def hits(rects, x, y):
    # GS Manual p46: include top/left, exclude bottom/right at pixel centers.
    return sum(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in rects)


def sprite(template, rect, final=True):
    words = list(template)
    x0, y0, x1, y1 = rect
    words[12:14] = [x0 << 4, y0 << 4]
    words[20:22] = [x1 << 4, y1 << 4]
    if not final:
        words[0] &= ~(1 << 15)
    return struct.pack("<24I", *words)


def compact(original, width):
    low, regs = struct.unpack_from("<QQ", original)
    color_low = (low & ~((15 << 60) | (1 << 15))) | (1 << 60)
    vertex_low = (low & ~((15 << 60) | 0x7fff | (1 << 46))) | (2 << 60) | (width // 64)
    vertex_regs = (regs & ~0xff) | ((regs >> 8) & 15) | (((regs >> 16) & 15) << 4)
    result = bytearray(struct.pack("<QQ", color_low, regs) + original[16:32]
                       + struct.pack("<QQ", vertex_low, vertex_regs))
    for x in range(0, width, 64):
        pair = bytearray(original[48:64] + original[80:96])
        struct.pack_into("<I", pair, 0, x << 4)
        struct.pack_into("<I", pair, 16, (x + 64) << 4)
        result.extend(pair)
    return bytes(result)


def decode_sprites(payload):
    """Decode effective PACKED writes, ignoring NOP and repeated PRIM writes."""
    offset = eops = prim_writes = 0
    prim = color = None
    vertices, draws = [], []
    while offset < len(payload):
        low, regs = struct.unpack_from("<QQ", payload, offset)
        offset += 16
        assert (low >> 58) & 3 == 0  # PACKED
        nreg = ((low >> 60) & 15) or 16
        if low & (1 << 46):
            prim = (low >> 47) & 0x7ff
            prim_writes += 1
            assert prim & 7 == 6  # independent SPRITE vertex pairs
            assert not vertices
        for _ in range(low & 0x7fff):
            for index in range(nreg):
                reg = (regs >> (4 * index)) & 15
                word = payload[offset:offset + 16]
                assert len(word) == 16
                offset += 16
                if reg == 1:
                    color = word
                elif reg == 4:
                    vertices.append(word)
                    if len(vertices) == 2:
                        draws.append((prim, color, *vertices))
                        vertices.clear()
                else:
                    assert reg == 15
        if low & (1 << 15):
            eops += 1
            assert offset == len(payload)
    assert not vertices
    return draws, eops, prim_writes


class ClearPageStrips(unittest.TestCase):
    def test_exact_coverage_including_every_shared_edge(self):
        for width, height in ((128, 1), (640, 224), (640, 448),
                              (640, 480), (2048, 2048)):
            old = [(0, 0, width, height)]
            new = rectangles(width, height)
            for x in range(-1, width + 2):
                for y in (-1, 0, 1, height - 1, height, height + 1):
                    self.assertEqual(hits(old, x, y), hits(new, x, y))
            self.assertEqual(sum((r - l) * (b - t) for l, t, r, b in new),
                             width * height)

    def test_fallback_and_supported_formats(self):
        for frame in COLOR_FORMATS:
            for depth in DEPTH_FORMATS:
                self.assertEqual(len(rectangles(640, 448, frame=frame, depth=depth)), 10)
        for width, height in ((0, 448), (64, 448), (65, 448), (639, 448),
                              (640, 0), (640, -1), (2112, 448), (640, 2049)):
            self.assertEqual(rectangles(width, height), [(0, 0, width, height)])
        for kwargs in ({"gate": False}, {"frame": 19}, {"depth": 51}):
            self.assertEqual(rectangles(640, 448, **kwargs), [(0, 0, 640, 448)])

    def test_compact_packet_preserves_every_effective_vertex_and_color(self):
        rng = random.Random(0x434c4541)
        for width in (128, 640, 2048):
            for _ in range(40):
                template = [rng.getrandbits(32) for _ in range(24)]
                # Exact CSprite tag: five PACKED registers, sprite/context2.
                low = 1 | (1 << 15) | (1 << 46) | (0x20e << 47) | (5 << 60)
                template[:4] = struct.unpack("<4I", struct.pack("<QQ", low, 0x4f4f1))
                original = sprite(template, (0, 0, width, 448))
                old = b"".join(sprite(template, rect, final=rect[2] == width)
                               for rect in rectangles(width, 448))
                new = compact(original, width)
                template[:] = [0] * 24  # packet bytes outlive mutable inputs.
                old_draws, old_eops, old_prims = decode_sprites(old)
                new_draws, new_eops, new_prims = decode_sprites(new)
                self.assertEqual(old_draws, new_draws)
                self.assertEqual((old_eops, new_eops), (1, 1))
                self.assertEqual((old_prims, new_prims), (width // 64, 1))
                self.assertEqual(len(new), (3 + 2 * width // 64) * 16)
                if width == 640:
                    self.assertEqual(len(old) - len(new), 592)
                    self.assertEqual(len(new) - len(original), 272)

    def test_source_keeps_state_and_packet_ownership(self):
        source = (FORK / "src/clear.cpp").read_text(encoding="utf-8")
        header = (FORK / "include/ps2gl/clear.h").read_text(encoding="utf-8")
        draw = source.split("void CClearEnv::ClearBuffers", 1)[1].split("C gl api", 1)[0]
        candidate = draw.split("#if PGL_CLEAR_PAGE_STRIPS", 1)[1].split("#endif", 1)[0]
        self.assertEqual(draw.count("pDrawEnv->SendSettings(packet)"), 1)
        self.assertEqual(candidate.count("packet.Cnt()"), 1)
        self.assertEqual(candidate.count("packet.OpenDirect()"), 1)
        self.assertIn("pSprite->GetPacket().GetBase()", candidate)
        self.assertIn("packet.Add(source + 1, 1)", candidate)
        self.assertIn("packet.Add(vertices, 2)", candidate)
        self.assertNotIn("packet.Ref", candidate)
        self.assertNotIn("packet.Flush", candidate)
        self.assertNotIn("pSprite->SetVertices", candidate)
        self.assertIn("colorTag.NREG = 1;", candidate)
        self.assertIn("colorTag.EOP = 0;", candidate)
        self.assertIn("vertexTag.NREG = 2;", candidate)
        self.assertIn("vertexTag.PRE = 0;", candidate)
        self.assertIn("vertexTag.REGS0 = colorTag.REGS2;", candidate)
        self.assertIn("vertexTag.REGS1 = colorTag.REGS4;", candidate)
        self.assertRegex(header, r"#define PGL_CLEAR_PAGE_STRIPS [01]\b")
        sprite_source = (STUFF / "src/sprite.cpp").read_text(encoding="utf-8")
        for field in ("tme", "fge", "abe", "aa1"):
            self.assertRegex(sprite_source, rf"prim\.{field}\s*=\s*0;")
        for field, value in (("NLOOP", 1), ("EOP", 1), ("PRE", 1), ("NREG", 5)):
            self.assertRegex(sprite_source, rf"DrawGifTag\.{field}\s*=\s*{value};")
        for field, value in (("REGS0", "0x1"), ("REGS2", "0x4"), ("REGS4", "0x4")):
            self.assertRegex(sprite_source, rf"DrawGifTag\.{field}\s*=\s*{value};")
        self.assertIn("packet.Add((uint128_t*)&DrawGifTag, 6)", sprite_source)
        packet_source = (STUFF / "include/ps2s/packet.h").read_text(encoding="utf-8")
        copied = packet_source.split("CDmaPacket::operator+=(const CDmaPacket&", 1)[1].split("inline", 1)[0]
        self.assertIn("Add(otherPkt.GetBase(), numQuads)", copied)


if __name__ == "__main__":
    unittest.main()
