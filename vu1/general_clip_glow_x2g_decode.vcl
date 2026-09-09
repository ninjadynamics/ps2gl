/* Compact glow decoder; separately assembled, NEVER regenerate the X2 body.
 * Four glows expand to the same 24-vertex X2 activation. Absolute context:
 *   q58 axis_u.xyz,0; q59 axis_v.xyz,0 (owned/restored by X2G InitContext).
 * Buffer-relative layout:
 *   0..4 header, 5..100 expanded X2 pos/hole/STQ/RGBA;
 *   101..108 GEO: [center.xyz,half][u0,v0,u1,v1] per glow;
 *   109..116 COL: [normalized RGBA][reserved] per glow.
 * Four original world corners are (center +/- axis_u*half) +/- axis_v*half
 * in that exact order. Output is A,B,C,A,C,D; Q=1, position W=1.
 * Float RGBA copies preserve caller opacity. Caller disables GS fog; this
 * avoids assigning X2's simultaneous XYZF fog byte any material meaning.
 * The main admission retains the old path for radial haze, floor-boundary
 * intersections and near/side uncertainty. The X2 body still protects GS.
 * VU rounding differs from EE scalar rounding: operation order is retained,
 * but this decoder is not a promise of bit-identical projected coordinates.
 * X2 prologue keeps VF01..VF06 live. Use only VF26..VF31 and VI10..VI15.
 */
kXgInput .equ 5
kXgGeo .equ 101
kXgCol .equ 109
kXgAxes .equ 58
kX2MainPc .equ 6

     .init_vf VF26-VF31
     .init_vi VI10-VI15
     .name vsmGeneralClipGlowX2GDecode
     --enter
     --endenter

     xtop VI10
     iaddiu VI11, VI10, kXgInput
     ilw.x VI12, 0(VI10)
     iadd VI12, VI12, VI12
     iadd VI12, VI12, VI12
     iadd VI12, VI11, VI12
     iaddiu VI13, VI10, kXgGeo
     iaddiu VI14, VI10, kXgCol

xg_decode_loop_lid:
     isub VI15, VI12, VI11
     iblez VI15, xg_decode_done_lid

     lq VF26, 0(VI13)       ; center.xyz,half
     lq VF27, kXgAxes(VI00)
     lq VF28, kXgAxes+1(VI00)
     mulw.xyz VF27, VF27, VF26
     mulw.xyz VF28, VF28, VF26
     move.xyzw VF29, VF00
     sub.xyz VF29, VF26, VF27
     sub.xyz VF29, VF29, VF28       ; A: (center - a) - b
     sq VF29, 0(VI11)
     sq VF29, 12(VI11)
     add.xyz VF29, VF26, VF27
     sub.xyz VF29, VF29, VF28       ; B: (center + a) - b
     sq VF29, 4(VI11)
     add.xyz VF29, VF26, VF27
     add.xyz VF29, VF29, VF28       ; C: (center + a) + b
     sq VF29, 8(VI11)
     sq VF29, 16(VI11)
     sub.xyz VF29, VF26, VF27
     add.xyz VF29, VF29, VF28       ; D: (center - a) + b
     sq VF29, 20(VI11)

     b xg_pos_fence_lid
xg_pos_fence_lid:
     lq VF26, 1(VI13)       ; u0,v0,u1,v1
     sub.xyzw VF30, VF00, VF00
     mr32.z VF30, VF00      ; [0,0,1,0] STQ template
     move.xy VF30, VF26
     sq VF30, 2(VI11)
     sq VF30, 14(VI11)
     mr32 VF28, VF26
     mr32 VF28, VF28
     move.x VF30, VF28
     sq VF30, 6(VI11)
     move.y VF30, VF28
     sq VF30, 10(VI11)
     sq VF30, 18(VI11)
     move.x VF30, VF26
     sq VF30, 22(VI11)

     b xg_uv_fence_lid
xg_uv_fence_lid:
     lq VF26, 0(VI14)       ; one exact float color, second qword reserved
     sq VF26, 3(VI11)
     sq VF26, 7(VI11)
     sq VF26, 11(VI11)
     sq VF26, 15(VI11)
     sq VF26, 19(VI11)
     sq VF26, 23(VI11)
     iaddiu VI11, VI11, 24
     iaddiu VI13, VI13, 2
     iaddiu VI14, VI14, 2
     b xg_decode_loop_lid

xg_decode_done_lid:
     b xg_tail_fence_lid
xg_tail_fence_lid:
     iaddiu xg_jump, VI00, kX2MainPc
     --exit
     out_vi xg_jump (VI15)
     --endexit
     .END
