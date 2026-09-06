/* HyperSolar X2D descriptor decoder.
 *
 * This is deliberately a SEPARATE microprogram from general_clip_tri_x2.
 * X2 is uploaded byte-for-byte at PC 0; this decoder is uploaded immediately
 * after it and entered with MSCAL(decoder_pc). It expands four wall
 * descriptors at most, then JR 6 tail-calls X2's guarded main_loop.
 * One MSCAL means TOP/DBF toggles exactly once for the buffer.
 *
 * Register contract: X2's initialization leaves VF01..VF06 live and its
 * checked-in body uses VF00..VF25. Restricting VCL to VF26..VF31 makes it
 * impossible for decoder compilation to corrupt that live state. X2
 * initializes its integer loop state after the jump, so VI10..VI15 are
 * scratch here.
 *
 * Buffer-relative layout (qwords):
 *   0..4     XferBufferHeader (x = expanded vertex count)
 *   5..100   expanded X2 input, 4q/vertex (pos, unused normal, stq, color)
 *   101..112 GEO, 4 descriptors x [ax az bx bz][y0 y1 uL uR][vB ...]
 *   113..124 COL, 4 descriptors x V4-8 [foot][top][pad]
 */

kXdInput           .equ 5
kXdGeo             .equ 101
kXdCol             .equ 113
kX2MainPc          .equ 6

     .init_vf      VF26-VF31
     .init_vi      VI10-VI15

     .name         vsmGeneralClipTriX2DDecode

     --enter
     --endenter

     xtop           VI10                 ; buffer top
     iaddiu         VI11, VI10, kXdInput ; expanded-output cursor
     ilw.x          VI12, 0(VI10)        ; expanded vertex count
     iadd           VI12, VI12, VI12     ; *2
     iadd           VI12, VI12, VI12     ; *4 qwords/vertex
     iadd           VI12, VI11, VI12     ; expanded-output end
     iaddiu         VI13, VI10, kXdGeo
     iaddiu         VI14, VI10, kXdCol

xd_decode_loop_lid:
     isub           VI15, VI12, VI11
     iblez          VI15, xd_decode_done_lid

     lq             VF26, 0(VI13)        ; g0 = ax az bx bz
     lq             VF27, 1(VI13)        ; g1 = y0 y1 uL uR
     lq             VF28, 2(VI13)        ; g2.x = vB

     ; Positions. load_vert consumes xyz; w is harmlessly left at vf00.w.
     move.xyzw      VF29, VF00            ; BL(ax,y0,az)
     addx.x         VF29, VF00, VF26
     addx.y         VF29, VF00, VF27
     addy.z         VF29, VF00, VF26
     sq             VF29, 0(VI11)
     sq             VF29, 12(VI11)

     move.xyzw      VF29, VF00            ; BR(bx,y0,bz)
     addz.x         VF29, VF00, VF26
     addx.y         VF29, VF00, VF27
     addw.z         VF29, VF00, VF26
     sq             VF29, 4(VI11)

     move.xyzw      VF29, VF00            ; TR(bx,y1,bz)
     addz.x         VF29, VF00, VF26
     addy.y         VF29, VF00, VF27
     addw.z         VF29, VF00, VF26
     sq             VF29, 8(VI11)
     sq             VF29, 16(VI11)

     move.xyzw      VF29, VF00            ; TL(ax,y1,az)
     addx.x         VF29, VF00, VF26
     addy.y         VF29, VF00, VF27
     addy.z         VF29, VF00, VF26
     sq             VF29, 20(VI11)

     ; Load-bearing VCL alias fence: UV stores use the same output region
     ; through VI11. Audit the generated VSM, not the presence of this label.
     b              xd_pos_fence_lid
xd_pos_fence_lid:

     sub.xyzw       VF30, VF00, VF00      ; UV template [0,0,1,0]
     addw.z         VF30, VF30, VF00

     move.xyzw      VF29, VF30            ; BL(uL,vB)
     addz.x         VF29, VF00, VF27
     addx.y         VF29, VF00, VF28
     sq             VF29, 2(VI11)
     sq             VF29, 14(VI11)

     move.xyzw      VF29, VF30            ; BR(uR,vB)
     addw.x         VF29, VF00, VF27
     addx.y         VF29, VF00, VF28
     sq             VF29, 6(VI11)

     move.xyzw      VF29, VF30            ; TR(uR,0)
     addw.x         VF29, VF00, VF27
     sq             VF29, 10(VI11)
     sq             VF29, 18(VI11)

     move.xyzw      VF29, VF30            ; TL(uL,0)
     addz.x         VF29, VF00, VF27
     sq             VF29, 22(VI11)

     b              xd_uv_fence_lid
xd_uv_fence_lid:

     ; Packed unsigned bytes -> the exact 0..1 float colors X2 consumes.
     lq             VF30, 0(VI14)         ; foot RGBA
     lq             VF31, 1(VI14)         ; top RGBA
     itof0          VF30, VF30
     itof0          VF31, VF31
     loi            0.003921569
     muli.xyzw      VF30, VF30, i
     muli.xyzw      VF31, VF31, i
     sq             VF30, 3(VI11)
     sq             VF30, 7(VI11)
     sq             VF31, 11(VI11)
     sq             VF30, 15(VI11)
     sq             VF31, 19(VI11)
     sq             VF31, 23(VI11)

     iaddiu         VI11, VI11, 24
     iaddiu         VI13, VI13, 3
     iaddiu         VI14, VI14, 3
     b              xd_decode_loop_lid

xd_decode_done_lid:
     ; Producer/consumer fence: the exact X2 body reads q5..q100 through a
     ; different pointer register. Keep a basic-block boundary before JR.
     b              xd_tail_fence_lid
xd_tail_fence_lid:
     iaddiu         xd_jump, VI00, kX2MainPc

     ; VCL requires an E-bit exit and cannot express an absolute jump into a
     ; separately assembled program. Binding the target keeps the PC-6 load
     ; live; x2d_microcode_guard.py replaces only VCL's E instruction with
     ; JR through this physical output register (the following NOP is its
     ; delay slot), then verifies the complete two-image contract.
     --exit
     out_vi         xd_jump (VI15)
     --endexit

     .END
