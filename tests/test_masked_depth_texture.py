"""Host GIF/depth and explicit-scope contracts; not a native renderer test."""
import random
import struct
import unittest
from pathlib import Path


FORK = Path(__file__).resolve().parents[1]
ROOT = FORK.parents[2]
TME = 1 << 4


def giftag(prim, loops=0, nreg=3):
    low = loops | (1 << 15) | (1 << 46) | (prim << 47) | (nreg << 60)
    return struct.pack("<QQ", low, 0x412)  # ST, RGBAQ, XYZF2, unchanged.


def admitted(*, gate=True, owner=False, dlist=False, texture=True,
             mask=0xffffffff, ate=False, logical_ate=False, depth=True,
             zwrite=True, blend=False, aa=False, fog=False):
    return (gate and not owner and not dlist and texture and mask == 0xffffffff
            and not ate and not logical_ate and depth and zwrite
            and not blend and not aa and not fog)


def pixel(frame, stored_z, incoming_z, sampled_alpha, source_alpha,
          textured, test, zwrite):
    # The GS tests alpha after sampling, but ATE is OFF in the admitted scope.
    alpha = sampled_alpha if textured else source_alpha
    assert 0 <= alpha <= 255
    passed = {"never": False, "always": True,
              "greater": incoming_z > stored_z,
              "gequal": incoming_z >= stored_z}[test]
    # FRAME.FBMSK masks every color bit. Sampling cannot change this Z test.
    return frame, incoming_z if passed and zwrite else stored_z


class MaskedDepthTexture(unittest.TestCase):
    def test_encoded_giftag_changes_only_tme(self):
        for prim in range(2048):
            for loops, nreg in ((0, 3), (3, 3), (30, 3), (32767, 4)):
                before = giftag(prim, loops, nreg)
                after = giftag(prim & ~TME, loops, nreg)
                xor = int.from_bytes(before, "little") ^ int.from_bytes(after, "little")
                self.assertEqual(xor, (1 << 51) if prim & TME else 0)
                self.assertEqual(before[8:], after[8:])
        self.assertEqual(giftag(0x1b).hex(), "0080000000c00d301204000000000000")
        self.assertEqual(giftag(0x0b).hex(), "0080000000c005301204000000000000")

    def test_masked_depth_result_ignores_texture_rgba(self):
        rng = random.Random(0x44505448)
        for bits in (16, 24):
            limit = (1 << bits) - 1
            for _ in range(2000):
                frame = rng.getrandbits(32)
                stored, incoming = rng.randrange(limit + 1), rng.randrange(limit + 1)
                for test in ("never", "always", "greater", "gequal"):
                    for zwrite in (False, True):
                        args = (frame, stored, incoming, rng.randrange(256), rng.randrange(256))
                        self.assertEqual(pixel(*args, True, test, zwrite),
                                         pixel(*args, False, test, zwrite))
                self.assertEqual(pixel(frame, stored, stored, 0, 255, True, "gequal", True),
                                 pixel(frame, stored, stored, 0, 255, False, "gequal", True))

    def test_unsupported_entry_is_transactional(self):
        self.assertTrue(admitted())
        for key, bad in (("gate", False), ("owner", True), ("dlist", True),
                         ("texture", False), ("mask", 0xfffffffe), ("ate", True),
                         ("logical_ate", True), ("depth", False), ("zwrite", False),
                         ("blend", True), ("aa", True), ("fog", True)):
            pending = ["earlier textured draw"]
            state = {"owner": None, "dirty": False}
            if admitted(**{key: bad}):
                pending.clear()
                state.update(owner="context", dirty=True)
            self.assertEqual(pending, ["earlier textured draw"])
            self.assertEqual(state, {"owner": None, "dirty": False})

    def test_source_scope_reload_and_fallback_contract(self):
        source = (FORK / "src/base_renderer.cpp").read_text(encoding="utf-8")
        begin = source.split('extern "C" GLboolean pglBeginMaskedDepthNoTexture', 1)[1]
        begin = begin.split('extern "C" void pglEndMaskedDepthNoTexture', 1)[0]
        self.assertLess(begin.index("return GL_FALSE"), begin.index(".Flush()"))
        self.assertLess(begin.index(".Flush()"), begin.index("maskedDepthTextureOwner ="))
        self.assertIn("pGLContext->PrimChanged();", begin)
        self.assertIn("#if PGL_MASKED_DEPTH_NO_TEXTURE", begin)
        self.assertIn("#else\n    return GL_FALSE;", begin)
        end = source.split('extern "C" void pglEndMaskedDepthNoTexture', 1)[1]
        end = end.split('extern "C" unsigned int pglGetContextOptimizationFlags', 1)[0]
        self.assertLess(end.index(".Flush()"), end.index("maskedDepthTextureOwner = NULL"))
        self.assertIn("pGLContext->PrimChanged();", end)
        tag = source.split("CBaseRenderer::BuildGiftag(GLenum primType)", 1)[1]
        tag = tag.split("void CBaseRenderer::CacheRendererState", 1)[0]
        self.assertIn("!glContext.GetImmGeomManager().GetRendererManager().IsCurRendererCustom()", tag)
        self.assertIn("MaskedDepthTextureStateAdmitted(glContext)", tag)
        self.assertIn("useTexture = false;", tag)
        self.assertIn("++maskedDepthTextureCounts[2];", tag)
        self.assertNotIn("SetTexEnabled", source)
        self.assertNotIn("InvalidateTextureSync", source)
        # Prim invalidates the giftag but does not enter either delta writer.
        for function in ("CanUseUnlitContextDelta", "CanUseUnlitGsContextDelta"):
            body = source.split("CBaseRenderer::" + function, 1)[1]
            allowed = body.split("const uint32_t allowed =", 1)[1].split(";", 1)[0]
            self.assertNotIn("RendererCtxtFlags::Prim", allowed)
        context = (FORK / "include/ps2gl/glcontext.h").read_text(encoding="utf-8")
        prim = context.split("inline void PrimChanged()", 1)[1].split("}", 1)[0]
        self.assertIn("RendererContextChanged |= RendererCtxtFlags::Prim", prim)
        game = (ROOT / "playstation2.c").read_text(encoding="utf-8")
        draw = game.split("void ps2_render_city_ground_depth(", 1)[1]
        draw = draw.split("/* Neon billboards", 1)[0]
        markers = ("glColorMask(GL_FALSE", "pglBeginMaskedDepthNoTexture()",
                   "batch_flush(ground_depth->first", "pglEndMaskedDepthNoTexture()",
                   "glPopMatrix()", "glColorMask(GL_TRUE")
        offsets = [draw.index(marker) for marker in markers]
        self.assertEqual(offsets, sorted(offsets))
        self.assertIn("#if PS2_TEMP && PS2_CITY_GROUND_DEPTH_NO_TEXTURE", draw)
        self.assertIn("if (no_texture) pglEndMaskedDepthNoTexture();", draw)
        self.assertRegex(draw, r"batch_flush\(ground_depth->first, ground_depth->count, 1, 1, 1, GL_TRIANGLES\)")
        self.assertEqual(draw.count("glDisable(GL_TEXTURE_2D)"), 1)
        gate = (FORK / "include/GL/ps2gl.h").read_text(encoding="utf-8")
        self.assertIn("#define PGL_MASKED_DEPTH_NO_TEXTURE 1", gate)
        self.assertIn("return PGL_MASKED_DEPTH_NO_TEXTURE ? 1u : 0u;", source)


if __name__ == "__main__":
    unittest.main()
