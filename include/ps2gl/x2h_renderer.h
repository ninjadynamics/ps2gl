#ifndef ps2gl_x2h_renderer_h
#define ps2gl_x2h_renderer_h

#include "ps2gl/clip_renderer.h"

class CClipTriX2HRenderer : public CClipTriX2DRenderer {
public:
    CClipTriX2HRenderer();
    static void Register();
};

/* Wall submission option bit7 (see pglGetWallSubmissionOptions). */
bool pglClipX2HRegistered(void);

#endif
