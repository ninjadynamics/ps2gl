     --enter
     --endenter
     load_vert_xfrm vert_xform
     --cont

r_main_lid:
     init_constants
     get_ones_vec ones
     xtop buffer_top
     ilw.x setup_n, 0(buffer_top)
     ibgtz setup_n, xb_header_nonempty_lid
     r_fail_stop
xb_header_nonempty_lid:
     isubiu setup_bad, setup_n, kBillboardCount
     iblez setup_bad, xb_header_count_ok_lid
     r_fail_stop
xb_header_count_ok_lid:
     iaddiu setup_ptr, buffer_top, 5
     isw.x setup_ptr, kRDesc(buffer_top)
     isw.y setup_n, kRDesc(buffer_top)
     isw.z vi00, kRDesc(buffer_top)
     iaddiu setup_ptr, buffer_top, kRAData
     isw.x setup_ptr, kROut(buffer_top)
     isw.y vi00, kROut(buffer_top)
     isw.x vi00, kRFailure(buffer_top)
     iaddiu adc_bit, vi00, 0x7fff
     iaddiu adc_bit, adc_bit, 1

     sub.xyzw pl_zero, vf00, vf00
     lq xb_cfg, 54(vi00)
     lq xb_proj, 53(vi00)
     move.xyzw pl_t, pl_zero
     addw.z pl_t, pl_zero, vf00
     subx.w pl_t, pl_zero, xb_cfg
     sq pl_t, kRPlanes(buffer_top)
     move.xyzw pl_t, pl_zero
     subz.x pl_t, pl_zero, xb_proj
     addy.z pl_t, pl_zero, xb_cfg
     sq pl_t, kRPlanes+1(buffer_top)
     move.xyzw pl_t, pl_zero
     addz.x pl_t, pl_zero, xb_proj
     addy.z pl_t, pl_zero, xb_cfg
     sq pl_t, kRPlanes+2(buffer_top)
     move.xyzw pl_t, pl_zero
     subw.y pl_t, pl_zero, xb_proj
     addy.z pl_t, pl_zero, xb_cfg
     sq pl_t, kRPlanes+3(buffer_top)
     move.xyzw pl_t, pl_zero
     addw.y pl_t, pl_zero, xb_proj
     addy.z pl_t, pl_zero, xb_cfg
     sq pl_t, kRPlanes+4(buffer_top)
     b r_decode_lid

