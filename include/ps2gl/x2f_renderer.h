#ifndef ps2gl_x2f_renderer_h
#define ps2gl_x2f_renderer_h
#include "ps2gl/clip_renderer.h"

class CClipQuadX2FRenderer : public CClipTriX2Renderer {
    // X2F owns only one material; its prefix reuses unused window color lanes.
    using CClipTriX2Renderer::SetWindowTexture;

protected:
    // The same program and input contract under another primitive (X2T).
    CClipQuadX2FRenderer(const char* name, uint64_t prop);

public:
    CClipQuadX2FRenderer();
    static void Register();
    virtual void DrawLinearArrays(CGeometryBlock& block);
};
#endif
