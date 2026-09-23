"""Host checks for GeneralClipTri's optional XYZ2 context encoding.

This models float32 scaling and GIF field extraction, not VU rounding, GS
rasterization or hardware correctness. The caller still owns exact positive
near clipping. No target compiler, assembler or hardware is used.
"""

from pathlib import Path
import math
import random
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAX_Z24 = (1 << 24) - 1


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def ftoi4(value):
    return max(-(1 << 31), min((1 << 31) - 1, math.trunc(value * 16)))


def projection(w, bias=0.0, scale=1.0):
    """Reversed GS Z row for n=1, f=2048 and a positive eye-depth W."""
    a = f32(f32(MAX_Z24 * f32(2049 / 2047)) / 2)
    b = f32(f32(MAX_Z24 * f32(4096 / 2047)) / 2)
    offset = f32(f32(MAX_Z24 / 2) + bias)
    a, b, offset = (f32(v * scale) for v in (a, b, offset))
    predivide = f32(f32(a * f32(-w)) + b)
    return f32(f32(predivide * f32(1 / w)) + offset)


def function(source, signature):
    start = source.index(signature)
    begin = source.index("{", start)
    level = 1
    pos = begin + 1
    while level:
        level += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


class Xyz2Encoding(unittest.TestCase):
    def test_near_crossing_witness_keeps_upper_z_bits(self):
        z = projection(f32(0.75))
        old = (ftoi4(z) >> 4) & MAX_Z24
        encoded = ftoi4(projection(f32(0.75), scale=1 / 16))
        self.assertEqual(old, 5595136)
        self.assertEqual(encoded, 22372352)
        self.assertGreater(encoded, MAX_Z24)

    def test_intended_positive_envelope_fits_signed_conversion(self):
        # Existing jet near=.05, NOT the projection near=1. A caller that
        # leaks W<=0 does not satisfy the encoding contract.
        z = projection(f32(0.05), scale=1 / 16)
        self.assertGreater(ftoi4(z), MAX_Z24)
        self.assertLess(z * 16, (1 << 31) - 1)

    def test_uniform_binary_scale_preserves_in_range_depth(self):
        rng = random.Random(2326)
        in_range = 0
        for _ in range(10000):
            w = f32(10 ** rng.uniform(math.log10(.05), math.log10(2048)))
            # Include the independently applied post-divide depth bias.
            bias = rng.choice((0.0, 1.0, -1.0, .25))
            z = projection(w, bias)
            encoded = projection(w, bias, 1 / 16)
            self.assertEqual(encoded, f32(z / 16))
            self.assertEqual(ftoi4(encoded), math.trunc(z))
            if 0 <= z <= MAX_Z24:
                self.assertEqual(ftoi4(encoded), (ftoi4(z) >> 4) & MAX_Z24)
                in_range += 1
        self.assertGreater(in_range, 7000)

    def test_matrix_scaling_leaves_xyw_and_clip_planes_unchanged(self):
        matrix = [[f32(c * 4 + r + .125) for r in range(4)] for c in range(4)]
        encoded = [[f32(v / 16) if r == 2 else v
                    for r, v in enumerate(col)] for col in matrix]
        for c in range(4):
            for r in (0, 1, 3):
                self.assertEqual(encoded[c][r], matrix[c][r])

    def test_xyz2_adc_retains_drawing_kick(self):
        # Both PACKED formats take ADC from bit111, i.e. word3 bit15;
        # XYZ2 ignores the fog coefficient in the other word3 bits.
        for fog in (0, 1, 127, 255):
            for adc in (0, 1):
                word3 = fog | (adc << 15)
                self.assertEqual((word3 >> 15) & 1, adc)

    def test_source_context_and_admission_contract(self):
        source = (ROOT / "src/clip_renderer.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/GL/ps2gl.h").read_text(encoding="utf-8")
        init = function(source, "void CClipTriRenderer::InitContext(")
        alias = init[:init.index("if (TryUnlitContextDelta(")]
        query = function(source, 'extern "C" GLboolean pglCanDrawClipTriXYZ2(')
        self.assertIn("#define PGL_CLIP_TRIANGLES_XYZ2 ((GLenum)0x80000000 | 13)", header)
        self.assertIn("GetDepthBits() == 24", query)
        self.assertIn("!pGLContext->InDListDef()", query)
        self.assertIn("!pGLContext->GetImmLighting().GetLightingEnabled()", query)
        self.assertIn("!pGLContext->GetImmDrawContext().GetFogEnabled()", query)
        self.assertIn("GetPolygonMode() == GL_FILL", query)
        self.assertIn("GetDepthOffset() == 0.0f", query)
        self.assertIn("pglInvalidateUnlitContextDelta();", alias)
        self.assertNotIn("NoteUnlitContext(", alias)
        for c in range(4):
            self.assertIn(f"c{c}.z *= encodingScale;", alias)
        self.assertNotIn(".w *=", alias)
        self.assertIn("giftag.REGS2 = 5;", alias)
        self.assertLess(alias.index("AddVu1RendererContext("), alias.index("packet += offset;"))
        self.assertLess(alias.index("packet += encoded;"), alias.index("packet += giftag;"))
        self.assertLess(alias.index("packet += giftag;"), alias.index("packet.Mscal(0);"))
        self.assertIn("packet.Flushe();", alias)
        self.assertIn("packet.Base(kDoubleBufBase);", alias)
        self.assertIn("packet.Offset(kDoubleBufOffset);", alias)

    def test_existing_vu_body_retains_conversion_and_output_size(self):
        source = (ROOT / "vu1/general_clip_tri.vcl").read_text(encoding="utf-8")
        geometry = (ROOT / "vu1/geometry.i").read_text(encoding="utf-8")
        self.assertIn("kOutputQPerV        .equ           3", source)
        self.assertIn("ftoi4.xyz      emp", source)
        self.assertIn("ftoi4.xyz      \\gs_vert, \\gs_vert", geometry)


if __name__ == "__main__":
    unittest.main()
