/* HyperSolar X2B: compact billboard source-view clipping before triangulation.
 * Input: [center.xyz,half][u0,v0,u1,v1][floatRGBA], 48B per quad.
 * Batch axes reconstruct each original world corner as
 * (center +/- axisU*half) +/- axisV*half, then subtract eye and apply the
 * ordered source dot products. No center/extent transform reassociation.
 * Admission: finite homogeneous billboard records, no source radial haze,
 * no original ABC/ACD triangle entirely on the source floor when bounded.
 * This is the same unlit source-SH view clipper as X2P, with no sky pass.
 * Native VU rounding can differ from EE despite matching operation order.
 *
 * Absolute q49 right,50 up,51 forward,52 eye.xyz/0,
 * q53 0/0/px/py,54 sourceNear/NDC/epsilon/0,55 axisU,56 axisV.
 * Standard q0/57/62..65/75..78 are the normal unlit context; q62..65 is
 * GSScale*eye-space projection with no model/view transform.
 * BASE79/OFFSET472. Relative q0.x count1..32, inputq5..100.
 * q101..105 source view planes; polyA106..201, polyB202..297 (32*3q),
 * private four-corner cache298..309, outcodes310..313;
 * control316..323; outputA324..414 (30 vertices), B415..469 (18 vertices),
 * guard470..471. Complete-triangle spill preserves input/material order.
 */

/* q54.w is the private PGL_CITY_BILLBOARD_CORNER_REUSE option. Zero keeps
 * the original six-corner path; nonzero computes ABCD once, then clips the
 * original ABC and ACD independently. The cache never aliases polygons/GIF. */

     #include "vu1_mem_linear.h"
     .include "db_in_db_out.i"
     .include "math.i"
     .include "geometry.i"
     .include "clip_cull.i"

kRPlanes            .equ 101
kRSkyA              .equ 106
kRSkyB              .equ 202
kBCache             .equ 298
kBCodes             .equ 310
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
     .name vsmGeneralClipBillboardX2B
     .include "ground_clip_shared.i"
     .include "source_unlit_emit.i"

kBillboardCount .equ 32
kBillboardWords .equ 3
     .include "source_billboard_begin.i"
r_decode_lid:
     ilw.w bc_enabled, 54(vi00)
     ibne bc_enabled, vi00, bc_decode_lid
     ilw.x desc_ptr, kRDesc(buffer_top)
     ilw.z desc_phase, kRDesc(buffer_top)
     lq xb_center, 0(desc_ptr)
     lq desc_v, 1(desc_ptr)
     lq material_color, 2(desc_ptr)
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
     move.x decode_s, xb_uvfar
     b xb_decode_second_lid
xb_decode_ac_lid:
     add.xyz decode_p, decode_p, xb_axis_v
     move.xy decode_s, xb_uvfar
xb_decode_second_lid:
     sq decode_p, kRSkyA+3(buffer_top)
     sq decode_s, kRSkyA+4(buffer_top)
     sq material_color, kRSkyA+5(buffer_top)
     ibne desc_phase, vi00, xb_decode_d_lid
     add.xyz decode_p, xb_center, xb_axis_u
     b xb_decode_third_lid
xb_decode_d_lid:
     sub.xyz decode_p, xb_center, xb_axis_u
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

     .macro bc_load_alpha
     .endm
     .macro bc_alpha_a
     .endm
     .macro bc_alpha_b
     .endm
     .macro bc_alpha_c
     .endm
     .macro bc_alpha_d
     .endm

     .include "source_billboard_cached_decode.i"
     .include "source_billboard_cached_classify.i"
     .include "source_billboard_end.i"
