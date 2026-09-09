#ifndef ps2gl_unlit_renderer_h
#define ps2gl_unlit_renderer_h

#include "ps2gl/linear_renderer.h"

class CUnlitTexTriRenderer : public CLinearRenderer {
public:
    CUnlitTexTriRenderer();
    static void Register();
    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif
