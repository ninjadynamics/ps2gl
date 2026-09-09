/* HyperSolar unlit textured RGBA triangles, already clipped on the EE.
 * Original linear ABI: 4 input qwords [XYZ, unused normal, STQ, RGBA],
 * 3 output qwords [STQ, RGBAQ, XYZF2]. No light or material reads.
 * Source RGBA is float0..1. Texture modulation uses128 and classic RGB
 * clamp/truncation; source alpha also supplies the optional GS fog factor.
 */
     #include       "vu1_mem_linear.h"
     .include       "db_in_db_out.i"
     .include       "math.i"
     .include       "clip_cull.i"
     .include       "geometry.i"
     .include       "io.i"

kInputQPerV         .equ 4
kOutputQPerV        .equ 3
     .init_vf_all
     .init_vi_all
     .name          vsmGeneralUnlitTexTri

     ; One explicitly landed reciprocal feeds position AND texture. VCL must
     ; not schedule a second Q consumer against the following vertex's DIV.
     .macro         unlit_vert off, dst, projected, gsvert, fog
     load_vert      uvp\@, \off
     mul_pt_mat_44  raw\@, vert_xform, uvp\@
     div            q, vf00w, raw\@[w]
     addq.x         reciprocal\@, vf00, q
     mulx.xyz       \projected, raw\@, reciprocal\@
     vert_to_gs     \gsvert, \projected
     load_stq       tex\@, \off
     mulx.xyz       tex\@, tex\@, reciprocal\@
     store_stq      tex\@, \dst
     lq             rgba\@, 3+\off(next_input)
     loi            128.0
     muli           scaled\@, rgba\@, i
     loi            255.0
     minii.xyz      scaled\@, scaled\@, i
     ftoi0          scaled\@, scaled\@
     store_rgba     scaled\@, \dst
     fog_coef_alpha \fog, rgba\@
     clip_vert      \projected
     .endm

     --enter
     --endenter
     load_vert_xfrm vert_xform
     --cont

main_loop_lid:
     init_constants
     init_clip_cnst
     init_io_loop
     init_bfc
     iaddiu         adc_bit, vi00, 0x7fff
     iaddiu         adc_bit, adc_bit, 1
     iaddiu         emitted, vi00, 0
     sub           zero_vec, vf00, vf00

triangle_loop_lid:
     isub           remain, last_input, next_input
     isubiu         remain, remain, 11
     iblez          remain, done_lid

     unlit_vert     0, 0, p0, g0, f0
     ior            f0, f0, adc_bit
     mfir.w         g0, f0
     store_xyzf     g0, 0
     unlit_vert     4, 3, p1, g1, f1
     ior            f1, f1, adc_bit
     mfir.w         g1, f1
     store_xyzf     g1, 3
     unlit_vert     8, 6, p2, g2, f2

     ; Same three-vertex GS guard verdict as classic PV triangles. Read it
     ; BEFORE reusing CLIP flags for the winding test below.
     fcand          vi01, 0x03ffff
     iand           guard, vi01, do_clipping
     sub.xyz        edge0, p0, p1
     sub.xyz        edge1, p2, p1
     mulw.xyz       edge0, edge0, bfc_multiplier
     opmula.xyz     acc, edge0, edge1
     opmsub.xyz     face, edge1, edge0
     ; CLIP compares z against -abs(0): exact negative sign without VCL's
     ; unsafe fmand/ABS regeneration or quantizing a tiny signed area to zero.
     clipw.xyz      face, zero_vec[w]
     fcand          vi01, 0x20
     isub           back, vi00, vi01
     iaddiu         enabled, vi00, 0x20
     iand           enabled, enabled, z_sign_mask
     iand           back, back, enabled
     ior            reject, guard, back
     iaddiu         reject, reject, 0x7fff
     iand           reject, reject, adc_bit
     ior            reject, reject, f2
     mfir.w         g2, reject
     store_xyzf     g2, 6

     next_io        3
     iaddiu         emitted, emitted, 3
     b              triangle_loop_lid

done_lid:
     ; VU-written output tag and exact count, including malformed short tails.
     ; Kicked memory is never VIF-written and keeps the stock double buffering.
     lq             tag, kGifTag(vi00)
     mtir           taglow, tag[x]
     ior            taglow, taglow, emitted
     mfir.x         tag, taglow
     iaddiu         tagaddr, buffer_top, kOutputStart
     sq             tag, 0(tagaddr)
     kick_to_gs
     --cont
     b              main_loop_lid
.END
