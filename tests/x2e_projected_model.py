"""Check scheduled exceptional NDC records against the old raw-X2 contract.

This is not a PS2 VU arithmetic or timing emulator. VF operands in an issue
pair see old values; documented data interlocks and explicit Q/CLIP scheduling
are assumed and reviewed separately. Verifies both output materials, identity
matrix operation order, GS fields and the existing A/B output arena transition.
"""
import json
import re
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
F = np.float32
lanes = 'xyzw'
labels = {}
code = []
for raw in (ROOT / 'vu1/general_clip_decal_x2e_vcl.vsm').read_text().splitlines():
    line = raw.split(';', 1)[0]
    if not line.strip():
        continue
    if line.strip().endswith(':'):
        labels[line.strip()[:-1]] = len(code)
    elif re.match(r'\s+(NOP|[a-z])', line) and not line.lstrip().startswith('.'):
        code.append([line[:67].strip(), line[67:].strip()])


def decode(s):
    a = s.split(None, 1)
    op = a[0].split('.')
    return op[0].lower(), op[1] if len(op) == 2 else lanes, a[1].split(',') if len(a) == 2 else []


parsed = [[decode(s) for s in pair] for pair in code]


def rn(s):
    return int(re.search(r'\d+', s)[0])


def s16(v):
    return v - 65536 if v & 32768 else v


def run(mem, vf, vi, start='e_projected_record_lid', stop='e_decode_lid', packets=None,
        max_steps=10000):
    pc = labels[start]
    stop = labels[stop]
    top = int(vi[2])
    pending = None
    q = F(0)
    acc = np.zeros(4, dtype=F)
    imm = F(0)
    clip = 0
    kicks = []
    steps = 0
    while pc != stop:
        assert steps < max_steps, (pc, code[pc])
        steps += 1
        oldvf = vf.copy()
        oldvi = vi.copy()
        oldimm = imm
        oldq = q
        oldacc = acc.copy()
        writes = []
        branch = None
        for op, mask, a in parsed[pc]:
            m = [lanes.index(c) for c in mask]
            if op in ('nop', 'nop[e]', 'waitq'):
                continue
            if op in ('lq', 'sq', 'ilw', 'isw'):
                off, idx = re.fullmatch(r'(-?\d+)\(VI(\d+)\)', a[1]).groups()
                address = int(off) + oldvi[int(idx)]
                assert 0 <= address < 1024, address
                if op == 'lq':
                    writes.append((rn(a[0]), m, mem[address, m].copy()))
                elif op == 'sq':
                    mem[address, m] = oldvf[rn(a[0]), m]
                elif op == 'ilw':
                    vi[rn(a[0])] = int(mem[address].view(np.uint32)[m[0]]) & 65535
                else:
                    mem[address].view(np.uint32)[m] = oldvi[rn(a[0])]
            elif op == 'loi':
                imm = np.array(int(a[0], 16), dtype=np.uint32).view(np.float32)[()]
            elif op == 'div':
                q = F(oldvf[rn(a[1]), lanes.index(a[1][-1])] / oldvf[rn(a[2]), lanes.index(a[2][-1])])
            elif op == 'clipw':
                value = oldvf[rn(a[0])]
                w = abs(oldvf[rn(a[1]), 3])
                clip = sum((int(value[k] > w) | (int(value[k] < -w) << 1)) << (k * 2) for k in range(3))
            elif op == 'fcand':
                vi[rn(a[0])] = int(bool(clip & int(a[1], 0)))
            elif op == 'mtir':
                vi[rn(a[0])] = int(oldvf[rn(a[1])].view(np.uint32)[lanes.index(a[1][-1])]) & 65535
            elif op == 'mfir':
                value = np.array(s16(oldvi[rn(a[1])]), dtype=np.int32).view(np.float32)[()]
                writes.append((rn(a[0]), m, np.full(len(m), value, dtype=np.float32)))
            elif op in ('iadd', 'isub', 'iaddiu', 'isubiu', 'ior'):
                av = oldvi[rn(a[1])]
                bv = int(a[2], 0) if op.endswith('iu') else oldvi[rn(a[2])]
                v = (av - bv) if op.startswith('isub') else (av | bv) if op == 'ior' else av + bv
                vi[rn(a[0])] = v & 65535
            elif op == 'b':
                branch = labels[a[0]]
            elif op.startswith('ib'):
                av = s16(oldvi[rn(a[0])])
                if op in ('ibeq', 'ibne'):
                    bv = s16(oldvi[rn(a[1])])
                    hit = av == bv if op == 'ibeq' else av != bv
                else:
                    hit = {'ibgtz': av > 0, 'ibltz': av < 0, 'ibgez': av >= 0, 'iblez': av <= 0}[op]
                if hit:
                    branch = labels[a[-1]]
            elif op == 'xgkick':
                address = oldvi[rn(a[0])]
                kicks.append(address)
                if packets is not None:
                    count = int(mem[address].view(np.uint32)[0]) & 0x7fff
                    assert address in (top + 224, top + 315)
                    assert count <= (30 if address == top + 224 else 18)
                    packets.append(mem[address + 1:address + 1 + count * 3].copy())
            elif op.startswith(('mula', 'madda', 'madd')):
                factor = oldvf[rn(a[2]), lanes.index(a[2][-1])]
                value = F(oldvf[rn(a[1])] * factor)
                if op.startswith('madd'):
                    value = F(oldacc + value)
                if a[0] == 'ACC':
                    acc[m] = value[m]
                else:
                    writes.append((rn(a[0]), m, value[m].copy()))
            else:
                av = oldvf[rn(a[1])].copy()
                if op == 'mr32':
                    value = np.roll(av, -1)
                elif op in ('abs', 'move'):
                    value = abs(av) if op == 'abs' else av
                elif op.startswith('ftoi'):
                    scale = 16 if op == 'ftoi4' else 1
                    value = np.trunc(av.astype(np.float64) * scale).astype(np.int32).view(np.float32)
                else:
                    base, lane = re.fullmatch(r'(add|sub|mul|max|minii)([xyzwqi]?)', op).groups()
                    if lane == 'q':
                        bv = oldq
                    elif lane == 'i' or base == 'minii':
                        bv = oldimm
                    else:
                        bv = oldvf[rn(a[2])].copy()
                        if lane:
                            assert a[2][-1] == lane
                            bv = bv[lanes.index(lane)]
                    value = {'add': np.add, 'sub': np.subtract, 'mul': np.multiply,
                             'max': np.maximum, 'minii': np.minimum}[base](av, bv)
                writes.append((rn(a[0]), m, value[m].copy()))
        touched = set()
        for dst, mask, value in writes:
            assert dst not in touched, (pc, code[pc], dst)
            touched.add(dst)
            vf[dst, mask] = value
        assert vi[0] == 0
        assert np.array_equal(vf[0], [0, 0, 0, 1])
        newpc = pending if pending is not None else pc + 1
        pending = branch
        pc = newpc
    return steps, kicks


def expected(record, matrix, gs, material):
    packets = []
    for index, (pos, stq, color) in enumerate(record):
        p = pos.copy()
        c = color.copy()
        if material:
            p[2] = p[3]
            c = np.array([1, 1, 1, stq[3]], dtype=F)
        # Original raw-X2 mul_pt_mat_44 order, including its forced W=1.
        clip = F(matrix[0] * p[0])
        clip = F(clip + F(matrix[1] * p[1]))
        clip = F(clip + F(matrix[2] * p[2]))
        clip = F(clip + matrix[3])
        assert clip[3] == 1
        q = F(1 / clip[3])
        xyz = F(F(clip[:3] * q) + gs)
        tex = F(stq[:3] * q)
        rgba = np.trunc(F(c * F(128)).astype(np.float64)).astype(np.int32)
        fog = int(np.trunc(float(min(F(255), max(F(0), F(c[3] * F(255))))) * 16))
        out = np.zeros((3, 4), dtype=np.uint32)
        out[0, :3] = tex.view(np.uint32)
        out[1] = rgba.view(np.uint32)
        out[2, :3] = np.trunc(xyz.astype(np.float64) * 16).astype(np.int32).view(np.uint32)
        adc = 32768 if index != 2 else 0
        out[2, 3] = np.array(s16(fog | adc), dtype=np.int32).view(np.uint32)
        packets.append(out)
    return np.concatenate(packets)


def check():
    rng = np.random.default_rng(0x58E014)
    cases = vertices = transitions = 0
    for case in range(200):
        top = 79 if case & 1 else 551
        maximum = F(65535 if case & 2 else 16777215)
        raster = np.array([320, -112 if case & 2 else -224, -maximum / F(2)], dtype=F)
        matrix = np.diag([*raster, F(1)]).astype(F)
        gs = np.array([2047.5, 2047.5, maximum / F(2)], dtype=F)
        record = rng.uniform(0, 1, (3, 3, 4)).astype(F)
        record[:, 0, :2] = rng.uniform(-1.9, 1.9, (3, 2)).astype(F)
        record[:, 0, 2] = rng.uniform(-.9, .9, 3).astype(F)
        record[:, 0, 3] = F(record[:, 0, 2] - F(.000125))
        record[:, 1, :2] = rng.uniform(-4, 4, (3, 2)).astype(F)
        if not case % 7:
            record[case % 3, 0, case % 2] = F(-0.0)
        for material in (0, 1):
            for arena, count in ((0, 0), (0, 27), (0, 30), (1, 0), (1, 15), (1, 18)):
                memory = np.full((1024, 4), F(23.0))
                memory[62:66] = matrix
                memory[75].view(np.uint32)[:] = [0x8000, 0x30004000, 0x412, 0]
                memory[top + 5:top + 14] = record.reshape(9, 4)
                memory[top + 214].view(np.uint32)[:] = [top + 5, 2, 0, material]
                initial_output = top + (225 if not arena else 316) + count * 3
                memory[top + 215].view(np.uint32)[:2] = [initial_output, count]
                regs = np.zeros((32, 4), dtype=F)
                regs[0] = [0, 0, 0, 1]
                regs[1, :3] = gs
                regs[2] = 1
                integer = np.zeros(16, dtype=np.int32)
                integer[2], integer[3] = top, 32768
                before = memory.copy()
                steps, kicks = run(memory, regs, integer)
                spill = count == (30 if not arena else 18)
                output = top + (316 if not arena else 225) if spill else initial_output
                actual = memory[output:output + 9].view(np.uint32)
                wanted = expected(record, matrix, gs, material)
                mask = np.ones((9, 4), dtype=bool)
                mask[0::3, 3] = False # GIF does not consume STQ.w.
                assert np.array_equal(actual[mask], wanted[mask]), (case, material, arena, count, actual, wanted)
                assert np.array_equal(memory[top + 214].view(np.uint32), [top + 14, 1, 0, material])
                assert np.array_equal(memory[top + 217].view(np.uint32)[:2], [top + 161, 3])
                assert np.array_equal(regs[1, :3], gs)
                assert integer[2] == top and integer[3] == 32768
                assert np.array_equal(regs[2], [1, 1, 1, 1])
                assert memory[:79].tobytes() == before[:79].tobytes()
                assert memory[top + 5:top + 149].tobytes() == before[top + 5:top + 149].tobytes()
                assert len(kicks) == int(spill)
                cases += 1
                vertices += 3
                transitions += len(kicks)
    return {'projected_record_cases': cases, 'packed_vertices': vertices,
            'arena_transitions': transitions, 'both_materials_and_depth_modes': 'pass',
            'actual_scheduled_X2E_vs_raw_X2_operation_contract': 'pass',
            'native_VU_arithmetic_timing_and_hardware': 'pending'}


def check_activations():
    rng = np.random.default_rng(0x58E016)
    activations = vertices = kick_count = 0
    for top in (79, 551):
        for maximum in (F(65535), F(16777215)):
            matrix = np.diag([F(320), F(-224), -maximum / F(2), F(1)]).astype(F)
            gs = np.array([2047.5, 2047.5, maximum / F(2)], dtype=F)
            for material in (0, 1):
                for count in range(1, 17):
                    records = rng.uniform(-.75, .75, (count, 3, 3, 4)).astype(F)
                    records[:, :, 1:, :] = rng.uniform(0, 1, (count, 3, 2, 4)).astype(F)
                    memory = np.full((1024, 4), F(23.0))
                    memory[62:66] = matrix
                    memory[75].view(np.uint32)[:] = [0x8000, 0x30004000, 0x412, 0]
                    memory[top].view(np.uint32)[:] = [count, 1, 0, 0]
                    memory[top + 5:top + 5 + count * 9] = records.reshape(count * 9, 4)
                    memory[top + 214].view(np.uint32)[:] = [top + 5, count, 0, material]
                    memory[top + 215].view(np.uint32)[:2] = [top + 225, 0]
                    regs = np.zeros((32, 4), dtype=F)
                    regs[0] = [0, 0, 0, 1]
                    regs[1, :3] = gs
                    regs[2] = 1
                    integer = np.zeros(16, dtype=np.int32)
                    integer[2], integer[3] = top, 32768
                    before = memory.copy()
                    packets = []
                    unused, kicks = run(memory, regs, integer, 'e_decode_lid', 'e_main_lid', packets)
                    actual = np.concatenate(packets).view(np.uint32)
                    wanted = np.concatenate([expected(record, matrix, gs, material) for record in records])
                    mask = np.ones(actual.shape, dtype=bool)
                    mask[0::3, 3] = False
                    assert np.array_equal(actual[mask], wanted[mask]), (top, maximum, material, count)
                    assert kicks == [top + 224] + ([top + 315] if count > 10 else [])
                    assert np.array_equal(memory[top + 214].view(np.uint32),
                                          [top + 5 + count * 9, 0, 0, material])
                    assert memory[:79].tobytes() == before[:79].tobytes()
                    assert memory[top:top + 149].tobytes() == before[top:top + 149].tobytes()
                    activations += 1
                    vertices += count * 3
                    kick_count += len(kicks)
    return {'complete_projected_activations': activations,
            'activation_vertices': vertices, 'activation_kicks': kick_count,
            'descriptor_exhaustion_and_final_kick': 'pass'}


def check_schedule():
    # VU User's Manual v6 sections 3.4.2/3.4.3/3.4.5: both pipes stall
    # together, Upper wins a same-register write even with different masks,
    # and Q has no data interlock. WAITQ makes Q usable in its own Upper pair.
    # DIV latency is seven cycles; CLIP flag latency is four (instruction refs).
    last_clip = last_div = None
    clip_gaps = []
    q_sites = []
    for pc, (upper, lower) in enumerate(parsed):
        if upper[0] == 'clipw':
            last_clip = pc
        if lower[0] == 'fcand':
            assert last_clip is not None and pc - last_clip >= 4
            clip_gaps.append(pc - last_clip)
        if lower[0] == 'div':
            last_div = pc
        if upper[0] == 'addq':
            assert last_div is not None
            assert lower[0] == 'waitq' or pc - last_div >= 7
            q_sites.append((pc, pc - last_div, lower[0] == 'waitq'))
        if upper[2] and lower[2] and lower[0] in ('lq', 'move', 'mr32', 'mfir'):
            if upper[2][0].startswith('VF') and lower[2][0].startswith('VF'):
                assert upper[2][0] != lower[2][0], (pc, upper, lower)
    return {'CLIP_FCAND_sites': len(clip_gaps),
            'minimum_CLIP_FCAND_issue_gap': min(clip_gaps),
            'Q_landed_sites_pc_gap_waitq': q_sites,
            'same_pair_VF_write_collisions': 0}


if __name__ == '__main__':
    print(json.dumps({**check(), **check_activations(), **check_schedule()}, indent=2))
