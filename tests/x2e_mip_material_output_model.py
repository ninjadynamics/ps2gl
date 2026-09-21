"""Execute native X2E material/arena control with ordered symbolic vertices.

The existing projected model covers the actual vertex math. This model runs
the emitted setup, fan admission, descriptor advance and XGKICK instructions;
symbolic vertices isolate the new ordering, packet and ownership contract.
It assumes documented VU pipeline interlocks and XGKICK ordering, not timing.
"""
import json
import numpy as np
from x2e_projected_model import F, run


class Output:
    def __init__(self):
        self.active = None
        self.vertices = []
        self.kicks = 0
        self.prefixes = 0
        self.empty = 0
        self.regional = False
        self.top = 0

    def before_store(self, address):
        assert self.active is None or not self.active[0] <= address < self.active[1], (
            'write before prior PATH1 completion', address, self.active)

    def kick(self, address, memory):
        # The new XGKICK stalls until its predecessor finishes. Until this
        # exact instruction, before_store protects every predecessor qword.
        self.active = None
        start = address
        words = memory.view(np.uint32)
        tag = words[address]
        cell = None
        if not int(tag[0]) & 0x8000:
            assert self.regional
            assert tag.tolist() == [1, 0x10000000, 8, 0], tag
            clamp = int(words[address + 1, 0]) | (int(words[address + 1, 1]) << 32)
            assert np.all(words[address + 1, 2:] == 0)
            minimum = (clamp >> 24) & 1023
            maximum = (clamp >> 34) & 1023
            assert minimum in (0, 32, 64, 96) and maximum == minimum + 31
            assert clamp == 8 | (minimum << 24) | (maximum << 34)
            cell = minimum // 32
            address += 2
            tag = words[address]
            self.prefixes += 1
        count = int(tag[0]) & 0x7fff
        assert tag.tolist() == [0xffff8000 | count, 0x30004000, 0x412, 0], tag
        allowed = {self.top + 224: 30, self.top + (317 if self.regional else 315): 18}
        assert address in allowed and count <= allowed[address]
        assert count % 3 == 0
        assert bool(count and self.regional) == (cell is not None)
        if not count:
            self.empty += 1
        for index in range(count):
            payload = words[address + 1 + index * 3:address + 4 + index * 3]
            token = int(payload[0, 0])
            assert np.all(payload == token), payload
            self.vertices.append((cell, token))
        self.active = (start, address + 1 + count * 3)
        self.kicks += 1


def setup(memory, top, cells, projected, regional, hooks):
    low = sum(cell << (index * 2) for index, cell in enumerate(cells[:8]))
    high = sum(cell << (index * 2) for index, cell in enumerate(cells[8:]))
    memory[top].view(np.uint32)[:] = [len(cells), projected | (2 if regional else 0), low, high]
    vf = np.zeros((32, 4), dtype=F)
    vf[0] = [0, 0, 0, 1]
    vi = np.zeros(16, dtype=np.int32)
    vi[2] = top
    run(memory, vf, vi, 'e_main_lid', 'e_decode_lid', hooks=hooks)
    assert memory[top + 220].view(np.uint32)[2:].tolist() == [2 if regional else 0, projected]
    if regional:
        assert memory[top + 372].view(np.uint32)[:2].tolist() == [4, 8]
        assert memory[top + 372, 2:].tolist() == [low, high]
    return vf, vi


def check():
    rng = np.random.default_rng(0xE_C1A)
    hooks = Output()
    memory = np.zeros((1024, 4), dtype=F)
    words = memory.view(np.uint32)
    words[9] = [1, 0x10000000, 8, 0]
    for cell in range(4):
        clamp = 8 | ((cell * 32) << 24) | ((cell * 32 + 31) << 34)
        words[10 + cell] = [clamp & 0xffffffff, clamp >> 32, 0, 0]
    words[75] = [0x8000, 0x30004000, 0x412, 0]
    memory[5] = [320, -224, -32767.5, 0.01]
    memory[8] = [1, 1, 0, 0]
    expected = []
    token = descriptors = fans = 0
    activations = 0
    # Every length and both formats, with all-cell, alternating-cell, bank
    # boundary and random patterns. Empty activations between busy ones prove
    # that the unchanged final EOP1 kick remains a required ownership fence.
    for regional in (False, True):
        for projected in (0, 1):
            for count in range(1, 17):
                for pattern in range(8):
                    top = 79 if activations % 2 == 0 else 551
                    hooks.top, hooks.regional = top, regional
                    cells = ([pattern] * count if pattern < 4 else
                             [index % 4 for index in range(count)] if pattern == 4 else
                             [3 if index < 8 else 1 for index in range(count)] if pattern == 5 else
                             rng.integers(0, 4, count).tolist())
                    vf, vi = setup(memory, top, cells, projected, regional, hooks)
                    for record, cell in enumerate(cells):
                        if regional:
                            assert int(memory[top + 372, 2]) & 3 == cell
                        for triangle in range(1 if projected else 2):
                            size = (0 if pattern == 7 else 3 if projected else
                                    (18 if pattern == 0 else 3 if pattern == 1 else
                                     int(rng.integers(0, 7)) * 3))
                            if not size:
                                continue
                            words[top + 217, 1] = size // 3 + 2
                            run(memory, vf, vi, 'e_output_begin_lid', 'e_emit_begin_lid', hooks=hooks)
                            cursor, buffered = int(vi[1]), int(vi[4])
                            base = cursor - buffered * 3
                            assert base in (top + 225, top + (318 if regional else 316))
                            cap = 30 if base == top + 225 else 18
                            assert buffered + size <= cap
                            if regional:
                                assert words[top + 372, 0] == cell
                            for vertex in range(size):
                                token += 1
                                for qword in range(cursor + vertex * 3, cursor + vertex * 3 + 3):
                                    hooks.before_store(qword)
                                    words[qword] = token
                                expected.append((cell if regional else None, token))
                            words[top + 215, :2] = [cursor + size * 3, buffered + size]
                            fans += 1
                        run(memory, vf, vi, 'e_next_descriptor_lid', 'e_decode_lid',
                            hooks=hooks, stop_on_end=True)
                        descriptors += 1
                    assert words[top + 214, 1] == 0
                    assert hooks.vertices == expected
                    activations += 1
    # Every possible VI-readable bank: quarter scaling retains exact binary
    # fractions and FTOI0 truncation exposes the next original pair of bits.
    banks = np.arange(65536, dtype=np.uint32)
    values = banks.astype(F)
    for index in range(8):
        decoded = np.trunc(values).astype(np.uint32) & 3
        assert np.array_equal(decoded, (banks >> (index * 2)) & 3)
        values = F(values * F(0.25))
    return {'native_control_activations': activations, 'descriptors': descriptors,
            'nonempty_fans': fans, 'ordered_vertices': token,
            'PATH1_kicks': hooks.kicks, 'CLAMP_prefixes': hooks.prefixes,
            'empty_completion_kicks': hooks.empty, 'decoded_bank_cells': 65536 * 8,
            'prefix_bits_capacity_order_and_inflight_ownership': 'pass',
            'legacy_packets_and_empty_activation_fence': 'pass',
            'hardware_timing_and_performance': 'pending'}


if __name__ == '__main__':
    print(json.dumps(check(), indent=2))
