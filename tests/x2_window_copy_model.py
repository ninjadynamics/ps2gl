#!/usr/bin/env python3
"""Check the X2 copy-by-three candidate against a pinned original X2 image.

Word-copy checks execute the scheduled loop, including its branch delay slot.
Whole-program checks reuse the float32 dependency model; they are not a VU/GS
timing or pixel emulator and do not establish a hardware performance gain.
"""

import json
import random
import re
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from x2f_source_quad_model import F, LANES, Program, ROOT, reg


# Original 806-pair canonical body, before either copy-by-three or near-skip.
# Pin source and generated image together: HEAD would silently move the oracle
# to the candidate after a commit. The near-identity model shares this reader.
X2_REFERENCE_COMMIT = '9dd42ad2b6c9abf364a406fd830d5faa77304fcf'


def baseline(name):
    return subprocess.check_output(
        ['git', 'show', X2_REFERENCE_COMMIT + ':vu1/' + name], cwd=ROOT).decode('utf-8')


def source_contract():
    source = (ROOT / 'vu1/general_clip_tri_x2.vcl').read_text()
    original = baseline('general_clip_tri_x2.vcl')
    pattern = r'\.macro\s+x2_kick_chunk\s.*?\.endm'
    new_macro = re.search(pattern, source, re.S)[0]
    old_macro = re.search(pattern, original, re.S)[0]
    off_macro = re.sub(
        r'^ *#if PGL_X2_WINDOW_COPY_TRIANGLES\n.*?^ *#else\n(.*?)^ *#endif\n',
        r'\1', new_macro, flags=re.S | re.M)
    assert off_macro == old_macro, 'OFF must retain the original macro verbatim'
    # All output commits are complete triangles. Other appearances are reads.
    writes = re.findall(r'^\s*(i\w+)\s+out_count,\s*([^;\n]+)', source, re.M)
    assert [(op, args.strip()) for op, args in writes] == [
        ('iaddiu', 'vi00, 0'), ('iaddiu', 'vi00, 0'),
        ('iaddiu', 'out_count, 3'), ('iaddiu', 'out_count, 3')]
    for decoder in ('x2d', 'x2q', 'x2c', 'x2g'):
        stem = 'general_clip_glow_' if decoder == 'x2g' else 'general_clip_tri_'
        text = (ROOT / ('vu1/' + stem + decoder + '_decode.vcl')).read_text()
        assert 'out_count' not in text
    gate = (ROOT / 'vu1/x2_window_copy_gate.h').read_text()
    selected = re.search(r'^#define PGL_X2_WINDOW_COPY_TRIANGLES ([01])$', gate, re.M)
    assert selected
    make = (ROOT / 'Makefile').read_text()
    assert 'general_clip_tri_x2_pp4.vcl: vu1/general_clip_tri_x2_pp3.vcl $(X2_WINDOW_COPY_GATE)' in make
    assert '-imacros $(X2_WINDOW_COPY_GATE)' in make
    guard_rule = make.split('vu1/general_clip_tri_x2.vo:', 1)[1].split('\n\n', 1)[0]
    assert guard_rule.index('python3 $(X2D_GUARD)') < guard_rule.index('dvp-as')
    assert '$(X2D_DECODER_VSM)' not in guard_rule.splitlines()[0]
    return int(selected.group(1))


def copy_kernel(program, label, top, arena, count, seed):
    """Exact raw-word execution; no floating-point conversion of copied data."""
    rng = random.Random(seed)
    memory = [[rng.getrandbits(32) for _ in range(4)] for _ in range(1024)]
    vf = [[rng.getrandbits(32) for _ in range(4)] for _ in range(32)]
    vi = [0] * 16
    start = program.labels[label]
    branch_pc = next(pc for pc in range(start, len(program.code))
        if program.code[pc][1][0] == 'ibgtz'
        and program.code[pc][1][2][-1] == label)
    stop = branch_pc + 2
    block = program.code[start:stop]
    src_reg = reg(next(p[1][2][1].split('(')[1] for p in block if p[1][0] == 'lq'))
    dst_reg = reg(next(p[1][2][1].split('(')[1] for p in block if p[1][0] == 'sq'))
    count_reg = reg(program.code[branch_pc][1][2][0])
    color_reg = reg(next(p[0][2][1] for p in block if p[0][0] == 'max'))
    source = top + arena + 1
    destination = source + count * 3 + 1
    limit = top + (362 if arena == 180 else 472)
    assert destination + count * 3 <= limit
    vi[src_reg], vi[dst_reg], vi[count_reg] = source, destination, count
    color = [0, 128, 255, 127]
    vf[color_reg] = color[:]
    original = [v[:] for v in memory]
    expected = [v[:] for v in memory]
    for vertex in range(count):
        expected[destination + vertex * 3] = original[source + vertex * 3][:]
        expected[destination + vertex * 3 + 1] = color[:]
        expected[destination + vertex * 3 + 2] = original[source + vertex * 3 + 2][:]
    pc, pending, issued = start, None, 0
    last_load = {}
    while pc < stop:
        old_vf, old_vi = [v[:] for v in vf], vi[:]
        writes = []
        branch = None
        for op, mask, args in program.code[pc]:
            lanes = [LANES.index(c) for c in mask]
            if op == 'nop':
                continue
            if op == 'max':
                assert args[1] == args[2], 'only identical-input MOVE admitted'
                writes.append((reg(args[0]), lanes, old_vf[reg(args[1])][:]))
            elif op in ('lq', 'sq'):
                offset, base = re.fullmatch(r'(-?\d+)\(VI(\d+)\)', args[1]).groups()
                address = int(offset) + old_vi[int(base)]
                if op == 'lq':
                    assert source <= address < source + count * 3
                    writes.append((reg(args[0]), lanes, memory[address][:]))
                    last_load[reg(args[0])] = issued
                else:
                    assert destination <= address < destination + count * 3
                    if reg(args[0]) in last_load:
                        assert issued - last_load[reg(args[0])] >= 4, ('LQ/SQ seam', pc)
                    for lane in lanes:
                        memory[address][lane] = old_vf[reg(args[0])][lane]
            elif op in ('iaddiu', 'isubiu'):
                value = int(args[2], 0) * (-1 if op == 'isubiu' else 1)
                vi[reg(args[0])] = (old_vi[reg(args[1])] + value) & 65535
            elif op == 'ibgtz':
                if old_vi[reg(args[0])] > 0:
                    branch = program.labels[args[-1]]
            else:
                raise AssertionError(('unexpected copy instruction', pc, op))
        for dst, lanes, value in writes:
            for lane in lanes:
                vf[dst][lane] = value[lane]
        next_pc = pending if pending is not None else pc + 1
        assert pending is None or branch is None
        pending = branch
        pc = next_pc
        issued += 1
        assert issued < 1000
    assert memory == expected, (label, top, count)
    assert vi[src_reg] == source + count * 3
    assert vi[dst_reg] == destination + count * 3
    assert vi[count_reg] == 0
    return issued


def fixture(top, shape, paired=True, count=30):
    memory = np.zeros((1024, 4), dtype=F)
    memory[57, 3] = 32767
    memory[62:66] = [[180, 0, 0, 0], [0, 150, 0, 0],
        [0, 0, -32767, 1], [0, 0, 32767, 0]]
    memory[75].view(np.uint32)[:] = [0x8000, 0x30000000, 0x412, 0]
    memory[76] = [1, 1, 1, 0]
    memory[77, 0] = 1
    memory[top + 178] = [64, 96, 128, 127] if paired else [-1, 0, 0, 0]
    memory[top + 179].view(np.uint32)[:] = [0x8000, 0x30000000, 0x412, 0]
    memory[top].view(np.uint32)[0] = count
    for vertex in range(count):
        corner = (0, 1, 2, 0, 2, 3)[vertex % 6]
        qword = top + 5 + vertex * 4
        memory[qword, :3] = shape[corner]
        memory[qword + 2] = [corner * .25, .5, 1, 0]
        memory[qword + 3] = [.2 + vertex / 40, .7, .9, .5]
    return memory


def run_packets(program, memory, top, fan_size=None):
    packets = []
    fans = [0]
    def trace(vf, vi, mem, pc, steps):
        if fan_size and pc == program.labels['x2_fan_cap_lid']:
            op, _, args = program.code[pc][1]
            assert op == 'isubiu' and int(args[-1], 0) == 2
            vi[reg(args[1])] = fan_size
            for vertex in range(fan_size):
                qword = top + 154 + vertex * 3
                mem[qword] = [vertex * .07 - .2, vertex * .03 - .1, .5, 2]
                mem[qword + 1] = [vertex * .1, .4, 1, 0]
                mem[qword + 2] = [.2 + vertex * .03, .7, .4, .6]
            fans[0] += 1
        op, _, args = program.code[pc][1]
        if op == 'xgkick':
            address = int(vi[reg(args[0])])
            word = int(mem[address].view(np.uint32)[0])
            count = word & 0x7fff
            assert count % 3 == 0
            end = address + 1 + count * 3
            if not word & 0x8000:
                window_word = int(mem[end].view(np.uint32)[0])
                assert window_word & 0x8000 and window_word & 0x7fff == count
                end += 1 + count * 3
            packets.append((address, count, mem[address:end].tobytes()))
    before = memory.copy()
    output, issued = program.run(memory, top, instruction_trace=trace, compound_windows=True)
    assert np.array_equal(memory[:top + 125], before[:top + 125])
    assert np.array_equal(memory[top + 178:top + 180], before[top + 178:top + 180])
    assert np.array_equal(memory[top + 472:], before[top + 472:])
    if fan_size:
        assert fans[0] > 0
    return packets, output.tobytes(), issued, program.max_arenas[:]


def check(old_path):
    enabled = source_contract()
    gate_text = (ROOT / 'vu1/x2_window_copy_gate.h').read_text()
    near_gate = re.search(r'^#define PGL_X2_SKIP_IDENTITY_NEAR ([01])$', gate_text, re.M)
    assert near_gate, 'missing independent near-skip gate'
    near_enabled = int(near_gate.group(1))
    old = Program(old_path)
    new = Program(ROOT / 'vu1/general_clip_tri_x2_vcl.vsm')
    assert new.code[:6] == old.code[:6], 'shared decoder entry prologue changed'
    old_labels = [label for label in old.labels if label.startswith('x2_wcopy_lid')]
    new_labels = [label for label in new.labels if label.startswith('x2_wcopy_lid')]
    assert len(old_labels) == len(new_labels) == 3
    kernel_cases = 0
    ratios = set()
    for top in (79, 551):
        for arena, capacity in ((180, 30), (362, 18)):
            for count in range(3, capacity + 1, 3):
                for seed in range(8):
                    for old_label, new_label in zip(old_labels, new_labels):
                        a = copy_kernel(old, old_label, top, arena, count, seed)
                        b = copy_kernel(new, new_label, top, arena, count, seed)
                        ratios.add((a // (count // 3), b // (count // 3)))
                        kernel_cases += 1
    assert ratios == {(27, 19 if enabled else 27)}, ('gate/schedule mismatch', ratios)
    inside = [[-.2, -.2, 2], [.2, -.2, 2], [.2, .2, 2], [-.2, .2, 2]]
    clipped = [[-300, -300, 20], [300, -300, 20], [300, 300, 20], [-300, 300, 20]]
    outside = [[3000, 3000, 2], [3100, 3000, 2], [3100, 3100, 2], [3000, 3100, 2]]
    cases, paired_saving, wall_saving, peaks = 0, set(), set(), [0, 0]
    for top in (79, 551):
        for paired in (False, True):
            for shape, fan, count in [(inside, None, 30), (inside, None, 5),
                    (outside, None, 30), (clipped, None, 30)] + [
                    (clipped, n, 30) for n in range(3, 9)]:
                a = run_packets(old, fixture(top, shape, paired, count), top, fan)
                b = run_packets(new, fixture(top, shape, paired, count), top, fan)
                assert a[:2] == b[:2], ('whole packet mismatch', top, paired, fan, count)
                assert a[3] == b[3]
                if paired:
                    emitted = sum(p[1] for p in a[0])
                    if not near_enabled:
                        assert a[2] - b[2] == emitted // 3 * (8 if enabled else 0)
                    paired_saving.add(a[2] - b[2])
                    peaks = [max(x, y) for x, y in zip(peaks, b[3])]
                else:
                    wall_saving.add(a[2] - b[2])
                cases += 1
    assert peaks == [30, 18], peaks
    if not near_enabled:
        assert wall_saving == {0}, wall_saving
    assert new.labels['main_loop_lid'] == old.labels['main_loop_lid'] == 6
    cost_assertion = ('skipped: near-skip ON independently changes whole-program issued pairs; '
                      'isolated copy-kernel cost and all correctness checks still run'
                      if near_enabled else 'checked against isolated copy-only savings')
    return dict(gate=enabled, near_gate=near_enabled,
        kernel_cases=kernel_cases, issue_pairs_per_triangle=sorted(ratios),
        full_program_cases=cases, full_arenas=peaks,
        whole_program_copy_cost_assertion=cost_assertion,
        wall_only_issue_pair_change=sorted(wall_saving),
        paired_issue_pairs_saved=sorted(paired_saving),
        result='exact complete packets, tags, word order and ownership match',
        limitation='host model; hardware performance and visuals pending')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        old_path = Path(directory) / 'general_clip_tri_x2_vcl.vsm'
        old_path.write_text(baseline('general_clip_tri_x2_vcl.vsm'), newline='\n')
        with np.errstate(all='ignore'):
            print(json.dumps(check(old_path), indent=2))
