#ifndef ps2gl_x2r_renderer_h
#define ps2gl_x2r_renderer_h

#include "ps2gl/linear_renderer.h"

class CClipRoadX2RRenderer : public CLinearRenderer {
    PGLRoadContext RoadContext;

public:
    CClipRoadX2RRenderer();
    static void Register();
    bool IsCodeValid() const;
    void SetRoadContext(const PGLRoadContext& context);
    void DrawRoadQuads(const PGLRoadQuad* quads, int count);
    virtual void Load();
    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif
