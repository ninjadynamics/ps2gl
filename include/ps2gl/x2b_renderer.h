#ifndef ps2gl_x2b_renderer_h
#define ps2gl_x2b_renderer_h

#include "ps2gl/x2r_renderer.h"

class CClipBillboardX2BRenderer : public CClipRoadX2RRenderer {
public:
    CClipBillboardX2BRenderer();
    static void Register();
};

class CClipBillboardAlphaX2ARenderer : public CClipRoadX2RRenderer {
public:
    CClipBillboardAlphaX2ARenderer();
    static void Register();
};

#endif
