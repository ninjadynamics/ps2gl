     ; Keep each exceptional branch local: one distant shared trap could
     ; exceed the signed branch displacement after five clip-pass expansions.
     .macro r_fail_stop
     iaddiu contract_error\@, vi00, 1
     isw.x contract_error\@, kRFailure(buffer_top)
     b r_contract_stop_lid\@
r_contract_stop_lid\@:
     b r_contract_stop_lid\@
     .endm

     ; Sign of d.x, exactly negative for the represented distance, including
     ; (-1/16,0). Zero/negative-zero are inside. Other clip lanes are zero.
     .macro r_negative result, d
     sub.xyzw rn_vec\@, vf00, vf00
     move.x rn_vec\@, \d
     clipw.xyz rn_vec\@, rn_vec\@[w]
     fcand vi01, 0x2
     iadd \result, vi01, vi00
     .endm

     ; Explicit EE operation order: ((A*x+B*y)+C*z)+D.
     .macro r_sky_distance distance, vertex, coeffs
     mul.xyz rs_terms\@, \vertex, \coeffs
     addy.x \distance, rs_terms\@, rs_terms\@
     addz.x \distance, \distance, rs_terms\@
     addw.x \distance, \distance, \coeffs
     .endm

     ; Copy actual XZ float bits. MR32 avoids arithmetic corner reconstruction
     ; and avoids adding zero to a possibly signed-zero authored coordinate.
     .macro r_corner_xy output, coords
     move.xyzw \output, vf00
     move.y \output, material_height
     move.x \output, \coords
     mr32 rc_rot\@, \coords
     mr32 rc_rot\@, rc_rot\@
     mr32 rc_rot\@, rc_rot\@
     move.z \output, rc_rot\@
     .endm

     .macro r_corner_zw output, coords
     move.xyzw \output, vf00
     move.y \output, material_height
     mr32 rc_rot\@, \coords
     move.z \output, rc_rot\@
     mr32 rc_rot\@, rc_rot\@
     move.x \output, rc_rot\@
     .endm

     ; View-stage distance/sign/edge order deliberately retain existing X2,
     ; including its FTOI4 near-zero sign tolerance and first->next anchor.
     .macro pd_plane d, v, pl
     mul.xy pdt\@, \pl, \v
     mulw.z pdt\@, \pl, \v
     adday.x acc, pdt\@, pdt\@
     maddz.x pdt\@, ones, pdt\@
     addw.x \d, pdt\@, \pl
     .endm

     .macro pd_sign f, d
     loi 2047.0
     minii.x pds\@, \d, i
     loi -2047.0
     maxi.x pds\@, pds\@, i
     ftoi4.x pds\@, pds\@
     mtir \f, pds\@[x]
     .endm

     .macro cp_store p, s, c
     isubiu cp_bound\@, cp_m, 8
     ibltz cp_bound\@, cp_store_ok\@
     r_fail_stop
cp_store_ok\@:
     sq \p, 0(cp_dst)
     sq \s, 1(cp_dst)
     sq \c, 2(cp_dst)
     iaddiu cp_dst, cp_dst, 3
     iaddiu cp_m, cp_m, 1
     .endm

     .macro cp_edge pa, sa, ca, da, fa, pb, sb, cb, db, fb
     iand ce_sa\@, \fa, adc_bit
     ibne ce_sa\@, vi00, ce_askip\@
     cp_store \pa, \sa, \ca
ce_askip\@:
     iand ce_sb\@, \fb, adc_bit
     ibeq ce_sa\@, ce_sb\@, ce_nocross\@
     sub.x ce_den\@, \da, \db
     div q, \da[x], ce_den\@[x]
     addq.x ce_q\@, vf00, q
     sub.xyzw ce_e\@, \pb, \pa
     mulaw.xyzw acc, \pa, vf00[w]
     maddx.xyzw ce_ip\@, ce_e\@, ce_q\@
     sub.xyz ce_es\@, \sb, \sa
     mulaw.xyz acc, \sa, vf00[w]
     maddx.xyz ce_is\@, ce_es\@, ce_q\@
     mulx.w ce_is\@, vf00, vf00
     sub.xyzw ce_ec\@, \cb, \ca
     mulaw.xyzw acc, \ca, vf00[w]
     maddx.xyzw ce_ic\@, ce_ec\@, ce_q\@
     cp_store ce_ip\@, ce_is\@, ce_ic\@
ce_nocross\@:
     .endm

     .macro clip_pass
     b cp_fence_lid\@
cp_fence_lid\@:
     ilw.z cp_plane_addr, kRViewLoop(buffer_top)
     lq cp_pl, 0(cp_plane_addr)
     ilw.x cp_src, kRViewLoop(buffer_top)
     ilw.y cp_dst, kRViewLoop(buffer_top)
     iaddiu cp_m, vi00, 0
     lq cp_pf, 0(cp_src)
     lq cp_sf, 1(cp_src)
     lq cp_cf, 2(cp_src)
     pd_plane cp_df, cp_pf, cp_pl
     pd_sign cp_ff, cp_df
     move.xyzw cp_pp, cp_pf
     move.xyzw cp_sp, cp_sf
     move.xyzw cp_cp, cp_cf
     move.x cp_dp, cp_df
     iadd cp_fp, cp_ff, vi00
     iaddiu cp_i, vi00, 1
     iaddiu cp_src, cp_src, 3
cp_loop_lid\@:
     ibeq cp_i, sh_n, cp_wrap_lid\@
     lq cp_pc, 0(cp_src)
     lq cp_sc, 1(cp_src)
     lq cp_cc, 2(cp_src)
     pd_plane cp_dc, cp_pc, cp_pl
     pd_sign cp_fc, cp_dc
     cp_edge cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pc, cp_sc, cp_cc, cp_dc, cp_fc
     move.xyzw cp_pp, cp_pc
     move.xyzw cp_sp, cp_sc
     move.xyzw cp_cp, cp_cc
     move.x cp_dp, cp_dc
     iadd cp_fp, cp_fc, vi00
     iaddiu cp_src, cp_src, 3
     iaddiu cp_i, cp_i, 1
     b cp_loop_lid\@
cp_wrap_lid\@:
     cp_edge cp_pp, cp_sp, cp_cp, cp_dp, cp_fp, cp_pf, cp_sf, cp_cf, cp_df, cp_ff
     iadd sh_n, cp_m, vi00
     .endm

     .macro fmt_color dst, src
     loi 128.0
     muli.xyzw \dst, \src, i
     ftoi0 \dst, \dst
     .endm

     .macro emit_mvert p, s, c, k, adcreg
     div q, vf00[w], \p[w]
     addq.x emq\@, vf00, q
     mulx.xyz emp\@, \p, emq\@
     add.xyz emp\@, emp\@, gs_offsets
     ftoi4.xyz emp\@, emp\@
     mulx.xyz ems\@, \s, emq\@
     sq.xyz ems\@, \k(next_output)
     fmt_color emc\@, \c
     store_rgba emc\@, \k
     fog_coef_alpha emf\@, \c
     ior emadc\@, \adcreg, emf\@
     mfir.w emp\@, emadc\@
     store_xyzf emp\@, \k
     .endm

     .macro r_kick_chunk
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     lq gif_tag_p, kGifTag(vi00)
     mtir eop_p, gif_tag_px
     ior eop_p, eop_p, out_count
     mfir.x gif_tag_p, eop_p
     sq gif_tag_p, 0(arena_tag)
     b r_kick_fence_lid\@
r_kick_fence_lid\@:
     ; SQ latency is4 (VU manualp190), and XGKICK must not start before the
     ; final tag store completes (p196). VCL removes empty branch fences;
     ; preserve three useful independent VI setup pairs, then the kick.
     ; The raw block is also a scheduler barrier. No VU memory is overwritten
     ; here; arena_tag still names the packet just completed above.
     .raw
     nop iaddiu arena_a, buffer_top, kRATag
     nop iaddiu next_output, buffer_top, kRAData
     nop iaddiu out_count, vi00, 0
     nop xgkick arena_tag
     .endraw
     --barrier
     b r_after_kick_lid\@
r_after_kick_lid\@:
     ibeq arena_tag, arena_a, r_use_b_lid\@
     b r_arena_ready_lid\@
r_use_b_lid\@:
     iaddiu next_output, buffer_top, kRBData
r_arena_ready_lid\@:
     .endm

     ; Load one sky-fan corner and retain the existing X2 fast-path decision.
     ; Store PRE-divide position to viewA; classification alone uses divide.
     .macro r_project_corner ptr, off, slot
     lq rp_eye\@, \off(\ptr)
     move.xyzw rp_batch\@, rp_eye\@
     loi -1.0
     muli.xz rp_batch\@, rp_eye\@, i
     mul_pt_mat_44 rp_clip\@, vert_xform, rp_batch\@
     sq rp_clip\@, kRViewA+(\slot*3)(buffer_top)
     lq rp_stq\@, \off+1(\ptr)
     lq rp_col\@, \off+2(\ptr)
     sq rp_stq\@, kRViewA+(\slot*3)+1(buffer_top)
     sq rp_col\@, kRViewA+(\slot*3)+2(buffer_top)
     sub.w rp_near\@, rp_clip\@, near_plane
     loi 2047.0
     minii.w rp_near\@, rp_near\@, i
     loi -2047.0
     maxi.w rp_near\@, rp_near\@, i
     ftoi4.w rp_near\@, rp_near\@
     mtir rp_flag\@, rp_near\@[w]
     iand rp_flag\@, rp_flag\@, adc_bit
     ior near_any, near_any, rp_flag\@
     div q, vf00[w], rp_clip\@[w]
     addq.x rp_q\@, vf00, q
     mulx.xyz rp_projected\@, rp_clip\@, rp_q\@
     clip_vert rp_projected\@
     .endm

     .macro r_sky_store p, s, c, limit=27
     isubiu sc_bound\@, sc_m, \limit
     ibltz sc_bound\@, sc_sky_store_ok\@
     r_fail_stop
sc_sky_store_ok\@:
     sq \p, 0(sc_dst)
     sq \s, 1(sc_dst)
     sq \c, 2(sc_dst)
     iaddiu sc_dst, sc_dst, 3
     iaddiu sc_m, sc_m, 1
     .endm

