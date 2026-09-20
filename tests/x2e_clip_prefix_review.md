# X2E proved clipping-prefix reuse - September 20, 2026

Implemented candidate. Only the changed X2E VU module was generated and
assembled. EE/library/game compilation, native timing and visual acceptance
remain pending. This repairs one source-level partial-view cost; it does not
explain the separately measured EE entrance-preparation regression.

## Contract and independent gate

`PGL_DECAL_CLIP_PREFIX_REUSE=1` is effective with `PGL_DECAL_TRIVIAL_ACCEPT`.
The linked decal options query reports effective bit 6 (`64`). The private
context `q8.w` holds exact float flags 0 (certificate OFF), 1 (old certificate)
or 3 (certificate plus prefix reuse). Public source `clip.z/w` remain zero;
`SetDecalContext` changes only its owned copy. No descriptor size, input count,
material, texture, depth, projection or completion boundary changes.

The existing four-corner certificate visits near, left, right, bottom and top
in the established order. If plane N first fails, every original corner passed
every plane before N, under the exact existing strict-negative classifier.
Consequently each earlier plane's original triangle classifier would find all
three corners inside and merely advance the plane index: no clipping, vertex
reordering, polygon-bank swap or interpolation can have happened there.

Reset private `q221.y` to zero on every compact quad. With the new flag, retain
N there when the certificate fails. Each original ABC/ACD polygon starts the
unchanged walker at N. The first failing plane and every later plane still
perform their original classification and Sutherland-Hodgman operations on the
current polygon; no proof survives a polygon mutation. Each original triangle
retains its own skip/depth coefficients, base/glow projection and output fan.
Wholly inside quads retain the direct projection path. Already projected
hybrid records never consume this prefix. The next compact quad resets it.

The scalar/vector plane arithmetic, triangle copy, edge interpolation,
projection, material and output macros are unchanged. The new operations are
integer control/memory only, and use one previously unused private control
word. The decoded corners, polygons, planes and output arenas retain their
original addresses, capacities and alternating XGKICK ownership.

## Verification

`x2e_clip_prefix_model.py` executes the actual scheduled code and the frozen
preceding image `x2e_prefix_before.vsm`:

- 1,800 classifier cases cover every first-failed plane, inside, exact and
  adjacent-ULP boundaries, signed zero, both halves and depth formats. Only
  the two private certificate words may change during classification.
- 240 complete base/glow payload comparisons cover old certificate OFF/ON
  and new prefix OFF/ON. Consumed GIF payloads and kick addresses match.
- 96 complete mixed activations cover 1/2/5/16 descriptors, both materials,
  halves and depth formats; each side plane, successive polygon cuts,
  inside/partial transitions, per-triangle skips and output spills. All
  4,728 emitted vertices and 232 alternating output kicks match. Context
  and descriptor source memory remain unchanged.
- Existing trivial-accept checks: 1,200 classifiers, 160 full outputs and
  32 compact activations / 1,224 vertices / 72 kicks pass.
- Existing XY model: 5,000 paths / 20,000 corners and 24 setup cases pass.
- Existing projected model: 2,400 records / 7,200 vertices / 800 arena
  transitions plus 128 complete activations / 3,264 vertices / 176 kicks pass.
- Existing mixed packet model: 1,008 sweeps / 72,048 activations plus 24
  source-alias boundaries and 14 admission/source checks pass.

Generated schedule/structural guard: **692 actual/padded instruction pairs**,
**51 bounded branches**, seven CLIP/FCAND sites with minimum four issue pairs,
three Q landings (two WAITQ and one seven-pair DIV gap), no simultaneous
upper/lower VF write collisions. Prefix publication precedes the original
triangle copy path; the unchanged plane-index loop retains its normal ordered
control stores/loads. Official VU User's Manual pp44-45 provides the VF/VI
interlocks, same-pair write priority and explicit Q/flag scheduling rules.

Compared with the prior 683 actual / 684 padded image, the upload grows by
**64 bytes**. Trailing whitespace was normalized after generation without
changing any instruction or comment. Only this module was assembled with
`make vu1/general_clip_decal_x2e.vo`; no archive/game target was invoked.

## Modeled work and remaining tradeoffs

The following scheduled issue-pair totals are host execution counts, not
cycles or hardware milliseconds. They exclude pipeline stalls, cache effects,
VIF/DMA transport and GS rasterization.

| First failed plane | Old certificate OFF | Old certificate ON | New prefix OFF | New prefix ON |
|---|---:|---:|---:|---:|
| Near | 39384 | 39928 | 40040 | 40088 |
| Left | 331134 | 338974 | 339870 | 316702 |
| Right | 119257 | 123849 | 124213 | 105233 |
| Bottom | 35866 | 37738 | 37850 | 29066 |
| Top | 8205 | 8781 | 8809 | 5877 |
| Inside | 38232 | 19680 | 19632 | 19632 |

Every side-failed group beats both old routes in this corpus. Mixed complete
activations fall **1,125,148 -> 941,388 pairs (16.33%)**. The earlier directed
right cut is **2104 -> 1746**, top cut **2112 -> 1386**, and inside **799 -> 797**.

A failure at the first near plane has no earlier work to reuse. That group
remains 0.40% above the prior certificate ON image. The game's compact route
projects any source quad with `W < SH_NEAR_Z` through its hybrid exceptional
route; the low-level renderer nevertheless preserves clipping for such
inputs rather than assuming they cannot be submitted. No native gain or
every-view superiority follows from this finite corpus.

Gate OFF retains the larger program and new control bookkeeping. The frozen
old image comparison above separates these from the enabled reuse gain;
setting only the new gate to zero is not a complete old-binary comparison.

SHA-256:

- VCL: `9308edbc07562d27173e0ceca1b61eb5647dd35faca7aebf2337cf144ddb2c52`
- VSM: `bb6eebb95272be0fcb1167e847b0c45229787510da8b0228fdebebd70fc174ff`
- Object: `f5473fdcf88940c779853cfe41c64c3326ef8675cac6a9c2b604d3131947f36b`
- Prior VSM: `7faf06a09df52a6248c9dfa19273fd440de1c9faaf7c502e2c9e431ca9a2d43e`

## Independent review completion - 10:26 local

`graphics_wait_audit` reviewed the current 692-pair image and integration:
PASS for the first-failed plane, strict-negative signed-zero policy, per-quad
reset, ABC/ACD order, projected/skipped transitions, private context copy and
effective option bit 64. No material, clipping, culling or arena change found.
The adjacent q216 store/load was checked against the existing `e_next_plane`
branch-delay store followed by the same reload. It preserves the established
memory ordering; no new defect was established and no padding was added.
The reviewed image and hashes above remain final. Recorded by Codex/root.
