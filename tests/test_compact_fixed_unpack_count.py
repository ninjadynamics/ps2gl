#!/usr/bin/env python3
"""Compare generic and known-count closes on complete compact DMA chains.

Models the old CloseUnpack VN/VL/byte-distance calculation independently of
the caller's known count. Encoded packets, REF targets, padding and final
open-code state must match. No target compilation or timing is claimed.
"""
import random
import struct
import unittest
from pathlib import Path

from compact_packet_model import Packet, U32, execute, sweep


class ClosingPacket(Packet):
    def __init__(self, fixed):
        super().__init__()
        self.fixed = fixed
        self.pending_unpack = None
        self.ref_payload = False
        self.closed = 0

    def close_unpack(self):
        if self.pending_unpack is None:
            return
        start, expected = self.pending_unpack
        command = U32.unpack_from(self.memory, start)[0]
        vn = (command >> 26) & 3
        vl = (command >> 24) & 3
        byte_count = len(self.memory) - start - 4
        # STCYCL(1,1): no filling adjustment. Count the actual owned bytes,
        # not the value passed by the stream builder.
        generic = (byte_count // (4 >> vl)) // (vn + 1)
        count = expected if self.fixed else generic
        assert generic == expected and 0 < count <= 256
        self.memory[start + 2] = count & 255
        self.pending_unpack = None
        self.closed += 1

    def word(self, value):
        if value == 0x17000000:  # header closes immediately before MSCNT
            self.close_unpack()
        super().word(value)

    def tag(self, kind, qwords=0, address=0):
        assert self.pending_unpack is None
        super().tag(kind, qwords, address)
        self.ref_payload = kind == 3

    def unpack(self, qword_address, count):
        self.close_unpack()
        self.pad(12)
        start = len(self.memory)
        self.word(0x6C008000 | qword_address)
        if self.ref_payload:
            # The pre-existing REF arm already supplies NUM explicitly;
            # there are deliberately no inline payload bytes to infer here.
            self.memory[start + 2] = count & 255
            self.ref_payload = False
        else:
            self.pending_unpack = start, count


def source_sweep(packet, source, stride, limit, compact):
    count = len(source) // (stride * 16)
    offset = 0
    if compact:
        packet.tag(1)
        packet.word(0x01000101)
    while count:
        batch = min(count, limit)
        if not compact:
            packet.tag(1)
            packet.word(0x01000101)
        packet.unpack(5, batch * stride)
        size = batch * stride * 16
        packet.memory += source[offset:offset + size]
        packet.unpack(0, 1 if compact else 5)
        packet.memory += struct.pack('<4I', batch, 0, 0, 0)
        if not compact:
            packet.memory += struct.pack('<16f', 1024.0, *([0.0] * 15))
        packet.word(0x17000000)
        packet.pad(0)
        if not compact:
            packet.close()
        offset += size
        count -= batch
    if compact:
        packet.close()


class CompactFixedUnpackCount(unittest.TestCase):
    def assert_same(self, old, new):
        self.assertEqual(old.memory, new.memory)
        self.assertEqual(old.closed, new.closed)
        for packet in (old, new):
            self.assertIsNone(packet.pending_unpack)
            self.assertIsNone(packet.open_tag)

    def test_num_byte_encoding_boundary(self):
        for count in (1, 5, 96, 144, 256):
            old, new = ClosingPacket(False), ClosingPacket(True)
            for packet in (old, new):
                packet.tag(1)
                packet.word(0x01000101)
                packet.unpack(5, count)
                command = packet.pending_unpack[0]
                packet.memory += bytes(count * 16)
                packet.close_unpack()
                self.assertEqual(packet.memory[command + 2], count & 255)
                packet.pad(0)
                packet.close()
            self.assert_same(old, new)

    def test_source_streams(self):
        rng = random.Random(0x21C1056)
        # Road/uniform billboard: 3q/32; pool/corner-alpha: 4q/24.
        for stride, limit in ((3, 32), (4, 24)):
            counts = list(range(1, 2 * limit + 2)) + [127, 128, 255, 256, 511, 512, 4096]
            for count in counts:
                source = rng.randbytes(count * stride * 16)
                for compact in (False, True):
                    old, new = ClosingPacket(False), ClosingPacket(True)
                    for packet in (old, new):
                        source_sweep(packet, source, stride, limit, compact)
                    self.assert_same(old, new)

    def test_decal_owned_and_ref_sweeps(self):
        rng = random.Random(0x21E1056)
        for count in list(range(1, 34)) + [127, 128, 255, 256, 511, 512]:
            source = rng.randbytes(count * 144)
            expected = [(min(count - i, 16), source[i * 144:min(i + 16, count) * 144])
                        for i in range(0, count, 16)]
            for compact in (False, True):
                for reuse in (False, True):
                    for form in (0, 1):
                        old, new = ClosingPacket(False), ClosingPacket(True)
                        for packet in (old, new):
                            owned = sweep(packet, source, compact, False, format=form)
                            # Preserve a nonempty intervening state packet.
                            packet.tag(1)
                            packet.pad(0)
                            packet.memory += bytes(32)
                            packet.close()
                            sweep(packet, source, compact, reuse, owned, format=form)
                            self.assertEqual(execute(packet), expected * 2)
                        self.assert_same(old, new)

    def test_source_contract(self):
        root = Path(__file__).resolve().parents[1]
        helper = (root / 'include/ps2gl/owned_payload.h').read_text()
        road = (root / 'src/x2r_renderer.cpp').read_text()
        decal = (root / 'src/x2e_renderer.cpp').read_text()
        self.assertIn('packet.CloseUnpack(qwords);', helper)
        self.assertNotIn('packet.CloseUnpack();', helper)
        for source, options in ((road, 15), (decal, 511)):
            self.assertEqual(source.count('pglCloseOwnedV4Unpack('), 2)
            self.assertIn('pglCloseOwnedV4Unpack(packet, 1u);', source)
            self.assertIn(f'return {options}u;', source)
            self.assertIn('packet.Stcycl(1, 1);', source)
        self.assertIn('(unsigned int)batch * (unsigned int)floatsPerQuad / 4u', road)
        self.assertIn('pglCloseOwnedV4Unpack(packet, (unsigned int)batch * 9u);', decal)
        self.assertIn('packet.CloseUnpack((unsigned int)batch * 9u);', decal)


if __name__ == '__main__':
    unittest.main()
