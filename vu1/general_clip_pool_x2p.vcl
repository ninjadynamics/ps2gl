/* HyperSolar X2P: exact horizontal colored/UV-rect pools.
 * Shared sky clipping arithmetic and output ownership with X2R; source SH
 * view clipping follows the sky pass BEFORE polygon triangulation, matching
 * ps2_city_prepare_glow_quads(... x2=false). No secondary X2 clipping.
 * Admission: finite exact coplanar ABC/ACD quads, common sourceY equal to
 * floorY, normalized float RGBA, no CPU haze, supported 24-side enclosure.
 * The backend owns the source descriptor bytes through normal frame DMA.
 *
 * Context is PGLRoadContext-compatible: q1..24 eye sky planes; q25..48
 * source sky planes; q49..51 eye basis; q52 eye.xyz/incircle2;
 * q53 center.x/center.z/px/py; q54 sourceNear/NDC/epsilon/0;
 * q55 unused; q56 sourceY/0/0/0. Standard q0/57/62..65/75..78 retained.
 * q62..65 is GSScale*eye-space projection, without a view transform.
 *
 * Buffer BASE79/OFFSET472, descriptor count1..24 atq0.x, inputq5..100:
 * [Ax,Az,Bx,Bz],[Cx,Cz,Dx,Dz],[u0,v0,u1,v1],[r,g,b,a].
 * q101..105 source view planes; polyA106..201; polyB202..297, each32*3q.
 * q298..315 unused; control316..323 as X2R; outputA324..414 (30verts),
 * outputB415..469 (18verts), guard470..471. One complete fan triangle
 * reserves three output vertices; no drop-on-full or partial primitive.
 * Source positions preserve ordered (world-eye) dot products; clip uses
 * last->first edges, strict represented signs and epsilon1e-6. Native EE/VU
 * rounding equivalence remains a hardware acceptance question.
 */

     #include "vu1_mem_linear.h"
     .include "db_in_db_out.i"
     .include "math.i"
     .include "geometry.i"
     .include "clip_cull.i"

kRPlanes            .equ 101
kRSkyA              .equ 106
kRSkyB              .equ 202
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
     .name vsmGeneralClipPoolX2P

     .include "ground_clip_shared.i"

     .include "source_unlit_emit.i"
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
     isubiu setup_bad, setup_n, 24
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
     ; Source SH planes, not the road's post-projection X2 guard planes.
     ; They run after the 24 sky planes, before the original polygon fan.
     lq pool_cfg, 54(vi00)
     lq pool_proj, 53(vi00)
     move.xyzw pl_t, pl_zero
     addw.z pl_t, pl_zero, vf00
     subx.w pl_t, pl_zero, pool_cfg
     sq pl_t, kRPlanes(buffer_top)
     move.xyzw pl_t, pl_zero
     subz.x pl_t, pl_zero, pool_proj
     addy.z pl_t, pl_zero, pool_cfg
     sq pl_t, kRPlanes+1(buffer_top)
     move.xyzw pl_t, pl_zero
     addz.x pl_t, pl_zero, pool_proj
     addy.z pl_t, pl_zero, pool_cfg
     sq pl_t, kRPlanes+2(buffer_top)
     move.xyzw pl_t, pl_zero
     subw.y pl_t, pl_zero, pool_proj
     addy.z pl_t, pl_zero, pool_cfg
     sq pl_t, kRPlanes+3(buffer_top)
     move.xyzw pl_t, pl_zero
     addw.y pl_t, pl_zero, pool_proj
     addy.z pl_t, pl_zero, pool_cfg
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
     lq material_color, 3(desc_ptr)
     r_corner_xy decode_p, desc_ab
     sq decode_p, kRSkyA(buffer_top)
     sub.xyzw decode_s, vf00, vf00
     mr32.z decode_s, vf00
     move.xy decode_s, desc_v
     sq decode_s, kRSkyA+1(buffer_top)
     sq material_color, kRSkyA+2(buffer_top)
     ibne desc_phase, vi00, r_decode_ac_lid
     r_corner_zw decode_p, desc_ab
     mr32 pool_uvfar, desc_v
     mr32 pool_uvfar, pool_uvfar
     move.x decode_s, pool_uvfar
     b r_decode_second_lid
r_decode_ac_lid:
     r_corner_xy decode_p, desc_cd
     mr32 pool_uvfar, desc_v
     mr32 pool_uvfar, pool_uvfar
     move.xy decode_s, pool_uvfar
r_decode_second_lid:
     sq decode_p, kRSkyA+3(buffer_top)
     sq decode_s, kRSkyA+4(buffer_top)
     sq material_color, kRSkyA+5(buffer_top)
     ibne desc_phase, vi00, r_decode_d_lid
     r_corner_xy decode_p, desc_cd
     b r_decode_third_lid
r_decode_d_lid:
     r_corner_zw decode_p, desc_cd
     move.x decode_s, desc_v
r_decode_third_lid:
     mr32 pool_uvfar, desc_v
     mr32 pool_uvfar, pool_uvfar
     move.y decode_s, pool_uvfar
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
     isw.w vi00, kRDesc(buffer_top)     ; original source-view OR mask
     isw.w vi00, kRViewLoop(buffer_top) ; 0 sky planes, 1 source-view planes
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

     .include "source_eye_classify.i"
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
     ; Interior pools carry zero source-side OR. The reference's clip_sides
     ; loop performs no work for them; skip 24 empty selector iterations too.
     ilw.x pool_sky_lo, kRMask(buffer_top)
     ilw.y pool_sky_hi, kRMask(buffer_top)
     ior pool_sky_any, pool_sky_lo, pool_sky_hi
     ibeq pool_sky_any, vi00, r_sky_fan_begin_lid
     b r_sky_plane_lid

r_sky_plane_lid:
     ilw.x sky_plane_idx, kRPlaneState(buffer_top)
     ilw.w pool_view_phase, kRViewLoop(buffer_top)
     ibne pool_view_phase, vi00, pool_source_view_plane_lid
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
     b pool_clip_plane_selected_lid
pool_source_view_plane_lid:
     isubiu pool_view_left, sky_plane_idx, 5
     ibgez pool_view_left, r_sky_fan_begin_lid
     iaddiu pool_next_plane, sky_plane_idx, 1
     isw.x pool_next_plane, kRPlaneState(buffer_top)
     iaddiu sky_plane_addr, buffer_top, kRPlanes
     iadd sky_plane_addr, sky_plane_addr, sky_plane_idx
     .include "source_clip_polygon.i"
r_sky_fan_begin_lid:
     ilw.w pool_view_phase, kRViewLoop(buffer_top)
     ibne pool_view_phase, vi00, pool_fan_ready_lid
     ilw.w pool_original_or, kRDesc(buffer_top)
     ibeq pool_original_or, vi00, pool_fan_ready_lid
     iaddiu pool_view_phase, vi00, 1
     isw.w pool_view_phase, kRViewLoop(buffer_top)
     isw.x vi00, kRPlaneState(buffer_top)
     b r_sky_plane_lid
     .include "source_fan_emit.i"
r_next_triangle_lid:
     ilw.z desc_phase, kRDesc(buffer_top)
     ibne desc_phase, vi00, r_next_descriptor_lid
     iaddiu desc_phase, vi00, 1
     isw.z desc_phase, kRDesc(buffer_top)
     b r_decode_lid
r_next_descriptor_lid:
     isw.z vi00, kRDesc(buffer_top)
     ilw.x desc_ptr, kRDesc(buffer_top)
     iaddiu desc_ptr, desc_ptr, 4
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
