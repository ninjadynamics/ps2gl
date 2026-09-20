# X2F authored four-corner submission review - 2026-09-20

Candidate, independently gated ON. Hardware correctness/performance remain pending.

## Contract

The game keeps the existing EE `(position-eye)` operation order and existing
radial-haze evaluation. Complete ABCACD pairs become actual ABCD source arrays,
without reconstructing D, UVs, RGB or alpha. Base span offset and vertex-count
admission remain exact; partial spans use the original triangle path. Whole
materials (including both additive families) retain the existing conservative
six-record capacity preflight before publishing any frame-owned array data.
The append-only source arena stays alive through DMA completion. Existing
material/depth/order state and fallback are unchanged.

The manager owns one additional X2F renderer; its linked options query requires
a current context and a registered matching renderer. Context recreation cannot
leave the options query pointing at a destroyed renderer. This complete program
invalidates the shared X2 prefix proof through normal user-renderer registration.

The specialized C++ transport uploads XYZ3/UV2/floatRGBA4 directly at offsets
0/1/2 with STCYCL(1,3). Existing STROW supplies STQ.z=1. Eight ABCD quads fit in
q5..100. The generic XferBlock and its normal/UV/color offsets are unchanged.
Source spans end on four-corner boundaries; every activation is at most 32 corners.
Headers retain corner counts and zero strip restart. Prefix/header helpers restore
STCYCL(1,3) before the next payload. Frame source pointers remain borrowed.

VU memory remains XTOP-relative (79 or 551): immutable cached predivide positions
q101..104, divided XYZ+original W q105..108, four GS triples q109..120, triangle
phase q121, whole-quad classification q122. Plane/polygon/output ranges remain the
original X2 ranges. Cache entries are reused only within their original quad.
The actual triangles remain ABC followed by ACD, with the same clipping diagonal,
near epsilon, 128-range color conversion, alpha-as-fog, ADC and output spill.
Cull-enable bit 0x20 disables the shortcut and retains original triangle routing.
Current game callers disable culling explicitly; no new MAC-flag correctness claim
is made for the inherited cull path.

## Permitted checks

- Generated and assembled only `general_clip_quad_x2f`: 969 instruction pairs,
  970 padded, 70 bounded branches; two E stops and three output-kick sites.
- `x2f_microcode_guard.py` validates instruction-memory/branch/E/kick structure
  before the module object and archive are built.
- `x2f_source_quad_model.py`: 300 full emitted-packet comparisons against original
  X2, using both XTOP halves, 1..8 quads, signed zero, independently varying actual
  corners/UV/colors, near/side/huge clipping, both Y scales and output spill.
  Bitwise consumed GS payloads match. STQ.w is excluded because neither path writes
  it and the GS does not consume it. Cull-on routing is separately asserted.
- Model checks simultaneous upper/lower reads, illegal duplicate VF writers,
  four-pair SQ/LQ separation and memory/output bounds. This is a float32 dependency
  model, not a native VU rounding, MAC-flag, DMA, GS pixel or timing emulator.
- Copied `pd_plane`, `pd_sign`, `cp_edge`, `clip_pass`, `emit_mvert` and
  `x2_kick_chunk` macro bodies compare exactly against original X2 source.
- Independent peer reviewed final transfer masks/counts, cache map, cull routing,
  default STQ, branch schedules and shared-prefix ownership. The generated image
  has no upper/lower VF duplicate writers; CLIP-to-flag distance is at least 4.
- No EE, game, library or other preexisting VU modules were rebuilt by this lane.

## Performance evidence limits

The main candidate removes one-third of authored source values and avoids duplicate
A/C transforms. VU input footprint is half the old six-vertex/four-qword layout;
eight quads fit per activation rather than five. These facts do not prove a speedup.

Dynamic model instruction-pair totals (old X2 -> X2F):

| Corpus | Cases | Old | X2F |
|---|---:|---:|---:|
| Ordinary mixed views |117|164420|163005|
| Extreme clipping stress |83|441104|475693|
| Fully inside |100|126681|125486|

Clipped stress has about 7.8% more issue pairs because cache/classification management
is additional work; ordinary/inside costs are slightly lower. Actual EE staging,
VIF/DMA throughput and hardware frame cost must be measured with the independent gate.
No sub-millisecond claim is established.

## Initial four-corner artifact hashes

- VCL: `c03e64a084107972430d88fc5fad6f71514c6b8c947af9c6c941e71d3de9d599`
- VSM (trailing whitespace normalized only): `d7d250bdc3f2273d9101f28391cfb1f28886a284fb1c1a3720090b216ecae057`
- Assembled object: `b874149a22c9a4ef9278bfda3f49f47f45687057d325cadaee8f1263229df992` (11804 bytes)


## Single-material output capacity pass - 2026-09-20

Independent `PGL_CITY_SOURCE_SINGLE_MATERIAL_OUTPUT=1`; linked source options bit 1.
OFF retains A30/B18. ON uses A60/B36 at the same tag/data addresses. The ordinary
8-quad/48-output-vertex case now needs one output kick instead of two.

The existing X2F class has no paired-material API and its private WinTex remains
NULL; the inherited window setter is now hidden on the derived class. After
BuildPrefixes, private q178.y/z holds float capacities (30/18 or 60/36), copied
into each activation's owned prefix. The existing resident window-color VF,
its 255 clamp and FTOI0 already exist; these exact small integers survive them.
Fast-triangle and clipped-fan guards consume its y/z via MTIR. There is no new
long-lived VF/VI register, per-activation mode decision or per-triangle LQ.
The window-disable x=-1 stays unchanged, so the original paired-kick body never
reads a second material. All source payload/texture/depth/order rules remain.

A60 occupies data q181..360, below B's tag 362; B36 occupies q363..470, below the
half end 472. The existing alternating XGKICK fence transfers ownership: the
VU manual's XGKICK operation waits for the previous PATH1 transfer before
starting its successor, but the newly kicked bytes remain GIF-owned. Every
complete clipped fan is at most 18 vertices, so it fits either empty arena.

The final module remains 969 instructions / 970 padded / 70 bounded branches.
Localized generation and assembly passed; game/library/EE builds were not run.
The model now compares original X2, new X2F OFF and ON. All 300 ordered consumed
payload comparisons pass. A separate six-case directed set fills A60/B36 exactly,
reaches four alternating kicks and checks both XTOP halves. Every simulated store
is rejected if it targets the currently GIF-owned packet. This checks ownership
structure, not real-time DMA latency or GS execution.

| Model work | X2F OFF | X2F ON |
|---|---:|---:|
| All 300 cases, issue pairs |764184|760057|
| Ordinary mixed views |163005|161250|
| Extreme clipping stress |475693|474449|
| Fully inside |125486|124358|
| Output kicks |464|312|

The OFF issue total matches the prior image. ON removes 32.8% of modeled kicks
and 0.54% of modeled issue pairs, while preserving the payload. Neither percentage
is a hardware speedup claim. Current native frame/visual validation is pending.

Final artifact hashes:

- `vu1/general_clip_quad_x2f.vcl`: `e4f49e0994a0a60d0f6f8a8799c43a5a884cc8af78fb7f2e1d7261ffcbc2392d` (29220 bytes)
- `vu1/general_clip_quad_x2f_vcl.vsm`: `a5fd41756814c06c330300db4766ead0129905a887402ccdfeaa89bc2fce2230` (104733 bytes)
- `vu1/general_clip_quad_x2f.vo`: `65327eb3b40ddebc9fa3a8d5096c96f17ef3c434210e102b922222f83cc1fcb7` (11804 bytes)

## Shared clipped-triangle dispatch - 2026-09-20

`PGL_CITY_SOURCE_CLIP_DISPATCH=1` is an independent default-ON gate; source
options bit 2 reports it. Private prefix q178.w holds float 0/1, converted by
the same existing FTOI0 as the output capacities. No additional public input,
EE staging, packet borrow or renderer instance is introduced.

The previous X2F cached all four corners, classified the complete quad, then
repeated triangle cache-address walks, near tests, CLIPs and optimistic GS
stores whenever any corner failed. A clipped triangle immediately discarded
those stores and rebuilt the original polygon for Sutherland-Hodgman.

The candidate retains a separate verdict for ABC and ACD only after the whole
quad proof fails and only when culling is OFF. Four consecutive CLIPs leave
A/B/C/D at shifts 18/12/6/0; masks 0x3cf3c0 and 0x3c03cf select the same XY
bits as the original per-triangle 0xf3cf. Each triangle's minimum W uses the
existing subtract/clamp/FTOI4 sign test. The verdicts occupy q122.x/y; z grants
direct dispatch. ACD's verdict moves to x when ABC finishes. Polygon scratch
begins at q125 and neither clipping nor output spills writes q122.

A proved inside triangle uses the existing literal-index emitter. Otherwise
the candidate enters the unchanged five-plane clipper before any optimistic
triangle stores. Culling ON or the gate OFF retains the original triangle
path. Original authored diagonal, clipping order/intersections, color/alpha,
ADC, GS depth and ordered consumed packet payload are unchanged. The arena
capacity check still precedes dispatch, preserving existing spill behavior.

Checks performed:

- Only X2F regenerated and assembled: 1002 pairs, 1002 padded, 74 bounded
  branches, the same two E stops and three kick sites. Instruction/branch
  guard passes; minimum emitted CLIP-to-FCAND distance is four issue pairs.
- All 300 complete packet cases match original X2 under both output-capacity
  settings and both dispatch settings. An additional 56 directed cases cover
  near epsilon, zero W, exact side boundaries and adjacent float32 values,
  both XTOP halves and independently varied UV/RGBA.
- Six spill fixtures still fill A60/B36 exactly, alternate four kicks and
  reject writes into the currently GIF-owned packet. Cull-enabled routing
  retains the original path. Clip/fan/kick macro bodies compare verbatim
  with original X2; no new culling-MAC or native-rounding claim is made.
- Independent peer reviewed clip history, W arithmetic, mode/prefix lifetime,
  q122 preservation through S-H/output spill and generated branch seams.
- Corrected the model's `wholly_inside` corpus: its old `*.01` shrink left
  some earlier huge-XY stress mutations outside. The new absolute bounded
  XY fixture really is inside. Earlier tables above describe their historical
  corpus and must not be interpreted as a strictly-inside guarantee.

Dynamic issue pairs, output capacity ON:

| Corpus | Cases | Original X2 | Prior X2F | New X2F dispatch ON |
|---|---:|---:|---:|---:|
| Ordinary mixed views | 117 | 164420 | 161250 | 160402 |
| Extreme clipping stress | 83 | 441104 | 474449 | 442248 |
| Strictly inside, corrected corpus | 100 | 100028 | 95468 | 97236 |

The clipping-stress tax falls from 7.56% to 0.26% relative to original X2,
or 6.79% fewer issue pairs than prior wide X2F. Ordinary work falls 0.53%.
The cost is four extra pairs per strictly-inside quad (an eight-quad activation
is 1685 -> 1717, +1.9%). This tradeoff is explicit: it targets manual/near
views and does not establish a native frame-time or idle-performance win.
The host model is not a VU floating-point, DMA-timing or GS pixel emulator.
Game/library/EE compilation and Bruno's hardware comparison remain pending.

Final artifact hashes for this pass:

- VCL: `03afca73909308693f6d7f40ef529f4239a1bdf347a8c9fb75eae2fca753f62c`
- VSM: `157baa37cb0a489fb05d237b78e6f18685aba1e36d000ef7b0671ba485c767c4`
- Object: `189bfa61ba4acdc6f17475b5b2deeed8ec4688c122cd29de84aaec893590ad9a`

Scratch source/before-image and generation/assembly evidence:
`profiling/ps2-city-spiral/x2f-dispatch-20260920/` in the main repository.
