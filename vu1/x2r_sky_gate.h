#ifndef PGL_X2R_SKY_GATE_H
#define PGL_X2R_SKY_GATE_H

/* VCL-only A/B: regenerate general_clip_road_x2r after changing this value.
   1 skips the 24 empty sky-plane selector iterations when no vertex of the
   triangle is outside any sky side (the X2P pool early-out). */
#define PGL_X2R_SKY_EMPTY_SKIP 1
#if PGL_X2R_SKY_EMPTY_SKIP != 0 && PGL_X2R_SKY_EMPTY_SKIP != 1
#error "PGL_X2R_SKY_EMPTY_SKIP must be 0 or 1"
#endif

#endif
