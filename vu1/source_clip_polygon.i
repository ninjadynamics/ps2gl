pool_clip_plane_selected_lid:
     lq sc_plane, 0(sky_plane_addr)
     ilw.z sc_n, kROut(buffer_top)
     ilw.w sc_src, kROut(buffer_top)
     ilw.z sc_dst, kRPlaneState(buffer_top)
     iadd sc_last, sc_n, sc_n
     iadd sc_last, sc_last, sc_n
     iadd sc_last, sc_last, sc_src
     isubiu sc_last, sc_last, 3
     lq sc_prev_p, 0(sc_last)
     lq sc_prev_s, 1(sc_last)
     lq sc_prev_c, 2(sc_last)
     r_sky_distance sc_prev_d, sc_prev_p, sc_plane
     r_negative sc_prev_out, sc_prev_d
     iadd sc_left, sc_n, vi00
     iaddiu sc_m, vi00, 0
     b r_sky_edge_lid
r_sky_edge_lid:
     lq sc_cur_p, 0(sc_src)
     lq sc_cur_s, 1(sc_src)
     lq sc_cur_c, 2(sc_src)
     r_sky_distance sc_cur_d, sc_cur_p, sc_plane
     r_negative sc_cur_out, sc_cur_d
     ibeq sc_prev_out, sc_cur_out, r_sky_inside_lid
     sub.x sc_den, sc_prev_d, sc_cur_d
     abs.x sc_abs_den, sc_den
     lq sc_eps, 54(vi00)
     addz.x sc_guard, vf00, sc_eps
     sub.x sc_guard, sc_guard, sc_abs_den
     r_negative sc_divide, sc_guard
     sub.x sc_t, vf00, vf00
     ibeq sc_divide, vi00, r_sky_lerp_lid
     div q, sc_prev_d[x], sc_den[x]
     addq.x sc_t, vf00, q
     b r_sky_lerp_lid
r_sky_lerp_lid:
     ; Match prev+(cur-prev)*t, never MADD; t=0 on the EE small-denom arm.
     sub.xyzw sc_delta, sc_cur_p, sc_prev_p
     mulx.xyzw sc_product, sc_delta, sc_t
     add.xyzw sc_ip, sc_prev_p, sc_product
     sub.xyz sc_delta, sc_cur_s, sc_prev_s
     mulx.xyz sc_product, sc_delta, sc_t
     add.xyz sc_is, sc_prev_s, sc_product
     mulx.w sc_is, vf00, vf00
     sub.xyzw sc_delta, sc_cur_c, sc_prev_c
     mulx.xyzw sc_product, sc_delta, sc_t
     add.xyzw sc_ic, sc_prev_c, sc_product
     r_sky_store sc_ip, sc_is, sc_ic, 32
r_sky_inside_lid:
     ibne sc_cur_out, vi00, r_sky_edge_next_lid
     r_sky_store sc_cur_p, sc_cur_s, sc_cur_c, 32
r_sky_edge_next_lid:
     move.xyzw sc_prev_p, sc_cur_p
     move.xyzw sc_prev_s, sc_cur_s
     move.xyzw sc_prev_c, sc_cur_c
     move.x sc_prev_d, sc_cur_d
     iadd sc_prev_out, sc_cur_out, vi00
     iaddiu sc_src, sc_src, 3
     isubiu sc_left, sc_left, 1
     ibgtz sc_left, r_sky_edge_lid
     isubiu sc_valid, sc_m, 3
     ibltz sc_valid, r_next_triangle_lid
     ilw.w sky_old_src, kROut(buffer_top)
     ilw.z sky_new_src, kRPlaneState(buffer_top)
     isw.w sky_new_src, kROut(buffer_top)
     isw.z sky_old_src, kRPlaneState(buffer_top)
     isw.z sc_m, kROut(buffer_top)
     b r_sky_plane_lid

