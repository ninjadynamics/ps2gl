/* Compact, camera-facing glow descriptors; the unchanged X2 body clips them. */
#ifndef ps2gl_x2g_renderer_h
#define ps2gl_x2g_renderer_h

#include "ps2gl/clip_renderer.h"

class CClipGlowX2GRenderer : public CClipTriX2DRenderer {
    float Axes[8]; // two V4-32 context qwords, xyz axes and deterministic zero w

public:
    CClipGlowX2GRenderer();
    static void Register();
    void SetAxes(float ux, float uy, float uz, float vx, float vy, float vz);
    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
};

#endif
