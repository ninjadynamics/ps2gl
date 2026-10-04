/* Same X2C wire format and X2 body, with a decoder that evaluates the building
   haze per corner from the law carried in each buffer header. */
#include "GL/ps2gl.h"
#include "ps2gl/x2v_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

extern "C" {
void vsmGeneralClipTriX2VDecode_CodeStart();
void vsmGeneralClipTriX2VDecode_CodeEnd();
}

CClipTriX2VRenderer::CClipTriX2VRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipTriX2VDecode_CodeStart,
          (const u8*)vsmGeneralClipTriX2VDecode_CodeEnd -
              (const u8*)vsmGeneralClipTriX2VDecode_CodeStart,
          "clip x2v, paired wall colors with VU1 haze", PGL_CLIP_TRI_X2V_PROP, 4, 4, 2)
{
    ContextDeltaEligible = true;
    // No haze until the caller sets a law: at = 0 keeps every corner clear.
    static const float clear[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    SetBufferParams(clear);
}

static CClipTriX2VRenderer* pX2VRenderer = NULL;

void CClipTriX2VRenderer::Register()
{
    if (pX2VRenderer) return;
    pX2VRenderer = new CClipTriX2VRenderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2VRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2V,
        PGL_CLIP_TRI_X2V_PROP, ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void pglRegisterClipTriX2VRenderer(void)
{
    CClipTriX2VRenderer::Register();
}

bool pglClipX2VRegistered(void)
{
    return pX2VRenderer != NULL;
}

void pglClipX2VSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2VRenderer == NULL, "pglRegisterClipTriX2VRenderer() first");
    pX2VRenderer->SetWindowTexture(texId, r, g, b, a);
}

void pglClipX2VSetHaze(const GLfloat haze[8])
{
    mErrorIf(pX2VRenderer == NULL, "pglRegisterClipTriX2VRenderer() first");
    pX2VRenderer->SetHaze(haze);
}
