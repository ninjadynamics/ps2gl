/* X2F retains four authored ABCD corners with the X2 raster contract.
 * This single-material module shares the existing five-plane triangle clipper,
 * near epsilon, GS color/fog packing and lossless output-arena spill.
 * Original X2 image/source remain untouched; tests compare copied clip bodies.
 * Input q5..124: up to ten quads, four corners each, pos/STQ/RGBA.
 * Private absolute q1..4 predivide positions; q5..8 divided positions;
 * q9..20 formatted STQ/RGBA/XYZF for four corners; q21 phase (0=ABC,1=ACD).
 * q22.x/y triangle verdicts, z allows direct dispatch into unchanged S-H.
 * These scratch slots are outside both VIF input halves and all GIF output.
 * X2F reads no fixed-function light records. Renderer transitions restore
 * the next owner's context; context writes retain FLUSH before UNPACK.
 * Planes125..129, polyA130..153,polyB154..177;staging178..179 and both
 * output arena addresses180..471 exactly match X2. With no window material,
 * private q178.y/z supplies float A60/B36 instead of A30/B18 under the linked gate.
 * Private q178.w selects shared classification dispatch, independently gated.
 * A data181..360 precedes B tag362, B data363..470 stays below the half end.
 * X2F exposes no paired-window setter; the unused window color lanes are its cap ABI.
 * All reusable corner data is immutable throughout both triangle clips.
 */

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

kCPlanes            .equ           125
kCPolyA             .equ           130
kCPolyB             .equ           154
kCWinClr            .equ           178
kCWinTag            .equ           179
kCAWallTag          .equ           180
kCAOutData          .equ           181
kCACapVerts         .equ           30
kCBWallTag          .equ           362
kCBOutData          .equ           363
kCBCapVerts         .equ           18

     .init_vf_all
     .init_vi_all

     .name          vsmGeneralClipQuadX2F

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
     ; STALE flags. Clamp to +-2047 so ftoi4's result (+-32752) fits VI's
     ; 16 bits with the sign in bit 15; the clamp is a COPY (t uses the
     ; true d), and truncation gives a d in (-1/16, 0) an "inside" verdict
     ; — a bounded epsilon (near cuts at w >= near-1/16).

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
     ; raw stq xyz, raw color xyzw — the EE colored clipper lerps color
     ; linearly, so this stays results-verbatim). Inside flags f are 0 (in)
     ; / nonzero (out) integer regs. Writes through cp_dst (pos,stq,col
     ; triples), counts in cp_m.

     .macro         cp_edge        pa, sa, ca, da, fa, pb, sb, cb, db, fb
     iand           ce_sa\@, \fa, adc_bit
     ibne           ce_sa\@, vi00, ce_askip\@
     sq             \pa, 0(cp_dst)
     sq             \sa, 1(cp_dst)
     sq             \ca, 2(cp_dst)
     iaddiu         cp_dst, cp_dst, 3
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
     mulx.w         ce_is\@, vf00, vf00
     sub.xyzw       ce_ec\@, \cb, \ca
     mulaw.xyzw     acc, \ca, vf00[w]
     maddq.xyzw     ce_ic\@, ce_ec\@, q
     sq             ce_ip\@, 0(cp_dst)
     sq             ce_is\@, 1(cp_dst)
     sq             ce_ic\@, 2(cp_dst)
     iaddiu         cp_dst, cp_dst, 3
     iaddiu         cp_m, cp_m, 1
ce_nocross\@:
     .endm

     ; ---------------------------------------------------
     ; one S-H pass: clip the sh_n-vert polygon at \srcb against plane
     ; slot \pslot, writing to \dstb; sh_n updated to the output count.

     .macro         clip_pass      srcb, dstb, pslot
     ; SCHEDULING FENCE (vcl memory-aliasing bug): the previous pass wrote
     ; this pass's source polygon through cp_dst; vcl does not model VU-mem
     ; aliasing between distinct pointer registers and will hoist this
     ; pass's first loads above those stores. The basic-block boundary
     ; stops it. Do NOT remove.
     b              cp_fence_lid\@
cp_fence_lid\@:
     lq             cp_pl, \pslot(buffer_top)
     iaddiu         cp_src, buffer_top, \srcb
     iaddiu         cp_dst, buffer_top, \dstb
     iaddiu         cp_m, vi00, 0
     ; first vert (kept for the wrap edge)
     lq             cp_pf, 0(cp_src)
     lq             cp_sf, 1(cp_src)
     lq             cp_cf, 2(cp_src)
     pd_plane       cp_df, cp_pf, cp_pl
     pd_sign        cp_ff, cp_df
     ; prev = first
     move.xyzw      cp_pp, cp_pf
     move.xyzw      cp_sp, cp_sf
     move.xyzw      cp_cp, cp_cf
     move.x         cp_dp, cp_df
     iadd           cp_fp, cp_ff, vi00
     iaddiu         cp_i, vi00, 1
     iaddiu         cp_src, cp_src, 3
cp_loop_lid\@:
     ibeq           cp_i, sh_n, cp_wrap_lid\@
     lq             cp_pc, 0(cp_src)
     lq             cp_sc, 1(cp_src)
     lq             cp_cc, 2(cp_src)
     pd_plane       cp_dc, cp_pc, cp_pl
     pd_sign        cp_fc, cp_dc
     cp_edge        cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pc, cp_sc, cp_cc, cp_dc, cp_fc
     move.xyzw      cp_pp, cp_pc
     move.xyzw      cp_sp, cp_sc
     move.xyzw      cp_cp, cp_cc
     move.x         cp_dp, cp_dc
     iadd           cp_fp, cp_fc, vi00
     iaddiu         cp_src, cp_src, 3
     iaddiu         cp_i, cp_i, 1
     b              cp_loop_lid\@
cp_wrap_lid\@:
     cp_edge        cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pf, cp_sf, cp_cf, cp_df, cp_ff
     iadd           sh_n, cp_m, vi00
     .endm

     ; ---------------------------------------------------
     ; format a raw input color into GS ints. Vertex ARRAYS transfer raw
     ; (XferVectors is a straight memory copy), so the color array contract
     ; is 0..1 floats (glColorPointer(4, GL_FLOAT), the batch_col format).
     ; Scale is x128 ACROSS THE BOARD: this renderer is always TEXTURED and
     ; GS modulate identity is 128, so ps2gl maps 1.0 -> 128 for textured
     ; draws (GetMaxColorValue(texEnabled) — base_renderer.h). x255 here
     ; rendered everything the kicks emit exactly 2x overbright (glaring on
     ; the additive window fields, near-invisible on dark walls).

     .macro         fmt_color      dst, src
     loi            128.0
     muli.xyzw      \dst, \src, i
     ftoi0          \dst, \dst
     .endm

     ; ---------------------------------------------------
     ; emit one polygon vert (pre-divide pos \p + raw stq \s + raw color
     ; \c) at WALL output slot \k: divide, gs convert, stq perspective,
     ; pv color, fog/adc; \adcreg = adc_bit or vi00. The window section is
     ; built AFTER the loop by copying the wall verts (constant-rgb color
     ; splice) — emission writes one stream only.
     ; 1/w is landed in a REGISTER (addq) instead of being consumed twice
     ; from the pipeline Q: vcl re-rolls the schedule every regen, and a
     ; second mulq far from its div can read a NEIGHBOR vert's Q. One
     ; consumer right at the div, then register dataflow.

     .macro         emit_mvert     p, s, c, k, adcreg
     div            q, vf00[w], \p[w]
     addq.x         emq\@, vf00, q
     mulx.xyz       emp\@, \p, emq\@
     add.xyz        emp\@, emp\@, gs_offsets
     ftoi4.xyz      emp\@, emp\@
     mulx.xyz       ems\@, \s, emq\@
     sq.xyz         ems\@, 0+\k(next_output)
     fmt_color      emc\@, \c
     store_rgba     emc\@, \k
     fog_coef_alpha emf\@, \c
     ior            emadc\@, \adcreg, emf\@
     mfir.w         emp\@, emadc\@
     store_xyzf     emp\@, \k
     .endm

     ; ---------------------------------------------------
     ; load input vert \in_off (quads, relative to next_input), re-transform
     ; WITHOUT the divide, store as polygon-A vert \slot (0-based).

     ; One actual corner is transformed, divided and formatted once. Q is
     ; landed immediately and all later consumers use the same register.
     .macro         xf_cache       input_off, corner
     lq.xyz         xf_src\@, \input_off(next_input)
     mul_pt_mat_44  xf_pre\@, vert_xform, xf_src\@
     sq             xf_pre\@, 1+\corner(vi00)
     div            q, vf00[w], xf_pre\@[w]
     addq.x         xf_q\@, vf00, q
     mulx.xyz       xf_ndc\@, xf_pre\@, xf_q\@
     move.w         xf_ndc\@, xf_pre\@
     sq             xf_ndc\@, 5+\corner(vi00)
     vert_to_gs     xf_gs\@, xf_ndc\@
     lq             xf_col\@, \input_off+2(next_input)
     fmt_color      xf_fmt\@, xf_col\@
     sq             xf_fmt\@, 10+\corner*3(vi00)
     lq.xyz         xf_stq\@, \input_off+1(next_input)
     mulx.xyz       xf_stq\@, xf_stq\@, xf_q\@
     sq.xyz         xf_stq\@, 9+\corner*3(vi00)
     fog_coef_alpha xf_fog\@, xf_col\@
     mfir.w         xf_gs\@, xf_fog\@
     sq             xf_gs\@, 11+\corner*3(vi00)
     .endm

     ; Resolve triangle slot to authored index, never reconstruct its geometry.
     .macro         xf_index       index, slot
     .aif           "\slot" eq "0"
     iaddiu         \index, vi00, 0
     .aelse
     ilw.x          \index, 21(vi00)
     iaddiu         \index, \index, \slot
     .aendi
     .endm

     .macro         xf_fast        ndc, gs, slot, outoff
     xf_index       xf_idx\@, \slot
     iadd           xf_np\@, vi00, xf_idx\@
     lq             \ndc, 5(xf_np\@)
     iadd           xf_gp\@, xf_idx\@, xf_idx\@
     iadd           xf_gp\@, xf_gp\@, xf_idx\@
     iadd           xf_gp\@, xf_gp\@, vi00
     lq             xf_s\@, 9(xf_gp\@)
     lq             xf_c\@, 10(xf_gp\@)
     lq             \gs, 11(xf_gp\@)
     store_stq      xf_s\@, \outoff
     store_rgba     xf_c\@, \outoff
     .endm

     .macro         xf_store       corner, outoff, leading
     lq.xyz         xf_s\@, 9+\corner*3(vi00)
     lq             xf_c\@, 10+\corner*3(vi00)
     lq             xf_gs\@, 11+\corner*3(vi00)
     store_stq      xf_s\@, \outoff
     store_rgba     xf_c\@, \outoff
     .aif           "\leading" eq "1"
     mtir           xf_fog\@, xf_gs\@[w]
     ior            xf_fog\@, xf_fog\@, adc_bit
     mfir.w         xf_gs\@, xf_fog\@
     .aendi
     store_xyzf     xf_gs\@, \outoff
     .endm

     .macro         sh_load        in_off, slot
     xf_index       xf_idx\@, \slot
     iadd           xf_np\@, vi00, xf_idx\@
     lq             shp\@, 1(xf_np\@)
     sq             shp\@, kCPolyA+(\slot*3)(buffer_top)
     iadd           xf_sp\@, xf_idx\@, xf_idx\@
     iadd           xf_sp\@, xf_sp\@, xf_idx\@
     iadd           xf_sp\@, xf_sp\@, next_input
     lq.xyz         shs\@, 1(xf_sp\@)
     mulx.w         shs\@, vf00, vf00
     sq             shs\@, kCPolyA+(\slot*3)+1(buffer_top)
     lq             shc\@, 2(xf_sp\@)
     sq             shc\@, kCPolyA+(\slot*3)+2(buffer_top)
     .endm

     ; ---------------------------------------------------
     ; Close and kick the current compound wall+window arena, then switch to
     ; the other arena and reset its output cursor. XGKICK serializes a second
     ; PATH1 kick behind the first; therefore when B is kicked, A is free to
     ; reuse (and vice versa). All labels are per-expansion because this macro
     ; is used by both overflow guards and by the normal end-of-buffer path.

     .macro         x2_kick_chunk
     ; Reconstruct the arena base from the live cursor. Keeping arena_tag or
     ; arena_cap live across the S-H passes would consume VI registers that
     ; the clipper needs. next_output = arena_tag + 1 + 3*out_count.
     iadd           arena_used, out_count, out_count
     iadd           arena_used, arena_used, out_count
     isub           arena_tag, next_output, arena_used
     isubiu         arena_tag, arena_tag, 1

     ; window section on? (.x < 0 = wall-only)
     lq.x           win_chk, kCWinClr(buffer_top)
     pd_sign        win_skip, win_chk
     iand           win_skip, win_skip, adc_bit

     ; wall tag: live primitive template + emitted count. Clear EOP when the
     ; window section follows in the same compound packet.
     lq             gif_tag_p, kGifTag(vi00)
     mtir           eop_p, gif_tag_px
     ior            eop_p, eop_p, out_count
     ibne           win_skip, vi00, x2_wtag_lid\@
     isub           eop_p, eop_p, adc_bit
x2_wtag_lid\@:
     mfir.x         gif_tag_p, eop_p
     sq             gif_tag_p, 0(arena_tag)

     ibne           win_skip, vi00, x2_kick_lid\@

     ; window tag follows the committed wall verts; window verts copy STQ and
     ; XYZF2 verbatim and splice the constant window RGBA. Wall alpha may carry
     ; the radial GS-fog coefficient and must never leak into the clear window.
     lq             gif_tag_p, kCWinTag(buffer_top)
     mtir           eop_p, gif_tag_px
     ior            eop_p, eop_p, out_count
     mfir.x         gif_tag_p, eop_p
     sq             gif_tag_p, 0(next_output)

     iadd           wc_n, out_count, vi00
     ibeq           wc_n, vi00, x2_kick_lid\@
     iaddiu         wc_src, arena_tag, 1
     iaddiu         wc_dst, next_output, 1
     ; Alias fence: output was written through next_output, read through
     ; wc_src. The vcl scheduler cannot infer that relationship.
     b              x2_wcopy_lid\@
x2_wcopy_lid\@:
     lq             wc_s, 0(wc_src)
     sq             wc_s, 0(wc_dst)
     lq             wc_c, 1(wc_src)
     move.xyzw      wc_t, win_color
     move.w         wc_t, win_color
     sq             wc_t, 1(wc_dst)
     lq             wc_p, 2(wc_src)
     sq             wc_p, 2(wc_dst)
     iaddiu         wc_src, wc_src, 3
     iaddiu         wc_dst, wc_dst, 3
     isubiu         wc_n, wc_n, 1
     ibgtz          wc_n, x2_wcopy_lid\@

x2_kick_lid\@:
     ; Store-to-kick scheduling fence, then a second fence prevents following
     ; arena-selection work from crossing back over XGKICK.
     b              x2_kick2_lid\@
x2_kick2_lid\@:
     xgkick         arena_tag
     b              x2_after_kick_lid\@
x2_after_kick_lid\@:

     ; Toggle A <-> B. If this was B's kick, XGKICK could only issue after
     ; A's earlier packet drained, so A is safe to write again.
     iaddiu         arena_a, buffer_top, kCAWallTag
     ibeq           arena_tag, arena_a, x2_use_b_lid\@
     iaddiu         next_output, buffer_top, kCAOutData
     b              x2_arena_ready_lid\@
x2_use_b_lid\@:
     iaddiu         next_output, buffer_top, kCBOutData
x2_arena_ready_lid\@:
     iaddiu         out_count, vi00, 0
     .endm

     --enter
     --endenter

     ; ------------------------ initialization ---------------------------------

     load_vert_xfrm vert_xform
     load_fog_params fog_params

     ; near plane (eye units) rides the unused x field of the fog params;
     ; w-aligned for the fast path's sub.w test. The additive base MUST be a
     ; zeroed .w — vf00.w is ONE, and using it here made near = 1 + near
     ; (everything near-clipped at w=2)
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

     init_io_loop   kInputStart, kCAOutData
     ; NO init_out_buf: the stock macro pre-writes a giftag at the stock
     ; output offset, which in THIS layout lands inside the polygon scratch.
     ; Both giftags are written post-loop instead.

     ; init_bfc minus the integer mask (reloaded per tri to keep its
     ; lifetime local — see the bfc_tri call site)
     lq.w           bfc_multiplier, kBackFaceCullMult(vi00)
     get_ones_vec   ones

     ; window color const, once per buffer: floats from the EE (RGBA already
     ; in GS 0..128 modulate range), GS ints after ftoi0. Wall A may carry the
     ; radial fog coefficient, so the window copies this constant A too.
     ; (.x < 0 disables the window section — checked post-loop, no extra
     ; register held here.) Held across the loop for the post-loop splice.
     lq             win_color, kCWinClr(buffer_top)
     loi            255.0
     minii.xyz      win_color, win_color, i
     ftoi0          win_color, win_color

     ; (no stage -> kick-site prefix copies anymore: per-buffer GS state is
     ; gone — walls use live context 1, windows use context 2 programmed
     ; once per pass by the EE. The 2q staging at 178 is VU-read only.)

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

     ; init_io_loop started next_output at arena A's first data qword.
     iaddiu         out_count, vi00, 0

xf_quad_lid:
     xf_cache       0, 0
     xf_cache       3, 1
     xf_cache       6, 2
     xf_cache       9, 3
     isw.x          vi00, 21(vi00)
     ; Cache stores must complete before the indirect whole-quad classifier.
     b              xf_quad_classify_lid
xf_quad_classify_lid:
     lq             xf_a, 5(vi00)
     lq             xf_b, 6(vi00)
     lq             xf_c, 7(vi00)
     lq             xf_d, 8(vi00)
     clip_vert      xf_a
     clip_vert      xf_b
     clip_vert      xf_c
     clip_vert      xf_d
     fcand          vi01, 0x3cf3cf
     miniw.w        xf_near, xf_a, xf_b
     miniw.w        xf_near, xf_near, xf_c
     miniw.w        xf_near, xf_near, xf_d
     sub.w          xf_near, xf_near, near_plane
     loi            2047.0
     minii.w        xf_near, xf_near, i
     loi            -2047.0
     maxi.w         xf_near, xf_near, i
     ftoi4.w        xf_near, xf_near
     mtir           xf_out, xf_near[w]
     iand           xf_out, xf_out, adc_bit
     ior            xf_out, xf_out, vi01
     ; Cull-on users retain the original per-triangle backface path.
     ilw.w          xf_cull, kBackFaceCullMult(vi00)
     iaddiu         xf_mask, vi00, 32
     iand           xf_cull, xf_cull, xf_mask
     ior            xf_out, xf_out, xf_cull
     ; Both original triangles share this verdict for wholly inside quads
     ; and the legacy route. Copy ACD into x only when ABC completes.
     isw.xy         xf_out, 22(vi00)
     ibeq           xf_out, vi00, xform_loop_lid
     isw.z          vi00, 22(vi00)
     ; Culling keeps the original per-triangle path, including its ADC.
     ibne           xf_cull, vi00, xform_loop_lid
     mtir           xf_direct, win_color[w]
     ibeq           xf_direct, vi00, xform_loop_lid
     ; Four CLIPs retain A/B/C/D in shifts 18/12/6/0. Ignore Z exactly
     ; as the original triangle fcand(0xf3cf), with near handled in W.
     ; These tests cannot reorder or discard a triangle: a failed proof
     ; goes directly to the unchanged five-plane S-H handler.
     miniw.w        xf_ac, xf_a, xf_c
     miniw.w        xf_abc, xf_ac, xf_b
     miniw.w        xf_acd, xf_ac, xf_d
     sub.w          xf_abc, xf_abc, near_plane
     sub.w          xf_acd, xf_acd, near_plane
     loi            2047.0
     minii.w        xf_abc, xf_abc, i
     minii.w        xf_acd, xf_acd, i
     loi            -2047.0
     maxi.w         xf_abc, xf_abc, i
     maxi.w         xf_acd, xf_acd, i
     ftoi4.w        xf_abc, xf_abc
     ftoi4.w        xf_acd, xf_acd
     mtir           xf_flag, xf_abc[w]
     iand           xf_flag, xf_flag, adc_bit
     fcand          vi01, 0x3cf3c0
     ior            xf_flag, xf_flag, vi01
     isw.x          xf_flag, 22(vi00)
     mtir           xf_flag, xf_acd[w]
     iand           xf_flag, xf_flag, adc_bit
     fcand          vi01, 0x3c03cf
     ior            xf_flag, xf_flag, vi01
     isw.y          xf_flag, 22(vi00)
     isw.z          xf_direct, 22(vi00)
     b              xform_loop_lid
xform_loop_lid:

     ; Fast-path tris optimistically store 3 verts. Close the current chunk
     ; before those stores whenever all 3 would not fit; this preserves the
     ; input triangle instead of applying the old scratch-full drop policy.
     iadd           arena_used, out_count, out_count
     iadd           arena_used, arena_used, out_count
     isub           arena_tag, next_output, arena_used
     isubiu         arena_tag, arena_tag, 1
     iaddiu         arena_a, buffer_top, kCAWallTag
     ibeq           arena_tag, arena_a, x2_fast_cap_a_lid
     mtir           arena_cap, win_color[z]
     b              x2_fast_cap_lid
x2_fast_cap_a_lid:
     mtir           arena_cap, win_color[y]
x2_fast_cap_lid:
     iaddiu         cap_need, out_count, 3
     isub           cap_chk, cap_need, arena_cap
     iblez          cap_chk, x2_fast_room_lid
     x2_kick_chunk
x2_fast_room_lid:
     ilw.x          xf_out, 22(vi00)
     ibeq           xf_out, vi00, xf_inside_triangle_lid
     ilw.z          xf_direct, 22(vi00)
     ibne           xf_direct, vi00, sh_handler_lid

     ; ---- vertex 1 (ADC always set; F field still feeds fog interpolation)

     xf_fast        xformed_vert_1, gs_vert_1, 0, 0

     ; near classify, flag-free (see pd_sign): sign of (eye w - near)
     sub.w          near_d1, xformed_vert_1, near_plane
     loi            2047.0
     minii.w        near_d1, near_d1, i
     loi            -2047.0
     maxi.w         near_d1, near_d1, i
     ftoi4.w        near_d1, near_d1
     mtir           near_f, near_d1[w]
     iand           near_any, near_f, adc_bit

     clip_vert      xformed_vert_1
     mtir           fog_i1, gs_vert_1[w]
     ior            fog_adc1, adc_bit, fog_i1
     mfir.w         gs_vert_1, fog_adc1
     store_xyzf     gs_vert_1, 0

     ; ---- vertex 2

     xf_fast        xformed_vert_2, gs_vert_2, 1, kOutputQPerV

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
     mtir           fog_i2, gs_vert_2[w]
     ior            fog_adc2, adc_bit, fog_i2
     mfir.w         gs_vert_2, fog_adc2
     store_xyzf     gs_vert_2, kOutputQPerV

     ; ---- vertex 3

     xf_fast        xformed_vert_3, gs_vert_3, 2, kOutputQPerV+kOutputQPerV

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
     mtir           fog_i3, gs_vert_3[w]
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
     ; rebuild the tri as polygon A (pre-divide pos + raw stq + raw color)
     sh_load        0, 0
     sh_load        kInputQPerV, 1
     sh_load        kInputQPerV+kInputQPerV, 2
     iaddiu         sh_n, vi00, 3

     ; SCHEDULING FENCE — load-bearing, do not remove (vcl memory-aliasing
     ; bug: without a block boundary it hoists pass 1's polygon[0] LOADS
     ; above sh_load's STORES of the same addresses through another pointer)
     b              sh_fence_lid
sh_fence_lid:

     ; 5 planes, ping-ponging A->B->A->B->A->B
     clip_pass      kCPolyA, kCPolyB, kCPlanes+0
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyB, kCPolyA, kCPlanes+1
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyA, kCPolyB, kCPlanes+2
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyB, kCPolyA, kCPlanes+3
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid
     clip_pass      kCPolyA, kCPolyB, kCPlanes+4
     isubiu         sh_t, sh_n, 3
     ibltz          sh_t, tri_next_lid

     ; Capacity guard: fan emits 3*(sh_n-2) verts. One fan is <=18 verts,
     ; so after a chunk kick it always fits even the smaller B arena.
     iadd           arena_used, out_count, out_count
     iadd           arena_used, arena_used, out_count
     isub           arena_tag, next_output, arena_used
     isubiu         arena_tag, arena_tag, 1
     iaddiu         arena_a, buffer_top, kCAWallTag
     ibeq           arena_tag, arena_a, x2_fan_cap_a_lid
     mtir           arena_cap, win_color[z]
     b              x2_fan_cap_lid
x2_fan_cap_a_lid:
     mtir           arena_cap, win_color[y]
x2_fan_cap_lid:
     isubiu         fe_t, sh_n, 2
     iadd           fe_v, fe_t, fe_t
     iadd           fe_v, fe_v, fe_t
     iadd           fe_chk, out_count, fe_v
     isub           fe_chk, fe_chk, arena_cap
     iblez          fe_chk, fe_fence_lid
     x2_kick_chunk

     ; SCHEDULING FENCE: pass 5 wrote the final polygon through cp_dst;
     ; the fan reads it through fe_p0/fe_pi. Same aliasing bug as above.
     b              fe_fence_lid
fe_fence_lid:

     ; fan emit from the final polygon (in B): tris (P0, Pi, Pi+1)
     iaddiu         fe_p0, buffer_top, kCPolyB
     iaddiu         fe_pi, fe_p0, 3
fe_loop_lid:
     lq             fe_pos, 0(fe_p0)
     lq             fe_stq, 1(fe_p0)
     lq             fe_col, 2(fe_p0)
     emit_mvert     fe_pos, fe_stq, fe_col, 0, adc_bit
     lq             fe_pos, 0(fe_pi)
     lq             fe_stq, 1(fe_pi)
     lq             fe_col, 2(fe_pi)
     emit_mvert     fe_pos, fe_stq, fe_col, kOutputQPerV, adc_bit
     lq             fe_pos, 3(fe_pi)
     lq             fe_stq, 4(fe_pi)
     lq             fe_col, 5(fe_pi)
     emit_mvert     fe_pos, fe_stq, fe_col, kOutputQPerV+kOutputQPerV, vi00
     next_o         3
     iaddiu         out_count, out_count, 3
     iaddiu         fe_pi, fe_pi, 3
     isubiu         fe_t, fe_t, 1
     ibgtz          fe_t, fe_loop_lid
     b              tri_next_lid

xf_inside_triangle_lid:
     ; Literal authored indices avoid three indirect address walks per tri.
     ilw.x          xf_phase, 21(vi00)
     ibne           xf_phase, vi00, xf_inside_acd_lid
     xf_store       0, 0, 1
     xf_store       1, kOutputQPerV, 1
     xf_store       2, kOutputQPerV+kOutputQPerV, 0
     b              xf_inside_commit_lid
xf_inside_acd_lid:
     xf_store       0, 0, 1
     xf_store       2, kOutputQPerV, 1
     xf_store       3, kOutputQPerV+kOutputQPerV, 0
xf_inside_commit_lid:
     next_o         3
     iaddiu         out_count, out_count, 3

tri_next_lid:
     ilw.x          xf_phase, 21(vi00)
     ibne           xf_phase, vi00, xf_next_quad_lid
     iaddiu         xf_phase, vi00, 1
     isw.x          xf_phase, 21(vi00)
     ilw.y          xf_out, 22(vi00)
     isw.x          xf_out, 22(vi00)
     b              xform_loop_lid
xf_next_quad_lid:
     next_i         4
     isub           loop_left, last_input, next_input
     isubiu         loop_left, loop_left, 11
     ibgtz          loop_left, xf_quad_lid

     ; Close the final (possibly empty) chunk. NLOOP=0 remains legal and is
     ; already torture-tested by the original renderer.
     x2_kick_chunk

     --cont

     b    main_loop_lid

.END ; for gasp
