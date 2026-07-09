/*     Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

            This file is subject to the terms and conditions of the GNU Lesser
       General Public License Version 2.1. See the file "COPYING" in the
       main directory of this archive for more details.                             */

     ; HyperSolar VU1 DOUBLE-KICK city renderer — the x2 companion of
     ; general_clip_tri.vcl (which stays byte-identical; see VU1_X2_PLAN.md).
     ; Selected via the PGL_CLIP_TRIANGLES_X2 custom prim.
     ;
     ; The city submits identical wall geometry twice (opaque base pass +
     ; additive window overlay). This renderer transforms + 5-plane-S-H-clips
     ; each shared vertex ONCE, then XGKICKs TWO packets from the same
     ; transformed data:
     ;   kick 1 (wall):   A+D prefix (base TEX0/TEX1/MIPTBP) + prim ABE=0,
     ;                    PER-VERTEX tint color (input color stream).
     ;   kick 2 (window): A+D prefix (window TEX0/TEX1/MIPTBP + additive
     ;                    ALPHA) + prim template ABE=1 (EE-built — VU cannot
     ;                    flip giftag bit 53, no integer shifts), CONSTANT
     ;                    color (g_win_bright is globally flat).
     ; Window verts are positionally IDENTICAL to wall verts (same arrays,
     ; same count — push_win writes both streams at one index), so depth
     ; needs no ZMSK switch: the window kick rewrites the identical Z.
     ;
     ; Inherits EVERY hardened pattern from the parent renderer — scheduling
     ; fences at all store->load seams (vcl does not model VU-mem aliasing
     ; between pointer registers), pd_sign instead of fmand (flag-inert ABS
     ; regeneration), register-landed Q (addq.x + mulx), overshoot-proof
     ; loop termination, zeroed-.w near base, fcand mask 0xf3cf. Do not
     ; "simplify" any of them; each one is a hardware-verified bug fix.
     ;
     ; Buffer layout is THIS RENDERER'S OWN, double-buffered via xtop
     ; (halves of 472 qwords). Offsets are buffer-relative:
     ;   0..4     header (kNumVertices + strip ADCs, stock XferBufferHeader)
     ;   5..124   input verts (30 max x 4q: pos, [normal unused], stq, col)
     ;   125..129 the 5 plane qwords (built per buffer)
     ;   130..153 polygon scratch A (8 verts x 3q: pos, stq, col)
     ;   154..177 polygon scratch B
     ;   178      window color const (floats: rgb 0..255, a 0..128 GS range;
     ;            x < 0 = WINDOW KICK DISABLED, wall-only mode)
     ;   179      pad
     ;   180..200 PREFIX STAGING (EE unpack, one 23q block starting at 178):
     ;            180..189 wall prefix - the BASE texture's FULL settings
     ;                     block (A+D giftag patched to NLOOP=9, EOP=0 +
     ;                     TEXFLUSH/CLAMP/TEX1/TEX0/TEXA/MIPTBP1/2, copied
     ;                     wholesale from the CTexEnv object so punch-through
     ;                     TEXA + custom mip MIPTBPs ride verbatim) + a
     ;                     NORMAL-blend ALPHA_1 (the previous buffer's window
     ;                     kick leaves ADDITIVE alpha behind, and the FADE
     ;                     overlay draws its walls with ABE=1; inert for the
     ;                     opaque main city, ABE=0) + TEST_1 with the alpha
     ;                     test OFF
     ;            190..199 window prefix - the WINDOW texture's settings
     ;                     block (giftag patched to NLOOP=9, EOP=0) +
     ;                     additive ALPHA_1 + TEST_1 with the alpha test ON
     ;                     (ATE=1, ATST=GREATER, AREF=0, AFAIL=KEEP): the
     ;                     window tile's gap texels sample alpha 0 (TEXA
     ;                     ta0=0), and additive blending of As=0 is
     ;                     Cs*0+Cd = Cd - a framebuffer read-modify-write
     ;                     that provably cannot change a bit. Discarding
     ;                     them is bit-identical and skips the RMW; the
     ;                     window kick's fill measured 2.26 ms of GS time.
     ;                     Both TEST values START from ps2gl's live drawenv
     ;                     TEST (it also carries ZTE/ZTST + dest-alpha).
     ;            200      window prim giftag TEMPLATE (ABE=1)
     ;            STAGED, NOT KICKED IN PLACE (hardware-learned): VIF runs a
     ;            buffer ahead and has NO interlock against GIF path 1 - a
     ;            VIF-written qword the GIF reads gets overwritten while the
     ;            PREVIOUS buffer's kick is still draining (per-building
     ;            texture/blend flicker in the city). The program copies the
     ;            staging block to the kick sites below; VU writes ARE
     ;            serialized against the previous kicks by the xgkick
     ;            interlock, so the copy is race-free by construction.
     ;   201..210 wall kick prefix (VU-copied from 180..189)
     ;   211      wall prim giftag (VU writes: kGifTag template + count)
     ;   212..334 wall output verts (41 x 3q: STQ, RGBA, XYZF2)
     ;   335..344 window kick prefix (VU-copied from 190..199)
     ;   345      window prim giftag (VU-copied template; NLOOP patched)
     ;   346..468 window output verts (41 x 3q)
     ;   469..471 spare
     ; EE coupling: CClipTriX2Renderer's ctor passes inGeomBufSize=120
     ; (30 verts x 4q), its DrawLinearArrays caps batches at multiples of 6,
     ; and its DrawBlock unpacks ONE 23q block at 178 per buffer. Changing
     ; any of these numbers means re-checking this layout end to end.

     #include       "vu1_mem_linear.h"

     .include       "db_in_db_out.i"
     .include       "math.i"
     .include       "lighting.i"
     .include       "clip_cull.i"
     .include       "geometry.i"
     .include       "io.i"
     .include       "general.i"

kInputQPerV         .equ           4
kOutputQPerV        .equ           3

kCPlanes            .equ           125
kCPolyA             .equ           130
kCPolyB             .equ           154
kCWinClr            .equ           178
kCStgWall           .equ           180
kCStgWin            .equ           190
kCWallPfx           .equ           201
kCWallTag           .equ           211
kCOutData           .equ           212
kCWinPfx            .equ           335
kCWinTag            .equ           345
kCWinOfs            .equ           134
kCCapVerts          .equ           39

     .init_vf_all
     .init_vi_all

     .name          vsmGeneralClipTriX2

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
     ; \c) at WALL output slot \k and WINDOW slot \k+kCWinOfs: divide, gs
     ; convert, stq perspective (stored to both), pv color (wall) + const
     ; color (window), shared fog/adc; \adcreg = adc_bit or vi00.
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
     sq.xyz         ems\@, 0+\k+kCWinOfs(next_output)
     fmt_color      emc\@, \c
     store_rgba     emc\@, \k
     move.xyzw      emw\@, win_color
     move.w         emw\@, emc\@
     store_rgba     emw\@, \k+kCWinOfs
     fog_coef       emf\@, \p, fog_params
     ior            emadc\@, \adcreg, emf\@
     mfir.w         emp\@, emadc\@
     store_xyzf     emp\@, \k
     store_xyzf     emp\@, \k+kCWinOfs
     .endm

     ; ---------------------------------------------------
     ; load input vert \in_off (quads, relative to next_input), re-transform
     ; WITHOUT the divide, store as polygon-A vert \slot (0-based).

     .macro         sh_load        in_off, slot
     lq.xyz         shv\@, \in_off(next_input)
     mul_pt_mat_44  shp\@, vert_xform, shv\@
     sq             shp\@, kCPolyA+(\slot*3)(buffer_top)
     lq.xyz         shs\@, \in_off+2(next_input)
     mulx.w         shs\@, vf00, vf00
     sq             shs\@, kCPolyA+(\slot*3)+1(buffer_top)
     lq             shc\@, \in_off+3(next_input)
     sq             shc\@, kCPolyA+(\slot*3)+2(buffer_top)
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

     init_io_loop   kInputStart, kCOutData
     ; NO init_out_buf: the stock macro pre-writes a giftag at the stock
     ; output offset, which in THIS layout lands inside the polygon scratch.
     ; Both giftags are written post-loop instead.

     ; init_bfc minus the integer mask (reloaded per tri to keep its
     ; lifetime local — see the bfc_tri call site)
     lq.w           bfc_multiplier, kBackFaceCullMult(vi00)
     get_ones_vec   ones

     ; window color const, once per buffer: floats from the EE (rgb already
     ; in GS 0..128 modulate range), GS ints after ftoi0. Only .xyz is used —
     ; each window vert takes its ALPHA from the wall vert's pv alpha, so the
     ; fade overlay's animated per-building alpha scales the additive add
     ; exactly like the classic window pass (main-city verts carry alpha 1.0
     ; -> 128, identical to a constant). (.x < 0 disables the window kick —
     ; checked post-loop, no register held here.)
     lq             win_color, kCWinClr(buffer_top)
     loi            255.0
     minii.xyz      win_color, win_color, i
     ftoi0          win_color, win_color

     ; stage -> kick-site prefix copy (see the layout header: kicked qwords
     ; must be VU-written, never VIF-written — VIF races the GIF). Wall 8q,
     ; then window 10q (prefix 9q + prim giftag template, contiguous).
     iaddiu         cpy_s, buffer_top, kCStgWall
     iaddiu         cpy_d, buffer_top, kCWallPfx
     iaddiu         cpy_n, vi00, 10
x2_cpyw_lid:
     lq             cpy_t, 0(cpy_s)
     sq             cpy_t, 0(cpy_d)
     iaddiu         cpy_s, cpy_s, 1
     iaddiu         cpy_d, cpy_d, 1
     isubiu         cpy_n, cpy_n, 1
     ibgtz          cpy_n, x2_cpyw_lid
     iaddiu         cpy_s, buffer_top, kCStgWin
     iaddiu         cpy_d, buffer_top, kCWinPfx
     iaddiu         cpy_n, vi00, 11
x2_cpyn_lid:
     lq             cpy_t, 0(cpy_s)
     sq             cpy_t, 0(cpy_d)
     iaddiu         cpy_s, cpy_s, 1
     iaddiu         cpy_d, cpy_d, 1
     isubiu         cpy_n, cpy_n, 1
     ibgtz          cpy_n, x2_cpyn_lid
     ; SCHEDULING FENCE: the giftag patch post-loop reads kCWinTag through
     ; buffer_top — a different pointer than cpy_d wrote it through (the
     ; vcl memory-aliasing bug, again). Block boundary before continuing.
     b              x2_cpyf_lid
x2_cpyf_lid:

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

     ; emitted output verts this buffer (patched into both NLOOPs after)
     iaddiu         out_count, vi00, 0

xform_loop_lid:

     ; BUFFER-FULL GUARD, loop top (hardware-learned): the fan guard alone
     ; leaves a hole — fans can fill the output to the cap and later
     ; fast-path tris still optimistically STORE + commit 3 verts unchecked.
     ; The wall overrun smashes the WINDOW KICK PREFIX (wild TEX0/ALPHA =
     ; corrupt textures + wrong tint) and the window overrun smashes the
     ; NEXT BUFFER HALF's header (GIF wedge). Drop the remaining tris when
     ; another 3 verts would not fit — the EE scratch-full-drop policy.
     isubiu         cap_chk, out_count, kCCapVerts-1
     ibgtz          cap_chk, tri_next_lid

     ; ---- vertex 1 (ADC always set; F field still feeds fog interpolation)

     load_vert      vert, 0
     xform_vert     xformed_vert_1, vert_xform, vert
     vert_to_gs     gs_vert_1, xformed_vert_1
     lq             pv_col, 3(next_input)
     fmt_color      pv_fmt, pv_col
     store_rgba     pv_fmt, 0
     move.xyzw      win_v, win_color
     move.w         win_v, pv_fmt
     store_rgba     win_v, 0+kCWinOfs
     load_stq       tex_stq, 0
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, 0
     store_stq      tex_stq, 0+kCWinOfs

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
     fog_coef       fog_i1, xformed_vert_1, fog_params
     ior            fog_adc1, adc_bit, fog_i1
     mfir.w         gs_vert_1, fog_adc1
     store_xyzf     gs_vert_1, 0
     store_xyzf     gs_vert_1, 0+kCWinOfs

     ; ---- vertex 2

     load_vert      vert, kInputQPerV
     xform_vert     xformed_vert_2, vert_xform, vert
     vert_to_gs     gs_vert_2, xformed_vert_2
     lq             pv_col, kInputQPerV+3(next_input)
     fmt_color      pv_fmt, pv_col
     store_rgba     pv_fmt, kOutputQPerV
     move.xyzw      win_v, win_color
     move.w         win_v, pv_fmt
     store_rgba     win_v, kOutputQPerV+kCWinOfs
     load_stq       tex_stq, kInputQPerV
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, kOutputQPerV
     store_stq      tex_stq, kOutputQPerV+kCWinOfs

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
     store_xyzf     gs_vert_2, kOutputQPerV+kCWinOfs

     ; ---- vertex 3

     load_vert      vert, kInputQPerV+kInputQPerV
     xform_vert     xformed_vert_3, vert_xform, vert
     vert_to_gs     gs_vert_3, xformed_vert_3
     lq             pv_col, kInputQPerV+kInputQPerV+3(next_input)
     fmt_color      pv_fmt, pv_col
     store_rgba     pv_fmt, kOutputQPerV+kOutputQPerV
     move.xyzw      win_v, win_color
     move.w         win_v, pv_fmt
     store_rgba     win_v, kOutputQPerV+kOutputQPerV+kCWinOfs
     load_stq       tex_stq, kInputQPerV+kInputQPerV
     xform_tex_stq  tex_stq, tex_stq, q
     store_stq      tex_stq, kOutputQPerV+kOutputQPerV
     store_stq      tex_stq, kOutputQPerV+kOutputQPerV+kCWinOfs

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
     store_xyzf     gs_vert_3, kOutputQPerV+kOutputQPerV+kCWinOfs

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

tri_next_lid:
     next_i         3
     ; overshoot-proof termination: loop only while a FULL tri (12 quads)
     ; remains, never on pointer equality (a 3n-1 vert buffer from ps2gl's
     ; spilled-strip splitter would spin VU1 forever)
     isub           loop_left, last_input, next_input
     isubiu         loop_left, loop_left, 11
     ibgtz          loop_left, xform_loop_lid

     ; ---------------- patch NLOOPs + kick both packets to GS ----------------

     ; wall giftag: kGifTag template + the emitted vert count
     lq             gif_tag_p, kGifTag(vi00)
     mtir           eop_p, gif_tag_px
     ior            eop_p, eop_p, out_count
     mfir.x         gif_tag_p, eop_p
     iaddiu         packet_start_p, buffer_top, kCWallTag
     sq             gif_tag_p, 0(packet_start_p)

     ; kick 1: wall packet (A+D base-texture prefix + prim + verts)
     iaddiu         packet_start_p, buffer_top, kCWallPfx
     xgkick         packet_start_p

     ; window kick disabled? (win color .x < 0 = wall-only mode)
     lq.x           win_chk, kCWinClr(buffer_top)
     pd_sign        win_skip, win_chk
     iand           win_skip, win_skip, adc_bit
     ibne           win_skip, vi00, no_win_kick_lid

     ; window giftag: the EE-built ABE=1 TEMPLATE at kCWinTag + the count
     lq             gif_tag_p, kCWinTag(buffer_top)
     mtir           eop_p, gif_tag_px
     ior            eop_p, eop_p, out_count
     mfir.x         gif_tag_p, eop_p
     sq             gif_tag_p, kCWinTag(buffer_top)

     ; SCHEDULING FENCE: the patched template store above must land before
     ; the GIF reads it — and before any future reordering pulls the kick
     ; earlier. Same aliasing-bug class; keep the boundary.
     b              wk_fence_lid
wk_fence_lid:

     ; kick 2: window packet (A+D window-texture+ALPHA prefix + prim + verts)
     iaddiu         packet_start_p, buffer_top, kCWinPfx
     xgkick         packet_start_p

no_win_kick_lid:
     --cont

     b    main_loop_lid

.END ; for gasp
