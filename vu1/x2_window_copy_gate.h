#ifndef PGL_X2_WINDOW_COPY_GATE_H
#define PGL_X2_WINDOW_COPY_GATE_H

/* VCL-only A/B: regenerate general_clip_tri_x2 after changing this value. */
#define PGL_X2_WINDOW_COPY_TRIANGLES 0
#define PGL_X2_SKIP_IDENTITY_NEAR 1
#if PGL_X2_WINDOW_COPY_TRIANGLES != 0 && PGL_X2_WINDOW_COPY_TRIANGLES != 1
#error "PGL_X2_WINDOW_COPY_TRIANGLES must be 0 or 1"
#endif
#if PGL_X2_SKIP_IDENTITY_NEAR != 0 && PGL_X2_SKIP_IDENTITY_NEAR != 1
#error "PGL_X2_SKIP_IDENTITY_NEAR must be 0 or 1"
#endif

#endif
