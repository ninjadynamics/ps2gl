     ; Private to X2B/X2A. Keep reconstruction operations identical to their
     ; gate-OFF decoder: (center +/- scaledU) +/- scaledV, not eye extents.
bc_decode_lid:
     ilw.z desc_phase, kRDesc(buffer_top)
     ibne desc_phase, vi00, bc_assemble_lid
     ilw.x desc_ptr, kRDesc(buffer_top)
     lq xb_center, 0(desc_ptr)
     lq desc_v, 1(desc_ptr)
     lq material_color, 2(desc_ptr)
     bc_load_alpha
     lq xb_axis_u, 55(vi00)
     lq xb_axis_v, 56(vi00)
     mulw.xyz xb_axis_u, xb_axis_u, xb_center
     mulw.xyz xb_axis_v, xb_axis_v, xb_center
     move.xyzw decode_p, vf00
     sub.xyz decode_p, xb_center, xb_axis_u
     sub.xyz decode_p, decode_p, xb_axis_v
     sub.xyzw decode_s, vf00, vf00
     mr32.z decode_s, vf00
     move.xy decode_s, desc_v
     bc_alpha_a
     sq decode_p, kBCache(buffer_top)
     sq decode_s, kBCache+1(buffer_top)
     sq material_color, kBCache+2(buffer_top)
     mr32 xb_uvfar, desc_v
     mr32 xb_uvfar, xb_uvfar
     add.xyz decode_p, xb_center, xb_axis_u
     sub.xyz decode_p, decode_p, xb_axis_v
     move.x decode_s, xb_uvfar
     bc_alpha_b
     sq decode_p, kBCache+3(buffer_top)
     sq decode_s, kBCache+4(buffer_top)
     sq material_color, kBCache+5(buffer_top)
     add.xyz decode_p, xb_center, xb_axis_u
     add.xyz decode_p, decode_p, xb_axis_v
     move.xy decode_s, xb_uvfar
     bc_alpha_c
     sq decode_p, kBCache+6(buffer_top)
     sq decode_s, kBCache+7(buffer_top)
     sq material_color, kBCache+8(buffer_top)
     sub.xyz decode_p, xb_center, xb_axis_u
     add.xyz decode_p, decode_p, xb_axis_v
     move.x decode_s, desc_v
     bc_alpha_d
     sq decode_p, kBCache+9(buffer_top)
     sq decode_s, kBCache+10(buffer_top)
     sq material_color, kBCache+11(buffer_top)
     iaddiu vertex_ptr, buffer_top, kBCache
     iaddiu bc_code_ptr, buffer_top, kBCodes
     iaddiu vertex_left, vi00, 4
     ; Fence aliasing: classification consumes stores through vertex_ptr.
     b bc_classify_lid

bc_assemble_lid:
     ; Clipping destroys only polyA/polyB, never this immutable ABCD cache.
     ilw.z desc_phase, kRDesc(buffer_top)
     iaddiu bc_second, buffer_top, kBCache+3
     iaddiu bc_second_code, buffer_top, kBCodes+1
     ibeq desc_phase, vi00, bc_copy_triangle_lid
     iaddiu bc_second, bc_second, 3
     iaddiu bc_second_code, bc_second_code, 1
bc_copy_triangle_lid:
     lq bc_p, kBCache(buffer_top)
     lq bc_s, kBCache+1(buffer_top)
     lq bc_c, kBCache+2(buffer_top)
     sq bc_p, kRSkyA(buffer_top)
     sq bc_s, kRSkyA+1(buffer_top)
     sq bc_c, kRSkyA+2(buffer_top)
     lq bc_p, 0(bc_second)
     lq bc_s, 1(bc_second)
     lq bc_c, 2(bc_second)
     sq bc_p, kRSkyA+3(buffer_top)
     sq bc_s, kRSkyA+4(buffer_top)
     sq bc_c, kRSkyA+5(buffer_top)
     lq bc_p, 3(bc_second)
     lq bc_s, 4(bc_second)
     lq bc_c, 5(bc_second)
     sq bc_p, kRSkyA+6(buffer_top)
     sq bc_s, kRSkyA+7(buffer_top)
     sq bc_c, kRSkyA+8(buffer_top)
     ilw.x bc_oa, kBCodes(buffer_top)
     ilw.x bc_ob, 0(bc_second_code)
     ilw.x bc_oc, 1(bc_second_code)
     ior bc_or, bc_oa, bc_ob
     ior bc_or, bc_or, bc_oc
     iand bc_and, bc_oa, bc_ob
     iand bc_and, bc_and, bc_oc
     isw.w bc_or, kRDesc(buffer_top)
     isw.x bc_and, kRFan(buffer_top)
     ; Fence aliasing: the existing clip/fan reads the just-copied polyA.
     b r_triangle_classified_lid
