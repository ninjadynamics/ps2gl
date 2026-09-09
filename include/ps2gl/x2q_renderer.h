/* Exact-corner city descriptors, sharing the legacy descriptor DMA driver. */
#ifndef ps2gl_x2q_renderer_h
#define ps2gl_x2q_renderer_h

#include "ps2gl/clip_renderer.h"

class CClipTriX2QRenderer : public CClipTriX2DRenderer {
public:
    CClipTriX2QRenderer();
    static void Register();
};

#endif
