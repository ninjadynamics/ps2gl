# Compact source context tail patch — 2026-09-20

Status: implemented, host packet models and independent source review pass.
No EE/library/game build, microcode generation, emulator or hardware test was
performed for this change. Packet savings are exact model results; native
timing and the frequency of eligible transitions remain unmeasured.

## Architecture finding

X2R roads, X2P pools, X2B billboards and X2A corner-alpha billboards already
share `CClipRoadX2RRenderer` submission. Their geometry, clipping programs and
output arenas remain separate. The existing retained-context optimization
requires the entire source context to match. This misses a useful shared
case: unchanged camera/projection and source prefix, with only q55/q56 changed.
Those fields are road color/height, pool height, or billboard axes. The game
road owner explicitly reuses its view prefix while replacing color/height.

The new `PGL_SOURCE_CONTEXT_TAIL_PATCH` header default is **1**. It is
independent of `PGL_ROAD_CONTEXT_REUSE`; all four gate pairs were modeled.
`pglGetSourceContextSubmissionOptions()` reports **bit0, value1**, when this
gate and at least one compact source renderer family are compiled in.
Existing query bits and the public source-context structs are unchanged.

The source prefix is compared before updating the cached public context:
864 bytes for roads/pools, 96 bytes for billboards. A matched prefix copies
only the final 32 bytes into the renderer. Comparing the tail first in the
old equality check avoids scanning an unchanged prefix twice for the normal
tail-only change. A matching source prefix alone never proves VU retention.

The VU update additionally requires the original retained-context validity,
renderer identity, packet identity/base/end, frame, and exact 108-byte raster
key. A microcode load invalidates retention. A program transition, packet
append, changed matrix/depth/culling/giftag/near, or changed source prefix
uses the original full context path. The existing unchanged-context path
still emits no context packet when its old gate is ON.

| Eligible changed context | Original full packet | New packet | Saved |
|---|---:|---:|---:|
| X2R road / X2P pool | 1152 B | 64 B | 1088 B (94.44%) |
| X2B / X2A billboard | 400 B | 64 B | 336 B (84.00%) |

The new CNT packet contains STCYCL(1,1), STMOD(none), FLUSH, and an absolute
V4_32 UNPACK of q55..56. Its 32-byte source is copied into packet-owned
storage. No REF survives to mutable renderer or caller storage. Full admission
and the existing conservative capacity reservation are unchanged; 64 bytes
fits inside either previous full-context budget. Context-byte metrics include
the patch, and delta-context metrics count it.

## VIF/VU proof

The relevant official references are the local EE User's Manual pp112
(FLUSH), 115 (MSCNT), and 135 (TOPS/DBF), routed through
`docs/Technical/PS2/BOOKMARKS.md`. FLUSH orders the prior VU context readers
before the overwrite. MSCNT resumes after the previous program end. No
elapsed-time or extra-NOP assumption is introduced. The documented FLUSH
exception concerns XGKICK with E set; these generated programs do not combine
XGKICK and E in one pair, and q55/q56 are outside every GIF output arena.

All four current generated VSM programs load the entry matrix from q62..65
into VF01..VF04. None writes those registers after `r_main_lid`. The X2R near
value in VF05.w is also never rewritten after that label. X2P's unused entry
near calculation is eliminated by VCL. The two billboard programs load only
the matrix at entry. Every program ends a batch at `--cont`, then branches to
`r_main_lid` on continuation. Road/pool material height and road color reload
inside descriptor decode; both cached and original billboard decoders reload
their axes after continuation. The patch leaves private billboard q54.w
unchanged. No absolute VU store overwrites retained context in these programs.

Consequently the patch needs no MSCAL0 reload, and it leaves BASE/OFFSET and
TOP/DBF progression untouched. Subsequent existing descriptor MSCNT commands
continue alternating the same 472-qword halves. Geometry/material order,
clipping arithmetic, output spill rules, color, alpha, depth and matrices are
unchanged. No VU source or generated image changed.

## Validation and limits

`python tests/test_source_context_tail_patch.py`: **5 tests PASS**.

- 2560 encoded material sequences with 632 actual tail patches; all four
  renderer formats, four gate pairs and both private billboard reuse flags.
- Single, odd, even and multiple-buffer draws; source-chain CNT/TTE decoding,
  actual context/descriptor byte comparison, entry matrix/near preservation,
  and both absolute input halves (79 and 551).
- Mutation of every source word, all 27 raster-key words, and every ownership
  field; exact signed-zero inequality; special-bit owned payload copies.
- Scheduled entry-register write audit and absence of absolute context stores;
  production source checks for retention guards, FLUSH ordering, absolute
  UNPACK, owned copying, and no MSCAL/BASE/OFFSET in the patch.

`python tests/test_billboard_context_serialization.py`: **3 tests PASS**.
`git diff --check`: PASS. These models do not execute EE binaries or establish
native EE/VU numerical equivalence. That limitation predates this byte-only
context update; arithmetic programs are unchanged.

Independent review by `city_arch_lighting`: **PASS**, including direct lesson
cross-check, exact prefix/tail sizes, compare-before-copy, all gate pairs,
owner/packet/raster proof, private q54.w retention, explicit STCYCL/STMOD,
FLUSH/MSCNT behavior, both billboard decode paths and final early guard.
The reviewer also reran all five focused tests successfully.

## Other audit findings and deliberate limits

- Renderer program changes still upload their microcode and full context.
  Cross-program retained-memory reuse would require a broader write-set and
  register-lifetime protocol. This patch does not claim that proof.
- The full context path still builds a 27-word raster key and writes portions
  of the same raster state again. Matrix admission also repeats identity and
  finite checks across direct calls. Their native cost is unmeasured; caching
  public mutable state without an exact invalidation proof would be unsafe.
- Matrix stack load/pop invalidation can repeat combined-matrix work across
  game subsystems. Eliding arbitrary equal forward matrices risks changing
  the existing deferred inverse journal and its numerical order. No such
  matrix rewrite was attempted.
- Changed-prefix misses may add a short tail comparison, while exact matches
  split the previous contiguous equality check into tail and prefix checks.
  Eligible tail transitions remove an entire prefix copy and most context
  transfer/setup. Target A/B should measure both sustained views and turns;
  transfer-byte savings alone do not establish a frame-time gain.
- The lesson constraints remain in force: separate EE source-cache validity
  from live VU context, separate construction from DMA lifetime, preserve
  typed packet ownership, and compare completed work/counter coverage.
  No lesson files or root documentation were edited by this change.
