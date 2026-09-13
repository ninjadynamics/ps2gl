/* Same X2Q geometry and X2 body, with a copy-only paired-color decoder. */
#include "GL/ps2gl.h"
#include "ps2gl/x2c_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

#if PGL_WALL_COLOR_COMPACT
extern "C" {
void vsmGeneralClipTriX2CDecode_CodeStart();
void vsmGeneralClipTriX2CDecode_CodeEnd();
}

CClipTriX2CRenderer::CClipTriX2CRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipTriX2CDecode_CodeStart,
          (const u8*)vsmGeneralClipTriX2CDecode_CodeEnd -
              (const u8*)vsmGeneralClipTriX2CDecode_CodeStart,
          "clip x2c, exact paired wall colors", PGL_CLIP_TRI_X2C_PROP, 4, 4, 2)
{
    ContextDeltaEligible = true;
}

static CClipTriX2CRenderer* pX2CRenderer = NULL;

void CClipTriX2CRenderer::Register()
{
    if (pX2CRenderer) return;
    pX2CRenderer = new CClipTriX2CRenderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2CRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2C,
        PGL_CLIP_TRI_X2C_PROP, ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}
#endif

void pglRegisterClipTriX2CRenderer(void)
{
#if PGL_WALL_COLOR_COMPACT
    CClipTriX2CRenderer::Register();
#endif
}

unsigned int pglGetWallSubmissionOptions(void)
{
    unsigned int options = (PGL_WALL_PACKET_DIRECT ? 2u : 0u) |
        (PGL_X2_PREFIX_SETUP ? 4u : 0u) |
        (PGL_X2_WINDOW_CONTEXT_REUSE ? 8u : 0u) |
        (PGL_X2_WINDOW_KEY_REUSE ? 16u : 0u) |
        (PGL_WALL_DESCRIPTOR_ARRAYS ? 32u : 0u) |
        (PGL_X2_CONTEXT_DELTA ? 64u : 0u);
#if PGL_WALL_COLOR_COMPACT
    if (pX2CRenderer) options |= 1u;
#endif
    return options;
}

void pglClipX2CSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
#if PGL_WALL_COLOR_COMPACT
    mErrorIf(pX2CRenderer == NULL, "pglRegisterClipTriX2CRenderer() first");
    pX2CRenderer->SetWindowTexture(texId, r, g, b, a);
#else
    (void)texId;
    (void)r;
    (void)g;
    (void)b;
    (void)a;
#endif
}
