"""Scheduled X2E quad certificate and complete output equivalence.

Binary32 host model, not a PS2 arithmetic or timing emulator. Execute actual
VSM pairs with simultaneous lane reads using the existing interpreter; compare
against the unchanged scalar plane arithmetic and complete gate-OFF output.
"""
import json
import numpy as np
from x2e_projected_model import F, run, check_schedule


def setup(corners, top, gate, material=0, maximum=65535):
    mem = np.full((1024, 4), F(23))
    mem[5] = [320, -224, -maximum / 2, .125]
    mem[6] = [1 / 320, -1 / 224, 2 / maximum, maximum]
    mem[7] = [32, .005, .0075, material]
    mem[8] = [320, 224, 1, gate]
    mem[75].view(np.uint32)[:] = [0x8000, 0x30004000, 0x412, 0]
    planes = np.array([[0, 0, 1, -.125], [1, 0, 320, 0],
                       [-1, 0, 320, 0], [0, 1, 224, 0],
                       [0, -1, 224, 0]], dtype=F)
    mem[top + 209:top + 214] = planes
    mem[top + 214].view(np.uint32)[:] = [top + 5, 1, 0, material]
    mem[top + 215].view(np.uint32)[:2] = [top + 225, 0]
    mem[top + 220].view(np.uint32)[:] = [1, gate, 0, 0]
    mem[top + 12] = [2, 8, 2, 0]
    mem[top + 13] = [3, 9, 1, 0]
    for i, corner in enumerate(corners):
        mem[top + 149 + i * 3] = corner
        mem[top + 150 + i * 3] = [i & 1, i >> 1, .4 + i * .1, 0]
        mem[top + 151 + i * 3] = [.2 + i * .1, .4, .6, .8 - i * .1]
    vf = np.zeros((32, 4), dtype=F)
    vf[0] = [0, 0, 0, 1]
    vf[1, :3] = [2047.5, 2047.5, maximum / 2]
    vf[2] = 1
    vi = np.zeros(16, dtype=np.int32)
    vi[2], vi[3] = top, 32768
    return mem, vf, vi, planes


def scalar_distances(corners, planes):
    out = np.zeros((len(planes), 4), dtype=F)
    for p, plane in enumerate(planes):
        for i, pos in enumerate(corners):
            d = F(F(0) + F(plane[2] * pos[3]))
            d = F(d + F(plane[0] * pos[0]))
            d = F(d + F(plane[1] * pos[1]))
            out[p, i] = F(d + plane[3])
    return out


def check():
    rng = np.random.default_rng(0xE20260920)
    accepted = rejected = classifications = outputs = 0
    accept_steps = []
    legacy_steps = candidate_steps = 0
    for case in range(600):
        top = 79 if case & 1 else 551
        maximum = 65535 if case & 2 else 16777215
        corners = rng.uniform(-300, 300, (4, 4)).astype(F)
        corners[:, 3] = rng.uniform(.125, 5, 4).astype(F)
        corners[:, 2] = F(F(-maximum / 4) * corners[:, 3])
        if case % 2 == 0:
            corners[:, :2] = F(corners[:, :2] * F(.001))
        if case % 6 in (2, 3, 4):
            # Exact boundary, adjacent inside/outside ULPs, all five planes.
            plane = (case // 6) % 5
            vertex = (case // 30) % 4
            if plane == 0:
                axis, value, outside = 3, F(.125), -np.inf
            else:
                axis = (plane - 1) // 2
                sign = -1 if plane & 1 else 1
                value = F(sign * F((320 if axis == 0 else 224) * corners[vertex, 3]))
                outside = np.copysign(np.inf, sign)
            corners[vertex, axis] = value
            if case % 6 == 3:
                corners[vertex, axis] = np.nextafter(value, F(outside), dtype=F)
            elif case % 6 == 4:
                corners[vertex, axis] = np.nextafter(value, F(-outside), dtype=F)
        if case % 23 == 0:
            corners[:, 0] = F(-0.0)
            corners[:, 1] = F(0.0)
        corners[:, 2] = F(F(-maximum / 4) * corners[:, 3])
        for gate in (0, 1):
            mem, vf, vi, planes = setup(corners, top, gate, maximum=maximum)
            before = mem.copy()
            steps, kicks = run(mem, vf, vi, 'e_quad_classify_lid', 'e_triangle_lid')
            want = int(gate and not np.any(scalar_distances(corners, planes) < 0))
            actual = int(mem[top + 221].view(np.uint32)[0])
            assert actual == want, (case, gate, actual, want, corners)
            assert not kicks
            before[top + 221, :2] = mem[top + 221, :2]
            assert before.tobytes() == mem.tobytes()
            assert np.array_equal(vf[1, :3], [2047.5, 2047.5, F(maximum / 2)])
            assert np.array_equal(vf[2], [1, 1, 1, 1])
            classifications += 1
            if gate:
                accepted += want
                rejected += 1 - want
                if want:
                    accept_steps.append(steps)
        if case < 80:
            for material in (0, 1):
                packed = []
                for gate in (0, 1):
                    mem, vf, vi, unused = setup(corners, top, gate, material, maximum)
                    packets = []
                    steps, unused = run(mem, vf, vi, 'e_quad_classify_lid',
                                        'e_main_lid', packets)
                    data = np.concatenate(packets).view(np.uint32)
                    # STQ.W is intentionally unused by the GIF tag.
                    data[0::3, 3] = 0
                    packed.append(data)
                    if gate:
                        candidate_steps += steps
                    else:
                        legacy_steps += steps
                assert np.array_equal(*packed), (case, material, packed)
                outputs += 1
    return {'scheduled_classifier_cases': classifications,
            'accepted_quads': accepted, 'fallback_quads': rejected,
            'complete_base_or_glow_outputs_equal': outputs,
            'all_inside_classifier_pair_lines': sorted(set(accept_steps)),
            'mixed_corpus_gate_off_pair_lines': legacy_steps,
            'mixed_corpus_gate_on_pair_lines': candidate_steps,
            'instruction_pair_counts_are_not_hardware_timings': True,
            **check_schedule()}


def check_activations():
    cases = vertices = kicks = 0
    for top in (79, 551):
        for maximum in (65535, 16777215):
            for material in (0, 1):
                for count in (1, 2, 5, 16):
                    results = []
                    for gate in (0, 1):
                        mem, vf, vi, unused = setup(np.zeros((4, 4), dtype=F),
                                                   top, gate, material, maximum)
                        mem[1:5] = np.diag([320, -224, 1, 1]).astype(F)
                        mem[top].view(np.uint32)[:] = [count, 0, 0, 0]
                        mem[top + 214].view(np.uint32)[1] = count
                        for i in range(count):
                            descriptor = mem[top + 5 + i * 9:top + 14 + i * 9]
                            descriptor[0] = [-.5, 0, .5, 0]
                            descriptor[1] = [-.5, .5, 0, 1]
                            descriptor[2] = [0, 1, .4, .7]
                            descriptor[3] = [.2, .4, .6, .8]
                            descriptor[4] = [.3, .5, .7, .9]
                            descriptor[5:7] = [-maximum / 4, 1, -maximum / 4, 1]
                            descriptor[7] = [2, 8, 2, int(i % 7 == 5)]
                            descriptor[8] = [3, 9, 1, int(i % 7 == 6)]
                            if i % 3 == 1:
                                # Alternating full/partial records exposes a stale
                                # certificate and repeatedly crosses output arenas.
                                descriptor[0, 2] = 1.5
                        before = mem.copy()
                        packets = []
                        unused, calls = run(mem, vf, vi, 'e_decode_lid',
                                            'e_main_lid', packets, max_steps=100000)
                        data = np.concatenate(packets).view(np.uint32)
                        data[0::3, 3] = 0
                        results.append(data)
                        assert mem[:79].tobytes() == before[:79].tobytes()
                        assert mem[top:top + 149].tobytes() == before[top:top + 149].tobytes()
                        assert np.array_equal(mem[top + 214].view(np.uint32),
                                              [top + 5 + count * 9, 0, 0, material])
                    assert np.array_equal(*results), (top, maximum, material, count)
                    cases += 1
                    vertices += len(results[0]) // 3
                    kicks += len(calls)
    return {'complete_compact_activation_pairs': cases,
            'complete_compact_vertices': vertices, 'complete_compact_kicks': kicks,
            'source_context_lifetime_and_repeated_certificates': 'pass'}


if __name__ == '__main__':
    print(json.dumps({**check(), **check_activations()}, indent=2))
