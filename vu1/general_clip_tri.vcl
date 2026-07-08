/*     Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

            This file is subject to the terms and conditions of the GNU Lesser
       General Public License Version 2.1. See the file "COPYING" in the
       main directory of this archive for more details.                             */

     ; HyperSolar VU1 clip renderer (tri lists) — a VERBATIM port of the EE
     ; software clipper (DrawMeshSHNearRotY's clip_triangle) to VU1. Selected
     ; via the PGL_CLIP_TRIANGLES custom prim (src/clip_renderer.cpp).
     ;
     ; Full 5-plane Sutherland-Hodgman: the near plane (eye units, from
     ; pglSetClipNear via kFogParams.x) plus the 4 side planes, all of which
     ; CUT geometry exactly like the EE clipper — never whole-tri rejection.
     ; The side planes sit at the GS guard band (~2x the screen edge, from
     ; kClipInfo's clip scales); the EE cut at 5.5x NDC — both are off-screen,
     ; so the visible result is pixel-identical while every emitted coordinate
     ; stays inside the GS 12.4 range.
     ;
     ; Per input tri:
     ;   - fast path: fully inside all 5 planes (near MAC-flag test + the 3
     ;     clipw side judgments) -> the optimistically-stored tri commits;
     ;     ADC carries backface cull + fog only.
     ;   - else -> S-H handler: the tri re-transforms WITHOUT the divide into
     ;     a VU-mem polygon buffer and ping-pongs A->B through the 5 planes
     ;     (exactly the EE's buf_a/buf_b), max 3+5=8 verts; degenerate (<3)
     ;     polys vanish; the survivor emits as a fan of independent tris with
     ;     per-vert divide, guard-safe coords, lerped STQ, const color, fog.
     ; The giftag NLOOP is patched with the real emitted count before xgkick.
     ; A capacity guard drops a clipped fan that would overflow the output
     ; buffer (mirrors the EE path's scratch-full drop; pathological only).
     ;
     ; Unlit / constant-color by design: the stock lighting + finish_colors
     ; passes walk the output 1:1 with the input, which 1:N breaks. RGBA
     ; (const color + material-diffuse alpha) is computed once per buffer;
     ; with GL lighting disabled ps2gl routes glColor through the emission
     ; slot, so this matches the stock unlit result.
     ;
     ; Buffer layout is THIS RENDERER'S OWN (not vu1_mem_linear.h's halves).
     ; Offsets are buffer-relative, double-buffered via xtop:
     ;   0..4     header (kNumVertices + strip ADCs, stock XferBufferHeader)
     ;   5..94    input verts (30 max x 3q)          = kInputStart
     ;   95..99   the 5 plane qwords (built per buffer)
     ;   100..115 polygon scratch A (8 verts x 2q: pos, stq)
     ;   116..131 polygon scratch B
     ;   132      output giftag
     ;   133..471 output verts (113 capacity; guard trips at 111)
     ; EE coupling: CClipTriRenderer's ctor passes inGeomBufSize=90 and its
     ; DrawLinearArrays caps batches at multiples of 6 verts. Changing any
     ; of these numbers means re-checking this layout.

     #include       "vu1_mem_linear.h"

     .include       "db_in_db_out.i"
     .include       "math.i"
     .include       "lighting.i"
     .include       "clip_cull.i"
     .include       "geometry.i"
     .include       "io.i"
     .include       "general.i"

kInputQPerV         .equ           3
kOutputQPerV        .equ           3

kCPlanes            .equ           95
kCPolyA             .equ           100
kCPolyB             .equ           116
kCOutTag            .equ           132
kCOutData           .equ           133
kCCapVerts          .equ           111

     .init_vf_all
     .init_vi_all

     .name          vsmGeneralClipTri

     ; ---------------------------------------------------
     ; signed distance of a pre-divide vert to a plane qword (A,B,C,D):
     ;   d.x = A*X + B*Y + C*w + D
     ; planes are built per buffer; near = (0,0,1,-near), sides =
     ; (-+sx/sy, 0/0, 2048, 0) from the kClipInfo guard-band scales.

     .macro         pd_plane       d, v, pl
     mul.xy         pdt\@, \pl, \v
     mulw.z         pdt\@, \pl, \v
     adday.x        acc, pdt\@, pdt\@
     maddz.x        pdt\@, ones, pdt\@
     addw.x         \d, pdt\@, \pl
     .endm

     ; ---------------------------------------------------
     ; deterministic sign of \d.x -> 16-bit int in \f (bit 15 = negative).
     ; NEVER use fmand for these signs: vcl "regenerates" MAC flags for a
     ; distant consumer by inserting an ABS — but ABS does not update MAC
     ; flags on real hardware (VU manual 3.3.5), so the consumer reads
     ; STALE flags (here: the near plane's d minus its D term = plain w ->
     ; cuts at w=0 -> exploding verts). Clamp to +-2047 so ftoi4's result
     ; (+-32752) fits VI's 16 bits with the sign in bit 15; the clamp is a
     ; COPY (t uses the true d), and truncation gives a d in (-1/16, 0) an
     ; "inside" verdict — a bounded epsilon (near cuts at w >= near-1/16).

     .macro         pd_sign        f, d
     loi            2047.0
     minii.x        pds\@, \d, i
     loi            -2047.0
     maxi.x         pds\@, pds\@, i
     ftoi4.x        pds\@, pds\@
     mtir           \f, pds\@[x]
     .endm

     ; ---------------------------------------------------
     ; S-H edge step (a -> b): if a inside, emit a; if the edge crosses,
     ; emit the intersection at t = d_a/(d_a - d_b) (pos xyzw pre-divide,
     ; raw stq xyz). Inside flags f are 0 (in) / nonzero (out) integer regs.
     ; Writes through cp_dst (pos,stq pairs), counts in cp_m.

     .macro         cp_edge        pa, sa, da, fa, pb, sb, db, fb, pid
     iand           ce_sa\@, \fa, adc_bit
     ibne           ce_sa\@, vi00, ce_askip\@
     sq             \pa, 0(cp_dst)
     sq             \sa, 1(cp_dst)
     iaddiu         cp_dst, cp_dst, 2
     iaddiu         cp_m, cp_m, 1
ce_askip\@:
     iand           ce_sb\@, \fb, adc_bit
     ibeq           ce_sa\@, ce_sb\@, ce_nocross\@
     sub.x          ce_den\@, \da, \db
     div            q, \da[x], ce_den\@[x]
     sub.xyzw       ce_e\@, \pb, \pa
     mulaw.xyzw     acc, \pa, vf00[w]
     maddq.xyzw     ce_ip\@, ce_e\@, q
     sub.xyz        ce_es\@, \sb, \sa
     mulaw.xyz      acc, \sa, vf00[w]
     maddq.xyz      ce_is\@, ce_es\@, q
     loi            \pid
     maxi.w         ce_is\@, vf00, i
     sq             ce_ip\@, 0(cp_dst)
     sq             ce_is\@, 1(cp_dst)
     iaddiu         cp_dst, cp_dst, 2
     iaddiu         cp_m, cp_m, 1
ce_nocross\@:
     .endm

     ; ---------------------------------------------------
     ; one S-H pass: clip the sh_n-vert polygon at \srcb against plane
     ; slot \pslot, writing to \dstb; sh_n updated to the output count.

     ; DIAG v8: \pid tags intersection verts with the pass id (1..5) in the
     ; stq qword's unused .w; originals carry 0 from sh_load. The fan colors
     ; verts by this tag, so bad geometry names the pass that made it.
     .macro         clip_pass      srcb, dstb, pslot, pid
     ; SCHEDULING FENCE (same vcl bug as the sh_load one): the previous
     ; pass wrote this pass's source polygon through cp_dst; vcl does not
     ; model VU-mem aliasing between distinct pointer registers and will
     ; hoist this pass's first loads above those stores. The basic-block
     ; boundary stops it. Do NOT remove.
     b              cp_fence_lid\@
cp_fence_lid\@:
     lq             cp_pl, \pslot(buffer_top)
     iaddiu         cp_src, buffer_top, \srcb
     iaddiu         cp_dst, buffer_top, \dstb
     iaddiu         cp_m, vi00, 0
     ; first vert (kept for the wrap edge)
     lq             cp_pf, 0(cp_src)
     lq             cp_sf, 1(cp_src)
     pd_plane       cp_df, cp_pf, cp_pl
     pd_sign        cp_ff, cp_df
     ; prev = first
     move.xyzw      cp_pp, cp_pf
     move.xyzw      cp_sp, cp_sf
     move.x         cp_dp, cp_df
     iadd           cp_fp, cp_ff, vi00
     iaddiu         cp_i, vi00, 1
     iaddiu         cp_src, cp_src, 2
cp_loop_lid\@:
     ibeq           cp_i, sh_n, cp_wrap_lid\@
     lq             cp_pc, 0(cp_src)
     lq             cp_sc, 1(cp_src)
     pd_plane       cp_dc, cp_pc, cp_pl
     pd_sign        cp_fc, cp_dc
     cp_edge        cp_pp, cp_sp, cp_dp, cp_fp, cp_pc, cp_sc, cp_dc, cp_fc, \pid
     move.xyzw      cp_pp, cp_pc
     move.xyzw      cp_sp, cp_sc
     move.x         cp_dp, cp_dc
     iadd           cp_fp, cp_fc, vi00
     iaddiu         cp_src, cp_src, 2
     iaddiu         cp_i, cp_i, 1
     b              cp_loop_lid\@
cp_wrap_lid\@:
     cp_edge        cp_pp, cp_sp, cp_dp, cp_fp, cp_pf, cp_sf, cp_df, cp_ff, \pid
     iadd           sh_n, cp_m, vi00
     .endm

     ; ---------------------------------------------------
     ; emit one polygon vert (pre-divide pos \p + raw stq \s) at output
     ; slot \k: divide, gs convert, stq perspective, const color, fog;
     ; \adcreg = adc_bit (suppress) or vi00 (kick the triangle).

     .macro         emit_mvert     p, s, k, adcreg
     ; 1/w is landed in a REGISTER (addq) instead of being consumed twice
     ; from the pipeline Q: vcl re-rolls the schedule every regen and
     ; interleaves the fan verts' divides, so a second mulq far from its div
     ; can read a NEIGHBOR vert's Q on some builds. Positions barely show
     ; it; texture perspective explodes on large-w-range tris (the ground
     ; "comb"). One consumer right at the div, then register dataflow.
     div            q, vf00[w], \p[w]
     addq.x         emq\@, vf00, q
     mulx.xyz       emp\@, \p, emq\@
     add.xyz        emp\@, emp\@, gs_offsets
     ftoi4.xyz      emp\@, emp\@
     mulx.xyz       ems\@, \s, emq\@
     sq.xyz         ems\@, 0+\k(next_output)
     store_rgba     const_color, \k
     fog_coef       emf\@, \p, fog_params
     ior            emadc\@, \adcreg, emf\@
     mfir.w         emp\@, emadc\@
     store_xyzf     emp\@, \k
     .endm

     ; ---------------------------------------------------
     ; load input vert \in_off (quads, relative to next_input), re-transform
     ; WITHOUT the divide, store as polygon-A vert \slot (0-based).

     .macro         sh_load        in_off, slot
     lq.xyz         shv\@, \in_off(next_input)
     mul_pt_mat_44  shp\@, vert_xform, shv\@
     sq             shp\@, kCPolyA+(\slot*2)(buffer_top)
     lq.xyz         shs\@, \in_off+2(next_input)
     mulx.w         shs\@, vf00, vf00
     sq             shs\@, kCPolyA+(\slot*2)+1(buffer_top)
     .endm

     --enter
     --endenter

     ; ------------------------ initialization ---------------------------------

     load_vert_xfrm vert_xform
     load_fog_params fog_params

     ; near plane (eye units) rides the unused x field of the fog params;
     ; w-aligned for the fast path's sub.w test. The additive base MUST be a
     ; zeroed .w — vf00.w is ONE, and using it here made near = 1 + near
     ; (everything near-clipped at w=2: eaten floor corners, flickering
     ; jets, and a firehose of false handler dispatches)
     sub.w          zw_ent, vf00, vf00
     addx.w         near_plane, zw_ent, fog_params[x]

     --cont

     ; -------------------- transform & clip loop ------------------------------

main_loop_lid:

     init_constants
     ; clip_cull.i's init_clip_cnst minus the do_clipping load (this
     ; renderer never consults it — one less register held across the loop)
     lq.xyz         clip_scales, kClipInfo(vi00)
     loi            2048.0
     maxi.w         clip_scales, vf00, i

     init_io_loop   kInputStart, kCOutData
     init_out_buf

     ; init_bfc minus the integer mask (reloaded per tri to keep its
     ; lifetime local — see the bfc_tri call site)
     lq.w           bfc_multiplier, kBackFaceCullMult(vi00)
     get_ones_vec   ones

     ; finished constant RGBA, once per buffer: rgb = emission +
     ; global_amb * material_amb, clamped; a = mat diffuse alpha * 128
     get_cnst_color const_color
     loi            128.0
     load_mat_diff  const_color, w
     muli.w         const_color, const_color, i
     loi            255.0
     minii.xyzw     const_color, const_color, i
     ftoi0          const_color, const_color

     ; build the 5 plane qwords (A,B,C,D), d = A*X + B*Y + C*w + D:
     ;   slot 0 near: (0, 0, 1, -near)
     ;   slot 1 +x:   (-sx, 0, 2048, 0)    slot 2 -x: (sx, 0, 2048, 0)
     ;   slot 3 +y:   (0, -sy, 2048, 0)    slot 4 -y: (0, sy, 2048, 0)
     ; sx/sy/2048 come from clip_scales (kClipInfo guard band).
     sub.xyzw       pl_zero, vf00, vf00
     move.xyzw      pl_t, pl_zero
     addw.z         pl_t, pl_zero, vf00
     subw.w         pl_t, pl_zero, near_plane
     sq             pl_t, kCPlanes+0(buffer_top)
     move.xyzw      pl_t, pl_zero
     subx.x         pl_t, pl_zero, clip_scales
     addw.z         pl_t, pl_zero, clip_scales
     sq             pl_t, kCPlanes+1(buffer_top)
     move.xyzw      pl_t, pl_zero
     addx.x         pl_t, pl_zero, clip_scales
     addw.z         pl_t, pl_zero, clip_scales
     sq             pl_t, kCPlanes+2(buffer_top)
     move.xyzw      pl_t, pl_zero
     suby.y         pl_t, pl_zero, clip_scales
     addw.z         pl_t, pl_zero, clip_scales
     sq             pl_t, kCPlanes+3(buffer_top)
     move.xyzw      pl_t, pl_zero
     addy.y         pl_t, pl_zero, clip_scales
     addw.z         pl_t, pl_zero, clip_scales
     sq             pl_t, kCPlanes+4(buffer_top)

     iaddiu         adc_bit, vi00, 0x7fff
     iaddiu         adc_bit, adc_bit, 1

     ; emitted output verts this buffer (patched into NLOOP after the loop)
     iaddiu         out_count, vi00, 0

xform_loop_lid:

     ; ---- vertex 1 (ADC always set; F field still feeds fog interpolation)

     load_vert      vert, 0
     xform_vert     xformed_vert_1, vert_xform, vert
     vert_to_gs     gs_vert_1, xformed_vert_1
     store_rgba     const_color, 0
     load_stq       tex_stq, 0
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, 0

     ; near classify, flag-free (see pd_sign): sign of (eye w - near),
     ; clamped + ftoi4 + mtir; bit 15 of the 16-bit int = behind
     sub.w          near_d1, xformed_vert_1, near_plane
     loi            2047.0
     minii.w        near_d1, near_d1, i
     loi            -2047.0
     maxi.w         near_d1, near_d1, i
     ftoi4.w        near_d1, near_d1
     mtir           near_f, near_d1[w]
     iand           near_any, near_f, adc_bit

     clip_vert      xformed_vert_1
     fog_coef       fog_i1, xformed_vert_1, fog_params
     ior            fog_adc1, adc_bit, fog_i1
     mfir.w         gs_vert_1, fog_adc1
     store_xyzf     gs_vert_1, 0

     ; ---- vertex 2

     load_vert      vert, kInputQPerV
     xform_vert     xformed_vert_2, vert_xform, vert
     vert_to_gs     gs_vert_2, xformed_vert_2
     store_rgba     const_color, kOutputQPerV
     load_stq       tex_stq, kInputQPerV
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, kOutputQPerV

     sub.w          near_d2, xformed_vert_2, near_plane
     loi            2047.0
     minii.w        near_d2, near_d2, i
     loi            -2047.0
     maxi.w         near_d2, near_d2, i
     ftoi4.w        near_d2, near_d2
     mtir           near_f, near_d2[w]
     iand           near_f, near_f, adc_bit
     ior            near_any, near_any, near_f

     clip_vert      xformed_vert_2
     fog_coef       fog_i2, xformed_vert_2, fog_params
     ior            fog_adc2, adc_bit, fog_i2
     mfir.w         gs_vert_2, fog_adc2
     store_xyzf     gs_vert_2, kOutputQPerV

     ; ---- vertex 3

     load_vert      vert, kInputQPerV+kInputQPerV
     xform_vert     xformed_vert_3, vert_xform, vert
     vert_to_gs     gs_vert_3, xformed_vert_3
     store_rgba     const_color, kOutputQPerV+kOutputQPerV
     load_stq       tex_stq, kInputQPerV+kInputQPerV
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, kOutputQPerV+kOutputQPerV

     sub.w          near_d3, xformed_vert_3, near_plane
     loi            2047.0
     minii.w        near_d3, near_d3, i
     loi            -2047.0
     maxi.w         near_d3, near_d3, i
     ftoi4.w        near_d3, near_d3
     mtir           near_f, near_d3[w]
     iand           near_f, near_f, adc_bit
     ior            near_any, near_any, near_f

     clip_vert      xformed_vert_3

     ; backface/frontface cull (fast path only; the integer mask reloads
     ; here so it is not held across the S-H handler)
     ilw.w          z_sign_mask, kBackFaceCullMult(vi00)
     bfc_tri        xformed_vert_1, xformed_vert_2, xformed_vert_3

     ; side judgments of the 3 clipws, X/Y bits only (near owns the Z axis;
     ; the EE plane set has no far plane)
     fcand          vi01, 0xf3cf

     ; optimistic third-vert ADC: backface verdict + fog (side planes are
     ; clean on the commit path by construction — see dispatch below)
     ior            new_adc_bit, z_sign, vi00
     iaddiu         new_adc_bit, new_adc_bit, 0x7fff
     iand           new_adc_bit, new_adc_bit, adc_bit
     fog_coef       fog_i3, xformed_vert_3, fog_params
     ior            new_adc_bit, new_adc_bit, fog_i3

     mfir.w         gs_vert_3, new_adc_bit
     store_xyzf     gs_vert_3, kOutputQPerV+kOutputQPerV

     ; ---- dispatch: commit the stored tri only if fully inside all 5
     ; planes; anything touching any plane goes to the S-H handler

     ior            disp, near_any, vi01
     ibne           disp, vi00, sh_handler_lid
     next_o         3
     iaddiu         out_count, out_count, 3
     b              tri_next_lid

sh_handler_lid:
     ; rebuild the tri as polygon A (pre-divide pos + raw stq)
     sh_load        0, 0
     sh_load        kInputQPerV, 1
     sh_load        kInputQPerV+kInputQPerV, 2
     iaddiu         sh_n, vi00, 3

     ; SCHEDULING FENCE — load-bearing, do not remove. vcl schedules per
     ; basic block and does NOT model VU-mem aliasing between distinct
     ; pointer registers: without a block boundary here it hoists the first
     ; pass's polygon[0] LOADS (via cp_src) above sh_load's STORES of the
     ; same addresses (via buffer_top), so the fan pivot vertex reads STALE
     ; scratch from the previous invocation. The branch-to-next-line forms
     ; a block boundary no memory op can cross.
     b              sh_fence_lid
sh_fence_lid:

     ; 5 planes, ping-ponging A->B->A->B->A->B
     clip_pass      kCPolyA, kCPolyB, kCPlanes+0, 1.0
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyB, kCPolyA, kCPlanes+1, 2.0
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyA, kCPolyB, kCPlanes+2, 3.0
     isubiu         sh_t, sh_n, 3
     ibltz           sh_t, tri_next_lid
     clip_pass      kCPolyB, kCPolyA, kCPlanes+3, 4.0
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyA, kCPolyB, kCPlanes+4, 5.0
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid

     ; capacity guard (mirrors the EE scratch-full drop): fan emits
     ; 3*(sh_n-2) verts
     isubiu         fe_t, sh_n, 2
     iadd           fe_v, fe_t, fe_t
     iadd           fe_v, fe_v, fe_t
     iadd           fe_chk, out_count, fe_v
     isubiu         fe_chk, fe_chk, kCCapVerts
     ibgtz          fe_chk, tri_next_lid

     ; SCHEDULING FENCE: pass 5 wrote the final polygon through cp_dst;
     ; the fan reads it through fe_p0/fe_pi. Same aliasing bug as above.
     b              fe_fence_lid
fe_fence_lid:

     ; fan emit from the final polygon (in B): tris (P0, Pi, Pi+1)
     iaddiu         fe_p0, buffer_top, kCPolyB
     iaddiu         fe_pi, fe_p0, 2
fe_loop_lid:
     lq             fe_pos, 0(fe_p0)
     lq             fe_stq, 1(fe_p0)
     emit_mvert     fe_pos, fe_stq, 0, adc_bit
     lq             fe_pos, 0(fe_pi)
     lq             fe_stq, 1(fe_pi)
     emit_mvert     fe_pos, fe_stq, kOutputQPerV, adc_bit
     lq             fe_pos, 2(fe_pi)
     lq             fe_stq, 3(fe_pi)
     emit_mvert     fe_pos, fe_stq, kOutputQPerV+kOutputQPerV, vi00
     next_o         3
     iaddiu         out_count, out_count, 3
     iaddiu         fe_pi, fe_pi, 2
     isubiu         fe_t, fe_t, 1
     ibgtz          fe_t, fe_loop_lid

tri_next_lid:
     next_i         3
     ; overshoot-proof termination: loop only while a FULL tri (9 quads)
     ; remains, never on pointer equality (a 3n-1 vert buffer from ps2gl's
     ; spilled-strip splitter would spin VU1 forever)
     isub           loop_left, last_input, next_input
     isubiu         loop_left, loop_left, 8
     ibgtz          loop_left, xform_loop_lid

     ; ---------------- patch NLOOP + kick packet to GS -----------------------

     ; rebuild the giftag from the template with the EMITTED vert count
     lq             gif_tag_p, kGifTag(vi00)
     mtir           eop_p, gif_tag_px
     ior            eop_p, eop_p, out_count
     mfir.x         gif_tag_p, eop_p
     iaddiu         packet_start_p, buffer_top, kCOutTag
     sq             gif_tag_p, 0(packet_start_p)

     xgkick         packet_start_p

     --cont

     b    main_loop_lid

.END ; for gasp
