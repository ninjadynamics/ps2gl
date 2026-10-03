#ifndef ps2gl_x2k_renderer_h
#define ps2gl_x2k_renderer_h

#include "ps2gl/x2r_renderer.h"

/* The pool program with planar UV and per-corner alpha (texture clouds). */
class CClipCloudX2KRenderer : public CClipRoadX2RRenderer {
public:
    CClipCloudX2KRenderer();
    static void Register();
};

#endif
