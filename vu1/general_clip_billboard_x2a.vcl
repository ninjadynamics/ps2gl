/* HyperSolar X2A: source-clipped billboards with four authored corner alphas.
 * Input: [center.xyz,half][u0,v0,u1,v1][floatRGBA][alphaA,B,C,D], 64B.
 * The EE supplies its existing final radial-haze alpha for each exact source
 * corner. RGB stays uniform; alpha replaces color.w before triangle clipping.
 * ABC and ACD preserve their own original vertices and interpolated alpha.
 * Geometry, source operation order, projection and output match X2B.
 *
 * The compact context remains absolute q49..56, identical to X2B.
 * BASE79/OFFSET472. Relative q0.x count1..24, inputq5..100.
 * q101..105 source view planes; polyA106..201, polyB202..297 (32*3q),
 * control316..323; outputA324..414 (30 vertices), B415..469 (18 vertices),
 * guard470..471. Whole source SH clipping precedes fan triangulation.
 * Native VU rounding and UV signed-zero limits remain those of X2B/X2P.
 */

     #include "vu1_mem_linear.h"
     .include "db_in_db_out.i"
     .include "math.i"
     .include "geometry.i"
     .include "clip_cull.i"

kRPlanes            .equ 101
kRSkyA              .equ 106
kRSkyB              .equ 202
kRDesc              .equ 316
kROut               .equ 317
kRPlaneState        .equ 319
kRFan               .equ 320
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
     .name vsmGeneralClipBillboardX2A
     .include "ground_clip_shared.i"
     .include "source_unlit_emit.i"

kBillboardCount .equ 24
kBillboardWords .equ 4
     .include "source_billboard_begin.i"
r_decode_lid:
     ilw.x desc_ptr, kRDesc(buffer_top)
     ilw.z desc_phase, kRDesc(buffer_top)
     lq xb_center, 0(desc_ptr)
     lq desc_v, 1(desc_ptr)
     lq material_color, 2(desc_ptr)
     lq xa_alphas, 3(desc_ptr)
     mulx.w material_color, ones, xa_alphas
     lq xb_axis_u, 55(vi00)
     lq xb_axis_v, 56(vi00)
     mulw.xyz xb_axis_u, xb_axis_u, xb_center
     mulw.xyz xb_axis_v, xb_axis_v, xb_center
     move.xyzw decode_p, vf00
     sub.xyz decode_p, xb_center, xb_axis_u
     sub.xyz decode_p, decode_p, xb_axis_v
     sq decode_p, kRSkyA(buffer_top)
     sub.xyzw decode_s, vf00, vf00
     mr32.z decode_s, vf00
     move.xy decode_s, desc_v
     sq decode_s, kRSkyA+1(buffer_top)
     sq material_color, kRSkyA+2(buffer_top)
     mr32 xb_uvfar, desc_v
     mr32 xb_uvfar, xb_uvfar
     add.xyz decode_p, xb_center, xb_axis_u
     ibne desc_phase, vi00, xb_decode_ac_lid
     sub.xyz decode_p, decode_p, xb_axis_v
     muly.w material_color, ones, xa_alphas
     move.x decode_s, xb_uvfar
     b xb_decode_second_lid
xb_decode_ac_lid:
     add.xyz decode_p, decode_p, xb_axis_v
     mulz.w material_color, ones, xa_alphas
     move.xy decode_s, xb_uvfar
xb_decode_second_lid:
     sq decode_p, kRSkyA+3(buffer_top)
     sq decode_s, kRSkyA+4(buffer_top)
     sq material_color, kRSkyA+5(buffer_top)
     ibne desc_phase, vi00, xb_decode_d_lid
     add.xyz decode_p, xb_center, xb_axis_u
     mulz.w material_color, ones, xa_alphas
     b xb_decode_third_lid
xb_decode_d_lid:
     sub.xyz decode_p, xb_center, xb_axis_u
     move.w material_color, xa_alphas
     move.x decode_s, desc_v
xb_decode_third_lid:
     add.xyz decode_p, decode_p, xb_axis_v
     move.y decode_s, xb_uvfar
     sq decode_p, kRSkyA+6(buffer_top)
     sq decode_s, kRSkyA+7(buffer_top)
     sq material_color, kRSkyA+8(buffer_top)
     iaddiu setup_mask, vi00, 31
     isw.x setup_mask, kRFan(buffer_top)
     isw.w vi00, kRDesc(buffer_top)
     iaddiu vertex_ptr, buffer_top, kRSkyA
     iaddiu vertex_left, vi00, 3
     b r_classify_lid

     .include "source_billboard_end.i"
