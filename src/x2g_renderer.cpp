/* Reconstruct on VU1 before the exact existing X2 transform/clip/output image.
 * Geometry/color inputs are borrowed through the ordinary frame DMA lifetime.
 */
#include "GL/ps2gl.h"
#include "ps2gl/x2g_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "vu1_mem_linear.h"
#include <string.h>

extern "C" {
void vsmGeneralClipGlowX2GDecode_CodeStart();
void vsmGeneralClipGlowX2GDecode_CodeEnd();
}

// The sparse X2 context does not consume q58/q59. They belong only to this
// renderer and are restored after any renderer change by InitContext.
enum { kGlowAxisContext = 58, kGlowAxisWords = 8 };
#if kClipToGsDepthOffset != 57 || kVertexXfrm != 62 || kDoubleBufBase != 79
#error "Review X2G axis context against the shared VU context layout"
#endif

CClipGlowX2GRenderer::CClipGlowX2GRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipGlowX2GDecode_CodeStart,
          (const u8*)vsmGeneralClipGlowX2GDecode_CodeEnd -
              (const u8*)vsmGeneralClipGlowX2GDecode_CodeStart,
          "clip x2g, compact glow quads", PGL_CLIP_GLOW_X2G_PROP, 2, 4)
{
    const float initial[kGlowAxisWords] = {1, 0, 0, 0, 0, 1, 0, 0};
    memcpy(Axes, initial, sizeof(Axes));
}

static CClipGlowX2GRenderer* pX2GRenderer = NULL;

void CClipGlowX2GRenderer::Register()
{
    pX2GRenderer = new CClipGlowX2GRenderer;
    pglRegisterRenderer(pX2GRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_GLOW_QUADS_X2G,
        PGL_CLIP_GLOW_X2G_PROP, ~(pglU64_t)0xffffffff, PGL_MERGE_CONTIGUOUS);
}

void CClipGlowX2GRenderer::SetAxes(float ux, float uy, float uz,
    float vx, float vy, float vz)
{
    const float next[kGlowAxisWords] = {ux, uy, uz, 0, vx, vy, vz, 0};
    if (memcmp(Axes, next, sizeof(Axes)) == 0) return;
    // Capture pending geometry with the OLD axes before mutating its context.
    // A scalar comparison is insufficient here: preserve signed-zero words.
    pGLContext->GetImmGeomManager().Flush();
    memcpy(Axes, next, sizeof(Axes));
    pGLContext->SetRendererContextChanged(true);
}

void CClipGlowX2GRenderer::InitContext(GLenum primType, uint32_t rcChanges,
    bool userRcChanged)
{
    CClipTriX2Renderer::InitContext(primType, rcChanges, userRcChanged);
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    packet.Cnt();
    packet.Stcycl(1, 1);
    // InitUnlitContext ends with FLUSHE: previous geometry has finished
    // consuming the shared context before these two qwords are replaced.
    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, kGlowAxisContext, Packet::kSingleBuff);
    for (int i = 0; i < kGlowAxisWords; ++i) packet += Axes[i];
    packet.CloseUnpack();
    packet.Pad128();
    packet.CloseTag();
}

extern "C" void pglRegisterClipGlowX2GRenderer(void)
{
    CClipGlowX2GRenderer::Register();
}

extern "C" void pglClipX2GSetAxes(float ux, float uy, float uz,
    float vx, float vy, float vz)
{
    mErrorIf(pX2GRenderer == NULL, "pglRegisterClipGlowX2GRenderer() first");
    if (pX2GRenderer) pX2GRenderer->SetAxes(ux, uy, uz, vx, vy, vz);
}
