"""Byte/lifetime model for direct owned billboard context serialization.

This checks the 128-byte payload and source contract, not EE code generation,
DMA execution or a hardware performance improvement.
"""

from pathlib import Path
import random
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]
SIZE = 8 * 16
SELECTOR = 5 * 16 + 12


def old_payload(source, reuse):
    temporary = bytearray(source)
    struct.pack_into('<I', temporary, SELECTOR, int(reuse))
    return bytes(temporary)


def direct_payload(packet, first, source, reuse):
    # pglAddOwnedPayload -> CSCDmaPacket::Add copies into the open frame chain.
    if first + SIZE > len(packet):
        raise BufferError('complete context capacity required')
    packet[first:first + SIZE] = source
    struct.pack_into('<I', packet, first + SELECTOR, int(reuse))
    return first + SIZE


class BillboardContextSerializationTests(unittest.TestCase):
    def test_all_payload_bytes_order_and_both_selector_values(self):
        rng = random.Random(0xB11B04D)
        special = struct.pack('<8I', 0, 0x80000000, 0x3f800000, 0xbf800000,
                              0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001)
        sources = [special * 4, bytes(SIZE), bytes([255]) * SIZE]
        sources.extend(rng.randbytes(SIZE) for _ in range(10000))
        for source in sources:
            for reuse in (False, True):
                packet = bytearray([0xa5] * (32 + SIZE + 32))
                self.assertEqual(direct_payload(packet, 32, source, reuse), 160)
                self.assertEqual(packet[32:160], old_payload(source, reuse))
                self.assertEqual(packet[:32], bytes([0xa5]) * 32)
                self.assertEqual(packet[160:], bytes([0xa5]) * 32)

    def test_source_can_change_after_owned_copy(self):
        source = bytearray(range(SIZE))
        original = bytes(source)
        packet = bytearray(SIZE)
        direct_payload(packet, 0, source, True)
        self.assertEqual(source, original)
        source[:] = bytes([0xcc]) * SIZE
        self.assertEqual(packet, old_payload(original, True))

    def test_one_complete_copy_and_raw_word_override_in_source(self):
        source = (ROOT / 'src/x2r_renderer.cpp').read_text()
        branch = source.split('if (ContextFirstQuad == 48u) {', 1)[1].split(
            '\n    } else {', 1)[0]
        self.assertEqual(branch.count('pglAddOwnedPayload('), 1)
        self.assertIn('(const float*)&RoadContext + ContextFirstQuad * 4u, 32u', branch)
        self.assertIn('const uint32_t reuse = 1u;', branch)
        self.assertIn('memcpy((char*)billboardContext + 5u * 16u + 12u, '
                      '&reuse, sizeof(reuse));', branch)
        self.assertNotIn('billboardContext[', branch)
        self.assertNotIn('packet.Add(billboardContext', branch)
        self.assertNotIn('packet.Ref(', branch)


if __name__ == '__main__':
    unittest.main()
