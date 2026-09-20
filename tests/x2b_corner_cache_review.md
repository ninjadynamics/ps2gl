# X2B/X2A four-corner cache review ? 2026-09-20

Candidate, hardware timing and visual acceptance pending.

## Contract

`PGL_CITY_BILLBOARD_CORNER_REUSE=1` sets private q54.w to raw integer one;
zero selects the previous decoder. This is an ILW input, not float 1.0.
The public 128-byte billboard context and descriptors remain unchanged.
The retained CPU context stays unmodified; the flag is injected only into the
owned packet copy. Submission option bit 2 reports the linked gate.

Both variants reconstruct `(center +/- axisU*half) +/- axisV*half` with the
same individual operations as the previous two-triangle decoder. They subtract
eye before the same ordered basis dot products. X2A retains each corner alpha,
including the original MUL by one on A/B/C and MOVE on D. No world-to-eye
reassociation, whole-quad clipping, diagonal change or material reorder occurs.

The candidate computes and classifies A/B/C/D once. Each original ABC/ACD
triangle is copied into polygon A and reduces only its own three outcodes.
The existing clipping, fan emission, near/side planes, interpolation and GS
formatting remain the same source includes.

## Memory and scheduling

Relative q298..309 holds four immutable three-qword source-eye vertices;
q310..313 holds their integer outcodes. q314..315 remains unused. The polygon
banks end at q297, control starts at 316, and output starts at 324. Each half
is still 472 qwords and bases remain 79/551. Clipping may overwrite both polygon
banks without touching the corner cache. Frame input and GIF output ownership,
A30/B18 spill, XGKICK serialization and GIF-tag store/kick spacing are unchanged.

Branch boundaries fence cache stores before indirect classification reads,
classification stores before triangle copies, and copies before clip/fan reads.
The emitted dependency model also overwrites the entire polygon banks between
triangles to verify that ACD comes from the private cache.

Reviewed VU User's Manual sections 3.4.1?3.4.3: VF per-field and ILW hazards
interlock, while Q/CLIP require explicit scheduling. No new Q instructions were
introduced. Every generated CLIP to FCAND/FCGET distance is at least four issue
pairs. Existing structural guards verify branch range, instruction fit and
SQ-to-XGKICK spacing. The model rejects same-pair upper/lower writes to the same
VF register rather than assuming different fields make that legal.

## Verification

Localized generation/assembly of only the changed X2B/X2A modules succeeded.
X2B is 513 pairs, 514 padded; X2A is 520 pairs, 520 padded. Both have 44 bounded
branches. Known VCL exhaustive-scheduler search timeouts produced scheduled
output successfully; there was no assembler or structural-guard failure.

`python -B tests/x2b_corner_cache_model.py` compares emitted gate-ON/OFF code:

- 4,800 triangle boundaries and 14,400 source corners, including random source
 axes, eye/view positions, near/side cuts, per-corner alpha, signed-zero UV,
 zero half-size and both VU buffer halves.
- 184 complete GIF packet comparisons over 96 activations, including multiple
 descriptors, clipped fans and A/B output-arena spills.
- Context/input/output guards and private-cache lifetime checks.
- An exact source-text assertion that the copied classifier arithmetic remains
 identical to `source_eye_classify.i` before its different cache sink/loop.

The sampled decode/classify path executes approximately 19% fewer issue pairs;
this excludes clipping/emission and is not a cycle or frame-time measurement.
`x2b_corner_cache_model.json` records exact generated/object hashes.

The host model uses float32 arithmetic and same-pair old operands/branch delay
slots. It does not emulate native VU rounding, stalls, GS rasterization, DMA
concurrency, or long-run hardware behavior. Those remain Bruno's hardware checks.

## Follow-up: single-material X2F output capacity

The independently reviewed X2F sign path retains X2's paired wall/window limits
of A30/B18 vertices, although its current contract disables the window section.
Its physical output spans A180..361 and B362..471 could hold A60/B36 vertices
for a proven single-material packet. This may reduce sign packet kicks without
changing triangle order. It is not implemented in this pass: admission must
prove windows stay disabled, and the larger limits need their own packet,
capacity, scheduling and ownership checks. Current paired capacities remain
the conservative candidate.
