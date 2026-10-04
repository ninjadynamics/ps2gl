#ifndef ps2gl_x2s_renderer_h
#define ps2gl_x2s_renderer_h

#include "ps2gl/clip_renderer.h"

/* Raw X2 triangles through a decoder that evaluates the haze per vertex. The
   descriptor base supplies the decoder upload and buffer parameters; the
   draw path is raw X2's. */
class CClipTriX2SRenderer : public CClipTriX2DRenderer {
protected:
    virtual void FinishRawBuffer(CVifSCDmaPacket& packet, int numVertsToBreakStrip,
        int numVertsInBuffer, int vu1QuadsPerVert, int numStripsInBuffer,
        unsigned short* stripOffsets);

public:
    CClipTriX2SRenderer();
    static void Register();
    void SetHaze(const float* haze) { SetBufferParams(haze); }
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

/* Raw X2 submission option bit1 (see pglGetRawX2SubmissionOptions). */
bool pglClipX2SRegistered(void);

#endif
