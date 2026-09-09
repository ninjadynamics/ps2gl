/* Textured per-vertex RGBA for geometry already clipped by the caller.
 * This is a separate primitive: fixed-function lighted models stay unchanged.
 */
#include "GL/ps2gl.h"
#include "ps2gl/unlit_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/texture.h"
#include "vu1_mem_linear.h"

extern "C" {
void vsmGeneralUnlitTexTri_CodeStart();
void vsmGeneralUnlitTexTri_CodeEnd();
}

CUnlitTexTriRenderer::CUnlitTexTriRenderer()
    : CLinearRenderer((void*)vsmGeneralUnlitTexTri_CodeStart,
        (u8*)vsmGeneralUnlitTexTri_CodeEnd - (u8*)vsmGeneralUnlitTexTri_CodeStart,
        4, 3, kInputStart, kInputBufSize - kInputStart, "unlit, textured RGBA, tris")
{
    CRendererProps caps = {
        PrimType : RendererProps::kTriangles, Lighting : 0, NumDirLights : 0, NumPtLights : 0,
        Texture : 1, Specular : 0, PerVtxMaterial : RendererProps::kDiffuse,
        Clipping : RendererProps::kNonClipped | RendererProps::kClipped, CullFace : 1,
        TwoSidedLighting : 0, ArrayAccess : RendererProps::kLinear
    };
    Capabilities = (uint64_t)caps | PGL_UNLIT_TEX_TRI_PROP;
    Requirements = PGL_UNLIT_TEX_TRI_PROP;
}

void CUnlitTexTriRenderer::Register()
{
    pglRegisterRenderer(new CUnlitTexTriRenderer);
    pglRegisterCustomPrimType(PGL_UNLIT_TEX_TRIANGLES, PGL_UNLIT_TEX_TRI_PROP,
        ~(pglU64_t)0xffffffff, PGL_MERGE_CONTIGUOUS);
}

void CUnlitTexTriRenderer::InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged)
{
    (void)primType;
    (void)rcChanges;
    (void)userRcChanged;
    InitUnlitContext();
}

void CUnlitTexTriRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    const int posWords = block.GetWordsPerVertex();
    const int uvWords = block.GetWordsPerTexCoord();
    const bool valid = block.GetVerticesAreValid() && block.GetTexCoordsAreValid()
        && block.GetColorsAreValid() && (posWords == 3 || posWords == 4)
        && (uvWords == 2 || uvWords == 3) && block.GetWordsPerColor() == 4
        && pGLContext->GetTexManager().GetTexEnabled();
    mErrorIf(!valid, "Unlit textured triangles require float XYZ, UV/STQ and RGBA arrays");
    if (!valid) return;

    block.SetNumVertsPerPrim(3);
    block.SetNumVertsToRestartStrip(0);
    block.SetStripsCanBeMerged(true);
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    InitXferBlock(packet, posWords, 0, uvWords, 4);
    // Leave the normal address hole intact, but never synthesize/transfer it.
    XferNormals = false;
    XferColors = true;
    XferTexCoords = true;
    int capacity = InputGeomBufSize / InputQuadsPerVert;
    if (capacity > 256) capacity = 256;
    capacity -= 3;
    capacity -= capacity % 6; // preserve the proven even triangle-list splitter
    DrawBlock(packet, block, capacity);
}

extern "C" void pglRegisterUnlitTexTriRenderer(void)
{
    CUnlitTexTriRenderer::Register();
}
