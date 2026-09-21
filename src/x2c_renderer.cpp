/* Same X2Q geometry and X2 body, with a copy-only paired-color decoder. */
#include "GL/ps2gl.h"
#include "ps2gl/x2c_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

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

void pglRegisterClipTriX2CRenderer(void)
{
    CClipTriX2CRenderer::Register();
}

unsigned int pglGetWallSubmissionOptions(void)
{
    unsigned int options = 126u;
    if (pX2CRenderer) options |= 1u;
    return options;
}

void pglClipX2CSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2CRenderer == NULL, "pglRegisterClipTriX2CRenderer() first");
    pX2CRenderer->SetWindowTexture(texId, r, g, b, a);
}
