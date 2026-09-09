/* X2Q only decodes the caller's four exact corners and attributes. The
   existing X2 body owns transforms, clipping, depth, fog and compound kicks. */
#include "GL/ps2gl.h"
#include "ps2gl/x2q_renderer.h"

extern "C" {
void vsmGeneralClipTriX2QDecode_CodeStart();
void vsmGeneralClipTriX2QDecode_CodeEnd();
}

CClipTriX2QRenderer::CClipTriX2QRenderer()
    : CClipTriX2DRenderer((const void*)vsmGeneralClipTriX2QDecode_CodeStart,
          (const u8*)vsmGeneralClipTriX2QDecode_CodeEnd -
              (const u8*)vsmGeneralClipTriX2QDecode_CodeStart,
          "clip x2q, exact wall corners", PGL_CLIP_TRI_X2Q_PROP, 4, 4)
{
}

static CClipTriX2QRenderer* pX2QRenderer = NULL;

void CClipTriX2QRenderer::Register()
{
    pX2QRenderer = new CClipTriX2QRenderer;
    pglRegisterRenderer(pX2QRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2Q,
        PGL_CLIP_TRI_X2Q_PROP, ~(pglU64_t)0xffffffff, PGL_MERGE_CONTIGUOUS);
}

void pglRegisterClipTriX2QRenderer(void)
{
    CClipTriX2QRenderer::Register();
}

void pglClipX2QSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2QRenderer == NULL, "pglRegisterClipTriX2QRenderer() first");
    pX2QRenderer->SetWindowTexture(texId, r, g, b, a);
}
