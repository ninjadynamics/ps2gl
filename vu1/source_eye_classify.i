     ; Source-relative subtraction first, then separate ordered dot products.
     sub.xyz eye_delta, world_p, eye_cfg
     move.xyzw eye_p, vf00
     lq eye_basis, 49(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     move.x eye_p, eye_sum
     lq eye_basis, 50(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     mulx.y eye_p, ones, eye_sum
     lq eye_basis, 51(vi00)
     mul.xyz eye_terms, eye_delta, eye_basis
     addy.x eye_sum, eye_terms, eye_terms
     addz.x eye_sum, eye_sum, eye_terms
     mulx.z eye_p, ones, eye_sum
     sq eye_p, 0(vertex_ptr)

     ; Exact source SH outcode: behind-near vertices carry only bit0.
     addz.x source_near_d, vf00, eye_p
     subx.x source_near_d, source_near_d, source_cfg
     r_negative source_near, source_near_d
     iaddiu source_oc, vi00, 1
     ibne source_near, vi00, r_source_view_done_lid
     sub.xyzw source_side, vf00, vf00
     mulz.x source_side, eye_p, center_cfg
     mulw.y source_side, eye_p, center_cfg
     mulz.w source_side, ones, eye_p
     muly.w source_side, source_side, source_cfg
     clipw.xyz source_side, source_side[w]
     fcget source_oc
     iaddiu source_side_mask, vi00, 15
     iand source_oc, source_oc, source_side_mask
     iadd source_oc, source_oc, source_oc
r_source_view_done_lid:
     ilw.w source_or, kRDesc(buffer_top)
     ior source_or, source_or, source_oc
     isw.w source_or, kRDesc(buffer_top)
     ilw.x source_and, kRFan(buffer_top)
     iand source_and, source_and, source_oc
     isw.x source_and, kRFan(buffer_top)
     iaddiu vertex_ptr, vertex_ptr, 3
     isubiu vertex_left, vertex_left, 1
     ibgtz vertex_left, r_classify_lid
     b r_triangle_classified_lid
