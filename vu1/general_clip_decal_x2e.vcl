/* Copyright (C) 2000,2001,2002 Sony Computer Entertainment America
 * LGPL Version2.1; see COPYING. HyperSolar compact entrance adapter, 2026.
 */

     ; Standalone X2E: EE owns Z/W and per-original-triangle depth admission.
     ; VU owns XY, strict five-plane clipping, projection and final packing.
     ; ALL base inputs precede ALL glow inputs in the caller's packet chain.
     ; Never use an interleaved base/glow compound kick for these stickers.
     ; Context q1..4 combined matrix; q5=[rx,ry,rd,near];
     ; q6=[invRx,invRy,invDepth,M]; q7=[D,baseBias,glowBias,pass];
     ; q8=[guardX,guardY,XY-reuse(0/1),accept/prefix flags(0/1/3)]. Public source keeps
     ; z/w=0; the
     ; linked library selects its private decoder gate after owning the copy.
     ; Standard q57 and q75..78 retain their ABI.
     ; Complete context precedes MSCAL0; Load must not start stale context.
     ; Half472q, BASE79/OFFSET472:
     ; 0..4 header;5..148 descriptors16*9q;149..160 decoded4corners*3q;
     ; 161..184 polyA;185..208 polyB;209..213 planes;214..221 controls;
     ; outputA tag224/data225..314 (30v), B315/data316..369 (18v).
     ; 370..471 unused. Polygon vertex=[homogeneousPos,UV/glow/0,RGBA].
     ; Each descriptor q0 endpointsAxAzBxBz;q1 ylo/yhi/u0/u1;
     ; q2 v0/v1/glowA/glowB;q3RGBA A/D;q4RGBA B/C;
     ; q5 Za/Wa/Zb/Wb;q6 Zc/Wc/Zd/Wd;
     ; q7/q8 perABC/ACD=[allowance,sumAbsZ,sumAbsW,skipFloat].
     ; No coordinate, Z/W or UV reconstruction from approximate axes.
     ; Source ABC then ACD; strict >=0 last->first clipping; identity passes
     ; retain the original polygon anchor. FMA does not replace EE lerps.
     ; Final depth preserves the accepted base-NDC then glow-NDC sequence.
     ; Header q0.y selects 0=compact quads or1=already projected triangles.
     ; Projected records are3 vertices *3q: [x,y,baseZ,glowZ], [S,T,Q,glowA],
     ; baseRGBA. They have the same9q stride and use the ordinary raw-X2
     ; identity-derived q62..65 transform before the shared final GS writer.

     #include "vu1_mem_linear.h"
     .include "db_in_db_out.i"
     .include "math.i"
     .include "geometry.i"

kEQuad              .equ 149
kEPolyA             .equ 161
kEPolyB             .equ 185
kEPlanes            .equ 209
kEDesc              .equ 214
kEOut               .equ 215
kEClip              .equ 216
kEFinal             .equ 217
kEAllowance         .equ 218
kEFailure           .equ 219
kEReuse             .equ 220
kETrivial           .equ 221
kEATag              .equ 224
kEAData             .equ 225
kEACap              .equ 30
kEBTag              .equ 315
kEBData             .equ 316
kEBCap              .equ 18

     .init_vf_all
     .init_vi_all
     .name vsmGeneralClipDecalX2E

     .macro e_fail_stop
     iaddiu ef_code\@, vi00, 1
     isw.x ef_code\@, kEFailure(buffer_top)
     b ef_stop_lid\@
ef_stop_lid\@:
     b ef_stop_lid\@
     .endm

     ; True strict negative test, including distances in (-1/16,0).
     ; The generated CLIPw->FCAND latency must still be checked.
     .macro e_negative flag, value
     sub.xyzw en_v\@, vf00, vf00
     move.x en_v\@, \value
     clipw.xyz en_v\@, en_v\@[w]
     fcand vi01, 2
     iadd \flag, vi01, vi00
     .endm

     .macro e_distance result, position, plane
     mulw.z ed_w\@, \plane, \position
     mul.xy ed_xy\@, \plane, \position
     addz.x \result, vf00, ed_w\@
     addx.x \result, \result, ed_xy\@
     addy.x \result, \result, ed_xy\@
     addw.x \result, \result, \plane
     .endm

     ; Rectangle products reused without reassociation: each corner still
     ; evaluates ((m0*x + m1*y) + m2*z) + m3 with separate MUL and ADD.
     ; Only common products are shared; Z/W, slope and clipping stay original.
     .macro e_decode_shared_xy
     lq er_edge0, 0(desc_ptr)
     lq er_yu0, 1(desc_ptr)
     lq er_m0, 1(vi00)
     lq er_m1, 2(vi00)
     lq er_m2, 3(vi00)
     lq er_m3, 4(vi00)
     mulx.xy er_lo0, er_m1, er_yu0
     muly.xy er_hi0, er_m1, er_yu0
     mulx.xy er_xterm0, er_m0, er_edge0
     muly.xy er_zterm0, er_m2, er_edge0
     add.xy er_a0, er_xterm0, er_lo0
     add.xy er_a0, er_a0, er_zterm0
     add.xy er_a0, er_a0, er_m3
     sq.xy er_a0, kEQuad(buffer_top)
     add.xy er_d0, er_xterm0, er_hi0
     add.xy er_d0, er_d0, er_zterm0
     add.xy er_d0, er_d0, er_m3
     sq.xy er_d0, kEQuad+9(buffer_top)
     mulz.xy er_xterm0, er_m0, er_edge0
     mulw.xy er_zterm0, er_m2, er_edge0
     add.xy er_b0, er_xterm0, er_lo0
     add.xy er_b0, er_b0, er_zterm0
     add.xy er_b0, er_b0, er_m3
     sq.xy er_b0, kEQuad+3(buffer_top)
     add.xy er_c0, er_xterm0, er_hi0
     add.xy er_c0, er_c0, er_zterm0
     add.xy er_c0, er_c0, er_m3
     sq.xy er_c0, kEQuad+6(buffer_top)
     .endm

     ; Static corner fields copy bits; only combined XY does arithmetic.
     .macro e_decode slot, side, high, zword, zsecond, colorword, prepared
     .aif "\prepared" eq "0"
     lq ec_ab\@, 0(desc_ptr)
     .aendi
     lq ec_yu\@, 1(desc_ptr)
     lq ec_vg\@, 2(desc_ptr)
     .aif "\prepared" eq "0"
     .aif "\side" eq "1"
     mr32 ec_ab\@, ec_ab\@
     mr32 ec_ab\@, ec_ab\@
     .aendi
     move.xyzw ec_p\@, vf00
     move.x ec_p\@, ec_ab\@
     mr32 ec_ab\@, ec_ab\@
     mr32 ec_ab\@, ec_ab\@
     mr32 ec_ab\@, ec_ab\@
     move.z ec_p\@, ec_ab\@
     .aif "\high" eq "1"
     move.y ec_p\@, ec_yu\@
     .aelse
     mr32 ec_rot\@, ec_yu\@
     mr32 ec_rot\@, ec_rot\@
     mr32 ec_rot\@, ec_rot\@
     move.y ec_p\@, ec_rot\@
     .aendi
     lq ec_m\@, 1(vi00)
     mulx.xy ec_clip\@, ec_m\@, ec_p\@
     lq ec_m\@, 2(vi00)
     muly.xy ec_term\@, ec_m\@, ec_p\@
     add.xy ec_clip\@, ec_clip\@, ec_term\@
     lq ec_m\@, 3(vi00)
     mulz.xy ec_term\@, ec_m\@, ec_p\@
     add.xy ec_clip\@, ec_clip\@, ec_term\@
     lq ec_m\@, 4(vi00)
     add.xy ec_clip\@, ec_clip\@, ec_m\@
     .aelse
     lq ec_clip\@, kEQuad+(\slot*3)(buffer_top)
     .aendi
     lq ec_zw\@, \zword(desc_ptr)
     .aif "\zsecond" eq "0"
     mr32 ec_zw\@, ec_zw\@
     mr32 ec_zw\@, ec_zw\@
     .aendi
     move.zw ec_clip\@, ec_zw\@
     sq ec_clip\@, kEQuad+(\slot*3)(buffer_top)
     sub.xyzw ec_s\@, vf00, vf00
     mr32 ec_yu\@, ec_yu\@
     mr32 ec_yu\@, ec_yu\@
     .aif "\side" eq "1"
     mr32 ec_yu\@, ec_yu\@
     mr32 ec_glow\@, ec_vg\@
     move.z ec_s\@, ec_glow\@
     .aelse
     move.z ec_s\@, ec_vg\@
     .aendi
     move.x ec_s\@, ec_yu\@
     .aif "\high" eq "0"
     mr32 ec_vg\@, ec_vg\@
     mr32 ec_vg\@, ec_vg\@
     mr32 ec_vg\@, ec_vg\@
     .aendi
     move.y ec_s\@, ec_vg\@
     sq ec_s\@, kEQuad+(\slot*3)+1(buffer_top)
     lq ec_color\@, \colorword(desc_ptr)
     sq ec_color\@, kEQuad+(\slot*3)+2(buffer_top)
     .endm

     .macro e_copy_corner source, destination
     lq et_p\@, kEQuad+(\source*3)(buffer_top)
     lq et_s\@, kEQuad+(\source*3)+1(buffer_top)
     lq et_c\@, kEQuad+(\source*3)+2(buffer_top)
     sq et_p\@, kEPolyA+(\destination*3)(buffer_top)
     sq et_s\@, kEPolyA+(\destination*3)+1(buffer_top)
     sq et_c\@, kEPolyA+(\destination*3)+2(buffer_top)
     .endm

     .macro e_poly_store position, texture, color
     isubiu es_bound\@, cp_m, 8
     ibltz es_bound\@, es_valid_lid\@
     e_fail_stop
es_valid_lid\@:
     sq \position, 0(cp_dst)
     sq \texture, 1(cp_dst)
     sq \color, 2(cp_dst)
     iaddiu cp_dst, cp_dst, 3
     iaddiu cp_m, cp_m, 1
     .endm

     .macro e_kick
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     lq gif_tag_p, kGifTag(vi00)
     mtir eop_p, gif_tag_px
     ior eop_p, eop_p, out_count
     mfir.x gif_tag_p, eop_p
     sq gif_tag_p, 0(arena_tag)
     b ek_fence_lid\@
ek_fence_lid\@:
     ; SQ requires4 issue pairs before XGKICK; useful VI setup is explicit.
     .raw
     nop iaddiu arena_a, buffer_top, kEATag
     nop iaddiu next_output, buffer_top, kEAData
     nop iaddiu out_count, vi00, 0
     nop xgkick arena_tag
     .endraw
     --barrier
     b ek_after_lid\@
ek_after_lid\@:
     ibne arena_tag, arena_a, ek_ready_lid\@
     iaddiu next_output, buffer_top, kEBData
ek_ready_lid\@:
     .endm

     .macro e_emit ptr, offset, output, adcreg
     lq ee_p\@, \offset(\ptr)
     lq ee_s\@, \offset+1(\ptr)
     lq ee_c\@, \offset+2(\ptr)
     add.xyz ee_p\@, ee_p\@, gs_offsets
     ftoi4.xyz ee_p\@, ee_p\@
     sq.xyz ee_s\@, \output(next_output)
     loi 128.0
     muli.xyzw ee_rgba\@, ee_c\@, i
     ftoi0 ee_rgba\@, ee_rgba\@
     store_rgba ee_rgba\@, \output
     fog_coef_alpha ee_fog\@, ee_c\@
     ior ee_adc\@, \adcreg, ee_fog\@
     mfir.w ee_p\@, ee_adc\@
     store_xyzf ee_p\@, \output
     .endm

     --enter
     --endenter
     --cont

e_main_lid:
     init_constants
     get_ones_vec ones
     xtop buffer_top
     ilw.x setup_count, 0(buffer_top)
     ibgtz setup_count, e_count_positive_lid
     e_fail_stop
e_count_positive_lid:
     isubiu setup_bad, setup_count, 16
     iblez setup_bad, e_count_valid_lid
     e_fail_stop
e_count_valid_lid:
     iaddiu setup_ptr, buffer_top, 5
     isw.x setup_ptr, kEDesc(buffer_top)
     isw.y setup_count, kEDesc(buffer_top)
     isw.z vi00, kEDesc(buffer_top)
     iaddiu setup_ptr, buffer_top, kEAData
     isw.x setup_ptr, kEOut(buffer_top)
     isw.y vi00, kEOut(buffer_top)
     isw.x vi00, kEFailure(buffer_top)
     lq setup_depth, 7(vi00)
     ftoi0.w setup_depth, setup_depth
     mtir setup_pass, setup_depth[w]
     isw.w setup_pass, kEDesc(buffer_top)
     iaddiu adc_bit, vi00, 0x7fff
     iaddiu adc_bit, adc_bit, 1

     sub.xyzw pl_zero, vf00, vf00
     lq pl_raster, 5(vi00)
     lq pl_guard, 8(vi00)
     ftoi0.z setup_reuse, pl_guard
     mtir setup_flag, setup_reuse[z]
     isw.x setup_flag, kEReuse(buffer_top)
     ftoi0.w setup_accept, pl_guard
     mtir setup_flag, setup_accept[w]
     isw.y setup_flag, kEReuse(buffer_top)
     ; Keep independent plane setup after the private control store. VCL
     ; does not model its memory dependency with the first decoder ILW.
     b e_planes_ready_lid
e_planes_ready_lid:
     move.xyzw pl_t, pl_zero
     addw.z pl_t, pl_zero, vf00
     subw.w pl_t, pl_zero, pl_raster
     sq pl_t, kEPlanes(buffer_top)
     move.xyzw pl_t, pl_zero
     addw.x pl_t, pl_zero, vf00
     addx.z pl_t, pl_zero, pl_guard
     sq pl_t, kEPlanes+1(buffer_top)
     subw.x pl_t, pl_zero, vf00
     sq pl_t, kEPlanes+2(buffer_top)
     move.xyzw pl_t, pl_zero
     addw.y pl_t, pl_zero, vf00
     addy.z pl_t, pl_zero, pl_guard
     sq pl_t, kEPlanes+3(buffer_top)
     subw.y pl_t, pl_zero, vf00
     sq pl_t, kEPlanes+4(buffer_top)
     b e_decode_lid

e_decode_lid:
     ilw.y record_format, 0(buffer_top)
     ibne record_format, vi00, e_projected_record_lid
     ilw.x desc_ptr, kEDesc(buffer_top)
     ilw.x reuse_flag, kEReuse(buffer_top)
     ibeq reuse_flag, vi00, e_decode_legacy_lid
     e_decode_shared_xy
     ; Fence the common-product stores before the payload macro reads the
     ; same slots through a new pointer/register allocation.
     b e_decode_payload_lid
e_decode_payload_lid:
     e_decode 0, 0, 0, 5, 0, 3, 1
     e_decode 1, 1, 0, 5, 1, 4, 1
     e_decode 2, 1, 1, 6, 0, 4, 1
     e_decode 3, 0, 1, 6, 1, 3, 1
     b e_quad_classify_lid
e_decode_legacy_lid:
     e_decode 0, 0, 0, 5, 0, 3, 0
     e_decode 1, 1, 0, 5, 1, 4, 0
     e_decode 2, 1, 1, 6, 0, 4, 0
     e_decode 3, 0, 1, 6, 1, 3, 0
     b e_quad_classify_lid

e_quad_classify_lid:
     ; This is only an exact all-inside certificate. Any outside corner
     ; retains the established per-triangle walker. q221.y records the first
     ; unproved plane; every earlier plane leaves both source triangles intact.
     isw.y vi00, kETrivial(buffer_top)
     ilw.y qa_enabled, kEReuse(buffer_top)
     ibeq qa_enabled, vi00, e_quad_partial_lid
     lq qa_a, kEQuad(buffer_top)
     lq qa_b, kEQuad+3(buffer_top)
     lq qa_c, kEQuad+6(buffer_top)
     lq qa_d, kEQuad+9(buffer_top)
     ; SoA lanes are A/B/C/D. Multiplication by one copies signed zero too.
     mulx.x qa_sx0, ones, qa_a
     mulx.y qa_sx0, ones, qa_b
     mulx.z qa_sx0, ones, qa_c
     mulx.w qa_sx0, ones, qa_d
     muly.x qa_sy0, ones, qa_a
     muly.y qa_sy0, ones, qa_b
     muly.z qa_sy0, ones, qa_c
     muly.w qa_sy0, ones, qa_d
     mulw.x qa_sw0, ones, qa_a
     mulw.y qa_sw0, ones, qa_b
     mulw.z qa_sw0, ones, qa_c
     mulw.w qa_sw0, ones, qa_d
     sub.xyzw qa_zero, vf00, vf00
     iaddiu qa_plane, buffer_top, kEPlanes
     iaddiu qa_left, vi00, 5
e_quad_plane_lid:
     lq qa_coeff, 0(qa_plane)
     ; Same separate MUL/ADD tree as e_distance, evaluated four-wide.
     mulz.xyzw qa_dist, qa_sw0, qa_coeff
     add.xyzw qa_dist, qa_zero, qa_dist
     mulx.xyzw qa_term, qa_sx0, qa_coeff
     add.xyzw qa_dist, qa_dist, qa_term
     muly.xyzw qa_term, qa_sy0, qa_coeff
     add.xyzw qa_dist, qa_dist, qa_term
     addw.xyzw qa_dist, qa_dist, qa_coeff
     move.xyzw qa_test, qa_zero
     move.xyz qa_test, qa_dist
     clipw.xyz qa_test, qa_test[w]
     fcand vi01, 42
     ibne vi01, vi00, e_quad_prefix_lid
     addw.x qa_test, qa_zero, qa_dist
     clipw.xyz qa_test, qa_test[w]
     fcand vi01, 2
     ibne vi01, vi00, e_quad_prefix_lid
     iaddiu qa_plane, qa_plane, 1
     isubiu qa_left, qa_left, 1
     ibgtz qa_left, e_quad_plane_lid
     iaddiu qa_inside, vi00, 1
     isw.x qa_inside, kETrivial(buffer_top)
     b e_triangle_lid
e_quad_prefix_lid:
     ilw.y qa_reuse, kEReuse(buffer_top)
     isubiu qa_reuse, qa_reuse, 3
     ibne qa_reuse, vi00, e_quad_partial_lid
     iaddiu qa_first, vi00, 5
     isub qa_first, qa_first, qa_left
     isw.y qa_first, kETrivial(buffer_top)
     b e_quad_partial_lid
e_quad_partial_lid:
     isw.x vi00, kETrivial(buffer_top)
     b e_triangle_lid

e_projected_record_lid:
     ; Exceptional geometry was clipped/depth-admitted by the EE before any
     ; source was published. Reproduce its accepted raw-X2 NDC transform,
     ; including the matrix operation tree and landed reciprocal (W==1).
     ilw.x nd_src, kEDesc(buffer_top)
     ilw.w nd_pass, kEDesc(buffer_top)
     load_vert_xfrm nd_xfrm
     iaddiu nd_dst, buffer_top, kEPolyA
     isw.x nd_dst, kEFinal(buffer_top)
     iaddiu nd_left, vi00, 3
     isw.y nd_left, kEFinal(buffer_top)
     ; One projected record owns one triangle, so normal output completion
     ; must advance the descriptor instead of requesting an ACD half.
     iaddiu nd_phase, vi00, 1
     isw.z nd_phase, kEDesc(buffer_top)
e_projected_vertex_lid:
     lq nd_p, 0(nd_src)
     lq nd_s, 1(nd_src)
     lq nd_c, 2(nd_src)
     ibeq nd_pass, vi00, e_projected_material_lid
     mr32.z nd_p, nd_p
     move.xyzw nd_c, ones
     move.w nd_c, nd_s
e_projected_material_lid:
     mul_pt_mat_44 nd_clip, nd_xfrm, nd_p
     div q, vf00[w], nd_clip[w]
     addq.x nd_q, vf00, q
     mulx.xyz nd_clip, nd_clip, nd_q
     mulx.xyz nd_s, nd_s, nd_q
     sq nd_clip, 0(nd_dst)
     sq nd_s, 1(nd_dst)
     sq nd_c, 2(nd_dst)
     iaddiu nd_src, nd_src, 3
     iaddiu nd_dst, nd_dst, 3
     isubiu nd_left, nd_left, 1
     ibgtz nd_left, e_projected_vertex_lid
     ; The existing output arena, typed tags, ADC/F packing and ordered kicks
     ; are shared. Preserve the source/store seam before the fan reads it.
     b e_output_begin_lid
e_triangle_lid:
     ilw.x desc_ptr, kEDesc(buffer_top)
     ilw.z desc_phase, kEDesc(buffer_top)
     iaddiu coef_ptr, desc_ptr, 7
     iadd coef_ptr, coef_ptr, desc_phase
     lq coeff, 0(coef_ptr)
     ftoi0.w coeff_skip, coeff
     mtir skip_triangle, coeff_skip[w]
     ibne skip_triangle, vi00, e_next_triangle_lid
     sq coeff, kEAllowance(buffer_top)
     e_copy_corner 0, 0
     ibne desc_phase, vi00, e_acd_lid
     e_copy_corner 1, 1
     e_copy_corner 2, 2
     b e_triangle_ready_lid
e_acd_lid:
     e_copy_corner 2, 1
     e_copy_corner 3, 2
e_triangle_ready_lid:
     iaddiu setup_ptr, buffer_top, kEPolyA
     isw.x setup_ptr, kEClip(buffer_top)
     iaddiu setup_ptr, buffer_top, kEPolyB
     isw.y setup_ptr, kEClip(buffer_top)
     iaddiu setup_ptr, vi00, 3
     isw.z setup_ptr, kEClip(buffer_top)
     ilw.x triangle_inside, kETrivial(buffer_top)
     ibne triangle_inside, vi00, e_project_begin_lid
     ilw.y setup_ptr, kETrivial(buffer_top)
     isw.w setup_ptr, kEClip(buffer_top)
     b e_plane_lid

e_plane_lid:
     ilw.w plane_index, kEClip(buffer_top)
     isubiu plane_done, plane_index, 5
     ibgez plane_done, e_project_begin_lid
     iaddiu plane_addr, buffer_top, kEPlanes
     iadd plane_addr, plane_addr, plane_index
     lq cp_plane, 0(plane_addr)
     ilw.x scan_ptr, kEClip(buffer_top)
     ilw.z scan_left, kEClip(buffer_top)
     iaddiu scan_inside, vi00, 0
e_classify_lid:
     lq scan_pos, 0(scan_ptr)
     e_distance scan_d, scan_pos, cp_plane
     e_negative scan_out, scan_d
     ibne scan_out, vi00, e_classify_next_lid
     iaddiu scan_inside, scan_inside, 1
e_classify_next_lid:
     iaddiu scan_ptr, scan_ptr, 3
     isubiu scan_left, scan_left, 1
     ibgtz scan_left, e_classify_lid
     ibeq scan_inside, vi00, e_next_triangle_lid
     ilw.z cp_n, kEClip(buffer_top)
     ibeq scan_inside, cp_n, e_next_plane_lid
     ilw.x cp_src, kEClip(buffer_top)
     ilw.y cp_dst, kEClip(buffer_top)
     iadd cp_last, cp_n, cp_n
     iadd cp_last, cp_last, cp_n
     iadd cp_last, cp_last, cp_src
     isubiu cp_last, cp_last, 3
     lq cp_prev_p, 0(cp_last)
     lq cp_prev_s, 1(cp_last)
     lq cp_prev_c, 2(cp_last)
     e_distance cp_prev_d, cp_prev_p, cp_plane
     e_negative cp_prev_out, cp_prev_d
     iadd cp_left, cp_n, vi00
     iaddiu cp_m, vi00, 0
     b e_edge_lid
e_edge_lid:
     lq cp_cur_p, 0(cp_src)
     lq cp_cur_s, 1(cp_src)
     lq cp_cur_c, 2(cp_src)
     e_distance cp_cur_d, cp_cur_p, cp_plane
     e_negative cp_cur_out, cp_cur_d
     ibeq cp_prev_out, cp_cur_out, e_inside_lid
     sub.x cp_den, cp_prev_d, cp_cur_d
     div q, cp_prev_d[x], cp_den[x]
     addq.x cp_t, vf00, q
     sub.xyzw cp_delta, cp_cur_p, cp_prev_p
     mulx.xyzw cp_product, cp_delta, cp_t
     add.xyzw cp_ip, cp_prev_p, cp_product
     sub.xyz cp_delta, cp_cur_s, cp_prev_s
     mulx.xyz cp_product, cp_delta, cp_t
     add.xyz cp_is, cp_prev_s, cp_product
     mulx.w cp_is, vf00, vf00
     sub.xyzw cp_delta, cp_cur_c, cp_prev_c
     mulx.xyzw cp_product, cp_delta, cp_t
     add.xyzw cp_ic, cp_prev_c, cp_product
     e_poly_store cp_ip, cp_is, cp_ic
e_inside_lid:
     ibne cp_cur_out, vi00, e_edge_next_lid
     e_poly_store cp_cur_p, cp_cur_s, cp_cur_c
e_edge_next_lid:
     move.xyzw cp_prev_p, cp_cur_p
     move.xyzw cp_prev_s, cp_cur_s
     move.xyzw cp_prev_c, cp_cur_c
     move.x cp_prev_d, cp_cur_d
     iadd cp_prev_out, cp_cur_out, vi00
     iaddiu cp_src, cp_src, 3
     isubiu cp_left, cp_left, 1
     ibgtz cp_left, e_edge_lid
     isubiu cp_valid, cp_m, 3
     ibltz cp_valid, e_next_triangle_lid
     ilw.x cp_old_src, kEClip(buffer_top)
     ilw.y cp_new_src, kEClip(buffer_top)
     isw.x cp_new_src, kEClip(buffer_top)
     isw.y cp_old_src, kEClip(buffer_top)
     isw.z cp_m, kEClip(buffer_top)
e_next_plane_lid:
     ilw.w plane_index, kEClip(buffer_top)
     iaddiu plane_index, plane_index, 1
     isw.w plane_index, kEClip(buffer_top)
     b e_plane_lid

e_project_begin_lid:
     ilw.x project_ptr, kEClip(buffer_top)
     ilw.z project_left, kEClip(buffer_top)
     isw.x project_ptr, kEFinal(buffer_top)
     isw.y project_left, kEFinal(buffer_top)
     b e_project_lid
e_project_lid:
     lq pr_p, 0(project_ptr)
     lq pr_s, 1(project_ptr)
     lq pr_c, 2(project_ptr)
     lq pr_raster, 5(vi00)
     lq pr_inverse, 6(vi00)
     lq pr_depth, 7(vi00)
     lq pr_coeff, kEAllowance(buffer_top)
     div q, vf00[w], pr_p[w]
     addq.x pr_q, vf00, q
     mulx.z pr_zq, pr_p, pr_q
     abs.z pr_abs, pr_zq
     addz.x pr_round, vf00, pr_abs
     mulz.x pr_round, pr_round, pr_coeff
     addy.x pr_round, pr_round, pr_coeff
     loi 0.00000095367431640625
     muli.x pr_epsilonq, pr_q, i
     mul.x pr_round, pr_epsilonq, pr_round
     loi 0.5
     muli.w pr_half, pr_inverse, i
     addw.z pr_z_v, pr_zq, pr_half
     addx.z pr_z_v, pr_z_v, pr_coeff
     addx.z pr_z_v, pr_z_v, pr_round
     muly.x pr_base_tier, pr_depth, pr_depth
     mulx.x pr_base_tier, pr_base_tier, pr_q
     addx.z pr_base, pr_z_v, pr_base_tier
     mulz.x pr_glow_tier, pr_depth, pr_depth
     mulx.x pr_glow_tier, pr_glow_tier, pr_q
     addx.z pr_glow_v, pr_z_v, pr_glow_tier
     addz.x pr_check, vf00, pr_base
     e_negative pr_out, pr_check
     ibeq pr_out, vi00, e_base_headroom_lid
     e_fail_stop
e_base_headroom_lid:
     loi 2.0
     subi.w pr_limit, pr_inverse, i
     addw.x pr_check, vf00, pr_limit
     subz.x pr_check, pr_check, pr_glow_v
     e_negative pr_out, pr_check
     ibeq pr_out, vi00, e_glow_headroom_lid
     e_fail_stop
e_glow_headroom_lid:
     mulx.xy pr_xy_v, pr_p, pr_q
     mul.xy pr_xy_v, pr_xy_v, pr_inverse
     mul.xy pr_p, pr_xy_v, pr_raster
     mulz.z pr_ndc, pr_base, pr_inverse
     sub.xyzw pr_zero, vf00, vf00
     addw.z pr_one, pr_zero, vf00
     sub.z pr_ndc, pr_one, pr_ndc
     ilw.w pr_pass, kEDesc(buffer_top)
     ibeq pr_pass, vi00, e_material_ready_lid
     addz.x pr_delta, vf00, pr_depth
     suby.x pr_delta, pr_delta, pr_depth
     mulx.x pr_delta, pr_delta, pr_depth
     mulx.x pr_delta, pr_delta, pr_q
     mulz.x pr_delta, pr_delta, pr_inverse
     subx.z pr_ndc, pr_ndc, pr_delta
     move.xyzw pr_c, ones
     mulz.w pr_c, ones, pr_s
e_material_ready_lid:
     mulz.z pr_p, pr_ndc, pr_raster
     mulx.xy pr_s, pr_s, pr_q
     mulx.z pr_s, ones, pr_q
     sq pr_p, 0(project_ptr)
     sq pr_s, 1(project_ptr)
     sq pr_c, 2(project_ptr)
     iaddiu project_ptr, project_ptr, 3
     isubiu project_left, project_left, 1
     ibgtz project_left, e_project_lid
     b e_output_begin_lid

e_output_begin_lid:
     ilw.x next_output, kEOut(buffer_top)
     ilw.y out_count, kEOut(buffer_top)
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     iaddiu arena_a, buffer_top, kEATag
     ibeq arena_tag, arena_a, e_cap_a_lid
     iaddiu arena_cap, vi00, kEBCap
     b e_cap_ready_lid
e_cap_a_lid:
     iaddiu arena_cap, vi00, kEACap
e_cap_ready_lid:
     ilw.y fan_n, kEFinal(buffer_top)
     isubiu fan_tris, fan_n, 2
     iadd fan_verts, fan_tris, fan_tris
     iadd fan_verts, fan_verts, fan_tris
     iadd fan_need, out_count, fan_verts
     isub fan_need, fan_need, arena_cap
     iblez fan_need, e_emit_begin_lid
     e_kick
     b e_emit_begin_lid
e_emit_begin_lid:
     ilw.x fe_p0, kEFinal(buffer_top)
     ilw.y fe_n, kEFinal(buffer_top)
     isubiu fe_left, fe_n, 2
     iaddiu fe_rim, fe_p0, 3
e_emit_lid:
     e_emit fe_p0, 0, 0, adc_bit
     e_emit fe_rim, 0, 3, adc_bit
     e_emit fe_rim, 3, 6, vi00
     iaddiu next_output, next_output, 9
     iaddiu out_count, out_count, 3
     iaddiu fe_rim, fe_rim, 3
     isubiu fe_left, fe_left, 1
     ibgtz fe_left, e_emit_lid
     isw.x next_output, kEOut(buffer_top)
     isw.y out_count, kEOut(buffer_top)
     b e_next_triangle_lid
e_next_triangle_lid:
     ilw.z desc_phase, kEDesc(buffer_top)
     ibne desc_phase, vi00, e_next_descriptor_lid
     iaddiu desc_phase, vi00, 1
     isw.z desc_phase, kEDesc(buffer_top)
     b e_triangle_lid
e_next_descriptor_lid:
     isw.z vi00, kEDesc(buffer_top)
     ilw.x desc_ptr, kEDesc(buffer_top)
     iaddiu desc_ptr, desc_ptr, 9
     isw.x desc_ptr, kEDesc(buffer_top)
     ilw.y desc_left, kEDesc(buffer_top)
     isubiu desc_left, desc_left, 1
     isw.y desc_left, kEDesc(buffer_top)
     ibgtz desc_left, e_decode_lid
     ilw.x next_output, kEOut(buffer_top)
     ilw.y out_count, kEOut(buffer_top)
     e_kick
     --cont
     b e_main_lid

     .END
