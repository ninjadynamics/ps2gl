/* Exact-corner X2Q decoder, assembled separately from the unchanged X2 body.
 * Four descriptors expand to the same 24-vertex activation as X2/X2D.
 * Buffer-relative layout:
 *   0..4     header (x = expanded vertex count)
 *   5..100   expanded X2 input (pos, unused normal, STQ, float RGBA)
 *   101..116 GEO: four descriptors x four qwords
 *   117..132 COL: four descriptors x four float RGBA qwords
 * The decoder consumes every source before X2 writes its plane/poly scratch
 * at125+. No source overlaps the expanded destination or window staging178.
 * VF01..VF06 remain live from X2's prologue; use only VF26..VF31.
 * Geometry and UV are bit copies (MOVE/MR32), not arithmetic reconstruction.
 * Float colors preserve the EE normalization and interpolated fog exactly;
 * VU byte normalization can differ by one ULP because VU rounds toward zero.
 */
kXqInput .equ 5
kXqGeo .equ 101
kXqCol .equ 117
kX2MainPc .equ 6

     .init_vf VF26-VF31
     .init_vi VI10-VI15
     .name vsmGeneralClipTriX2QDecode
     --enter
     --endenter

     xtop VI10
     iaddiu VI11, VI10, kXqInput
     ilw.x VI12, 0(VI10)
     iadd VI12, VI12, VI12
     iadd VI12, VI12, VI12
     iadd VI12, VI11, VI12
     iaddiu VI13, VI10, kXqGeo
     iaddiu VI14, VI10, kXqCol

xq_decode_loop_lid:
     isub VI15, VI12, VI11
     iblez VI15, xq_decode_done_lid

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

     b xq_pos_fence_lid
xq_pos_fence_lid:
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

     b xq_uv_fence_lid
xq_uv_fence_lid:
     lq VF26, 0(VI14)
     lq VF27, 1(VI14)
     lq VF28, 2(VI14)
     lq VF29, 3(VI14)
     sq VF26, 3(VI11)
     sq VF27, 7(VI11)
     sq VF28, 11(VI11)
     sq VF26, 15(VI11)
     sq VF28, 19(VI11)
     sq VF29, 23(VI11)

     iaddiu VI11, VI11, 24
     iaddiu VI13, VI13, 4
     iaddiu VI14, VI14, 4
     b xq_decode_loop_lid

xq_decode_done_lid:
     b xq_tail_fence_lid
xq_tail_fence_lid:
     iaddiu xq_jump, VI00, kX2MainPc
     --exit
     out_vi xq_jump (VI15)
     --endexit
     .END
