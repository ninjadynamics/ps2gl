/* Raw X2 input and the unchanged X2 body, entered through a decoder that
   replaces each vertex alpha with the haze keep from the law carried in the
   buffer header. */
#include "GL/ps2gl.h"
#include "ps2gl/x2s_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

extern "C" {
void vsmGeneralClipTriX2SDecode_CodeStart();
void vsmGeneralClipTriX2SDecode_CodeEnd();
}

CClipTriX2SRenderer::CClipTriX2SRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipTriX2SDecode_CodeStart,
          (const u8*)vsmGeneralClipTriX2SDecode_CodeEnd -
              (const u8*)vsmGeneralClipTriX2SDecode_CodeStart,
          "clip x2s, raw triangles with VU1 haze", PGL_CLIP_TRI_X2S_PROP, 4, 4)
{
    ContextDeltaEligible = true;
    RawPrim = PGL_CLIP_TRIANGLES_X2S;
    // No haze until the caller sets a law: at = 0 keeps every vertex clear.
    static const float clear[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    SetBufferParams(clear);
}

void CClipTriX2SRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    CClipTriX2Renderer::DrawLinearArrays(block);
}

void CClipTriX2SRenderer::FinishRawBuffer(CVifSCDmaPacket& packet, int numVertsToBreakStrip,
    int numVertsInBuffer, int vu1QuadsPerVert, int numStripsInBuffer,
    unsigned short* stripOffsets)
{
    FinishDecodedRawBuffer(packet, numVertsToBreakStrip, numVertsInBuffer,
        vu1QuadsPerVert, numStripsInBuffer, stripOffsets);
}

static CClipTriX2SRenderer* pX2SRenderer = NULL;

void CClipTriX2SRenderer::Register()
{
    if (pX2SRenderer) return;
    pX2SRenderer = new CClipTriX2SRenderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2SRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2S,
        PGL_CLIP_TRI_X2S_PROP, ~(pglU64_t)0xffffffff, PGL_MERGE_CONTIGUOUS);
}

void pglRegisterClipTriX2SRenderer(void)
{
    CClipTriX2SRenderer::Register();
}

bool pglClipX2SRegistered(void)
{
    return pX2SRenderer != NULL;
}

void pglClipX2SSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2SRenderer == NULL, "pglRegisterClipTriX2SRenderer() first");
    pX2SRenderer->SetWindowTexture(texId, r, g, b, a);
}

void pglClipX2SSetHaze(const GLfloat haze[8])
{
    mErrorIf(pX2SRenderer == NULL, "pglRegisterClipTriX2SRenderer() first");
    pX2SRenderer->SetHaze(haze);
}
