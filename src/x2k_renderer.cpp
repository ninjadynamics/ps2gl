/* Texture clouds: X2P sky/source-view clipping, planar UV, corner alpha. */
#include "GL/ps2gl.h"
#include "ps2gl/x2k_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include <stddef.h>

typedef char CloudQuadSize[sizeof(PGLCloudQuad) == 64u ? 1 : -1];
typedef char CloudQuadAlpha[offsetof(PGLCloudQuad, alpha) == 32u ? 1 : -1];
typedef char CloudQuadColor[offsetof(PGLCloudQuad, color) == 48u ? 1 : -1];

extern "C" {
void vsmGeneralClipCloudX2K_CodeStart();
void vsmGeneralClipCloudX2K_CodeEnd();
}

CClipCloudX2KRenderer::CClipCloudX2KRenderer()
    : CClipRoadX2RRenderer((void*)vsmGeneralClipCloudX2K_CodeStart,
          (u8*)vsmGeneralClipCloudX2K_CodeEnd - (u8*)vsmGeneralClipCloudX2K_CodeStart,
          "cloud sky/source-view, planar UV, corner alpha", PGL_CLIP_CLOUD_X2K_PROP)
{
}

void CClipCloudX2KRenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetCloudRenderer()) return;
    manager.RegisterCloudRenderer(new CClipCloudX2KRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_CLOUD_QUADS_X2K, PGL_CLIP_CLOUD_X2K_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void pglRegisterCloudRenderer(void)
{
    if (pGLContext) CClipCloudX2KRenderer::Register();
}
