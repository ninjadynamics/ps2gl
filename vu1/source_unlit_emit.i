     ; Match the unlit sink after CPU-equivalent polygon clipping. The batch
     ; writer normalizes signed-zero UV through +0 before VU projection.
     .macro pool_emit_corner ptr, off, out, adcreg
     lq pe_eye\@, \off(\ptr)
     move.xyzw pe_batch\@, pe_eye\@
     loi -1.0
     muli.xz pe_batch\@, pe_eye\@, i
     mul_pt_mat_44 pe_clip\@, vert_xform, pe_batch\@
     lq pe_stq\@, \off+1(\ptr)
     add.xy pe_stq\@, pe_stq\@, vf00
     lq pe_rgba\@, \off+2(\ptr)
     div q, vf00[w], pe_clip\@[w]
     addq.x pe_recip\@, vf00, q
     mulx.xyz pe_pos\@, pe_clip\@, pe_recip\@
     add.xyz pe_pos\@, pe_pos\@, gs_offsets
     ftoi4.xyz pe_pos\@, pe_pos\@
     mulx.xyz pe_stq\@, pe_stq\@, pe_recip\@
     sq.xyz pe_stq\@, \out(next_output)
     loi 128.0
     muli.xyzw pe_color\@, pe_rgba\@, i
     loi 255.0
     minii.xyz pe_color\@, pe_color\@, i
     ftoi0.xyzw pe_color\@, pe_color\@
     store_rgba pe_color\@, \out
     fog_coef_alpha pe_fog\@, pe_rgba\@
     ior pe_adc\@, \adcreg, pe_fog\@
     mfir.w pe_pos\@, pe_adc\@
     store_xyzf pe_pos\@, \out
     .endm
