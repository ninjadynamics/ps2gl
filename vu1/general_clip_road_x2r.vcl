/* Copyright (C) 2000,2001,2002 Sony Computer Entertainment America
 * This file is subject to the GNU Lesser General Public License Version2.1.
 * See COPYING. HyperSolar dedicated compact-road renderer, September2026.
 */

     ; Standalone program: do not append to or regenerate the existing X2.
     ; EE owns eligibility and copies immutable descriptors into frame DMA.
     ; Context MUST be installed before MSCAL0. Stock InitUnlitContext and
     ; CBaseRenderer::Load issue MSCAL0 themselves and cannot precede a late
     ; road-context append. Entry waits at --cont; one MSCNT owns each half.
     ; No backface culling, window/context2 section or VU-to-EE output count.
     ;
     ; Absolute context q1..24: eye sky planes [A,B,C,D]; q25..48: source
     ; sky planes [nx,nz,d,0]; q49..51: right/up/forward [xyz,0];
     ; q52: [eye.xyz,incircle2]; q53: [center.x,center.z,px,py];
     ; q54: [sourceNear,sourceNdcLimit,denominatorEpsilon,0]; q55: RGBA;
     ; q56: [sourceY,0,0,0]. Standard unlit q0,57,62..65,75..78 remain.
     ; The matrix at q62..65 is GSScale*eye-space projection, without view.
     ;
     ; Half size472, BASE79/OFFSET472. Relative memory owners:
     ; 0..4 VIF header (q0.x descriptor count1..32); 5..100 VIF descriptors;
     ; descriptor=[Ax,Az,Bx,Bz],[Cx,Cz,Dx,Dz],[v1,0,0,0].
     ; 101..105 VU view planes;106..186 skyA27*3;187..267 skyB27*3;
     ; 268..291 viewA8*3;292..315 viewB8*3;316..323 control;
     ; 324..414 outputA(tag+30*3);415..469 outputB(tag+18*3);470..471 guard.
     ; Each polygon vertex: position,STQ,RGBA. Sky position is SH eye space;
     ; view position is GS-scaled homogeneous clip space. Original ABC then
     ; ACD, sky last->first S-H, original sky fan, then X2 five-plane clipping.
     ;
     ; Control q316: descriptor pointer,left,ABC/ACD phase,reserved.
     ; q317: output cursor,count,sky count,sky source pointer.
     ; q318: sky OR low12/high12,sky AND low12/high12.
     ; q319: next sky plane index,bit,sky destination pointer,reserved.
     ; q320: source view AND,sky fan first pointer,rim pointer,triangles left.
     ; q321: final view polygon pointer,count,reserved,reserved.
     ; q322: view source,destination,plane pointer,planes left.
     ; q323.x diagnostic contract failure (VU fail-stop).
     ;
     ; No FMAND for new signs. CLIPw against zero gives strict <0 and its
     ; FCAND consumer must retain the four-cycle flag dependency after VCL.
     ; Sky MUL/ADD are separate, no reassociation/FMA. Land every divided Q
     ; in a VF register before using it. Branch fences protect all store/load
     ; alias seams and store/XGKICK ownership from the VCL scheduler.
     ; Arithmetic order is preserved; native EE/VU bit parity is not claimed.

     #include "vu1_mem_linear.h"
     .include "db_in_db_out.i"
     .include "math.i"
     .include "geometry.i"
     .include "clip_cull.i"

kRPlanes            .equ 101
kRSkyA              .equ 106
kRSkyB              .equ 187
kRViewA             .equ 268
kRViewB             .equ 292
kRDesc              .equ 316
kROut               .equ 317
kRMask              .equ 318
kRPlaneState        .equ 319
kRFan               .equ 320
kRView              .equ 321
kRViewLoop          .equ 322
kRFailure           .equ 323
kRATag              .equ 324
kRAData             .equ 325
kRACap              .equ 30
kRBTag              .equ 415
kRBData             .equ 416
kRBCap              .equ 18
kOutputQPerV        .equ 3

     .init_vf_all
     .init_vi_all
     .name vsmGeneralClipRoadX2R

     ; Keep each exceptional branch local: one distant shared trap could
     ; exceed the signed branch displacement after five clip-pass expansions.
     .macro r_fail_stop
     iaddiu contract_error\@, vi00, 1
     isw.x contract_error\@, kRFailure(buffer_top)
     b r_contract_stop_lid\@
r_contract_stop_lid\@:
     b r_contract_stop_lid\@
     .endm

     ; Sign of d.x, exactly negative for the represented distance, including
     ; (-1/16,0). Zero/negative-zero are inside. Other clip lanes are zero.
     .macro r_negative result, d
     sub.xyzw rn_vec\@, vf00, vf00
     move.x rn_vec\@, \d
     clipw.xyz rn_vec\@, rn_vec\@[w]
     fcand vi01, 0x2
     iadd \result, vi01, vi00
     .endm

     ; Explicit EE operation order: ((A*x+B*y)+C*z)+D.
     .macro r_sky_distance distance, vertex, coeffs
     mul.xyz rs_terms\@, \vertex, \coeffs
     addy.x \distance, rs_terms\@, rs_terms\@
     addz.x \distance, \distance, rs_terms\@
     addw.x \distance, \distance, \coeffs
     .endm

     ; Copy actual XZ float bits. MR32 avoids arithmetic corner reconstruction
     ; and avoids adding zero to a possibly signed-zero authored coordinate.
     .macro r_corner_xy output, coords
     move.xyzw \output, vf00
     move.y \output, material_height
     move.x \output, \coords
     mr32 rc_rot\@, \coords
     mr32 rc_rot\@, rc_rot\@
     mr32 rc_rot\@, rc_rot\@
     move.z \output, rc_rot\@
     .endm

     .macro r_corner_zw output, coords
     move.xyzw \output, vf00
     move.y \output, material_height
     mr32 rc_rot\@, \coords
     move.z \output, rc_rot\@
     mr32 rc_rot\@, rc_rot\@
     move.x \output, rc_rot\@
     .endm

     ; View-stage distance/sign/edge order deliberately retain existing X2,
     ; including its FTOI4 near-zero sign tolerance and first->next anchor.
     .macro pd_plane d, v, pl
     mul.xy pdt\@, \pl, \v
     mulw.z pdt\@, \pl, \v
     adday.x acc, pdt\@, pdt\@
     maddz.x pdt\@, ones, pdt\@
     addw.x \d, pdt\@, \pl
     .endm

     .macro pd_sign f, d
     loi 2047.0
     minii.x pds\@, \d, i
     loi -2047.0
     maxi.x pds\@, pds\@, i
     ftoi4.x pds\@, pds\@
     mtir \f, pds\@[x]
     .endm

     .macro cp_store p, s, c
     isubiu cp_bound\@, cp_m, 8
     ibltz cp_bound\@, cp_store_ok\@
     r_fail_stop
cp_store_ok\@:
     sq \p, 0(cp_dst)
     sq \s, 1(cp_dst)
     sq \c, 2(cp_dst)
     iaddiu cp_dst, cp_dst, 3
     iaddiu cp_m, cp_m, 1
     .endm

     .macro cp_edge pa, sa, ca, da, fa, pb, sb, cb, db, fb
     iand ce_sa\@, \fa, adc_bit
     ibne ce_sa\@, vi00, ce_askip\@
     cp_store \pa, \sa, \ca
ce_askip\@:
     iand ce_sb\@, \fb, adc_bit
     ibeq ce_sa\@, ce_sb\@, ce_nocross\@
     sub.x ce_den\@, \da, \db
     div q, \da[x], ce_den\@[x]
     addq.x ce_q\@, vf00, q
     sub.xyzw ce_e\@, \pb, \pa
     mulaw.xyzw acc, \pa, vf00[w]
     maddx.xyzw ce_ip\@, ce_e\@, ce_q\@
     sub.xyz ce_es\@, \sb, \sa
     mulaw.xyz acc, \sa, vf00[w]
     maddx.xyz ce_is\@, ce_es\@, ce_q\@
     mulx.w ce_is\@, vf00, vf00
     sub.xyzw ce_ec\@, \cb, \ca
     mulaw.xyzw acc, \ca, vf00[w]
     maddx.xyzw ce_ic\@, ce_ec\@, ce_q\@
     cp_store ce_ip\@, ce_is\@, ce_ic\@
ce_nocross\@:
     .endm

     .macro clip_pass
     b cp_fence_lid\@
cp_fence_lid\@:
     ilw.z cp_plane_addr, kRViewLoop(buffer_top)
     lq cp_pl, 0(cp_plane_addr)
     ilw.x cp_src, kRViewLoop(buffer_top)
     ilw.y cp_dst, kRViewLoop(buffer_top)
     iaddiu cp_m, vi00, 0
     lq cp_pf, 0(cp_src)
     lq cp_sf, 1(cp_src)
     lq cp_cf, 2(cp_src)
     pd_plane cp_df, cp_pf, cp_pl
     pd_sign cp_ff, cp_df
     move.xyzw cp_pp, cp_pf
     move.xyzw cp_sp, cp_sf
     move.xyzw cp_cp, cp_cf
     move.x cp_dp, cp_df
     iadd cp_fp, cp_ff, vi00
     iaddiu cp_i, vi00, 1
     iaddiu cp_src, cp_src, 3
cp_loop_lid\@:
     ibeq cp_i, sh_n, cp_wrap_lid\@
     lq cp_pc, 0(cp_src)
     lq cp_sc, 1(cp_src)
     lq cp_cc, 2(cp_src)
     pd_plane cp_dc, cp_pc, cp_pl
     pd_sign cp_fc, cp_dc
     cp_edge cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pc, cp_sc, cp_cc, cp_dc, cp_fc
     move.xyzw cp_pp, cp_pc
     move.xyzw cp_sp, cp_sc
     move.xyzw cp_cp, cp_cc
     move.x cp_dp, cp_dc
     iadd cp_fp, cp_fc, vi00
     iaddiu cp_src, cp_src, 3
     iaddiu cp_i, cp_i, 1
     b cp_loop_lid\@
cp_wrap_lid\@:
     cp_edge cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pf, cp_sf, cp_cf, cp_df, cp_ff
     iadd sh_n, cp_m, vi00
     .endm

     .macro fmt_color dst, src
     loi 128.0
     muli.xyzw \dst, \src, i
     ftoi0 \dst, \dst
     .endm

     .macro emit_mvert p, s, c, k, adcreg
     div q, vf00[w], \p[w]
     addq.x emq\@, vf00, q
     mulx.xyz emp\@, \p, emq\@
     add.xyz emp\@, emp\@, gs_offsets
     ftoi4.xyz emp\@, emp\@
     mulx.xyz ems\@, \s, emq\@
     sq.xyz ems\@, \k(next_output)
     fmt_color emc\@, \c
     store_rgba emc\@, \k
     fog_coef_alpha emf\@, \c
     ior emadc\@, \adcreg, emf\@
     mfir.w emp\@, emadc\@
     store_xyzf emp\@, \k
     .endm

     .macro r_kick_chunk
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     lq gif_tag_p, kGifTag(vi00)
     mtir eop_p, gif_tag_px
     ior eop_p, eop_p, out_count
     mfir.x gif_tag_p, eop_p
     sq gif_tag_p, 0(arena_tag)
     b r_kick_fence_lid\@
r_kick_fence_lid\@:
     ; SQ latency is4 (VU manualp190), and XGKICK must not start before the
     ; final tag store completes (p196). VCL removes empty branch fences;
     ; preserve three useful independent VI setup pairs, then the kick.
     ; The raw block is also a scheduler barrier. No VU memory is overwritten
     ; here; arena_tag still names the packet just completed above.
     .raw
     nop iaddiu arena_a, buffer_top, kRATag
     nop iaddiu next_output, buffer_top, kRAData
     nop iaddiu out_count, vi00, 0
     nop xgkick arena_tag
     .endraw
     --barrier
     b r_after_kick_lid\@
r_after_kick_lid\@:
     ibeq arena_tag, arena_a, r_use_b_lid\@
     b r_arena_ready_lid\@
r_use_b_lid\@:
     iaddiu next_output, buffer_top, kRBData
r_arena_ready_lid\@:
     .endm

     ; Load one sky-fan corner and retain the existing X2 fast-path decision.
     ; Store PRE-divide position to viewA; classification alone uses divide.
     .macro r_project_corner ptr, off, slot
     lq rp_eye\@, \off(\ptr)
     move.xyzw rp_batch\@, rp_eye\@
     loi -1.0
     muli.xz rp_batch\@, rp_eye\@, i
     mul_pt_mat_44 rp_clip\@, vert_xform, rp_batch\@
     sq rp_clip\@, kRViewA+(\slot*3)(buffer_top)
     lq rp_stq\@, \off+1(\ptr)
     lq rp_col\@, \off+2(\ptr)
     sq rp_stq\@, kRViewA+(\slot*3)+1(buffer_top)
     sq rp_col\@, kRViewA+(\slot*3)+2(buffer_top)
     sub.w rp_near\@, rp_clip\@, near_plane
     loi 2047.0
     minii.w rp_near\@, rp_near\@, i
     loi -2047.0
     maxi.w rp_near\@, rp_near\@, i
     ftoi4.w rp_near\@, rp_near\@
     mtir rp_flag\@, rp_near\@[w]
     iand rp_flag\@, rp_flag\@, adc_bit
     ior near_any, near_any, rp_flag\@
     div q, vf00[w], rp_clip\@[w]
     addq.x rp_q\@, vf00, q
     mulx.xyz rp_projected\@, rp_clip\@, rp_q\@
     clip_vert rp_projected\@
     .endm

     .macro r_sky_store p, s, c
     isubiu sc_bound\@, sc_m, 27
     ibltz sc_bound\@, sc_sky_store_ok\@
     r_fail_stop
sc_sky_store_ok\@:
     sq \p, 0(sc_dst)
     sq \s, 1(sc_dst)
     sq \c, 2(sc_dst)
     iaddiu sc_dst, sc_dst, 3
     iaddiu sc_m, sc_m, 1
     .endm

     --enter
     --endenter
     load_vert_xfrm vert_xform
     load_fog_params fog_params
     sub.w zw_ent, vf00, vf00
     addx.w near_plane, zw_ent, fog_params[x]
     --cont

r_main_lid:
     init_constants
     get_ones_vec ones
     lq.xyz clip_scales, kClipInfo(vi00)
     loi 2048.0
     maxi.w clip_scales, vf00, i
     xtop buffer_top
     ilw.x setup_n, 0(buffer_top)
     ibgtz setup_n, r_header_nonempty_lid
     r_fail_stop
r_header_nonempty_lid:
     isubiu setup_bad, setup_n, 32
     iblez setup_bad, r_header_count_ok_lid
     r_fail_stop
r_header_count_ok_lid:
     iaddiu setup_ptr, buffer_top, 5
     isw.x setup_ptr, kRDesc(buffer_top)
     isw.y setup_n, kRDesc(buffer_top)
     isw.z vi00, kRDesc(buffer_top)
     iaddiu setup_ptr, buffer_top, kRAData
     isw.x setup_ptr, kROut(buffer_top)
     isw.y vi00, kROut(buffer_top)
     isw.x vi00, kRFailure(buffer_top)
     iaddiu adc_bit, vi00, 0x7fff
     iaddiu adc_bit, adc_bit, 1

     sub.xyzw pl_zero, vf00, vf00
     move.xyzw pl_t, pl_zero
     addw.z pl_t, pl_zero, vf00
     subw.w pl_t, pl_zero, near_plane
     sq pl_t, kRPlanes(buffer_top)
     move.xyzw pl_t, pl_zero
     subx.x pl_t, pl_zero, clip_scales
     addw.z pl_t, pl_zero, clip_scales
     sq pl_t, kRPlanes+1(buffer_top)
     move.xyzw pl_t, pl_zero
     addx.x pl_t, pl_zero, clip_scales
     addw.z pl_t, pl_zero, clip_scales
     sq pl_t, kRPlanes+2(buffer_top)
     move.xyzw pl_t, pl_zero
     suby.y pl_t, pl_zero, clip_scales
     addw.z pl_t, pl_zero, clip_scales
     sq pl_t, kRPlanes+3(buffer_top)
     move.xyzw pl_t, pl_zero
     addy.y pl_t, pl_zero, clip_scales
     addw.z pl_t, pl_zero, clip_scales
     sq pl_t, kRPlanes+4(buffer_top)
     b r_decode_lid

r_decode_lid:
     ilw.x desc_ptr, kRDesc(buffer_top)
     ilw.z desc_phase, kRDesc(buffer_top)
     lq desc_ab, 0(desc_ptr)
     lq desc_cd, 1(desc_ptr)
     lq desc_v, 2(desc_ptr)
     lq material_height, 56(vi00)
     mr32 material_height, material_height
     mr32 material_height, material_height
     mr32 material_height, material_height
     lq material_color, 55(vi00)
     r_corner_xy decode_p, desc_ab
     sq decode_p, kRSkyA(buffer_top)
     sub.xyzw decode_s, vf00, vf00
     mr32.z decode_s, vf00
     sq decode_s, kRSkyA+1(buffer_top)
     sq material_color, kRSkyA+2(buffer_top)
     ibne desc_phase, vi00, r_decode_ac_lid
     r_corner_zw decode_p, desc_ab
     addw.x decode_s, vf00, vf00
     b r_decode_second_lid
r_decode_ac_lid:
     r_corner_xy decode_p, desc_cd
     addw.x decode_s, vf00, vf00
     mulx.y decode_s, ones, desc_v
r_decode_second_lid:
     sq decode_p, kRSkyA+3(buffer_top)
     sq decode_s, kRSkyA+4(buffer_top)
     sq material_color, kRSkyA+5(buffer_top)
     ibne desc_phase, vi00, r_decode_d_lid
     r_corner_xy decode_p, desc_cd
     b r_decode_third_lid
r_decode_d_lid:
     r_corner_zw decode_p, desc_cd
     move.x decode_s, vf00
r_decode_third_lid:
     mulx.y decode_s, ones, desc_v
     sq decode_p, kRSkyA+6(buffer_top)
     sq decode_s, kRSkyA+7(buffer_top)
     sq material_color, kRSkyA+8(buffer_top)
     isw.x vi00, kRMask(buffer_top)
     isw.y vi00, kRMask(buffer_top)
     iaddiu setup_mask, vi00, 4095
     isw.z setup_mask, kRMask(buffer_top)
     isw.w setup_mask, kRMask(buffer_top)
     iaddiu setup_mask, vi00, 31
     isw.x setup_mask, kRFan(buffer_top)
     iaddiu vertex_ptr, buffer_top, kRSkyA
     iaddiu vertex_left, vi00, 3
     b r_classify_lid

r_classify_lid:
     lq world_p, 0(vertex_ptr)
     lq eye_cfg, 52(vi00)
     lq center_cfg, 53(vi00)
     lq source_cfg, 54(vi00)
     move.x source_pair, world_p
     mr32 source_rot, world_p
     move.y source_pair, source_rot
     sub.xy source_pair, source_pair, center_cfg
     mul.xy radius_terms, source_pair, source_pair
     addy.x radius2, radius_terms, radius_terms
     addw.x inside_test, vf00, eye_cfg
     sub.x inside_test, inside_test, radius2
     iaddiu mask_lo, vi00, 0
     iaddiu mask_hi, vi00, 0
     r_negative radius_out, inside_test
     ibeq radius_out, vi00, r_masks_done_lid
     iaddiu source_plane_ptr, vi00, 25
     iaddiu source_bit, vi00, 1
     b r_source_plane_lid
r_source_plane_lid:
     lq source_plane, 0(source_plane_ptr)
     mul.xy source_terms, source_plane, source_pair
     addy.x source_d, source_terms, source_terms
     addz.x source_d, source_d, source_plane
     r_negative source_out, source_d
     ibeq source_out, vi00, r_source_next_lid
     ior mask_hi, mask_hi, source_bit
r_source_next_lid:
     iaddiu source_plane_ptr, source_plane_ptr, 1
     iadd source_bit, source_bit, source_bit
     isubiu source_half, source_plane_ptr, 37
     ibne source_half, vi00, r_source_not_half_lid
     iadd mask_lo, mask_hi, vi00
     iaddiu mask_hi, vi00, 0
     iaddiu source_bit, vi00, 1
r_source_not_half_lid:
     isubiu source_left, source_plane_ptr, 49
     ibltz source_left, r_source_plane_lid
r_masks_done_lid:
     ilw.x merge_mask, kRMask(buffer_top)
     ior merge_mask, merge_mask, mask_lo
     isw.x merge_mask, kRMask(buffer_top)
     ilw.y merge_mask, kRMask(buffer_top)
     ior merge_mask, merge_mask, mask_hi
     isw.y merge_mask, kRMask(buffer_top)
     ilw.z merge_mask, kRMask(buffer_top)
     iand merge_mask, merge_mask, mask_lo
     isw.z merge_mask, kRMask(buffer_top)
     ilw.w merge_mask, kRMask(buffer_top)
     iand merge_mask, merge_mask, mask_hi
     isw.w merge_mask, kRMask(buffer_top)

     ; Source-relative subtraction first, then separate ordered dot products.
     sub.xyz eye_delta, world_p, eye_cfg
     move.xyzw eye_p, vf00
     lq eye_basis, 49(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     move.x eye_p, eye_sum
     lq eye_basis, 50(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     mulx.y eye_p, ones, eye_sum
     lq eye_basis, 51(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     mulx.z eye_p, ones, eye_sum
     sq eye_p, 0(vertex_ptr)

     ; Exact source SH outcode: behind-near vertices carry only bit0.
     addz.x source_near_d, vf00, eye_p
     subx.x source_near_d, source_near_d, source_cfg
     r_negative source_near, source_near_d
     iaddiu source_oc, vi00, 1
     ibne source_near, vi00, r_source_view_done_lid
     sub.xyzw source_side, vf00, vf00
     mulz.x source_side, eye_p, center_cfg
     mulw.y source_side, eye_p, center_cfg
     mulz.w source_side, ones, eye_p
     muly.w source_side, source_side, source_cfg
     clipw.xyz source_side, source_side[w]
     fcget source_oc
     iaddiu source_side_mask, vi00, 15
     iand source_oc, source_oc, source_side_mask
     iadd source_oc, source_oc, source_oc
r_source_view_done_lid:
     ilw.x source_and, kRFan(buffer_top)
     iand source_and, source_and, source_oc
     isw.x source_and, kRFan(buffer_top)
     iaddiu vertex_ptr, vertex_ptr, 3
     isubiu vertex_left, vertex_left, 1
     ibgtz vertex_left, r_classify_lid
     b r_triangle_classified_lid
r_triangle_classified_lid:
     ilw.x source_and, kRFan(buffer_top)
     ibne source_and, vi00, r_next_triangle_lid
     ilw.z source_and, kRMask(buffer_top)
     ilw.w source_and_hi, kRMask(buffer_top)
     ior source_and, source_and, source_and_hi
     ibne source_and, vi00, r_next_triangle_lid
     iaddiu setup_ptr, buffer_top, kRSkyA
     isw.w setup_ptr, kROut(buffer_top)
     iaddiu setup_ptr, buffer_top, kRSkyB
     isw.z setup_ptr, kRPlaneState(buffer_top)
     iaddiu setup_n, vi00, 3
     isw.z setup_n, kROut(buffer_top)
     isw.x vi00, kRPlaneState(buffer_top)
     iaddiu setup_n, vi00, 1
     isw.y setup_n, kRPlaneState(buffer_top)
     b r_sky_plane_lid

r_sky_plane_lid:
     ilw.x sky_plane_idx, kRPlaneState(buffer_top)
     isubiu sky_plane_left, sky_plane_idx, 24
     ibgez sky_plane_left, r_sky_fan_begin_lid
     ilw.y sky_bit, kRPlaneState(buffer_top)
     ilw.x sky_mask, kRMask(buffer_top)
     isubiu sky_half, sky_plane_idx, 12
     ibltz sky_half, r_sky_mask_lid
     ilw.y sky_mask, kRMask(buffer_top)
     ibne sky_half, vi00, r_sky_mask_lid
     iaddiu sky_bit, vi00, 1
r_sky_mask_lid:
     iand sky_selected, sky_mask, sky_bit
     iadd sky_bit, sky_bit, sky_bit
     isw.y sky_bit, kRPlaneState(buffer_top)
     iaddiu sky_plane_addr, sky_plane_idx, 1
     isw.x sky_plane_addr, kRPlaneState(buffer_top)
     ibeq sky_selected, vi00, r_sky_plane_lid
     lq sc_plane, 0(sky_plane_addr)
     ilw.z sc_n, kROut(buffer_top)
     ilw.w sc_src, kROut(buffer_top)
     ilw.z sc_dst, kRPlaneState(buffer_top)
     iadd sc_last, sc_n, sc_n
     iadd sc_last, sc_last, sc_n
     iadd sc_last, sc_last, sc_src
     isubiu sc_last, sc_last, 3
     lq sc_prev_p, 0(sc_last)
     lq sc_prev_s, 1(sc_last)
     lq sc_prev_c, 2(sc_last)
     r_sky_distance sc_prev_d, sc_prev_p, sc_plane
     r_negative sc_prev_out, sc_prev_d
     iadd sc_left, sc_n, vi00
     iaddiu sc_m, vi00, 0
     b r_sky_edge_lid
r_sky_edge_lid:
     lq sc_cur_p, 0(sc_src)
     lq sc_cur_s, 1(sc_src)
     lq sc_cur_c, 2(sc_src)
     r_sky_distance sc_cur_d, sc_cur_p, sc_plane
     r_negative sc_cur_out, sc_cur_d
     ibeq sc_prev_out, sc_cur_out, r_sky_inside_lid
     sub.x sc_den, sc_prev_d, sc_cur_d
     abs.x sc_abs_den, sc_den
     lq sc_eps, 54(vi00)
     addz.x sc_guard, vf00, sc_eps
     sub.x sc_guard, sc_guard, sc_abs_den
     r_negative sc_divide, sc_guard
     sub.x sc_t, vf00, vf00
     ibeq sc_divide, vi00, r_sky_lerp_lid
     div q, sc_prev_d[x], sc_den[x]
     addq.x sc_t, vf00, q
     b r_sky_lerp_lid
r_sky_lerp_lid:
     ; Match prev+(cur-prev)*t, never MADD; t=0 on the EE small-denom arm.
     sub.xyzw sc_delta, sc_cur_p, sc_prev_p
     mulx.xyzw sc_product, sc_delta, sc_t
     add.xyzw sc_ip, sc_prev_p, sc_product
     sub.xyz sc_delta, sc_cur_s, sc_prev_s
     mulx.xyz sc_product, sc_delta, sc_t
     add.xyz sc_is, sc_prev_s, sc_product
     mulx.w sc_is, vf00, vf00
     sub.xyzw sc_delta, sc_cur_c, sc_prev_c
     mulx.xyzw sc_product, sc_delta, sc_t
     add.xyzw sc_ic, sc_prev_c, sc_product
     r_sky_store sc_ip, sc_is, sc_ic
r_sky_inside_lid:
     ibne sc_cur_out, vi00, r_sky_edge_next_lid
     r_sky_store sc_cur_p, sc_cur_s, sc_cur_c
r_sky_edge_next_lid:
     move.xyzw sc_prev_p, sc_cur_p
     move.xyzw sc_prev_s, sc_cur_s
     move.xyzw sc_prev_c, sc_cur_c
     move.x sc_prev_d, sc_cur_d
     iadd sc_prev_out, sc_cur_out, vi00
     iaddiu sc_src, sc_src, 3
     isubiu sc_left, sc_left, 1
     ibgtz sc_left, r_sky_edge_lid
     isubiu sc_valid, sc_m, 3
     ibltz sc_valid, r_next_triangle_lid
     ilw.w sky_old_src, kROut(buffer_top)
     ilw.z sky_new_src, kRPlaneState(buffer_top)
     isw.w sky_new_src, kROut(buffer_top)
     isw.z sky_old_src, kRPlaneState(buffer_top)
     isw.z sc_m, kROut(buffer_top)
     b r_sky_plane_lid

r_sky_fan_begin_lid:
     ilw.w sky_first, kROut(buffer_top)
     ilw.z sky_count, kROut(buffer_top)
     isw.y sky_first, kRFan(buffer_top)
     iaddiu sky_rim, sky_first, 3
     isw.z sky_rim, kRFan(buffer_top)
     isubiu sky_left, sky_count, 2
     isw.w sky_left, kRFan(buffer_top)
     b r_sky_fan_lid
r_sky_fan_lid:
     ilw.y sky_first, kRFan(buffer_top)
     ilw.z sky_rim, kRFan(buffer_top)
     iaddiu near_any, vi00, 0
     r_project_corner sky_first, 0, 0
     r_project_corner sky_rim, 0, 1
     r_project_corner sky_rim, 3, 2
     fcand vi01, 0xf3cf
     ior view_dispatch, near_any, vi01
     iaddiu sh_n, vi00, 3
     ibne view_dispatch, vi00, r_view_clip_lid
     iaddiu view_final, buffer_top, kRViewA
     b r_view_ready_lid
r_view_clip_lid:
     ; One runtime pass body preserves the five original X2 plane/edge orders
     ; without five unrolled copies pushing outer branches beyond +/-1024.
     iaddiu view_state, buffer_top, kRViewA
     isw.x view_state, kRViewLoop(buffer_top)
     iaddiu view_state, buffer_top, kRViewB
     isw.y view_state, kRViewLoop(buffer_top)
     iaddiu view_state, buffer_top, kRPlanes
     isw.z view_state, kRViewLoop(buffer_top)
     iaddiu view_state, vi00, 5
     isw.w view_state, kRViewLoop(buffer_top)
     b r_view_plane_lid
r_view_plane_lid:
     clip_pass
     isubiu view_valid, sh_n, 3
     ibltz view_valid, r_next_sky_fan_lid
     ilw.x view_old_src, kRViewLoop(buffer_top)
     ilw.y view_final, kRViewLoop(buffer_top)
     isw.x view_final, kRViewLoop(buffer_top)
     isw.y view_old_src, kRViewLoop(buffer_top)
     ilw.z view_state, kRViewLoop(buffer_top)
     iaddiu view_state, view_state, 1
     isw.z view_state, kRViewLoop(buffer_top)
     ilw.w view_remaining, kRViewLoop(buffer_top)
     isubiu view_remaining, view_remaining, 1
     isw.w view_remaining, kRViewLoop(buffer_top)
     ibgtz view_remaining, r_view_plane_lid
     b r_view_ready_lid
r_view_ready_lid:
     isw.x view_final, kRView(buffer_top)
     isw.y sh_n, kRView(buffer_top)
     ilw.x next_output, kROut(buffer_top)
     ilw.y out_count, kROut(buffer_top)
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     iaddiu arena_a, buffer_top, kRATag
     ibeq arena_tag, arena_a, r_fan_cap_a_lid
     iaddiu arena_cap, vi00, kRBCap
     b r_fan_cap_lid
r_fan_cap_a_lid:
     iaddiu arena_cap, vi00, kRACap
r_fan_cap_lid:
     isubiu fan_tris, sh_n, 2
     iadd fan_verts, fan_tris, fan_tris
     iadd fan_verts, fan_verts, fan_tris
     iadd fan_need, out_count, fan_verts
     isub fan_need, fan_need, arena_cap
     iblez fan_need, r_emit_fence_lid
     r_kick_chunk
     b r_emit_fence_lid
r_emit_fence_lid:
     ilw.x fe_p0, kRView(buffer_top)
     ilw.y fe_n, kRView(buffer_top)
     isubiu fe_t, fe_n, 2
     iaddiu fe_pi, fe_p0, 3
r_emit_lid:
     lq fe_pos, 0(fe_p0)
     lq fe_stq, 1(fe_p0)
     lq fe_col, 2(fe_p0)
     emit_mvert fe_pos, fe_stq, fe_col, 0, adc_bit
     lq fe_pos, 0(fe_pi)
     lq fe_stq, 1(fe_pi)
     lq fe_col, 2(fe_pi)
     emit_mvert fe_pos, fe_stq, fe_col, 3, adc_bit
     lq fe_pos, 3(fe_pi)
     lq fe_stq, 4(fe_pi)
     lq fe_col, 5(fe_pi)
     emit_mvert fe_pos, fe_stq, fe_col, 6, vi00
     iaddiu next_output, next_output, 9
     iaddiu out_count, out_count, 3
     iaddiu fe_pi, fe_pi, 3
     isubiu fe_t, fe_t, 1
     ibgtz fe_t, r_emit_lid
     isw.x next_output, kROut(buffer_top)
     isw.y out_count, kROut(buffer_top)
     b r_next_sky_fan_lid
r_next_sky_fan_lid:
     ilw.z sky_rim, kRFan(buffer_top)
     iaddiu sky_rim, sky_rim, 3
     isw.z sky_rim, kRFan(buffer_top)
     ilw.w sky_left, kRFan(buffer_top)
     isubiu sky_left, sky_left, 1
     isw.w sky_left, kRFan(buffer_top)
     ibgtz sky_left, r_sky_fan_lid
     b r_next_triangle_lid
r_next_triangle_lid:
     ilw.z desc_phase, kRDesc(buffer_top)
     ibne desc_phase, vi00, r_next_descriptor_lid
     iaddiu desc_phase, vi00, 1
     isw.z desc_phase, kRDesc(buffer_top)
     b r_decode_lid
r_next_descriptor_lid:
     isw.z vi00, kRDesc(buffer_top)
     ilw.x desc_ptr, kRDesc(buffer_top)
     iaddiu desc_ptr, desc_ptr, 3
     isw.x desc_ptr, kRDesc(buffer_top)
     ilw.y desc_left, kRDesc(buffer_top)
     isubiu desc_left, desc_left, 1
     isw.y desc_left, kRDesc(buffer_top)
     ibgtz desc_left, r_decode_lid
     ilw.x next_output, kROut(buffer_top)
     ilw.y out_count, kROut(buffer_top)
     r_kick_chunk
     --cont
     b r_main_lid

     .END
