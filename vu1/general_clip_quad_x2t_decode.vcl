/* X2T decoder: X2F source quads with the building haze evaluated here.
 * Assembled separately from the unchanged X2F program, which it enters at
 * its main loop after rewriting each input corner's alpha.
 * Buffer-relative layout:
 *   0        header (x = corner count, at most 40)
 *   1        haze: [cx s cz m]              (radial center; alpha mix)
 *   2        haze: [1/R^2, alpha, t0, -1/H] (t = t0 - y/H; t0 = 1 + ground/H)
 *   5..124   X2F input (pos, STQ, float RGBA), three qwords a corner
 * X2F never reads header q1..4, so the two haze qwords ride there, written by
 * the EE after every buffer header. Positions are the caller's (eye-relative)
 * source frame, so the EE states the center and ground in that frame.
 * The closed-form law per corner:
 *   at = min(((x-cx)^2 + (z-cz)^2) / R^2, 1)      q = alpha * at^2
 *   t  = clamp(t0 - y/H, 0, 1)                    v = t * sqrt(t)
 *   keep = 1 - q*(q + v*(1 - q))
 *   color.w = keep * (s + m*color.w)
 * (s, m) = (1, 0) replaces alpha with the keep (GS fog); (0, 1) scales the
 * source alpha by it (additive attenuation). RGB, position and STQ are
 * untouched. One SQRT and its one MULQ per loop pass.
 * VF01..VF05 remain live from X2F's prologue and X2F never uses VF26..VF31;
 * use only those.
 */
kXtInput .equ 5
kX2fMainPc .equ 6

     .init_vf VF26-VF31
     .init_vi VI10-VI15
     .name vsmGeneralClipQuadX2TDecode
     --enter
     --endenter

     xtop VI10
     iaddiu VI11, VI10, kXtInput
     ilw.x VI12, 0(VI10)
     iadd VI13, VI12, VI12
     iadd VI12, VI13, VI12
     iadd VI12, VI11, VI12

xt_decode_loop_lid:
     isub VI15, VI12, VI11
     iblez VI15, xt_decode_done_lid

     lq VF26, 0(VI11)       ; x y z
     lq VF27, 1(VI10)       ; cx s cz m
     lq VF28, 2(VI10)       ; 1/R^2 alpha t0 -1/H
     sub.xz VF31, VF26, VF27
     mul.xz VF31, VF31, VF31
     addz.x VF31, VF31, VF31        ; distance^2
     mulx.x VF31, VF31, VF28        ; at
     miniw.x VF31, VF31, VF00       ; at <= 1
     mul.x VF31, VF31, VF31
     muly.x VF31, VF31, VF28        ; q = alpha*at^2

     mulw.y VF29, VF26, VF28
     addz.y VF29, VF29, VF28        ; t = t0 - y/H
     maxx.y VF29, VF29, VF00        ; t >= 0
     miniw.y VF29, VF29, VF00       ; t <= 1
     sqrt q, VF29[y]
     mulq.y VF29, VF29, q           ; v = t*sqrt(t)
     mr32.x VF29, VF29              ; v into lane x

     addw.x VF30, VF00, VF00        ; one
     sub.x VF28, VF30, VF31         ; 1 - q
     mul.x VF29, VF29, VF28
     add.x VF29, VF29, VF31         ; q + v*(1 - q)
     mul.x VF29, VF29, VF31
     sub.x VF29, VF30, VF29         ; keep

     b xt_fog_fence_lid
xt_fog_fence_lid:
     lq VF26, 2(VI11)       ; RGBA
     mulw.w VF30, VF26, VF27        ; m*a
     addy.w VF30, VF30, VF27        ; s + m*a
     mulx.w VF26, VF30, VF29        ; times keep
     sq VF26, 2(VI11)

     iaddiu VI11, VI11, 3
     b xt_decode_loop_lid

xt_decode_done_lid:
     b xt_tail_fence_lid
xt_tail_fence_lid:
     iaddiu xt_jump, VI00, kX2fMainPc
     --exit
     out_vi xt_jump (VI15)
     --endexit
     .END
