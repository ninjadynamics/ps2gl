/* Exact authored ABCD source arrays, shared transform/projection on VU1. */
#include "GL/ps2gl.h"
#include "ps2gl/x2f_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include <string.h>

#if PGL_CITY_SOURCE_QUADS
extern "C" {
void vsmGeneralClipQuadX2F_CodeStart();
void vsmGeneralClipQuadX2F_CodeEnd();
}

CClipQuadX2FRenderer::CClipQuadX2FRenderer()
    : CClipTriX2Renderer((void*)vsmGeneralClipQuadX2F_CodeStart,
        (u8*)vsmGeneralClipQuadX2F_CodeEnd - (u8*)vsmGeneralClipQuadX2F_CodeStart,
        "clip x2f, four authored source corners", PGL_CLIP_QUAD_X2F_PROP)
{
    ContextDeltaEligible = true;
    InputQuadsPerVert = 3;
}

void CClipQuadX2FRenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetSourceQuadRenderer()) return;
    // This complete program is not the shared X2 instruction prefix. The
    // generic registration invalidates that prefix before loading X2F.
    manager.RegisterSourceQuadRenderer(new CClipQuadX2FRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_QUADS_X2F, PGL_CLIP_QUAD_X2F_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void CClipQuadX2FRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    block.SetNumVertsPerPrim(4);
    block.SetNumVertsToRestartStrip(0);
    block.SetStripsCanBeMerged(true);
    mErrorIf(block.GetWordsPerVertex() != 3 || !block.GetTexCoordsAreValid()
        || block.GetWordsPerTexCoord() != 2 || !block.GetColorsAreValid()
        || block.GetWordsPerColor() != 4, "x2f: XYZ3/UV2/floatRGBA4 required");
    for (int strip = 0; strip < block.GetNumStrips(); ++strip)
        mErrorIf(block.GetStripLength(strip) % 4 || block.StripIsContinued(strip),
            "x2f: complete independent ABCD source quads required");
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    InitXferBlock(packet, 3, 0, 2, 4);
    XferNormals = false;
    XferColors = true;
    BuildPrefixes(packet, block);
    // X2F never binds a paired window. Its unused color y/z fields carry
    // capacities and w selects shared clip dispatch. Existing VU FTOI0
    // lands these exact small integers in an already-live VF.
    // Each activation owns its copied prefix; no new VF/VI lifetime or loads.
    const float outputOptions[3] = {
        PGL_CITY_SOURCE_SINGLE_MATERIAL_OUTPUT ? 60.0f : 30.0f,
        PGL_CITY_SOURCE_SINGLE_MATERIAL_OUTPUT ? 36.0f : 18.0f,
        PGL_CITY_SOURCE_CLIP_DISPATCH ? 1.0f : 0.0f
    };
    memcpy((unsigned char*)&Pfx[0] + 4, outputOptions, sizeof(outputOptions));
    // Private absolute q1..22 holds the per-quad cache. The double-buffered
    // input can fill q5..124 as XYZ/STQ/RGBA (ten complete ABCD quads),
    // ending before the unchanged clip planes at q125. No source restart or
    // paired-material ordering boundary exists in this single-material path.
    const int cornersPerBuffer = PGL_CITY_SOURCE_TEN_QUADS ? 40 : 32;
    packet.Cnt();
    packet.Stcycl(1, InputQuadsPerVert);
    packet.Pad128();
    packet.CloseTag();
    int buffered = 0;
    for (int strip = 0; strip < block.GetNumStrips(); ++strip) {
        const int length = block.GetStripLength(strip);
        for (int first = 0; first < length;) {
            const int room = cornersPerBuffer - buffered;
            const int count = length - first < room ? length - first : room;
            const int offset = InputGeomOffset + buffered * InputQuadsPerVert;
            XferVectors(packet, (unsigned int*)block.GetVertices(strip),
                first, count, WordsPerVertex, VertexUnpackMask,
                VertexUnpackMode, offset);
            XferVectors(packet, (unsigned int*)block.GetTexCoords(strip),
                first, count, WordsPerTexCoord, TexCoordUnpackMask,
                TexCoordUnpackMode, offset + 1);
            XferVectors(packet, (unsigned int*)block.GetColors(strip),
                first, count, WordsPerColor, ColorUnpackMask,
                ColorUnpackMode, offset + 2);
            buffered += count;
            first += count;
            if (buffered == cornersPerBuffer) {
                XferPrefixes(packet);
                FinishBuffer(packet, 0, buffered, InputQuadsPerVert, 0, NULL);
                buffered = 0;
            }
        }
    }
    if (buffered) {
        XferPrefixes(packet);
        FinishBuffer(packet, 0, buffered, InputQuadsPerVert, 0, NULL);
    }
    RememberContextEnd();
}
#endif

void pglRegisterClipQuadX2FRenderer(void)
{
#if PGL_CITY_SOURCE_QUADS
    if (pGLContext) CClipQuadX2FRenderer::Register();
#endif
}

unsigned int pglGetSourceQuadSubmissionOptions(void)
{
#if PGL_CITY_SOURCE_QUADS
    if (!pGLContext || !pGLContext->GetImmGeomManager().GetRendererManager()
        .GetSourceQuadRenderer()) return 0u;
    return 1u | (PGL_CITY_SOURCE_SINGLE_MATERIAL_OUTPUT ? 2u : 0u)
        | (PGL_CITY_SOURCE_CLIP_DISPATCH ? 4u : 0u)
        | (PGL_CITY_SOURCE_TEN_QUADS ? 8u : 0u);
#else
    return 0u;
#endif
}
