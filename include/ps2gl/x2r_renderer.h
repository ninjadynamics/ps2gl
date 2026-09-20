#ifndef ps2gl_x2r_renderer_h
#define ps2gl_x2r_renderer_h

#include "ps2gl/linear_renderer.h"

class CClipRoadX2RRenderer : public CLinearRenderer {
    PGLRoadContext RoadContext;
    const unsigned int ContextFirstQuad;
    bool HasRoadContext;
    bool RoadContextUnchanged;
    bool SourcePrefixUnchanged;
    const CVifSCDmaPacket* RetainedPacket;
    const void* RetainedBase;
    const void* RetainedEnd;
    unsigned int RetainedFrame;
    uint32_t RetainedRaster[27];
    bool RetainedContextValid;

    void GetRasterContextKey(uint32_t* key);

protected:
    CClipRoadX2RRenderer(void* code, int codeSize, const char* name, uint64_t prop,
        unsigned int contextFirstQuad = 0);

public:
    CClipRoadX2RRenderer();
    static void Register();
    bool IsCodeValid() const;
    bool MatchesRoadContext(const PGLRoadContext& context) const;
    void SetRoadContext(const PGLRoadContext& context, bool unchanged);
    bool MatchesSourceContext(const void* context) const;
    void SetSourceContext(const void* context, bool unchanged);
    void DrawCompactGroundQuads(const float* quads, int count,
        int floatsPerQuad, int quadsPerBuffer);
    void DrawRoadQuads(const PGLRoadQuad* quads, int count);
    virtual void Load();
    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif
