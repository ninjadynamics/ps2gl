#!/usr/bin/env python3
"""Check X2F scratch relocation and 8/10-quad activation packet equivalence.

Uses the earlier scheduled X2F image, not a C reimplementation, as the oracle.
This float32 dependency model checks payload, memory ownership and issue work;
it is not a VU arithmetic, DMA timing or GS pixel emulator.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path
import numpy as np
sys.dont_write_bytecode = True
from x2f_source_quad_model import F, Program, ROOT

BASELINE = '33086320aca9023edf4559d27c4edeb7d066ba57'
IMAGE = 'vu1/general_clip_quad_x2f_vcl.vsm'
CONTEXT_READS = {0, 57, 62, 63, 64, 65, 75, 76, 77}
PRIVATE = set(range(1, 23))


class MemoryTrace:
    def __init__(self, top, corners):
        self.top = top
        self.input_end = 5 + corners * 3
        self.written = set()
        self.context_reads = set()
        self.private_reads = set()
        self.private_writes = set()
        self.max_input = 0

    def __call__(self, op, address, lanes, pc, steps):
        write = op in ('sq', 'isw')
        if op == 'ilw':
            lanes = lanes[:1]
        if address < 79:
            if address in PRIVATE:
                target = self.private_writes if write else self.private_reads
                target.add(address)
                for lane in lanes:
                    key = address, lane
                    if write:
                        self.written.add(key)
                    elif lane != 3 or address not in (9, 12, 15, 18):
                        # STQ.w is intentionally unwritten/unconsumed by GS.
                        assert key in self.written, ('uninitialized private scratch', key, pc)
            else:
                assert not write, ('retained/global context overwritten', address, pc)
                assert address in CONTEXT_READS, ('unexpected context read', address, pc)
                self.context_reads.add(address)
        else:
            relative = address - self.top
            assert 0 <= relative < 472, ('wrong double-buffer half', address, pc)
            if write:
                assert 125 <= relative < 472, ('VIF input overwritten', relative, pc)
            elif 5 <= relative < 125:
                assert relative < self.input_end, ('input read past activation', relative, pc)
                self.max_input = max(self.max_input, relative)


def run_stream(program, limit, positions, uv, colors, top, wide, dispatch, trace):
    pieces = []
    stats = dict(activations=0, issue_pairs=0, kicks=0)
    reads, writes, contexts = set(), set(), set()
    maximum_input = 0
    # Keep memory across activations; private scratch starts poisoned and the
    # opposite half contains a distinct sentinel. Either XTOP may be first.
    memory = np.full((1024, 4), F(-12345.75))
    memory[0] = 0
    memory[57] = [0, 0, 0, 32767]
    memory[62:66] = [[180, 0, 0, 0], [0, 150, 0, 0], [0, 0, -32767, 1], [0, 0, 32767, 0]]
    memory[75:79] = 0
    memory[75].view(np.uint32)[0] = 0x8000
    memory[76] = [1, 1, 1, 0]
    memory[77, 0] = 1
    context_before = memory[list(CONTEXT_READS)].tobytes()
    for first in range(0, len(positions), limit):
        count = min(limit, len(positions) - first)
        for corner in range(count * 4):
            quad = first + corner // 4
            index = corner % 4
            q = top + 5 + corner * 3
            memory[q, :3] = positions[quad, index]
            memory[q + 1] = [*uv[quad, index], 1, 0]
            memory[q + 2] = colors[quad, index]
        memory[top].view(np.uint32)[0] = count * 4
        memory[top + 178] = [-1, 60 if wide else 30, 36 if wide else 18, int(dispatch)]
        opposite = 551 if top == 79 else 79
        other_before = memory[opposite:opposite + 472].tobytes()
        audit = MemoryTrace(top, count * 4) if trace else None
        output, work = program.run(memory, top, wide=wide, memory_trace=audit)
        output[0::3, 3] = 0
        pieces.append(output)
        stats['activations'] += 1
        stats['issue_pairs'] += work
        stats['kicks'] += program.kicks
        assert memory[list(CONTEXT_READS)].tobytes() == context_before
        assert memory[opposite:opposite + 472].tobytes() == other_before
        if audit:
            reads |= audit.private_reads
            writes |= audit.private_writes
            contexts |= audit.context_reads
            maximum_input = max(maximum_input, audit.max_input)
        top = opposite
    return np.concatenate(pieces), stats, (reads, writes, contexts, maximum_input)


def check():
    before = subprocess.check_output(['git', '-C', str(ROOT), 'show', BASELINE + ':' + IMAGE])
    with tempfile.TemporaryDirectory(prefix='x2f-reference-') as directory:
        baseline_path = Path(directory) / 'before.vsm'
        baseline_path.write_bytes(before)
        old, new = Program(baseline_path), Program(ROOT / IMAGE)
    rng = np.random.default_rng(0xF210)
    totals = [{k: 0 for k in ('activations', 'issue_pairs', 'kicks')} for _ in range(3)]
    variants = [(old, 8), (new, 8), (new, 10)]
    reads, writes, contexts = set(), set(), set()
    maximum_input = 0
    cases = 0
    for sample in range(48):
        # Includes the newly admitted ninth/tenth quads, exact multiples and
        # partial final activations. Both starting halves, every prefix mode.
        count = (1, 4, 8, 9, 10, 11, 16, 19, 20, 21, 30, 31)[sample % 12]
        positions = rng.uniform(-100, 100, (count, 4, 3)).astype(F)
        positions[:, :, 2] += 105
        if sample % 4 == 1:
            positions[:, :, 2] = rng.uniform(-5, 5, (count, 4)).astype(F)
        elif sample % 4 == 2:
            # Multiple clipped fans force repeated output arena reuse.
            positions[:, :, :2] *= 10
            positions[:, :, 2] = 20
        else:
            positions[:, :, :2] *= .01
        uv = rng.uniform(-3, 3, (count, 4, 2)).astype(F)
        colors = rng.uniform(0, 1, (count, 4, 4)).astype(F)
        top = 79 if sample % 2 else 551
        for wide, dispatch in ((False, False), (True, False), (False, True), (True, True)):
            outputs = []
            for column, (program, limit) in enumerate(variants):
                output, stats, audit = run_stream(program, limit, positions, uv, colors,
                    top, wide, dispatch, program is new)
                outputs.append(output.tobytes())
                for key, value in stats.items():
                    totals[column][key] += value
                reads |= audit[0]
                writes |= audit[1]
                contexts |= audit[2]
                maximum_input = max(maximum_input, audit[3])
            assert outputs[0] == outputs[1] == outputs[2], ('ordered payload mismatch', sample, wide, dispatch)
            cases += 1
    assert reads == writes == PRIVATE, (reads, writes)
    assert contexts == CONTEXT_READS, contexts
    assert maximum_input == 124, maximum_input
    return dict(baseline=BASELINE, cases=cases,
        variants=['prior X2F 8', 'relocated X2F 8 (gate OFF)', 'relocated X2F 10 (gate ON)'],
        work=totals, instruction_pairs=[len(old.code), len(new.code)],
        context_reads=sorted(contexts), private_scratch=sorted(writes), maximum_input_qword=maximum_input,
        result='ordered consumed payloads identical; context/opposite input half preserved; scratch initialized before consumed reads',
        hardware='pending')


if __name__ == '__main__':
    with np.errstate(all='ignore'):
        print(json.dumps(check(), indent=2))
