"""Host raw-X2 scatter/activation and pending-state contract model.

Decodes VIF-style UNPACK words independently of the split planner, then
compares every transferred source word/triangle with separate-run rendering.
This does not emulate VU arithmetic, GS rasterization or native performance.
"""
import random
import struct
import unittest
from pathlib import Path


FORK = Path(__file__).resolve().parents[1]
ROOT = FORK.parents[2]
CAP = 30
INPUT = 5
STRIDE = 4
WORDS = (3, 2, 4)
OFFSETS = (0, 2, 3)


def plan(lengths, continued=None, paired=False, gate=True):
    """Return activations containing (source run, first vertex, count)."""
    if (not gate or paired or len(lengths) < 2 or
            any(n <= 0 or n % 3 for n in lengths) or
            any(continued or [])):
        return None
    result, pending, used = [], [], 0
    for run, count in enumerate(lengths):
        remainder = count % CAP
        if used and (not remainder or used + remainder > CAP):
            result.append(pending)
            pending, used = [], 0
        first = 0
        while first < count:
            added = min(count - first, CAP - used)
            pending.append((run, first, added))
            first += added
            used += added
            if used == CAP:
                result.append(pending)
                pending, used = [], 0
    if used:
        result.append(pending)
    return result


def baseline(lengths):
    return [[(run, first, min(CAP, count - first))]
            for run, count in enumerate(lengths)
            for first in range(0, count, CAP)]


def source_word(run, lane, vertex, component):
    # Unique arbitrary 32-bit payloads; no float reassociation is permitted.
    return ((run + 1) << 24) | (lane << 20) | (vertex << 3) | component


def dma_payload(run, lane, first, count, address_mod16):
    """Same aligned REF interior and copied edge words as XferVectors."""
    words = WORDS[lane]
    values = [source_word(run, lane, v, c)
              for v in range(first, first + count) for c in range(words)]
    address = address_mod16 + first * words * 4
    prepend = min((-address // 4) % 4, len(values))
    ref_count = (len(values) - prepend) // 4 * 4
    prefix = values[:prepend]
    referenced = values[prepend:prepend + ref_count]
    suffix = values[prepend + ref_count:]
    return prefix + referenced + suffix


def encode(activations, alignments):
    """Encode cyclic UNPACK commands/payloads and an activation count.

    Mask expansion lanes are not consumed in this source-word comparison.
    Each source field uses the real VIF V2/V3/V4-32 encoding and TOPS flag.
    Source memory is represented through its copied edges / REF interior.
    """
    packets = []
    for fragments in activations:
        words = [0x01000104]  # STCYCL: CL=4, WL=1 (skip three destination rows).
        used = 0
        for run, first, count in fragments:
            for lane, width in enumerate(WORDS):
                command = 0x60 + (width - 1) * 4
                address = INPUT + used * STRIDE + OFFSETS[lane]
                words.append((command << 24) | (count << 16) | 0x8000 | address)
                words.extend(dma_payload(run, lane, first, count,
                                         alignments[run][lane]))
            used += count
        # Model the unchanged independent header and MSCNT completion boundary.
        words.extend((0x01000404, 0x6C018000, used, 0, 0, 0, 0x17000000))
        packets.append(struct.pack("<" + "I" * len(words), *words))
    return packets


def decode(packets):
    output = []
    for packet in packets:
        words = struct.unpack("<" + "I" * (len(packet) // 4), packet)
        memory = {}
        cursor = 0
        cycle = None
        while cursor < len(words):
            command = words[cursor]
            cursor += 1
            op = command >> 24
            if op == 1:
                cycle = (command & 255, (command >> 8) & 255)
            elif op in (0x64, 0x68, 0x6C):
                count = (command >> 16) & 255
                address = command & 0x3FF
                width = (op - 0x60) // 4 + 1
                assert command & 0x8000
                assert count > 0
                step = cycle[0] if cycle[1] == 1 else 1
                for vertex in range(count):
                    destination = address + vertex * step
                    assert destination not in memory
                    memory[destination] = words[cursor:cursor + width]
                    cursor += width
            elif op == 0x17:
                count = memory[0][0]
                assert 0 < count <= CAP and count % 3 == 0
                assert max(memory) <= 124  # q125 starts the clipping planes.
                output.extend(tuple(memory[INPUT + vertex * STRIDE + off]
                                    for off in OFFSETS)
                              for vertex in range(count))
            else:
                raise AssertionError(f"Unknown VIF command {command:08x}")
    return output


def expected(lengths):
    return [tuple(tuple(source_word(run, lane, vertex, component)
                        for component in range(width))
                  for lane, width in enumerate(WORDS))
            for run, count in enumerate(lengths) for vertex in range(count)]


class RawX2Multispan(unittest.TestCase):
    def check_runs(self, lengths, alignments=None):
        admitted = plan(lengths)
        self.assertIsNotNone(admitted)
        separate = baseline(lengths)
        self.assertLessEqual(len(admitted), len(separate))
        # Every chosen cross-run merge pays for itself without new transfers.
        self.assertEqual(sum(map(len, admitted)), len(separate))
        alignments = alignments or [(0, 4, 12)] * len(lengths)
        actual = decode(encode(admitted, alignments))
        self.assertEqual(actual, expected(lengths))
        self.assertEqual(actual, decode(encode(separate, alignments)))

    def test_boundary_triangles_and_payback(self):
        for first in range(3, 94, 3):
            for second in range(3, 94, 3):
                self.check_runs([first, second])
        self.assertEqual(len(plan([24, 24])), 2)
        self.assertEqual([len(x) for x in plan([24, 24])], [1, 1])
        self.assertEqual(len(plan([12, 48])), 2)
        self.assertEqual(len(baseline([12, 48])), 3)
        self.assertEqual(len(plan([3] * 40)), 4)

    def test_discontinuous_aligned_and_unaligned_sources(self):
        rng = random.Random(0x583253)
        for _ in range(1200):
            lengths = [rng.randrange(1, 90) * 3
                       for _ in range(rng.randrange(2, 41))]
            alignments = [tuple(rng.randrange(4) * 4 for _ in WORDS)
                          for _ in lengths]
            self.check_runs(lengths, alignments)

    def test_whole_block_rejection_precedes_emission(self):
        for lengths in ([3], [0, 3], [3, 4], [6, -3], [6, 9, 31]):
            self.assertIsNone(plan(lengths))
        self.assertIsNone(plan([6, 9], continued=[False, True]))
        self.assertIsNone(plan([6, 9], continued=[True, False]))
        self.assertIsNone(plan([6, 9], paired=True))
        self.assertIsNone(plan([6, 9], gate=False))

    def test_geometry_block_limit_preserves_order(self):
        lengths = [3, 21, 48, 6] * 37
        combined = []
        for first in range(0, len(lengths), 40):
            block = lengths[first:first + 40]
            schedules = plan(block) if len(block) > 1 else baseline(block)
            combined.extend([(run + first, offset, count)
                             for activation in schedules
                             for run, offset, count in activation])
        flattened = [(run, vertex) for run, first, count in combined
                     for vertex in range(first, first + count)]
        self.assertEqual(flattened, [(run, vertex)
                         for run, count in enumerate(lengths)
                         for vertex in range(count)])

    def test_capacity_keeps_preexisting_and_pending_source_storage(self):
        # The game still consumes each span's original whole-triangle prefix.
        for capacity in (0, 1, 2, 3, 17, 30, 31, 59, 60, 61, 500):
            arena = ["older-pending"] * 12
            used, draws = 0, []
            for run, length in enumerate((21, 48, 12)):
                count = min(length, (capacity - used) // 3 * 3)
                if count:
                    draws.append((run, tuple(range(len(arena), len(arena) + count))))
                    arena.extend([run] * count)
                    used += count
            # A queued block reads live material when it drains. Drain before
            # the next facade/state restoration, including an exhausted tail.
            texture = "roof"
            emitted = [(texture, run, tuple(arena[i] for i in indices))
                       for run, indices in draws]
            texture = "facade"
            self.assertTrue(all(t == "roof" for t, _, _ in emitted))
            self.assertEqual(arena[:12], ["older-pending"] * 12)
            self.assertTrue(all(all(word == run for word in payload)
                                for _, run, payload in emitted))
            self.assertLessEqual(used, capacity)

    def test_source_admission_and_drain_ownership(self):
        source = (FORK / "src/clip_renderer.cpp").read_text(encoding="utf-8")
        body = source.split("bool CClipTriX2Renderer::TryDrawIndependentSpans", 1)[1]
        body = body.split("// Copy of CLinearRenderer::DrawBlock", 1)[0]
        self.assertLess(body.index("block.StripIsContinued(strip)"), body.index("packet.Cnt()"))
        self.assertIn("!remainder || used + remainder > 30", body)
        self.assertIn("Math::Min(count - first, 30 - used)", body)
        self.assertIn("InputGeomOffset + used * InputQuadsPerVert", body)
        self.assertIn("WinTex || pGLContext->InDListDef()", body)
        self.assertNotIn("FindNumBuffers", body)
        game = (ROOT / "playstation2.c").read_text(encoding="utf-8")
        admission = game.split("static bool ps2_city_soup_run_batch_admitted", 1)[1]
        self.assertIn("#if PS2_TEMP && PS2_CITY_SOUP_RUN_BATCH", admission)
        self.assertIn("pglGetRawX2SubmissionOptions() & 1u", admission)
        scope = game.split("if (chunks && chunks->soup_spans)", 1)[1]
        scope = scope.split("if (x2_soup && nchunk > 0)", 1)[0]
        self.assertEqual(scope.count("pglClipX2SetWindowTexture("), 1)
        self.assertIn("x2_soup && raw_view && !lit_needed", scope)
        self.assertLess(scope.index("draw_mesh_sh_tex_colored_x2_raw_tint(part"),
                        scope.index("if (soup_scope) SYNC_FLUSH();"))
        self.assertNotIn("glPopMatrix", scope)
        self.assertNotIn("g_batch_n =", scope)


if __name__ == "__main__":
    unittest.main()
