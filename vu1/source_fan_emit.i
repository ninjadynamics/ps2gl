pool_fan_ready_lid:
     ilw.w sky_first, kROut(buffer_top)
     ilw.z sky_count, kROut(buffer_top)
     isw.y sky_first, kRFan(buffer_top)
     iaddiu sky_rim, sky_first, 3
     isw.z sky_rim, kRFan(buffer_top)
     isubiu sky_left, sky_count, 2
     isw.w sky_left, kRFan(buffer_top)
     b r_sky_fan_lid
r_sky_fan_lid:
     ; The reference clips the complete source polygon before triangulating.
     ; Emit exactly one fan triangle at a time and spill only on arena edges.
     ilw.x next_output, kROut(buffer_top)
     ilw.y out_count, kROut(buffer_top)
     iadd arena_used, out_count, out_count
     iadd arena_used, arena_used, out_count
     isub arena_tag, next_output, arena_used
     isubiu arena_tag, arena_tag, 1
     iaddiu arena_a, buffer_top, kRATag
     ibeq arena_tag, arena_a, pool_fan_cap_a_lid
     iaddiu arena_cap, vi00, kRBCap
     b pool_fan_cap_lid
pool_fan_cap_a_lid:
     iaddiu arena_cap, vi00, kRACap
pool_fan_cap_lid:
     isub pool_room, arena_cap, out_count
     isubiu pool_room, pool_room, 3
     ibgez pool_room, pool_emit_fence_lid
     r_kick_chunk
     b pool_emit_fence_lid
pool_emit_fence_lid:
     ilw.y sky_first, kRFan(buffer_top)
     ilw.z sky_rim, kRFan(buffer_top)
     pool_emit_corner sky_first, 0, 0, adc_bit
     pool_emit_corner sky_rim, 0, 3, adc_bit
     pool_emit_corner sky_rim, 3, 6, vi00
     iaddiu next_output, next_output, 9
     iaddiu out_count, out_count, 3
     isw.x next_output, kROut(buffer_top)
     isw.y out_count, kROut(buffer_top)
     b r_next_sky_fan_lid
r_next_sky_fan_lid:
     ilw.z sky_rim, kRFan(buffer_top)
     iaddiu sky_rim, sky_rim, 3
     isw.z sky_rim, kRFan(buffer_top)
     ilw.w sky_left, kRFan(buffer_top)
     isubiu sky_left, sky_left, 1
     isw.w sky_left, kRFan(buffer_top)
     ibgtz sky_left, r_sky_fan_lid
     b r_next_triangle_lid
