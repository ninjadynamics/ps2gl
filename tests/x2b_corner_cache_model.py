"""Compare scheduled X2B/X2A gate ON/OFF corner preparation, bit for bit.

Models same-pair old operand values and branch delay slots with float32 math.
This is not native VU arithmetic, cycle timing, or GS/DMA emulation. It checks
emitted register/data dependencies, source arithmetic, triangle assembly and
private-memory ownership before the unchanged clipping/emit implementation.
"""
import hashlib
import json
import re
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
F = np.float32
lanes = 'xyzw'
labels = {}
code = []
parsed = []


def image(path):
    global labels, code, parsed
    labels, code = {}, []
    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0]
        if not line.strip():
            continue
        if line.strip().endswith(':'):
            labels[line.strip()[:-1]] = len(code)
        elif re.match(r'\s+(NOP|[a-z])', line) and not line.lstrip().startswith('.'):
            code.append([line[:67].strip(), line[67:].strip()])
    parsed = [[decode(s) for s in pair] for pair in code]


def decode(s):
    a = s.split(None, 1)
    op = a[0].split('.')
    return op[0].lower(), op[1] if len(op) == 2 else lanes, a[1].split(',') if len(a) == 2 else []





def rn(s):
    return int(re.search(r'\d+', s)[0])


def s16(v):
    return v - 65536 if v & 32768 else v


def run(mem, vf, vi, start='r_main_lid', stop='r_triangle_classified_lid', packets=None,
        max_steps=10000, top=79):
    pc = labels[start]
    stop = labels[stop]
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
            elif op == 'xtop':
                vi[rn(a[0])] = top
            elif op == 'fcset':
                clip = int(a[0], 0)
            elif op == 'fcget':
                vi[rn(a[0])] = clip & 65535
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
            elif op in ('iadd', 'isub', 'iaddiu', 'isubiu', 'ior', 'iand'):
                av = oldvi[rn(a[1])]
                bv = int(a[2], 0) if op.endswith('iu') else oldvi[rn(a[2])]
                v = (av - bv) if op.startswith('isub') else (av | bv) if op == 'ior' else (av & bv) if op == 'iand' else av + bv
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
                    assert address in (top + 324, top + 415)
                    assert count <= (30 if address == top + 324 else 18)
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



def check():
    reference = (ROOT / 'vu1/source_eye_classify.i').read_text().split('r_source_view_done_lid:')[0]
    cached = (ROOT / 'vu1/source_billboard_cached_classify.i').read_text()
    copied = cached[cached.index('     ; Source-relative subtraction'):cached.index('bc_source_view_done_lid:')]
    assert reference.replace('r_source_view_done_lid', 'bc_source_view_done_lid') == copied
    for suffix in ('a', 'b'):
        source = (ROOT / ('vu1/general_clip_billboard_x2' + suffix + '.vcl')).read_text()
        assert 'kBCache             .equ 298' in source
        assert 'kBCodes             .equ 310' in source
        assert 'ilw.w bc_enabled, 54(vi00)' in source
    rng = np.random.default_rng(0x20260920)
    comparisons = corners = packet_comparisons = 0
    totals = {}
    for module, words in (('x2b', 3), ('x2a', 4)):
        path = ROOT / ('vu1/general_clip_billboard_' + module + '_vcl.vsm')
        image(path)
        steps_sum = [0, 0]
        for case in range(1200):
            top = 79 if case & 1 else 551
            initial = rng.uniform(-1000, 1000, (1024, 4)).astype(F)
            initial[top, :].view(np.uint32)[:] = [1, 0, 0, 0]
            angle = rng.uniform(-np.pi, np.pi)
            cs, sn = F(np.cos(angle)), F(np.sin(angle))
            initial[49] = [cs, 0, sn, 0]
            initial[50] = [0, 1, 0, 0]
            initial[51] = [-sn, 0, cs, 0]
            center = rng.uniform(-32000, 32000, 3).astype(F)
            initial[52, :3] = F(center + rng.uniform(-100, 100, 3).astype(F))
            initial[52, 3] = 0
            initial[53] = [0, 0, rng.uniform(.5, 3), rng.uniform(.5, 3)]
            initial[54] = [1, 5.5, 1e-12, 0]
            initial[55, :3] = rng.uniform(-1, 1, 3)
            initial[56, :3] = rng.uniform(-1, 1, 3)
            initial[top + 5, :3] = center
            initial[top + 5, 3] = 0 if case % 29 == 0 else rng.uniform(.001, 180)
            initial[top + 6] = rng.uniform(-4, 4, 4)
            initial[top + 7] = rng.uniform(0, 1, 4)
            if module == 'x2a':
                initial[top + 8] = rng.uniform(0, 1, 4)
            if case % 17 == 0:
                initial[top + 6] = [0, -0., 1, -0.]
            if case % 23 == 0:
                initial[55, :3] = 0
                initial[56, :3] = 0
                initial[52, :3] = center
                initial[52, 2] = F(center[2] - F(1))
            outputs = []
            for gate in (0, 1):
                mem = initial.copy()
                mem[54].view(np.uint32)[3] = gate
                vf = rng.uniform(-100, 100, (32, 4)).astype(F)
                vf[0] = [0, 0, 0, 1]
                vi = np.zeros(16, dtype=np.int32)
                before = mem.copy()
                output = []
                for phase in (0, 1):
                    steps, kicks = run(mem, vf, vi,
                        start='r_main_lid' if not phase else 'r_next_triangle_lid', top=top)
                    steps_sum[gate] += steps
                    assert not kicks
                    output.append((mem[top + 106:top + 115].view(np.uint32).copy(),
                        int(mem[top + 316].view(np.uint32)[3]),
                        int(mem[top + 320].view(np.uint32)[0])))
                    # The real clipper may overwrite both polygon banks and
                    # RFan. It never owns the four-corner cache or descriptor.
                    mem[top + 106:top + 298] = rng.uniform(-100, 100, (192, 4))
                    mem[top + 320] = rng.uniform(-100, 100, 4)
                outputs.append(output)
                assert mem[:79].tobytes() == before[:79].tobytes()
                assert mem[top + 1:top + 101].tobytes() == before[top + 1:top + 101].tobytes()
                assert mem[top + 324:top + 472].tobytes() == before[top + 324:top + 472].tobytes()
                assert mem[top + 314:top + 316].tobytes() == before[top + 314:top + 316].tobytes()
                if not gate:
                    assert mem[top + 298:top + 316].tobytes() == before[top + 298:top + 316].tobytes()
            for phase in (0, 1):
                old, new = outputs[0][phase], outputs[1][phase]
                assert np.array_equal(old[0], new[0]), (module, case, phase, old, new)
                assert old[1:] == new[1:], (module, case, phase, old[1:], new[1:])
                comparisons += 1
                corners += 3
        # Execute the actual unchanged clipping/fan/output code too. This
        # catches live-register mistakes which matching triangle input alone
        # cannot reveal, and forces output-arena spills across descriptors.
        for case in range(48):
            top = 79 if case & 1 else 551
            count = (1, 3, 8, 24)[case % 4]
            initial = np.full((1024, 4), F(19.0))
            initial[49:52] = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]], dtype=F)
            initial[52] = [1000, -2000, 1500, 0]
            initial[53] = [0, 0, 1.35, 2.1]
            initial[54] = [1, 5.5, 1e-12, 0]
            initial[55] = [1, 0, 0, 0]
            initial[56] = [0, 1, 0, 0]
            initial[57, 3] = 32767.5
            initial[62:66] = [[320, 0, 0, 0], [0, -224, 0, 0],
                [0, 0, -200, -1], [0, 0, -1, 0]]
            initial[75].view(np.uint32)[:] = [0x8000, 0x30004000, 0x412, 0]
            initial[top].view(np.uint32)[:] = [count, 0, 0, 0]
            for record in range(count):
                base = top + 5 + record * words
                delta = rng.uniform(-20, 20, 3).astype(F)
                if case % 3 == 0:
                    delta[2] = 50
                initial[base, :3] = F(initial[52, :3] + delta)
                initial[base, 3] = rng.uniform(.01, 15)
                initial[base + 1] = rng.uniform(-3, 3, 4)
                initial[base + 2] = rng.uniform(0, 1, 4)
                if words == 4:
                    initial[base + 3] = rng.uniform(0, 1, 4)
            variants = []
            for gate in (0, 1):
                mem = initial.copy()
                mem[54].view(np.uint32)[3] = gate
                vf = np.zeros((32, 4), dtype=F)
                vf[0] = [0, 0, 0, 1]
                vi = np.zeros(16, dtype=np.int32)
                run(mem, vf, vi, start='vsmGeneralClipBillboard' + module.upper() + '_CodeStart',
                    stop='r_decode_lid', top=top)
                packets = []
                run(mem, vf, vi, start='r_decode_lid', stop='r_main_lid', top=top,
                    packets=packets, max_steps=200000)
                variants.append(packets)
                assert mem[:79].tobytes() == initial[:54].tobytes() + mem[54:55].tobytes() + initial[55:79].tobytes()
                assert mem[top + 1:top + 101].tobytes() == initial[top + 1:top + 101].tobytes()
                assert int(mem[top + 323].view(np.uint32)[0]) == 0
            assert len(variants[0]) == len(variants[1]), (module, case)
            for old, new in zip(*variants):
                mask = np.ones(old.shape, dtype=bool)
                mask[::3, 3] = False # STQ.w is not consumed by GIF.
                assert np.array_equal(old.view(np.uint32)[mask], new.view(np.uint32)[mask]), (module, case)
                packet_comparisons += 1
        totals[module] = dict(cases=1200, gate_off_issue_pairs=steps_sum[0],
            gate_on_issue_pairs=steps_sum[1], sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    return dict(triangle_comparisons=comparisons, corner_comparisons=corners,
        full_packet_comparisons=packet_comparisons,
        emitted=totals, limitation='Float32 dependency model, not native VU/GS or cycle emulation')


if __name__ == '__main__':
    print(json.dumps(check(), indent=2))
