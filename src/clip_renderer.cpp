/* HyperSolar VU1 near-plane clip renderer (tri lists). See clip_renderer.h. */

#include "GL/ps2gl.h"

#include "ps2gl/clip_renderer.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

#include "vu1_mem_linear.h"

#define VU_FUNCTIONS(name)        \
    void vsm##name##_CodeStart(); \
    void vsm##name##_CodeEnd()

#define mVsmAddr(name) ((void*)vsm##name##_CodeStart)
#define mVsmSize(name) ((u8*)vsm##name##_CodeEnd - (u8*)vsm##name##_CodeStart)

extern "C" {
VU_FUNCTIONS(GeneralClipTri);
}

using namespace RendererProps;

CClipTriRenderer::CClipTriRenderer()
    : CLinearRenderer(mVsmAddr(GeneralClipTri), mVsmSize(GeneralClipTri), 3, 3,
          kInputStart, 90,
          "clip, tris, no specular")
{
    // 90 quads = 30 verts of input region: the microcode owns its own
    // buffer layout (input + plane/polygon scratch + a 113-vert output
    // region — see general_clip_tri.vcl's header). Changing this number
    // means re-checking that layout end to end.
    // Only reachable through PGL_CLIP_TRIANGLES: the prim's renderer-req mask
    // hides every standard state bit, so matching happens on the UserProps
    // flag alone. The standard caps below just document what the microcode
    // actually implements (the general_nospec_tri pipeline).
    CRendererProps caps = {
        PrimType : kTriangles,
        Lighting : 1,
        NumDirLights : k3DirLights | k8DirLights,
        NumPtLights : k1PtLight | k2PtLights | k8PtLights,
        Texture : 1,
        Specular : 0,
        PerVtxMaterial : kNoMaterial,
        Clipping : kNonClipped | kClipped,
        CullFace : 1,
        TwoSidedLighting : 0,
        ArrayAccess : kLinear
    };

    Capabilities = (uint64_t)caps | PGL_CLIP_TRI_PROP;
    Requirements = PGL_CLIP_TRI_PROP;
}

void CClipTriRenderer::Register()
{
    pglRegisterRenderer(new CClipTriRenderer);

    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES,
        PGL_CLIP_TRI_PROP,
        ~(pglU64_t)0xffffffff, // match on the custom bits only
        PGL_MERGE_CONTIGUOUS);
}

void CClipTriRenderer::InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged)
{
    // The context/giftag builder doesn't know the custom prim enum; the
    // microcode consumes plain tri lists (tri-strip prim + ADC on the first
    // two verts of each tri, like general_nospec_tri), which is exactly what
    // GL_TRIANGLES builds.
    CLinearRenderer::InitContext(GL_TRIANGLES, rcChanges, userRcChanged);
}

void CClipTriRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    // CommitPrimType only fills these for the standard GL prims; a custom
    // prim leaves them unset (same situation as the billboard example) —
    // including StripsCanBeMerged, which would otherwise carry a stale value
    // from whatever standard prim drew last.
    block.SetNumVertsPerPrim(3);
    block.SetNumVertsToRestartStrip(0);
    block.SetStripsCanBeMerged(true);

    // Local copy of CLinearRenderer::DrawLinearArrays with one change: the
    // per-buffer vert cap is rounded to a multiple of SIX, not just of 3.
    // FindNumBuffers/DrawBlock subtract 1 from ODD split points ("even verts
    // for spilled strips"), and a 3n-1 buffer sends the microcode's
    // 3-verts-per-iteration loop straight past its pointer-equality bound —
    // an infinite VU1 loop (hard boot hang, DMA busy + VEW). The stock tri
    // renderers dodge it by luck (their 72-vert cap is already even).
    int wordsPerVert   = block.GetWordsPerVertex();
    int wordsPerNormal = (block.GetNormalsAreValid()) ? block.GetWordsPerNormal() : 0;
    int wordsPerTex    = (block.GetTexCoordsAreValid()) ? block.GetWordsPerTexCoord() : 0;
    int wordsPerColor  = (block.GetColorsAreValid()) ? block.GetWordsPerColor() : 0;

    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    InitXferBlock(packet, wordsPerVert, wordsPerNormal, wordsPerTex, wordsPerColor);

    int maxVertsPerBuffer = InputGeomBufSize / InputQuadsPerVert;
    if (maxVertsPerBuffer > 256)
        maxVertsPerBuffer = 256;
    maxVertsPerBuffer -= 3;
    maxVertsPerBuffer -= maxVertsPerBuffer % 6;

    DrawBlock(packet, block, maxVertsPerBuffer);
}

/********************************************
 * ps2gl C api
 */

/**
 * Register the VU1 clip-tri renderer and its PGL_CLIP_TRIANGLES prim type.
 * Call once after pglInit(); afterwards glDrawArrays(PGL_CLIP_TRIANGLES, ...)
 * routes tri lists through the clip microcode.
 */
void pglRegisterClipTriRenderer(void)
{
    CClipTriRenderer::Register();
}

/**
 * Eye-space near plane for the clip renderer (default 1.0). Set it to the
 * projection's near value; verts at eye depth < near classify as outside.
 */
void pglSetClipNear(float near_z)
{
    pGLContext->GetImmDrawContext().SetClipNear(near_z);
}
