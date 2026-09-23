#!/usr/bin/env python3
"""Exact near-identity source/sign and scheduled-packet checks.

Uses the existing float32 dependency model, not native VU arithmetic/timing or
GS pixels. A pinned original X2 commit is the independent gate-OFF reference.
"""
import argparse
import json
import re
import tempfile
import warnings
from pathlib import Path

import numpy as np

from x2f_source_quad_model import F, Program, ROOT, reg
from x2_window_copy_model import baseline


def sign(distance):
    value = np.trunc(np.clip(distance, F(-2047), F(2047)).astype(np.float64) * 16)
    return value.astype(np.int32) & 0x8000


def near_plane(pos, near):
    # Keep pd_plane's separately rounded products/accumulation, including zero.
    xy = F(F(pos[:, 0] * F(0)) + F(pos[:, 1] * F(0)))
    return F(F(xy + F(F(1) * F(pos[:, 3] * F(1)))) + F(-near))


def source_and_signs():
    source = (ROOT / 'vu1/general_clip_tri_x2.vcl').read_text()
    old = baseline('general_clip_tri_x2.vcl')
    off = re.sub(r'^#if PGL_X2_SKIP_IDENTITY_NEAR\n.*?^#endif\n', '',
                 source, flags=re.S | re.M)
    region = lambda text: text.split('sh_handler_lid:\n', 1)[1].split('tri_next_lid:', 1)[0]
    instructions = lambda text: '\n'.join(line.split(';', 1)[0].rstrip()
        for line in text.splitlines() if line.split(';', 1)[0].strip())
    assert instructions(region(off)) == instructions(region(old)), 'OFF handler must remain literal original'
    macro = lambda text, name: re.search(r'\.macro\s+' + name + r'\s.*?\.endm', text, re.S)[0]
    assert macro(source, 'sh_load') == macro(old, 'sh_load')
    alternate = macro(source, 'sh_load_near_identity')
    alternate = alternate.replace('sh_load_near_identity', 'sh_load').replace('kCPolyB', 'kCPolyA')
    words = lambda text: re.sub(r'\s+', ' ', text).strip()
    assert words(alternate) == words(macro(old, 'sh_load'))
    for name in ('pd_plane', 'pd_sign', 'cp_edge', 'clip_pass', 'emit_mvert'):
        assert macro(source, name) == macro(old, name), name
    assert 'ibne           near_any, vi00, sh_near_original_lid' in source
    assert 'b              sh_side_start_lid' in source
    header = (ROOT / 'vu1/x2_window_copy_gate.h').read_text()
    assert '#define PGL_X2_WINDOW_COPY_TRIANGLES 0' in header
    gate = int(re.search(r'^#define PGL_X2_SKIP_IDENTITY_NEAR ([01])$', header, re.M)[1])
    values = np.concatenate((np.linspace(-4096, 4096, 32769, dtype=F),
        np.array([0, -0., .9375, np.nextafter(F(.9375), F(-np.inf)),
                  np.nextafter(F(.9375), F(np.inf)), 1, -1], dtype=F)))
    cases = 0
    for near in (F(0), F(.0625), F(1), F(17.25)):
        for x in (F(-0.), F(0), F(1e20), F(-1e20)):
            positions = np.zeros((len(values), 4), dtype=F)
            positions[:, 0], positions[:, 1], positions[:, 3] = x, -x, values
            assert np.array_equal(sign(F(values - near)), sign(near_plane(positions, near)))
            cases += len(values)
    return dict(near_gate=gate, sign_cases=cases, off_handler='literal original',
                alternate_loader='same nine qwords and arithmetic, A changed to B')


def execute(program, memory, top):
    packets, at_side = [], []
    plane_load = next(pc for pc, pair in enumerate(program.code)
        if pair[1][0] == 'lq' and pair[1][2][1].startswith('126('))
    side_loop = min(pc for name, pc in program.labels.items()
                    if name.startswith('cp_loop_lid') and pc > plane_load)
    branch = next(pair[1] for pair in program.code[side_loop:side_loop + 5]
                  if pair[1][0] == 'ibeq')
    index_reg, count_reg = reg(branch[2][0]), reg(branch[2][1])
    skips = 0

    def observe(vf, vi, mem, pc, steps):
        nonlocal skips
        if pc == side_loop and int(vi[index_reg]) == 1:
            count = int(vi[count_reg])
            at_side.append(mem[top + 154:top + 154 + 3 * count].tobytes())
        lower = program.code[pc][1]
        if (pc == program.labels['sh_handler_lid'] and lower[0] == 'ibne'
                and lower[2][-1] == 'sh_near_original_lid'
                and int(vi[reg(lower[2][0])]) == 0):
            skips += 1
        if lower[0] == 'xgkick':
            start = int(vi[reg(lower[2][0])])
            count = int(mem[start].view(np.uint32)[0]) & 0x7fff
            end = start + 1 + count * 3
            wall_tag = int(mem[start].view(np.uint32)[0])
            if not wall_tag & 0x8000:
                window_tag = int(mem[end].view(np.uint32)[0])
                assert window_tag & 0x8000 and window_tag & 0x7fff == count
                end += 1 + count * 3
            packets.append((start - top, mem[start:end].tobytes()))

    with warnings.catch_warnings():
        # Existing model saturates extreme FTOI inputs via NumPy casts; both
        # images use the same approximation. This is not native arithmetic proof.
        warnings.simplefilter('ignore', RuntimeWarning)
        _, issued = program.run(memory, top, instruction_trace=observe,
                                compound_windows=True)
    return packets, at_side, issued, program.max_arenas, skips


def scheduled_packets():
    current = Program(ROOT / 'vu1/general_clip_tri_x2_vcl.vsm')
    gate = source_and_signs()['near_gate']
    assert ('sh_near_original_lid' in current.labels) == bool(gate), 'regenerate selected gate first'
    rng = np.random.default_rng(0x1DE17)
    groups = {name: [0, 0, 0] for name in ('inside', 'side', 'near', 'epsilon', 'spill')}
    cases = 0
    kicks = 0
    max_arenas = [0, 0]
    side_states = 0
    skipped = 0
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / 'original.vsm'
        path.write_text(baseline('general_clip_tri_x2_vcl.vsm'))
        reference = Program(path)
        for sample in range(100):
            group = tuple(groups)[sample % len(groups)]
            top = (79, 551)[sample & 1]
            count = 24 if sample % 3 else 30
            memory = np.zeros((1024, 4), dtype=F)
            memory[57, 3] = 32767
            memory[62:66] = [[180, 0, 0, 0], [0, 150, 0, 0],
                             [0, 0, -32767, 1], [0, 0, 32767, 0]]
            memory[75].view(np.uint32)[0] = 0x8000
            memory[76] = [1, 1, 1, 0]
            memory[77, 0] = 1
            memory[top].view(np.uint32)[0] = count
            memory[top + 178] = [-1, 64, 64, 128] if sample % 7 == 0 else [64, 64, 64, 128]
            memory[top + 179].view(np.uint32)[0] = 0x8000
            positions = rng.uniform(-600, 600, (count, 3)).astype(F)
            positions[:, 2] = rng.uniform(5, 60, count).astype(F)
            if group == 'inside':
                positions[:, :2] = np.tanh(positions[:, :2])
            elif group == 'near':
                positions[:, 2] = rng.uniform(-2, 2, count).astype(F)
            elif group == 'epsilon':
                positions[:, 2] = np.resize(np.array([.9375,
                    np.nextafter(F(.9375), F(-np.inf)),
                    np.nextafter(F(.9375), F(np.inf))], dtype=F), count)
            elif group == 'spill':
                positions[:, :2] *= F(5)
                positions[:, 2] = F(20)
            for vertex in range(count):
                q = top + 5 + vertex * 4
                memory[q, :3] = positions[vertex]
                memory[q + 2] = [*rng.uniform(-3, 3, 2), 1, 0]
                memory[q + 3] = rng.uniform(0, 1, 4)
            old = execute(reference, memory.copy(), top)
            new = execute(current, memory.copy(), top)
            assert old[0] == new[0], ('full ordered compound packet words', sample, group)
            assert old[1] == new[1], ('all raw B record words before side1', sample, group)
            assert old[3] == new[3], ('arena capacity', sample, group)
            groups[group][0] += 1
            groups[group][1] += old[2]
            groups[group][2] += new[2]
            max_arenas = [max(a, b) for a, b in zip(max_arenas, new[3])]
            kicks += len(new[0])
            side_states += len(new[1])
            skipped += new[4]
            cases += 1
    assert max_arenas == [30, 18], max_arenas
    return dict(cases=cases, kicks=kicks, side1_raw_record_states=side_states,
                skipped_identity_near_passes=skipped,
                maximum_arena_vertices=max_arenas,
                groups_cases_reference_candidate_issued_pairs=groups,
                limits='Synthetic, cull-OFF float32 model; issued pairs are not elapsed cycles.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-only', action='store_true')
    args = parser.parse_args()
    result = dict(source=source_and_signs())
    if not args.source_only:
        result['scheduled'] = scheduled_packets()
    print(json.dumps(result, indent=2))
