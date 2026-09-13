r_classify_lid:
     lq world_p, 0(vertex_ptr)
     lq eye_cfg, 52(vi00)
     lq center_cfg, 53(vi00)
     lq source_cfg, 54(vi00)
     .include "source_eye_classify.i"

r_triangle_classified_lid:
     ilw.x source_and, kRFan(buffer_top)
     ibne source_and, vi00, r_next_triangle_lid
     iaddiu setup_ptr, buffer_top, kRSkyA
     isw.w setup_ptr, kROut(buffer_top)
     iaddiu setup_ptr, buffer_top, kRSkyB
     isw.z setup_ptr, kRPlaneState(buffer_top)
     iaddiu setup_n, vi00, 3
     isw.z setup_n, kROut(buffer_top)
     isw.x vi00, kRPlaneState(buffer_top)
     ilw.w xb_source_or, kRDesc(buffer_top)
     ibeq xb_source_or, vi00, pool_fan_ready_lid
     b r_sky_plane_lid

r_sky_plane_lid:
     ; Shared clipper driver label retained; this module has only view planes.
     ilw.x xb_plane_index, kRPlaneState(buffer_top)
     isubiu xb_planes_left, xb_plane_index, 5
     ibgez xb_planes_left, pool_fan_ready_lid
     iaddiu xb_next_plane, xb_plane_index, 1
     isw.x xb_next_plane, kRPlaneState(buffer_top)
     iaddiu sky_plane_addr, buffer_top, kRPlanes
     iadd sky_plane_addr, sky_plane_addr, xb_plane_index
     b pool_clip_plane_selected_lid

     .include "source_clip_polygon.i"
     .include "source_fan_emit.i"

r_next_triangle_lid:
     ilw.z desc_phase, kRDesc(buffer_top)
     ibne desc_phase, vi00, xb_next_descriptor_lid
     iaddiu desc_phase, vi00, 1
     isw.z desc_phase, kRDesc(buffer_top)
     b r_decode_lid
xb_next_descriptor_lid:
     isw.z vi00, kRDesc(buffer_top)
     ilw.x desc_ptr, kRDesc(buffer_top)
     iaddiu desc_ptr, desc_ptr, kBillboardWords
     isw.x desc_ptr, kRDesc(buffer_top)
     ilw.y desc_left, kRDesc(buffer_top)
     isubiu desc_left, desc_left, 1
     isw.y desc_left, kRDesc(buffer_top)
     ibgtz desc_left, r_decode_lid
     ilw.x next_output, kROut(buffer_top)
     ilw.y out_count, kROut(buffer_top)
     r_kick_chunk
     --cont
     b r_main_lid
     .END
