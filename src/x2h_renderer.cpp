/* Same X2Q geometry and X2 body, with X2C's paired wall RGB and a fog value
   per corner: a copy-only decoder for height-dependent fog. */
#include "GL/ps2gl.h"
#include "ps2gl/x2h_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

extern "C" {
void vsmGeneralClipTriX2HDecode_CodeStart();
void vsmGeneralClipTriX2HDecode_CodeEnd();
}

CClipTriX2HRenderer::CClipTriX2HRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipTriX2HDecode_CodeStart,
          (const u8*)vsmGeneralClipTriX2HDecode_CodeEnd -
              (const u8*)vsmGeneralClipTriX2HDecode_CodeStart,
          "clip x2h, paired wall colors with corner fog", PGL_CLIP_TRI_X2H_PROP,
          4, 4, 1, 3)
{
    ContextDeltaEligible = true;
}

static CClipTriX2HRenderer* pX2HRenderer = NULL;

void CClipTriX2HRenderer::Register()
{
    if (pX2HRenderer) return;
    pX2HRenderer = new CClipTriX2HRenderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2HRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2H,
        PGL_CLIP_TRI_X2H_PROP, ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void pglRegisterClipTriX2HRenderer(void)
{
    CClipTriX2HRenderer::Register();
}

bool pglClipX2HRegistered(void)
{
    return pX2HRenderer != NULL;
}

void pglClipX2HSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2HRenderer == NULL, "pglRegisterClipTriX2HRenderer() first");
    pX2HRenderer->SetWindowTexture(texId, r, g, b, a);
}
