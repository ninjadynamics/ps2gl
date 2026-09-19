#!/usr/bin/env python3
"""Host DMA/VIF model of owned compact payload/header changes.

Independently walks encoded CNT/REF tags with TTE, executes V4_32 UNPACK and
alternating TOP halves, and compares the actual descriptor bytes seen at each
MSCNT. This does not execute EE copies or prove target instruction selection.
"""
import random
import re
import struct
from pathlib import Path

RNG = random.Random(0x58324551)
U32 = struct.Struct('<I')


class Packet:
    def __init__(self):
        self.memory = bytearray()
        self.open_tag = None

    def word(self, value):
        self.memory += U32.pack(value)

    def tag(self, kind, qwords=0, address=0):
        assert len(self.memory) % 16 == 0 and self.open_tag is None
        self.open_tag = len(self.memory) if kind == 1 else None
        self.word((kind << 28) | qwords)
        self.word(address)

    def pad(self, remainder):
        while len(self.memory) % 16 != remainder:
            self.word(0)

    def close(self):
        assert self.open_tag is not None and len(self.memory) % 16 == 0
        count = (len(self.memory) - self.open_tag) // 16 - 1
        assert 0 <= count <= 65535
        U32.pack_into(self.memory, self.open_tag, (1 << 28) | count)
        self.open_tag = None

    def unpack(self, qword_address, count):
        self.pad(12)
        assert 0 < count <= 256
        self.word(0x6C008000 | ((count & 255) << 16) | qword_address)


def sweep(packet, source, compact, reuse, owned=None, format=0):
    count = len(source) // 144
    owned = [] if owned is None else owned
    if compact:
        packet.tag(1)
        packet.word(0x01000101)  # STCYCL(1,1)
    batch_index = 0
    consumed = 0
    while count:
        batch = min(count, 16)
        if not compact:
            packet.tag(1)
            packet.word(0x01000101)
        if reuse:
            packet.pad(0)
            packet.close()
            packet.tag(3, batch * 9, owned[batch_index])
            packet.unpack(5, batch * 9)
            packet.tag(1)
        else:
            packet.unpack(5, batch * 9)
            owned.append(len(packet.memory))
            packet.memory += source[consumed:consumed + batch * 144]
        packet.unpack(0, 1 if compact else 5)
        packet.memory += struct.pack('<4I', batch, format, 0, 0)
        if not compact:
            packet.memory += struct.pack('<16f', 1024.0, *([0.0] * 15))
        packet.word(0x17000000)  # MSCNT
        packet.pad(0)
        if not compact:
            packet.close()
        consumed += batch * 144
        count -= batch
        batch_index += 1
    if compact:
        packet.close()
    return owned


def execute(packet, capture_format=False):
    # Traverse source-chain DMA separately from the packet construction model.
    data = packet.memory
    stream = bytearray()
    cursor = 0
    while cursor < len(data):
        control, address = struct.unpack_from('<2I', data, cursor)
        count = control & 65535
        kind = (control >> 28) & 7
        stream += data[cursor + 8:cursor + 16]  # TTE
        if kind == 1:
            stream += data[cursor + 16:cursor + 16 + count * 16]
            cursor += 16 + count * 16
        else:
            assert kind == 3 and address % 16 == 0
            assert address + count * 16 <= cursor  # owned earlier base bytes
            stream += data[address:address + count * 16]
            cursor += 16
    assert cursor == len(data)
    memory = [bytearray(472 * 16), bytearray(472 * 16)]
    top = 0
    cursor = 0
    snapshots = []
    while cursor < len(stream):
        command = U32.unpack_from(stream, cursor)[0]
        cursor += 4
        if command in (0, 0x01000101):
            continue
        if command == 0x17000000:
            count = U32.unpack_from(memory[top], 0)[0]
            assert 0 < count <= 16
            sample = (count, bytes(memory[top][80:80 + count * 144]))
            if capture_format:
                sample += (U32.unpack_from(memory[top], 4)[0],)
            snapshots.append(sample)
            top ^= 1
            continue
        assert command >> 24 == 0x6C and command & 0x8000
        count = ((command >> 16) & 255) or 256
        start = (command & 1023) * 16
        assert start + count * 16 <= 472 * 16
        memory[top][start:start + count * 16] = stream[cursor:cursor + count * 16]
        cursor += count * 16
    assert cursor == len(stream)
    return snapshots


def check_packets():
    comparisons = 0
    savings = {}
    counts = [1, 15, 16, 17, 31, 32, 127, 128, 511]
    counts += [RNG.randrange(1, 512) for _ in range(120)]
    for count in counts:
        source = RNG.randbytes(count * 144)
        expected = [(min(count - i, 16), source[i * 144:min(i + 16, count) * 144])
                    for i in range(0, count, 16)]
        for use_reuse in (False, True):
            lengths = []
            for compact in (False, True):
                packet = Packet()
                owned = sweep(packet, source, compact, False)
                # Variable intervening GS setup must not become a stride-based REF.
                packet.tag(1)
                packet.pad(0)
                packet.memory += bytes(RNG.randrange(0, 24) * 16)
                packet.close()
                gap = len(packet.memory)
                sweep(packet, source, compact, use_reuse, owned if use_reuse else None)
                assert execute(packet) == expected * 2
                assert all(address < gap for address in owned)
                lengths.append(len(packet.memory))
                comparisons += 1
            if count == 128:
                # Padding gaps differ deliberately, so only report payload/header
                # representation savings, not this complete synthetic chain size.
                savings[str(use_reuse)] = 8 * 64 * 2
    return comparisons, savings


def check_copies():
    cases = 0
    for source_align in range(0, 16, 4):
        for destination_align in range(0, 16, 4):
            for words in [0, 1, 2, 3, 4, 7, 12, 16, 36, 384, 576, 2304]:
                source = bytes([0xA7] * source_align) + RNG.randbytes(words * 4)
                expected = source[source_align:]
                for gate in (False, True):
                    destination = bytearray([0xD5] * (destination_align + words * 4 + 16))
                    stride = 16 if gate and not ((source_align | destination_align) & 15) \
                        and not (words & 3) else 4
                    for offset in range(0, words * 4, stride):
                        destination[destination_align + offset:destination_align + offset + stride] = \
                            source[source_align + offset:source_align + offset + stride]
                    assert destination[destination_align:destination_align + words * 4] == expected
                    assert destination[:destination_align] == bytes([0xD5] * destination_align)
                    assert destination[-16:] == bytes([0xD5] * 16)
                    cases += 1
    return cases


def source_contracts():
    root = Path(__file__).resolve().parents[1]
    helper = (root / 'include/ps2gl/owned_payload.h').read_text()
    decal = (root / 'src/x2e_renderer.cpp').read_text()
    road = (root / 'src/x2r_renderer.cpp').read_text()
    vu = (root / 'vu1/general_clip_decal_x2e.vcl').read_text()
    assert 'struct __attribute__((aligned(16), may_alias)) PglOwnedQword' in helper
    assert 'packet.Add(' in helper and 'packet.ReserveWords(' not in helper
    assert 'return packet.Add(source, floatCount);' in helper
    assert '(floatCount & 3u) == 0u' in helper
    assert 'pglAddOwnedPayload(packet' in decal and 'pglAddOwnedPayload(packet' in road
    assert '#if !PGL_DECAL_HEADER_COMPACT\n        packet.Add(independentAdc, 16);' in decal
    reads = re.findall(r'\b(?:ilw\.\w+|lq)\s+\w+,\s*([0-4])\(buffer_top\)', vu)
    assert reads and set(reads) == {'0'}, reads
    assert 'iaddiu setup_ptr, buffer_top, 5' in vu
    assert 'packet.Ref(Core::MakePtrNormal(ownedPayloads[batchIndex])' in decal
    return 10


if __name__ == '__main__':
    packets, _ = check_packets()
    print(f'PASS: {packets} encoded DMA/VIF sweeps, {check_copies()} bit-copy/alignment cases, '
          f'{source_contracts()} source contracts. EE compilation/instruction selection and hardware pending.')
