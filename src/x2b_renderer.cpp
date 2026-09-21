/* Compact billboards keep source-view clipping on VU1, including edge cards. */
#include "GL/ps2gl.h"
#include "ps2gl/x2b_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include <stddef.h>

typedef char BillboardContextSize[sizeof(PGLBillboardContext) == 128u ? 1 : -1];
typedef char BillboardQuadSize[sizeof(PGLBillboardQuad) == 48u ? 1 : -1];
typedef char BillboardUvOffset[offsetof(PGLBillboardQuad, uv) == 16u ? 1 : -1];
typedef char BillboardColorOffset[offsetof(PGLBillboardQuad, color) == 32u ? 1 : -1];
typedef char BillboardAxisOffset[offsetof(PGLBillboardContext, axisU) == 96u ? 1 : -1];
typedef char BillboardAlphaSize[sizeof(PGLBillboardAlphaQuad) == 64u ? 1 : -1];
typedef char BillboardAlphaOffset[offsetof(PGLBillboardAlphaQuad, alpha) == 48u ? 1 : -1];

extern "C" {
void vsmGeneralClipBillboardX2B_CodeStart();
void vsmGeneralClipBillboardX2B_CodeEnd();
}

CClipBillboardX2BRenderer::CClipBillboardX2BRenderer()
    : CClipRoadX2RRenderer((void*)vsmGeneralClipBillboardX2B_CodeStart,
          (u8*)vsmGeneralClipBillboardX2B_CodeEnd - (u8*)vsmGeneralClipBillboardX2B_CodeStart,
          "billboard source-view, owned compact quads", PGL_CLIP_BILLBOARD_X2B_PROP, 48)
{
}

void CClipBillboardX2BRenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetBillboardRenderer()) return;
    manager.RegisterBillboardRenderer(new CClipBillboardX2BRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_BILLBOARD_QUADS_X2B, PGL_CLIP_BILLBOARD_X2B_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

extern "C" {
void vsmGeneralClipBillboardX2A_CodeStart();
void vsmGeneralClipBillboardX2A_CodeEnd();
}

CClipBillboardAlphaX2ARenderer::CClipBillboardAlphaX2ARenderer()
    : CClipRoadX2RRenderer((void*)vsmGeneralClipBillboardX2A_CodeStart,
          (u8*)vsmGeneralClipBillboardX2A_CodeEnd - (u8*)vsmGeneralClipBillboardX2A_CodeStart,
          "billboard corner-alpha source-view", PGL_CLIP_BILLBOARD_X2A_PROP, 48)
{
}

void CClipBillboardAlphaX2ARenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetBillboardAlphaRenderer()) return;
    manager.RegisterBillboardAlphaRenderer(new CClipBillboardAlphaX2ARenderer);
    pglRegisterCustomPrimType(PGL_CLIP_BILLBOARD_QUADS_X2A, PGL_CLIP_BILLBOARD_X2A_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void pglRegisterBillboardRenderer(void)
{
    if (!pGLContext) return;
    CClipBillboardX2BRenderer::Register();
    CClipBillboardAlphaX2ARenderer::Register();
}

unsigned int pglGetBillboardSubmissionOptions(void)
{
    unsigned int options = 0;
    if (!pGLContext) return options;
    if (pGLContext->GetImmGeomManager().GetRendererManager().GetBillboardRenderer()) options |= 1u;
    if (pGLContext->GetImmGeomManager().GetRendererManager().GetBillboardAlphaRenderer()) options |= 2u;
    if (options & 3u) options |= 4u;
    return options;
}
