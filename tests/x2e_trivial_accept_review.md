# X2E four-corner classification candidate — September 20, 2026

Gate: `PGL_DECAL_TRIVIAL_ACCEPT=1`, reported by linked decal option bit 5
(`32`). The library owns private `PGLDecalContext.clip[3]`; public callers
still supply zero. No public structure size or input format changed.

The compact decoder now evaluates the existing five plane distances four-wide
for A/B/C/D. It retains the scalar distance's separate multiplication/addition
tree, including its leading zero add. Strict CLIP negative flags preserve the
old inclusive boundary rule. Only a quad whose four corners pass all five
planes bypasses the per-triangle polygon walker. Any outside corner, or gate
OFF, takes the existing walker. Every source triangle still uses its own
skip/depth allowance, projection, material and output fan. Projected exception
records take their preceding path.

The certificate is rewritten for every compact descriptor at half-relative
q221, previously unused before the q224 output tag. Input halves, output arena
caps, base-before-glow sweep order, scratch ownership and all synchronization
remain unchanged. No texture-cache flush or completion wait was removed.

## Verification

- `x2e_trivial_accept_model.py`: 1,200 scheduled classifier cases, including
  exact clip boundaries and adjacent inside/outside float ULPs, signed zero,
  both gates and both halves. 160 complete base/glow outputs match gate OFF.
- 32 complete compact activation pairs exercise 1/2/5/16 descriptors, both
  depth formats/materials/halves, alternating accepted/partial quads and
  independently skipped triangles: 1,224 vertices and 72 output kicks match.
  Context/source bytes remain unchanged and every descriptor resets its proof.
- Existing XY decoder: 5,000 paths / 20,000 corners; exceptional decoder:
  2,400 records / 7,200 vertices plus 128 full activations; mixed packet model:
  1,008 sweeps / 72,048 activations. All pass.
- Generated schedule: seven CLIP/FCAND sites have at least four issue pairs;
  all three landed-Q reads retain WAITQ or the required seven-pair DIV gap.
  No simultaneous upper/lower VF destination collision.
- Only X2E generated and assembled: 683 instruction pairs / 684 padded,
  50 bounded branches, object 8,652 bytes. No game/library/EE compilation.

For one wholly inside quad/material, the host interpreter executes 1,572 pair
lines with gate OFF and 799 with it ON. Across the mixed corpus: 290,032 versus
206,142. These counts exclude actual pipeline stalls, GIF backpressure and
hardware timing; they are not a measured frame-time gain.

The initial draft exposed VCL's suffix interpretation for virtual names ending
`_x`/`_y`/`_w`; assembly review caught unintended register aliases. The final
SoA names end in `0`, and scheduled tests check all three independent vectors.

Final SHA-256:

| Artifact | SHA-256 |
|---|---|
| `general_clip_decal_x2e.vcl` | `277a071d69f906b414236934f8b00d4b57a15edd05400ae3195f2c8508a0c891` |
| `general_clip_decal_x2e_vcl.vsm` | `7faf06a09df52a6248c9dfa19273fd440de1c9faaf7c502e2c9e431ca9a2d43e` |
| `general_clip_decal_x2e.vo` | `2ca3dab4fb07288ee43b6e7e6d6f02bbd7eb5ddc78853cf2b21f62c25c1e0bab` |

Hardware acceptance and end-to-end savings remain pending. Keep the gate for
the dense idle-cycle comparison and near/side-crossing manual flight.

Independent source/scheduled-memory review found no correctness issue. The
actual execution trace gives decoded-position SQ to classifier LQ gaps of at
least 7 issue pairs with XY reuse OFF and 9 ON, certificate ISW to triangle ILW
35 pairs, and initial gate ISW to classifier ILW at least 81 pairs. Both halves,
both gates and inside-to-partial transitions were checked. Strict CLIP flags
(VU User's Manual p40) agree with masks 42/2 and keep both signed zeros inside.
GS-offset XYZ and the fourth distance lane survive until their consumers;
per-triangle skip checks and projected-record isolation remain intact.

## Related read-only audit

`FinishRenderingGeometry` waits for the previous normal chain's SIGNAL, which
is required by packet/frame ownership. GS manual page 95 distinguishes SIGNAL
command progress from FINISH raster completion. It is unsafe to erase that wait
because it is visible in telemetry. The candidate reduces work before the
acknowledgement instead.

Ordinary `CMMTexture::Use` still sends a TEXFLUSH-containing settings block when
the existing complete-settings reuse proof does not apply. GS manual page 51
requires invalidation after image/CLUT writes and rendered texture updates;
direct-color resident-image binds might admit a narrower future flush-elision
proof. That requires a real memory-write/residency/packet epoch and ownership
audit, not simply equal texture IDs. No such optimization is included here.
