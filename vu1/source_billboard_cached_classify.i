     ; Same ordered source transform/outcode arithmetic as source_eye_classify.i.
     ; Only the sink/loop differs: cache four independent corners and codes,
     ; then reduce the original ABC / ACD triangles separately.
bc_classify_lid:
     lq world_p, 0(vertex_ptr)
     lq eye_cfg, 52(vi00)
     lq center_cfg, 53(vi00)
     lq source_cfg, 54(vi00)
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
     ibne source_near, vi00, bc_source_view_done_lid
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
bc_source_view_done_lid:
     isw.x source_oc, 0(bc_code_ptr)
     iaddiu vertex_ptr, vertex_ptr, 3
     iaddiu bc_code_ptr, bc_code_ptr, 1
     isubiu vertex_left, vertex_left, 1
     ibgtz vertex_left, bc_classify_lid
     ; Fence aliasing: assembly reads cache values just stored by this loop.
     b bc_assemble_lid
