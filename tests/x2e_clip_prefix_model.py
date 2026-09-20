"""Execute the generated X2E prefix reuse against the preceding image.

Checked issue pairs and consumed GIF payloads are not native VU cycles,
rounding, DMA timing or GS pixel validation. The original image is retained
in x2e_prefix_before.vsm so OFF bookkeeping can be measured separately too.
"""
import json
import re
from pathlib import Path

import numpy as np
import x2e_projected_model as machine
from x2e_trivial_accept_model import F, scalar_distances, setup


HERE = Path(__file__).resolve().parent


def program(path):
    labels, code = {}, []
    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0]
        if not line.strip():
            continue
        if line.strip().endswith(':'):
            labels[line.strip()[:-1]] = len(code)
        elif re.match(r'\s+(NOP|[a-z])', line) and not line.lstrip().startswith('.'):
            code.append([line[:67].strip(), line[67:].strip()])
    return labels, code, [[machine.decode(s) for s in pair] for pair in code]


CURRENT = program(HERE.parent / 'vu1/general_clip_decal_x2e_vcl.vsm')
BEFORE = program(HERE / 'x2e_prefix_before.vsm')


def run(image, *args, **kwargs):
    old = machine.labels, machine.code, machine.parsed
    machine.labels, machine.code, machine.parsed = image
    try:
        return machine.run(*args, **kwargs)
    finally:
        machine.labels, machine.code, machine.parsed = old


def payload(packets):
    if not packets:
        return np.zeros((0, 4), dtype=np.uint32)
    result = np.concatenate(packets).view(np.uint32)
    result[0::3, 3] = 0  # STQ.w is neither written nor consumed by the GS.
    return result


def corners_for(case, rng, maximum):
    corners = rng.uniform(-1200, 1200, (4, 4)).astype(F)
    corners[:, 3] = rng.uniform(.125, 3, 4).astype(F)
    if case % 7 == 0:
        corners[:, :2] *= F(.001)
    plane = case % 5
    vertex = (case // 5) % 4
    if plane == 0:
        axis, bound, outside = 3, F(.125), F(-np.inf)
    else:
        axis = (plane - 1) // 2
        sign = -1 if plane & 1 else 1
        bound = F(F(sign * (320 if axis == 0 else 224)) * corners[vertex, 3])
        outside = F(np.copysign(np.inf, sign))
    mode = (case // 20) % 3
    corners[vertex, axis] = bound if mode == 0 else np.nextafter(
        bound, outside if mode == 1 else -outside, dtype=F)
    if case % 23 == 0:
        corners[:, 0] = F(-0.0)
    corners[:, 2] = F(F(-maximum / 4) * corners[:, 3])
    return corners


def check_classification():
    rng = np.random.default_rng(0xE20260921)
    counts = [0] * 6
    for case in range(600):
        top = 79 if case & 1 else 551
        maximum = 65535 if case & 2 else 16777215
        corners = corners_for(case, rng, maximum)
        for flags in (0, 1, 3):
            mem, vf, vi, planes = setup(corners, top, flags, maximum=maximum)
            before = mem.copy()
            distances = scalar_distances(corners, planes)
            failed = np.flatnonzero(np.any(distances < 0, axis=1))
            first = int(failed[0]) if len(failed) else 5
            steps, kicks = run(CURRENT, mem, vf, vi,
                               'e_quad_classify_lid', 'e_triangle_lid')
            expected = [int(bool(flags) and first == 5),
                        first if flags == 3 and first < 5 else 0]
            assert mem[top + 221].view(np.uint32)[:2].tolist() == expected
            assert not kicks
            before[top + 221, :2] = mem[top + 221, :2]
            assert before.tobytes() == mem.tobytes()
            if flags == 3:
                counts[first] += 1
    assert all(counts), counts
    return {'classifier_cases': 1800, 'first_failed_plane_or_inside': counts}


def check_outputs():
    rng = np.random.default_rng(0xE20260922)
    totals = {str(i): [0, 0, 0, 0] for i in range(6)}
    comparisons = 0
    for case in range(120):
        top = 79 if case & 1 else 551
        maximum = 65535 if case & 2 else 16777215
        corners = corners_for(case, rng, maximum)
        for material in (0, 1):
            outputs, addresses = [], []
            for option, (image, flags) in enumerate(
                    ((BEFORE, 0), (BEFORE, 1), (CURRENT, 1), (CURRENT, 3))):
                mem, vf, vi, planes = setup(corners, top, flags, material, maximum)
                initial = mem.copy()
                packets = []
                steps, kicks = run(image, mem, vf, vi,
                                   'e_quad_classify_lid', 'e_main_lid', packets)
                outputs.append(payload(packets))
                addresses.append(kicks)
                assert mem[:79].tobytes() == initial[:79].tobytes()
                assert mem[top:top + 149].tobytes() == initial[top:top + 149].tobytes()
                failed = np.flatnonzero(np.any(scalar_distances(corners, planes) < 0, axis=1))
                bucket = str(int(failed[0]) if len(failed) else 5)
                totals[bucket][option] += steps
            assert all(np.array_equal(outputs[0], output) for output in outputs[1:])
            assert all(addresses[0] == address for address in addresses[1:])
            comparisons += 1
    return {'complete_material_output_comparisons': comparisons,
            'issue_pairs_by_first_failed_plane_or_inside': totals,
            'issue_pair_columns': ['before_trivial_off', 'before_trivial_on',
                                   'new_prefix_off', 'new_prefix_on']}


def check_activations():
    cases = vertices = kicks_count = 0
    steps_total = [0, 0]
    for top in (79, 551):
        for maximum in (65535, 16777215):
            for material in (0, 1):
                for count in (1, 2, 5, 16):
                    for fixture in (0, 1, 2):
                        outputs, addresses = [], []
                        for which, (image, flags) in enumerate(((BEFORE, 1), (CURRENT, 3))):
                            mem, vf, vi, unused = setup(np.zeros((4, 4), dtype=F),
                                                       top, flags, material, maximum)
                            mem[1:5] = np.diag([320, -224, 1, 1]).astype(F)
                            mem[top].view(np.uint32)[:] = [count, 0, 0, 0]
                            mem[top + 214].view(np.uint32)[1] = count
                            for q in range(count):
                                record = mem[top + 5 + q * 9:top + 14 + q * 9]
                                record[0] = [-.5, 0, .5, 0]
                                record[1] = [-.5, .5, 0, 1]
                                record[2] = [0, 1, .4, .7]
                                record[3] = [.2, .4, .6, .8]
                                record[4] = [.3, .5, .7, .9]
                                record[5:7] = [-maximum / 4, 1, -maximum / 4, 1]
                                record[7] = [2, 8, 2, int(q % 7 == 5)]
                                record[8] = [3, 9, 1, int(q % 7 == 6)]
                                phase = (q + fixture) % 6
                                if phase == 1:  # first side plane
                                    record[0, 0] = -1.5
                                elif phase == 2:  # opposite side; then inside
                                    record[0, 2] = 1.5
                                elif phase == 3:  # fourth plane
                                    record[1, 1] = 1.5
                                elif phase == 4:  # fifth plane
                                    record[1, 0] = -1.5
                                elif phase == 5:  # two planes mutate the polygon
                                    record[0, :3:2] = [-2, 2]
                                    record[1, :2] = [-2, 2]
                            before = mem.copy()
                            packets = []
                            steps, kicks = run(image, mem, vf, vi, 'e_decode_lid',
                                               'e_main_lid', packets, max_steps=100000)
                            steps_total[which] += steps
                            outputs.append(payload(packets))
                            addresses.append(kicks)
                            assert mem[:79].tobytes() == before[:79].tobytes()
                            assert mem[top:top + 149].tobytes() == before[top:top + 149].tobytes()
                            assert mem[top + 214].view(np.uint32).tolist() == [
                                top + 5 + count * 9, 0, 0, material]
                        assert np.array_equal(*outputs)
                        assert addresses[0] == addresses[1]
                        assert all(a != b for a, b in zip(addresses[1], addresses[1][1:]))
                        cases += 1
                        vertices += len(outputs[1]) // 3
                        kicks_count += len(addresses[1])
    return {'complete_activation_pairs': cases, 'emitted_vertices': vertices,
            'matched_alternating_output_kicks': kicks_count,
            'activation_issue_pairs_before_new': steps_total}


if __name__ == '__main__':
    print(json.dumps({**check_classification(), **check_outputs(),
                      **check_activations(), **machine.check_schedule()}, indent=2))
