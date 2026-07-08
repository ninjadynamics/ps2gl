/* HyperSolar VU1 near-plane clip renderer (tri lists).

   A user renderer wrapping vu1/general_clip_tri.vcl, selected through the
   PGL_CLIP_TRIANGLES custom prim type (see GL/ps2gl.h). Phase 1 is a 1:1
   clone of the stock "linear, tris, no specular" renderer under its own
   microcode so the toolchain + registration loop is proven before any clip
   logic lands; phases 2/3 give the microcode true near-plane classify/clip
   with 1:N output. */

#ifndef clip_renderer_h
#define clip_renderer_h

#include "ps2gl/linear_renderer.h"
#include "ps2gl/renderer.h"

class CClipTriRenderer : public CLinearRenderer {
public:
    CClipTriRenderer();

    static void Register();

    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif // clip_renderer_h
