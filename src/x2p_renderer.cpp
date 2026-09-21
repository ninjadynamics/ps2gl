/* Whole-pool source clipping before triangulation, with owned inline input. */
#include "GL/ps2gl.h"
#include "ps2gl/x2p_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include <stddef.h>

typedef char PoolQuadSize[sizeof(PGLPoolQuad) == 64u ? 1 : -1];
typedef char PoolQuadUv[offsetof(PGLPoolQuad, uv) == 32u ? 1 : -1];
typedef char PoolQuadColor[offsetof(PGLPoolQuad, color) == 48u ? 1 : -1];

extern "C" {
void vsmGeneralClipPoolX2P_CodeStart();
void vsmGeneralClipPoolX2P_CodeEnd();
}

CClipPoolX2PRenderer::CClipPoolX2PRenderer()
    : CClipRoadX2RRenderer((void*)vsmGeneralClipPoolX2P_CodeStart,
          (u8*)vsmGeneralClipPoolX2P_CodeEnd - (u8*)vsmGeneralClipPoolX2P_CodeStart,
          "pool sky/source-view, owned compact quads", PGL_CLIP_POOL_X2P_PROP)
{
}

void CClipPoolX2PRenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetPoolRenderer()) return;
    manager.RegisterPoolRenderer(new CClipPoolX2PRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_POOL_QUADS_X2P, PGL_CLIP_POOL_X2P_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void CClipPoolX2PRenderer::DrawPoolQuads(const PGLPoolQuad* quads, int count)
{
    DrawCompactGroundQuads((const float*)quads, count, 16, 24);
}

void pglRegisterPoolRenderer(void)
{
    if (pGLContext) CClipPoolX2PRenderer::Register();
}

unsigned int pglGetPoolSubmissionOptions(void)
{
    return pGLContext && pGLContext->GetImmGeomManager().GetRendererManager().GetPoolRenderer()
        ? 1u : 0u;
}
