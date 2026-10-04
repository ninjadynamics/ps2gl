#ifndef ps2gl_x2v_renderer_h
#define ps2gl_x2v_renderer_h

#include "ps2gl/clip_renderer.h"

class CClipTriX2VRenderer : public CClipTriX2DRenderer {
public:
    CClipTriX2VRenderer();
    static void Register();
    void SetHaze(const float* haze) { SetBufferParams(haze); }
};

/* Wall submission option bit8 (see pglGetWallSubmissionOptions). */
bool pglClipX2VRegistered(void);

#endif
