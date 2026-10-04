/* X2F source quads through a decoder that rewrites each corner alpha from the
   haze law in the buffer header, then enters the unchanged X2F program. */
#include "GL/ps2gl.h"
#include "ps2gl/x2t_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/metrics.h"
#include <string.h>

extern "C" {
void vsmGeneralClipQuadX2TDecode_CodeStart();
void vsmGeneralClipQuadX2TDecode_CodeEnd();
}

CClipQuadX2TRenderer::CClipQuadX2TRenderer()
    : CClipQuadX2FRenderer("clip x2t, source quads with VU1 haze", PGL_CLIP_QUAD_X2T_PROP)
    , DecoderAddr64(MicrocodePacketSize / 8)
    , HazeValid(false)
{
    // No haze until the caller sets a law: at = 0 and s = 1 keep alpha = 1.
    static const float clear[8] = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    memcpy(Haze, clear, sizeof(Haze));
}

void CClipQuadX2TRenderer::SetHaze(const float* haze)
{
    if (HazeValid) {
        if (memcmp(Haze, haze, sizeof(Haze)) == 0) return;
        // A pending block reads the law when it is finally drawn: submit it
        // under the law it was prepared with.
        pGLContext->GetGeomManager().Flush();
    }
    memcpy(Haze, haze, sizeof(Haze));
    HazeValid = true;
}

void CClipQuadX2TRenderer::Load()
{
    // The complete X2F program at PC 0 (with its prologue activation), then
    // the decoder right behind it. MPG waits for that activation to finish.
    CClipTriX2Renderer::Load();
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    pglUploadVu1Decoder(packet, (const void*)vsmGeneralClipQuadX2TDecode_CodeStart,
        (const u8*)vsmGeneralClipQuadX2TDecode_CodeEnd -
            (const u8*)vsmGeneralClipQuadX2TDecode_CodeStart,
        DecoderAddr64);
}

void CClipQuadX2TRenderer::FinishRawBuffer(CVifSCDmaPacket& packet, int numVertsToBreakStrip,
    int numVertsInBuffer, int vu1QuadsPerVert, int numStripsInBuffer,
    unsigned short* stripOffsets)
{
    // CLinearRenderer::FinishBuffer with the decoder's MSCAL in place of
    // MSCNT (the decoder tail-jumps to X2F PC 6) and the law over the unread
    // strip ADC qwords 1..2.
    pglCountSubmission(PGL_SUBMIT_BUFFERS);
    packet.Cnt();
    {
        XferBufferHeader(packet, numVertsToBreakStrip, numVertsInBuffer,
            numStripsInBuffer, stripOffsets);
        packet.Stcycl(1, 1);
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 1, Packet::kDoubleBuff);
        for (int i = 0; i < 8; ++i)
            packet += Haze[i];
        packet.CloseUnpack(2);
        // the next buffer's corner streams expect their write cycle
        packet.Stcycl(1, vu1QuadsPerVert);
        packet.Mscal(DecoderAddr64);
        packet.Pad128();
    }
    packet.CloseTag();
}

static CClipQuadX2TRenderer* pX2TRenderer = NULL;

void CClipQuadX2TRenderer::Register()
{
    if (pX2TRenderer) return;
    pX2TRenderer = new CClipQuadX2TRenderer;
    // A complete program plus decoder: the generic registration, which
    // invalidates the shared X2 prefix before this renderer loads.
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterUserRenderer(pX2TRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_QUADS_X2T, PGL_CLIP_QUAD_X2T_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void pglRegisterClipQuadX2TRenderer(void)
{
    if (pGLContext) CClipQuadX2TRenderer::Register();
}

bool pglClipX2TRegistered(void)
{
    return pX2TRenderer != NULL;
}

void pglClipX2TSetHaze(const GLfloat haze[8])
{
    mErrorIf(pX2TRenderer == NULL, "pglRegisterClipQuadX2TRenderer() first");
    pX2TRenderer->SetHaze(haze);
}
