"""Replay compact-source context packets, retention proofs and MSCNT state.

This host model checks consumed bytes and scheduled-register ownership. It
does not execute EE code or claim a measured target performance improvement.
"""

from pathlib import Path
import random
import re
import struct
import unittest

from compact_packet_model import Packet, U32


ROOT = Path(__file__).resolve().parents[1]
FORMATS = {'road_x2r': (0, 3, 32), 'pool_x2p': (0, 4, 24),
           'billboard_x2b': (48, 3, 32), 'billboard_x2a': (48, 4, 24)}
STCYCL, STMOD, FLUSH = 0x01000101, 0x05000000, 0x11000000
MSCAL, FLUSHE, MSCNT = 0x14000000, 0x10000000, 0x17000000
BASE, OFFSET = 0x0300004f, 0x020001d8


def unpack(packet, address, source, relative=False):
    assert len(source) % 16 == 0
    packet.pad(12)
    packet.word(0x6c000000 | (len(source) // 16 << 16) | address
                | (0x8000 if relative else 0))
    packet.memory += source


def emit_context(packet, kind, source, raster, first, corner_reuse):
    if kind == 'none':
        return
    packet.tag(1)
    for command in (STCYCL, STMOD, FLUSH):
        packet.word(command)
    if kind == 'tail':
        unpack(packet, 55, source[-32:])
    else:
        cull = bytes(12) + raster[84:88]
        owned = bytearray(source)
        if first:
            U32.pack_into(owned, 5 * 16 + 12, corner_reuse)
            unpack(packet, 0, cull)
            unpack(packet, first + 1, owned)
        else:
            unpack(packet, 0, cull + owned)
        unpack(packet, 57, bytes(12) + raster[64:68])
        unpack(packet, 62, raster[:64])
        unpack(packet, 75, raster[92:108] + raster[68:80] + raster[88:92]
               + raster[80:84] + bytes(28))
        for command in (MSCAL, FLUSHE, BASE, OFFSET):
            packet.word(command)
    packet.close()


def emit_draw(packet, records, words, capacity):
    packet.tag(1)
    packet.word(STCYCL)
    stride = words * 16
    for cursor in range(0, len(records), capacity * stride):
        batch = records[cursor:cursor + capacity * stride]
        unpack(packet, 5, batch, True)
        unpack(packet, 0, struct.pack('<4I', len(batch) // stride, 0, 0, 0), True)
        packet.word(MSCNT)
        packet.pad(0)
    packet.close()


class Interpreter:
    def __init__(self):
        self.memory = bytearray([0xa5] * (1024 * 16))
        self.tops = 79
        self.matrix = None
        self.near = None
        self.busy = False
        self.halves = set()

    def execute(self, data, first, words):
        # Walk actual source-chain CNT/TTE bytes, independently of emission.
        stream = bytearray()
        cursor = 0
        while cursor < len(data):
            control, address = struct.unpack_from('<2I', data, cursor)
            assert control >> 28 == 1 and address == 0
            size = ((control & 65535) + 1) * 16
            stream += data[cursor + 8:cursor + size]
            cursor += size
        assert cursor == len(data)
        cursor = 0
        snapshots = []
        while cursor < len(stream):
            command = U32.unpack_from(stream, cursor)[0]
            cursor += 4
            if command >> 24 == 0x6c:
                count = ((command >> 16) & 255) or 256
                address = command & 1023
                if command & 0x8000:
                    address += self.tops
                else:
                    assert not self.busy, 'context overwrite before VU completion'
                    assert address + count <= 79
                size = count * 16
                self.memory[address * 16:(address + count) * 16] = stream[cursor:cursor + size]
                cursor += size
            elif command in (FLUSH, FLUSHE):
                self.busy = False
            elif command == MSCAL:
                self.matrix = bytes(self.memory[62 * 16:66 * 16])
                self.near = bytes(self.memory[77 * 16:77 * 16 + 4])
                self.tops = 551 if self.tops == 79 else 79
                self.busy = True
            elif command == OFFSET:
                self.tops = 79
            elif command == MSCNT:
                self.halves.add(self.tops)
                count = U32.unpack_from(self.memory, self.tops * 16)[0]
                begin = (self.tops + 5) * 16
                read_context = bytes(self.memory[:16]
                    + self.memory[(first + 1) * 16:58 * 16]
                    + self.memory[62 * 16:66 * 16]
                    + self.memory[75 * 16:79 * 16])
                snapshots.append((read_context, self.matrix, self.near,
                                  bytes(self.memory[begin:begin + count * words * 16])))
                self.tops = 551 if self.tops == 79 else 79
                self.busy = True
            else:
                assert command in (0, STCYCL, STMOD, BASE), hex(command)
        assert cursor == len(stream)
        return snapshots


def choose(previous, source, old_raster, raster, old_owner, owner, reuse, patch):
    if previous is None or old_owner != owner or old_raster != raster:
        return 'full'
    if reuse and source == previous:
        return 'none'
    if patch and source[:-32] == previous[:-32]:
        return 'tail'
    return 'full'


class SourceContextTailTests(unittest.TestCase):
    def test_encoded_sequences_all_formats_gates_and_buffer_halves(self):
        rng = random.Random(0x5441494c)
        cases = patches = 0
        for name, (first, words, capacity) in FORMATS.items():
            for reuse in (False, True):
                for patch in (False, True):
                    for corner_reuse in (0, 1):
                        old = Interpreter()
                        new = Interpreter()
                        source = rng.randbytes((56 - first) * 16)
                        raster = rng.randbytes(108)
                        previous = old_raster = old_owner = None
                        owner = (1, 2, 3, 4, 5)  # packet/base/end/frame/renderer
                        for step in range(80):
                            if step % 8 in (1, 2, 3):
                                source = source[:-32] + rng.randbytes(32)
                            elif step % 8 == 4:
                                source = rng.randbytes(len(source))
                            elif step % 8 == 5:
                                raster = rng.randbytes(108)
                            elif step % 8 == 6:
                                owner = (*owner[:3], owner[3] + 1, owner[4])
                            kind = choose(previous, source, old_raster, raster,
                                          old_owner, owner, reuse, patch)
                            baseline = choose(previous, source, old_raster, raster,
                                              old_owner, owner, reuse, False)
                            records = rng.randbytes((1 + step % (capacity * 3)) * words * 16)
                            left, right = Packet(), Packet()
                            emit_context(left, baseline, source, raster, first, corner_reuse)
                            emit_context(right, kind, source, raster, first, corner_reuse)
                            patches += kind == 'tail'
                            emit_draw(left, records, words, capacity)
                            emit_draw(right, records, words, capacity)
                            self.assertEqual(old.execute(left.memory, first, words),
                                             new.execute(right.memory, first, words),
                                             (name, reuse, patch, corner_reuse, step))
                            previous, old_raster, old_owner = source, raster, owner
                            cases += 1
                        self.assertEqual(new.halves, {79, 551})
        self.assertEqual(cases, 2560)
        self.assertEqual(patches, 632)

    def test_every_input_word_and_owner_change(self):
        raster = bytes(108)
        owner = (1, 2, 3, 4, 5)
        for first, _, _ in FORMATS.values():
            source = bytes((56 - first) * 16)
            for word in range(len(source) // 4):
                changed = bytearray(source)
                U32.pack_into(changed, word * 4, 0x80000000)  # signed zero differs
                expect = 'tail' if word * 4 >= len(source) - 32 else 'full'
                self.assertEqual(choose(source, changed, raster, raster,
                                        owner, owner, True, True), expect)
            for word in range(27):
                changed = bytearray(raster)
                U32.pack_into(changed, word * 4, 0x7fc12345)
                self.assertEqual(choose(source, source, raster, changed,
                                        owner, owner, True, True), 'full')
            for field in range(len(owner)):
                changed = list(owner)
                changed[field] += 1
                self.assertEqual(choose(source, source, raster, raster,
                                        owner, tuple(changed), True, True), 'full')

    def test_owned_tail_bytes_and_exact_packet_sizes(self):
        sizes = {}
        special = struct.pack('<8I', 0, 0x80000000, 0x3f800000, 0xbf800000,
                              0x7f800000, 0xff800000, 0x7fc00001, 0x7f800001)
        for name, (first, _, _) in FORMATS.items():
            source = bytearray((56 - first) * 16)
            source[-32:] = special
            full, tail = Packet(), Packet()
            emit_context(full, 'full', source, bytes(108), first, 1)
            emit_context(tail, 'tail', source, bytes(108), first, 1)
            source[:] = bytes([0xcc]) * len(source)
            self.assertEqual(tail.memory[-32:], special)
            self.assertEqual(len(tail.memory), 64)
            sizes[name] = len(full.memory)
        self.assertEqual(sizes, {'road_x2r': 1152, 'pool_x2p': 1152,
                                 'billboard_x2b': 400, 'billboard_x2a': 400})

    def test_generated_entry_registers_are_live_across_continuation(self):
        for name in FORMATS:
            text = (ROOT / f'vu1/general_clip_{name}_vcl.vsm').read_text()
            entry, body = text.split('r_main_lid:', 1)
            for register, address in zip(range(1, 5), range(62, 66)):
                self.assertRegex(entry, rf'lq\s+VF{register:02},{address}\(VI00\)')
            self.assertRegex(body, r'b\s+r_main_lid')
            self.assertNotRegex(body, r'\[E\][^\n]*\bxgkick\b')
            for line in body.splitlines():
                if not line.startswith(' '):
                    continue
                destinations = re.findall(
                    r'\b(\w+)(?:\[E\])?(?:\.([xyzw]+))?\s+(VF\d+),', line, re.I)
                for opcode, mask, register in destinations:
                    if opcode.lower().startswith('sq'):
                        continue
                    self.assertNotIn(register, ('VF01', 'VF02', 'VF03', 'VF04'), line)
                    if name == 'road_x2r' and register == 'VF05':
                        self.assertTrue(mask and 'w' not in mask, line)
            # All absolute stores would threaten retained context; these
            # programs keep their writable arenas relative to the input half.
            self.assertNotRegex(body, r'\b(?:sq|isw)(?:\.[xyzw]+)?\s+[^\n]*\(VI00\)')

    def test_source_ownership_and_retained_context_contract(self):
        text = (ROOT / 'src/x2r_renderer.cpp').read_text()
        proof = text.split('if (RetainedContextValid', 1)[1].split('if (RoadContextUnchanged)', 1)[0]
        for guard in ('RetainedPacket == &packet', 'RetainedBase == packet.GetBase()',
                      'RetainedEnd == packet.GetNextPtr()',
                      'RetainedFrame == pGLContext->GetFrameNumber()',
                      'GetCurRenderer() == this',
                      'memcmp(RetainedRaster, rasterKey, sizeof(rasterKey)) == 0'):
            self.assertIn(guard, proof)
        patch = text.split('if (SourcePrefixUnchanged) {', 2)[2].split('return;', 1)[0]
        self.assertLess(patch.index('packet.Flush();'), patch.index('packet.OpenUnpack('))
        self.assertIn('packet.Stcycl(1, 1);', patch)
        self.assertIn('packet.Stmod(Vifs::AddModes::kNone);', patch)
        self.assertIn('55, Packet::kSingleBuff', patch)
        self.assertIn('pglAddOwnedPayload(packet, RoadContext.color, 8u);', patch)
        for forbidden in ('packet.Ref(', 'packet.Mscal(', 'packet.Base(', 'packet.Offset('):
            self.assertNotIn(forbidden, patch)
        self.assertIn('RetainedContextValid = false;', text.split('void CClipRoadX2RRenderer::Load()', 1)[1])
        self.assertIn('RetainedFrame = pGLContext->GetFrameNumber();', text)


if __name__ == '__main__':
    unittest.main()
