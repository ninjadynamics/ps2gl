/* X2V decoder: X2C's wall descriptors with the building haze evaluated here.
 * Assembled separately from the unchanged X2 body; four descriptors expand to
 * the same 24-vertex activation as X2C.
 * Buffer-relative layout:
 *   0        header (x = expanded vertex count)
 *   1        haze: [cx cz cx cz]            (radial center, twice)
 *   2        haze: [1/R^2, alpha, t0, -1/H] (t = t0 - y/H; t0 = 1 + ground/H)
 *   5..100   expanded X2 input (pos, unused normal, STQ, float RGBA)
 *   101..116 GEO: four descriptors x four qwords
 *   117..124 COL: four descriptors x two qwords [bottom RGB, -] [top RGB, -]
 * Neither X2 nor any decoder reads header q1..4, so the two haze qwords ride
 * there, written by the EE with every buffer header.
 * The closed-form law per corner, in the wall's own source frame:
 *   at = min(((x-cx)^2 + (z-cz)^2) / R^2, 1)      q = alpha * at^2
 *   t  = clamp(t0 - y/H, 0, 1)                    v = t * sqrt(t)
 *   keep = 1 - q*(q + v*(1 - q))
 * A/D share X/Z and so do B/C: two radial values, four heights. Each SQRT
 * has exactly one MULQ consumer, chained by data flow so that no other root
 * can be in flight; the guard rejects any schedule that breaks this.
 * The decoder consumes every source before X2 writes its plane/poly scratch
 * at125+. No source overlaps the expanded destination or window staging178.
 * VF01..VF06 remain live from X2's prologue; use only VF26..VF31.
 * Geometry, UV and RGB are bit copies (MOVE/MR32); only the fog lane is
 * arithmetic.
 */
kXvInput .equ 5
kXvGeo .equ 101
kXvCol .equ 117
kX2MainPc .equ 6

     .init_vf VF26-VF31
     .init_vi VI10-VI15
     .name vsmGeneralClipTriX2VDecode
     --enter
     --endenter

     xtop VI10
     iaddiu VI11, VI10, kXvInput
     ilw.x VI12, 0(VI10)
     iadd VI12, VI12, VI12
     iadd VI12, VI12, VI12
     iadd VI12, VI11, VI12
     iaddiu VI13, VI10, kXvGeo
     iaddiu VI14, VI10, kXvCol

xv_decode_loop_lid:
     isub VI15, VI12, VI11
     iblez VI15, xv_decode_done_lid

     lq VF26, 0(VI13)       ; Ax Az Bx Bz
     lq VF27, 1(VI13)       ; Ay By Cy Dy
     move.xyzw VF30, VF00
     move.x VF30, VF26      ; A.x
     mr32 VF28, VF26
     mr32 VF28, VF28
     mr32 VF28, VF28
     move.z VF30, VF28      ; A.z
     mr32 VF29, VF27
     mr32 VF29, VF29
     mr32 VF29, VF29
     move.y VF30, VF29      ; A.y
     sq VF30, 0(VI11)
     sq VF30, 12(VI11)

     mr32 VF28, VF26
     mr32 VF28, VF28
     move.x VF30, VF28      ; B.x
     mr32 VF28, VF26
     move.z VF30, VF28      ; B.z
     move.y VF30, VF27      ; B.y
     sq VF30, 4(VI11)

     mr32 VF29, VF27
     move.y VF30, VF29      ; C.y (same X/Z as B)
     sq VF30, 8(VI11)
     sq VF30, 16(VI11)

     move.x VF30, VF26
     mr32 VF28, VF26
     mr32 VF28, VF28
     mr32 VF28, VF28
     move.z VF30, VF28
     mr32 VF29, VF27
     mr32 VF29, VF29
     move.y VF30, VF29      ; D.y (same X/Z as A)
     sq VF30, 20(VI11)

     b xv_pos_fence_lid
xv_pos_fence_lid:
     lq VF26, 2(VI13)       ; uA vA uB vB
     lq VF27, 3(VI13)       ; uC vC uD vD
     sub.xyzw VF30, VF00, VF00
     mr32.z VF30, VF00      ; [0,0,1,0] STQ template
     move.xy VF30, VF26
     sq VF30, 2(VI11)
     sq VF30, 14(VI11)
     mr32 VF28, VF26
     mr32 VF28, VF28
     move.xy VF30, VF28
     sq VF30, 6(VI11)
     move.xy VF30, VF27
     sq VF30, 10(VI11)
     sq VF30, 18(VI11)
     mr32 VF28, VF27
     mr32 VF28, VF28
     move.xy VF30, VF28
     sq VF30, 22(VI11)

     b xv_uv_fence_lid
xv_uv_fence_lid:
     lq VF26, 0(VI13)       ; Ax Az Bx Bz
     lq VF27, 1(VI10)       ; cx cz cx cz
     lq VF28, 2(VI10)       ; 1/R^2 alpha t0 -1/H
     sub.xyzw VF26, VF26, VF27
     mul.xyzw VF26, VF26, VF26
     mr32.xyzw VF27, VF26
     add.xyzw VF26, VF26, VF27      ; x = A distance^2, z = B distance^2
     mulx.xyzw VF26, VF26, VF28     ; at
     miniw.xyzw VF26, VF26, VF00    ; at <= 1
     mul.xyzw VF26, VF26, VF26
     muly.xyzw VF26, VF26, VF28     ; q = alpha*at^2 (x = A, z = B)
     mr32.xyzw VF27, VF26
     move.yw VF26, VF27             ; q: A B B A, in corner order A B C D

     lq VF27, 1(VI13)       ; Ay By Cy Dy
     mulw.xyzw VF27, VF27, VF28
     addz.xyzw VF27, VF27, VF28     ; t = t0 - y/H
     maxx.xyzw VF27, VF27, VF00     ; t >= 0
     miniw.xyzw VF27, VF27, VF00    ; t <= 1
     ; v = t*sqrt(t), one lane at a time through lane x. Each MR32 reads the
     ; lane the MULQ just wrote, so the next SQRT cannot be scheduled before
     ; the previous root is consumed: Q has no interlock, and vcl otherwise
     ; interleaves the four roots and leans on stall timing for each MULQ to
     ; see its own Q (the neighbor-Q hazard noted in general_clip_tri.vcl).
     sqrt q, VF27[x]
     mulq.x VF27, VF27, q
     mr32.xyzw VF27, VF27
     sqrt q, VF27[x]
     mulq.x VF27, VF27, q
     mr32.xyzw VF27, VF27
     sqrt q, VF27[x]
     mulq.x VF27, VF27, q
     mr32.xyzw VF27, VF27
     sqrt q, VF27[x]
     mulq.x VF27, VF27, q
     mr32.xyzw VF29, VF27           ; v for A B C D

     maxw.xyzw VF30, VF00, VF00     ; ones
     sub.xyzw VF28, VF30, VF26      ; 1 - q
     mul.xyzw VF29, VF29, VF28
     add.xyzw VF29, VF29, VF26      ; q + v*(1 - q)
     mul.xyzw VF29, VF29, VF26
     sub.xyzw VF31, VF30, VF29      ; keep: fogA fogB fogC fogD

     b xv_fog_fence_lid
xv_fog_fence_lid:
     lq VF26, 0(VI14)       ; bottom RGB
     lq VF27, 1(VI14)       ; top RGB
     mr32 VF30, VF31        ; w = fogA
     move.w VF26, VF30      ; A: bottom RGB, fogA
     mr32 VF30, VF30        ; w = fogB
     move.xyzw VF28, VF26
     move.w VF28, VF30      ; B: bottom RGB, fogB
     mr32 VF30, VF30        ; w = fogC
     move.w VF27, VF30      ; C: top RGB, fogC
     move.xyzw VF29, VF27
     move.w VF29, VF31      ; D: top RGB, fogD
     sq VF26, 3(VI11)
     sq VF28, 7(VI11)
     sq VF27, 11(VI11)
     sq VF26, 15(VI11)
     sq VF27, 19(VI11)
     sq VF29, 23(VI11)

     iaddiu VI11, VI11, 24
     iaddiu VI13, VI13, 4
     iaddiu VI14, VI14, 2
     b xv_decode_loop_lid

xv_decode_done_lid:
     b xv_tail_fence_lid
xv_tail_fence_lid:
     iaddiu xv_jump, VI00, kX2MainPc
     --exit
     out_vi xv_jump (VI15)
     --endexit
     .END
