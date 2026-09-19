#!/usr/bin/env python3
"""Execute both scheduled X2E decoder branches with simultaneous lane reads.

This checks binary32 operation order, branch delay slots, masked field copies,
the shared scratch store/load seam and context preservation. It is not a VU
arithmetic/timing emulator; native visual acceptance remains a hardware check.
"""
import json
import re
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
F = np.float32
LANES = 'xyzw'
PAIRS, LABELS = [], {}
for raw in (ROOT / 'vu1/general_clip_decal_x2e_vcl.vsm').read_text().splitlines():
    line = raw.split(';', 1)[0]
    if not line.strip() or line.lstrip().startswith('.'):
        continue
    if line.strip().endswith(':'):
        LABELS[line.strip()[:-1]] = len(PAIRS)
    else:
        PAIRS.append([line[:67].strip(), line[67:].strip()])


def decode(text):
    parts = text.split(None, 1)
    name = parts[0].split('.')
    return name[0].lower(), name[1] if len(name) == 2 else LANES, \
        parts[1].split(',') if len(parts) == 2 else []


PARSED = [[decode(inst) for inst in pair] for pair in PAIRS]

# VU User's Manual section3.4.3: same-cycle Upper/Lower writes to one VF
# register conflict even when their lane masks are disjoint.
for pc, (upper, lower) in enumerate(PARSED):
    if lower[0] in ('lq', 'lqi', 'lqd', 'move', 'mfir', 'mr32', 'rget', 'rnext', 'mfp'):
        if upper[2] and upper[2][0].startswith('VF'):
            assert upper[2][0] != lower[2][0], ('Upper/Lower VF destination conflict', pc)


def reg(token):
    match = re.fullmatch(r'VF(\d+)([xyzw]*)', token)
    assert match, token
    return int(match[1]), match[2]


def check_setup():
    """Check the generated masked setup and private control store/load seam."""
    stores = [pc for pc in range(LABELS['e_count_valid_lid'], LABELS['e_decode_lid'])
              if PARSED[pc][1][0:2] == ('isw', 'x') and PARSED[pc][1][2][1] == '220(VI02)']
    assert len(stores) == 1
    control_gap = LABELS['e_decode_lid'] - stores[0]
    assert control_gap >= 4, ('ISW/ILW control seam', control_gap)
    cases = 0
    for top in (79, 551):
        for gate in (0, 1):
            for material in (0, 1):
                for count in (1, 4, 16):
                    memory = np.full((1024, 4), np.nan, dtype=F)
                    memory[5] = [1, 1, 1, 0.125]
                    memory[7] = [1, -2, -3, material]
                    memory[8] = [1.25, 1.5, gate, 0]
                    vf = np.full((32, 4), np.nan, dtype=F)
                    vf[0] = [0, 0, 0, 1]
                    vf[1] = [2047, 2047, -8191, 93]
                    vi = np.zeros(16, dtype=np.int32)
                    vi[1], vi[2] = count, top
                    for pc in range(LABELS['e_count_valid_lid'], LABELS['e_decode_lid']):
                        oldvf, oldvi = vf.copy(), vi.copy()
                        writes = []
                        for op, mask, args in PARSED[pc]:
                            selected = [LANES.index(c) for c in mask]
                            if op == 'nop':
                                continue
                            if op in ('lq', 'sq', 'isw'):
                                match = re.fullmatch(r'(-?\d+)\(VI(\d+)\)', args[1])
                                address = int(match[1]) + oldvi[int(match[2])]
                                if op == 'lq':
                                    writes.append((reg(args[0])[0], selected, memory[address, selected].copy()))
                                elif op == 'sq':
                                    memory[address, selected] = oldvf[reg(args[0])[0], selected]
                                else:
                                    memory[address].view(np.uint32)[selected] = oldvi[int(args[0][2:])]
                                continue
                            if op == 'mtir':
                                vi[int(args[0][2:])] = int(oldvf[reg(args[1])[0]].view(np.uint32)[LANES.index(args[1][-1])]) & 65535
                                continue
                            if op == 'iaddiu':
                                vi[int(args[0][2:])] = (oldvi[int(args[1][2:])] + int(args[2], 0)) & 65535
                                continue
                            a = oldvf[reg(args[1])[0]].copy()
                            if op == 'ftoi0':
                                value = np.zeros(4, dtype=np.int32)
                                value[selected] = np.trunc(a[selected]).astype(np.int32)
                                value = value.view(F)
                            elif op == 'move':
                                value = a
                            else:
                                match = re.fullmatch(r'(add|sub|max)([xyzw]?)', op)
                                assert match, (pc, op)
                                b = oldvf[reg(args[2])[0]].copy()
                                if match[2]:
                                    b = np.full(4, b[LANES.index(match[2])], dtype=F)
                                value = {'add': np.add, 'sub': np.subtract, 'max': np.maximum}[match[1]](a, b)
                            writes.append((reg(args[0])[0], selected, value[selected]))
                        touched = set()
                        for dst, selected, values in writes:
                            for lane in selected:
                                assert (dst, lane) not in touched
                                touched.add((dst, lane))
                            vf[dst, selected] = values
                    assert memory[top + 220].view(np.uint32)[0] == gate
                    assert np.array_equal(memory[top + 214].view(np.uint32), [top + 5, count, 0, material])
                    assert np.array_equal(memory[top + 215].view(np.uint32)[:2], [top + 225, 0])
                    planes = [[0, 0, 1, -0.125], [1, 0, 1.25, 0], [-1, 0, 1.25, 0],
                              [0, 1, 1.5, 0], [0, -1, 1.5, 0]]
                    assert np.array_equal(memory[top + 209:top + 214], np.array(planes, dtype=F))
                    assert np.array_equal(vf[1, :3], [2047, 2047, -8191])
                    assert vi[3] == 0x8000
                    cases += 1
    return cases, control_gap


def run(memory, reuse, top):
    vf = np.full((32, 4), np.nan, dtype=F)
    vf[0] = [0, 0, 0, 1]
    vf[1] = [2047, 2047, -8191, 93]
    vi = np.zeros(16, dtype=np.int32)
    vi[2] = top
    memory[top + 214].view(np.int32)[0] = top + 5
    memory[top + 220].view(np.int32)[0] = reuse
    pc = LABELS['e_decode_lid']
    stop = LABELS['e_quad_classify_lid']
    pending = None
    cycles = 0
    writes = set()
    last_store = {}
    min_reload_gap = 100000
    products = 0
    matrix_loads = 0
    while pc != stop:
        assert cycles < 400, (pc, PAIRS[pc])
        oldvf, oldvi = vf.copy(), vi.copy()
        branch = None
        updates = []
        for op, mask, args in PARSED[pc]:
            selected = [LANES.index(c) for c in mask]
            if op == 'nop':
                continue
            if op == 'b':
                branch = LABELS[args[0]]
                continue
            if op in ('ibeq', 'ibne'):
                equal = oldvi[int(args[0][2:])] == oldvi[int(args[1][2:])]
                if equal if op == 'ibeq' else not equal:
                    branch = LABELS[args[2]]
                continue
            if op in ('lq', 'sq', 'ilw'):
                match = re.fullmatch(r'(-?\d+)\(VI(\d+)\)', args[1])
                assert match, args
                address = int(match[1]) + oldvi[int(match[2])]
                assert 0 <= address < 1024
                if op == 'ilw':
                    vi[int(args[0][2:])] = memory[address].view(np.int32)[selected[0]]
                elif op == 'lq':
                    if 1 <= address <= 4:
                        matrix_loads += 1
                    for lane in selected:
                        if (address, lane) in last_store:
                            gap = cycles - last_store[address, lane]
                            assert gap >= 4, ('SQ/LQ alias seam', pc, address, lane, gap)
                            min_reload_gap = min(min_reload_gap, gap)
                    updates.append((reg(args[0])[0], selected, memory[address, selected].copy()))
                else:
                    memory[address, selected] = oldvf[reg(args[0])[0], selected]
                    for lane in selected:
                        writes.add((address, lane))
                        last_store[address, lane] = cycles
                continue
            destination = reg(args[0])[0]
            if op == 'mr32':
                value = np.roll(oldvf[reg(args[1])[0]], -1)
            else:
                match = re.fullmatch(r'(add|sub|mul|max)([xyzw]?)', op)
                assert match, op
                a = oldvf[reg(args[1])[0]].copy()
                source, suffix = reg(args[2])
                b = oldvf[source].copy()
                if match[2]:
                    assert suffix == match[2], (op, args)
                    b = np.full(4, b[LANES.index(match[2])], dtype=F)
                if match[1] == 'mul':
                    products += 1
                with np.errstate(invalid='ignore', under='ignore'):
                    value = {'add': np.add, 'sub': np.subtract,
                             'mul': np.multiply, 'max': np.maximum}[match[1]](a, b)
            updates.append((destination, selected, value[selected].copy()))
        touched = set()
        for destination, selected, values in updates:
            for lane in selected:
                assert (destination, lane) not in touched
                touched.add((destination, lane))
            vf[destination, selected] = values
        next_pc = pending if pending is not None else pc + 1
        assert pending is None or branch is None, 'branch inside delay slot'
        pending = branch
        pc = next_pc
        cycles += 1
    assert np.array_equal(vf[1, :3], [2047, 2047, -8191])
    assert writes == {(top + qword, lane) for qword in range(149, 161) for lane in range(4)}
    return cycles, products, matrix_loads, min_reload_gap


def reference(matrix, quad):
    q = quad
    sources = (
        (q[0, 0], q[1, 0], q[0, 1], q[1, 2], q[2, 0], q[2, 2], q[5, :2], q[3]),
        (q[0, 2], q[1, 0], q[0, 3], q[1, 3], q[2, 0], q[2, 3], q[5, 2:], q[4]),
        (q[0, 2], q[1, 1], q[0, 3], q[1, 3], q[2, 1], q[2, 3], q[6, :2], q[4]),
        (q[0, 0], q[1, 1], q[0, 1], q[1, 2], q[2, 1], q[2, 2], q[6, 2:], q[3]),
    )
    output = []
    for x, y, z, u, v, glow, zw, color in sources:
        xy = F(F(F(matrix[0, :2] * x) + F(matrix[1, :2] * y)) + F(matrix[2, :2] * z))
        xy = F(xy + matrix[3, :2])
        output.extend(([*xy, *zw], [u, v, glow, 0], color))
    return np.array(output, dtype=F)


def check():
    setup_cases, control_gap = check_setup()
    rng = np.random.default_rng(0xEED14)
    counts = []
    for case in range(2500):
        top = 79 if case & 1 else 551
        memory = rng.uniform(-100, 100, (1024, 4)).astype(F)
        matrix = rng.uniform(-16384, 16384, (4, 4)).astype(F)
        quad = rng.uniform(-8192, 8192, (9, 4)).astype(F)
        if case % 8 == 0:
            matrix[case % 4, :2] = F(-0.0 if case & 8 else 0.0)
            quad[0, case % 4] = F(-0.0)
        if case % 11 == 0:
            # A cancellation boundary stresses reuse without reassociation.
            matrix[2, :2] = -matrix[0, :2]
            quad[0, 1] = quad[0, 0]
        memory[1:5] = matrix
        memory[top].view(np.int32)[1] = 0
        memory[top + 5:top + 14] = quad
        wanted = reference(matrix, quad)
        for gate in (0, 1):
            candidate = memory.copy()
            counts.append(run(candidate, gate, top))
            actual = candidate[top + 149:top + 161]
            assert actual.tobytes() == wanted.tobytes(), (case, gate, actual, wanted)
            # Only four decoded corners and the test's two control seeds change.
            for a, b in ((0, top + 149), (top + 161, top + 214),
                         (top + 215, top + 220), (top + 221, 1024)):
                assert candidate[a:b].tobytes() == memory[a:b].tobytes()
    old, new = counts[0], counts[1]
    assert all(v[:3] == (old if not (i & 1) else new)[:3] for i, v in enumerate(counts))
    return {'cases': 2500, 'scheduled_paths': len(counts), 'corners': len(counts) * 4,
            'masked_setup_cases': setup_cases, 'setup_ISW_ILW_issue_gap': control_gap,
            'legacy_decode_issue_pairs': old[0], 'reuse_decode_issue_pairs': new[0],
            'legacy_xy_products': old[1], 'reuse_xy_products': new[1],
            'legacy_matrix_loads': old[2], 'reuse_matrix_loads': new[2],
            'minimum_shared_SQ_LQ_issue_gap': new[3],
            'bitwise_attributes_context_and_branch_delays': 'pass',
            'native_VU_arithmetic_timing_and_hardware': 'pending'}


if __name__ == '__main__':
    print(json.dumps(check(), indent=2))
