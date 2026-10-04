/* X2S decoder: raw X2 triangles with the building haze evaluated here.
 * Assembled separately from the unchanged X2 body, which it enters at its
 * main loop after rewriting each input vertex's alpha.
 * Buffer-relative layout:
 *   0        header (x = vertex count, at most 30)
 *   1        haze: [cx 0 cz 0]              (radial center)
 *   2        haze: [1/R^2, alpha, t0, -1/H] (t = t0 - y/H; t0 = 1 + ground/H)
 *   5..124   X2 input (pos, unused normal, STQ, float RGBA), four qwords each
 * X2 never reads header q1..4, so the two haze qwords ride there, written by
 * the EE after every buffer header.
 * The closed-form law per vertex, in its own source frame:
 *   at = min(((x-cx)^2 + (z-cz)^2) / R^2, 1)      q = alpha * at^2
 *   t  = clamp(t0 - y/H, 0, 1)                    v = t * sqrt(t)
 *   keep = 1 - q*(q + v*(1 - q))
 * The keep replaces color.w in place; RGB, position and STQ are untouched.
 * One SQRT and its one MULQ per loop pass.
 * VF01..VF06 remain live from X2's prologue; use only VF26..VF31.
 */
kXsInput .equ 5
kX2MainPc .equ 6

     .init_vf VF26-VF31
     .init_vi VI10-VI15
     .name vsmGeneralClipTriX2SDecode
     --enter
     --endenter

     xtop VI10
     iaddiu VI11, VI10, kXsInput
     ilw.x VI12, 0(VI10)
     iadd VI12, VI12, VI12
     iadd VI12, VI12, VI12
     iadd VI12, VI11, VI12

xs_decode_loop_lid:
     isub VI15, VI12, VI11
     iblez VI15, xs_decode_done_lid

     lq VF26, 0(VI11)       ; x y z
     lq VF27, 1(VI10)       ; cx 0 cz 0
     lq VF28, 2(VI10)       ; 1/R^2 alpha t0 -1/H
     sub.xyz VF27, VF26, VF27
     mul.xz VF27, VF27, VF27
     addz.x VF27, VF27, VF27        ; distance^2
     mulx.x VF27, VF27, VF28        ; at
     miniw.x VF27, VF27, VF00       ; at <= 1
     mul.x VF27, VF27, VF27
     muly.x VF27, VF27, VF28        ; q = alpha*at^2

     mulw.y VF29, VF26, VF28
     addz.y VF29, VF29, VF28        ; t = t0 - y/H
     maxx.y VF29, VF29, VF00        ; t >= 0
     miniw.y VF29, VF29, VF00       ; t <= 1
     sqrt q, VF29[y]
     mulq.y VF29, VF29, q           ; v = t*sqrt(t)
     mr32.x VF29, VF29              ; v into lane x

     addw.x VF30, VF00, VF00        ; one
     sub.x VF31, VF30, VF27         ; 1 - q
     mul.x VF29, VF29, VF31
     add.x VF29, VF29, VF27         ; q + v*(1 - q)
     mul.x VF29, VF29, VF27
     sub.x VF29, VF30, VF29         ; keep

     b xs_fog_fence_lid
xs_fog_fence_lid:
     lq VF26, 3(VI11)       ; RGB, -
     mulx.w VF26, VF00, VF29        ; a = keep
     sq VF26, 3(VI11)

     iaddiu VI11, VI11, 4
     b xs_decode_loop_lid

xs_decode_done_lid:
     b xs_tail_fence_lid
xs_tail_fence_lid:
     iaddiu xs_jump, VI00, kX2MainPc
     --exit
     out_vi xs_jump (VI15)
     --endexit
     .END
