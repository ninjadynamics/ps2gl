#ifndef ps2gl_x2t_renderer_h
#define ps2gl_x2t_renderer_h

#include "ps2gl/x2f_renderer.h"

/* X2F's program and input, entered through a decoder that evaluates the haze
   per corner from the law carried in each buffer header. */
class CClipQuadX2TRenderer : public CClipQuadX2FRenderer {
    unsigned int DecoderAddr64;
    bool HazeValid;
    uint32_t Haze[8];

protected:
    virtual void FinishRawBuffer(CVifSCDmaPacket& packet, int numVertsToBreakStrip,
        int numVertsInBuffer, int vu1QuadsPerVert, int numStripsInBuffer,
        unsigned short* stripOffsets);

public:
    CClipQuadX2TRenderer();
    static void Register();
    void SetHaze(const float* haze);
    virtual void Load();
};

/* Source-quad submission option bit4 (see pglGetSourceQuadSubmissionOptions). */
bool pglClipX2TRegistered(void);

#endif
