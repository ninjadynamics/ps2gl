#ifndef ps2gl_x2p_renderer_h
#define ps2gl_x2p_renderer_h

#include "ps2gl/x2r_renderer.h"

/* Shares the owned ground context/packet writer, not road clipping topology. */
class CClipPoolX2PRenderer : public CClipRoadX2RRenderer {
public:
    CClipPoolX2PRenderer();
    static void Register();
    void DrawPoolQuads(const PGLPoolQuad* quads, int count);
};

#endif
