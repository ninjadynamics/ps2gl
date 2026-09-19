#ifndef ps2gl_x2e_renderer_h
#define ps2gl_x2e_renderer_h

#include "ps2gl/linear_renderer.h"

class CClipDecalX2ERenderer : public CLinearRenderer {
    PGLDecalContext DecalContext;

public:
    // More than the normal 65,000-qword frame packet can hold even when only
    // one 9-qword copy per descriptor is stored. Checked before publication.
    enum { kMaxOwnedPayloadBatches = 512 };
    CClipDecalX2ERenderer();
    static void Register();
    bool IsCodeValid() const;
    void SetDecalContext(const PGLDecalContext& context, bool glow);
    void DrawDecalRecords(const void* records, int count, unsigned int format,
        const float** ownedPayloads, bool reusePayload);
    virtual void Load();
    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif
