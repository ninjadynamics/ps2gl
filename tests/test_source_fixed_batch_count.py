#!/usr/bin/env python3
"""Check fixed source batch counts against admission bounds and DMA sweeps.

The candidate narrows only the division operands, then widens the result to
the existing uint64_t reservation arithmetic. This is a host/source check;
it does not compile EE code or establish a target performance improvement.
"""
import random
import unittest
from pathlib import Path

from compact_packet_model import Packet
from test_source_context_tail_patch import Interpreter, emit_draw


INT_MAX = (1 << 31) - 1
UINT_MAX = (1 << 32) - 1
FAMILIES = ((48, 32), (64, 24))


def fixed_batches(count, limit):
    # Match the candidate's unsigned-int arithmetic before uint64_t assignment.
    if limit == 32:
        return ((count + 31) & UINT_MAX) // 32
    return ((count + 23) & UINT_MAX) // 24


def generic_batches(count, limit):
    return (count + limit - 1) // limit


def reservation_words(count, size, batches):
    return (count * (size // 16) + batches * 16 + 272) * 4


class SourceFixedBatchCountTests(unittest.TestCase):
    def test_all_normal_packet_counts_and_large_boundaries(self):
        rng = random.Random(0x2150B47C)
        comparisons = 0
        for size, limit in FAMILIES:
            maximum = INT_MAX // size
            # Even 65536 minimum-size records cannot fit a 65000q frame
            # packet. Cover every smaller count, plus the public API domain.
            counts = set(range(1, 65537))
            for value in (maximum, *(1 << bit for bit in range(26))):
                counts.update(value + delta for delta in range(-limit, limit + 1)
                              if 0 < value + delta <= maximum)
            for _ in range(5000):
                boundary = rng.randrange(1, maximum // limit + 1) * limit
                counts.update(value for value in (boundary - 1, boundary, boundary + 1)
                              if 0 < value <= maximum)
            self.assertLessEqual(maximum + limit - 1, UINT_MAX)
            for count in counts:
                expected = 1 + (count - 1) // limit
                self.assertEqual(generic_batches(count, limit), expected)
                self.assertEqual(fixed_batches(count, limit), expected)
                comparisons += 1
        self.assertGreater(comparisons, 150000)

    def test_reservation_edges_keep_the_wide_footprint(self):
        for size, limit in FAMILIES:
            maximum = INT_MAX // size
            counts = [1, limit - 1, limit, limit + 1, 65535, 65536,
                      maximum - 1, maximum]
            for count in counts:
                old = reservation_words(count, size, generic_batches(count, limit))
                new = reservation_words(count, size, fixed_batches(count, limit))
                self.assertEqual(old, new)
                for remaining in (0, old - 1, old, old + 1, UINT_MAX):
                    self.assertEqual(old <= UINT_MAX and old <= remaining,
                                     new <= UINT_MAX and new <= remaining)
                # Keep byte/address sums wide even when a packet cannot fit.
                for start in (0, 0x0FFFFFFC, UINT_MAX):
                    self.assertEqual(start + old * 4, start + new * 4)
            self.assertGreater(reservation_words(maximum, size,
                               fixed_batches(maximum, limit)), 65000 * 4)

    def test_count_matches_encoded_source_activations(self):
        sweeps = 0
        for size, limit in FAMILIES:
            counts = list(range(1, limit * 2 + 2)) + [127, 128, 255, 256, 511, 512, 4096]
            for count in counts:
                records = bytes((index * 37 + count) & 255 for index in range(count * size))
                packet = Packet()
                emit_draw(packet, records, size // 16, limit)
                snapshots = Interpreter().execute(packet.memory, 48, size // 16)
                self.assertEqual(len(snapshots), fixed_batches(count, limit))
                self.assertEqual(b''.join(item[3] for item in snapshots), records)
                self.assertLessEqual(len(packet.memory) // 4,
                                     reservation_words(count, size, len(snapshots)))
                sweeps += 1
        self.assertEqual(sweeps, 128)

    def test_source_gate_and_original_wide_contract(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / 'src/immgmanager.cpp').read_text()
        source = source.split('bool CImmGeomManager::DrawSourceViewQuads(', 1)[1]
        source = source.split('bool CImmGeomManager::DrawPoolQuads(', 1)[0]
        header = (root / 'include/GL/ps2gl.h').read_text()
        options = (root / 'src/x2r_renderer.cpp').read_text()
        guard = source.index('count > INT_MAX / (int)quadBytes')
        division = source.index('#if PGL_SOURCE_FIXED_BATCH_COUNT')
        self.assertLess(guard, division)
        self.assertEqual(source.count('batchLimit = 24;'), 2)
        self.assertEqual(source.count('batchLimit = 32;'), 1)
        self.assertIn('const uint64_t batches = batchLimit == 32u\n'
                      '        ? ((unsigned int)count + 31u) / 32u\n'
                      '        : ((unsigned int)count + 23u) / 24u;', source)
        self.assertIn('#else\n    const uint64_t batches = '
                      '((uint64_t)count + batchLimit - 1u) / batchLimit;\n#endif', source)
        self.assertIn('const uint64_t words = ((uint64_t)count * (quadBytes / 16u) '
                      '+ batches * 16u + 272u) * 4u;', source)
        self.assertIn('words > UINT_MAX', source)
        self.assertIn('const uint64_t writeEnd = (uint64_t)writeBegin + words * sizeof(uint32_t);', source)
        self.assertIn('#define PGL_SOURCE_FIXED_BATCH_COUNT 1', header)
        self.assertIn('PGL_SOURCE_FIXED_BATCH_COUNT != 0 && PGL_SOURCE_FIXED_BATCH_COUNT != 1', header)
        self.assertIn('PGL_SOURCE_CONTEXT_TAIL_PATCH ? 1u : 0u', options)
        self.assertIn('PGL_SOURCE_FIXED_BATCH_COUNT ? 2u : 0u', options)


if __name__ == '__main__':
    unittest.main()
