/* Owned compact road input and one ordered sky-then-view VU program.
 * No source descriptor or mutable context pointer survives this API call.
 */
#include "GL/ps2gl.h"
#include "ps2gl/x2r_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/metrics.h"
#include "vu1_mem_linear.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char RoadContextSize[sizeof(PGLRoadContext) == 56u * 16u ? 1 : -1];
typedef char RoadQuadSize[sizeof(PGLRoadQuad) == 3u * 16u ? 1 : -1];
typedef char RoadSourcePlanesOffset[offsetof(PGLRoadContext, sourcePlanes) == 24u * 16u ? 1 : -1];
typedef char RoadBasisOffset[offsetof(PGLRoadContext, right) == 48u * 16u ? 1 : -1];
typedef char RoadEyeOffset[offsetof(PGLRoadContext, eyeIncircle) == 51u * 16u ? 1 : -1];
typedef char RoadCenterOffset[offsetof(PGLRoadContext, skyCenterScale) == 52u * 16u ? 1 : -1];
typedef char RoadClipOffset[offsetof(PGLRoadContext, clipParams) == 53u * 16u ? 1 : -1];
typedef char RoadColorOffset[offsetof(PGLRoadContext, color) == 54u * 16u ? 1 : -1];
typedef char RoadYOffset[offsetof(PGLRoadContext, sourceY) == 55u * 16u ? 1 : -1];

#if PGL_CITY_ROADS_VU1
extern "C" {
void vsmGeneralClipRoadX2R_CodeStart();
void vsmGeneralClipRoadX2R_CodeEnd();
}

#if kBackFaceCullMult != 0 || kClipToGsDepthOffset != 57 || \
    kVertexXfrm != 62 || kGifTag != 75 || kClipInfo != 76 || \
    kFogParams != 77 || kFogPad != 78 || kDoubleBufBase != 79 || \
    kDoubleBufOffset != 472 || kInputStart != 5
#error "Review road context and 472-qword halves against the VU ABI"
#endif

CClipRoadX2RRenderer::CClipRoadX2RRenderer()
    : CLinearRenderer((void*)vsmGeneralClipRoadX2R_CodeStart,
          (u8*)vsmGeneralClipRoadX2R_CodeEnd - (u8*)vsmGeneralClipRoadX2R_CodeStart,
          3, 3, 5, 96, "road sky/view, owned compact quads")
{
    // The public direct API admits all ordinary state; custom selection masks
    // it out so stale client-array formats cannot affect this private ABI.
    Capabilities = PGL_CLIP_ROAD_X2R_PROP;
    Requirements = PGL_CLIP_ROAD_X2R_PROP;
    memset(&RoadContext, 0, sizeof(RoadContext));
}

bool CClipRoadX2RRenderer::IsCodeValid() const
{
    return MicrocodePacketSize > 0 && MicrocodePacketSize <= 16384
        && (MicrocodePacketSize & 15) == 0
        && ((uintptr_t)MicrocodePacket & 15u) == 0;
}

void CClipRoadX2RRenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetRoadRenderer()) return;
    manager.RegisterRoadRenderer(new CClipRoadX2RRenderer);
    pglRegisterCustomPrimType(PGL_CLIP_ROAD_QUADS_X2R, PGL_CLIP_ROAD_X2R_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void CClipRoadX2RRenderer::SetRoadContext(const PGLRoadContext& context)
{
    memcpy(&RoadContext, &context, sizeof(RoadContext));
    pGLContext->SetRendererContextChanged(true);
}

void CClipRoadX2RRenderer::Load()
{
    // Generic Load also executes MSCAL0. This program must not latch its
    // context until InitContext has installed every road and standard field.
    pglInvalidateX2BasePrefix();
    pglInvalidateUnlitContextDelta();
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    packet.Cnt();
    packet.Flush();
    packet.Pad128();
    packet.CloseTag();
    unsigned int remaining = (unsigned int)MicrocodePacketSize / 8u;
    const u64* code = (const u64*)MicrocodePacket;
    unsigned int address = 0;
    while (remaining) {
        const unsigned int count = remaining > 256u ? 256u : remaining;
        packet.Ref(code, count / 2u);
        packet.Pad96();
        packet.Mpg(count & 255u, address);
        code += count;
        remaining -= count;
        address += count;
    }
}

void CClipRoadX2RRenderer::InitContext(GLenum primType, uint32_t rcChanges,
    bool userRcChanged)
{
    (void)primType;
    (void)rcChanges;
    (void)userRcChanged;
    pglInvalidateUnlitContextDelta();
    CImmDrawContext& draw = pGLContext->GetImmDrawContext();
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
#if PGL_SUBMISSION_METRICS
    const unsigned int start = packet.GetByteLength();
#endif
    packet.Cnt();
    packet.Stcycl(1, 1);
    packet.Stmod(Vifs::AddModes::kNone);
    packet.Flush();
    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, 0, Packet::kSingleBuff);
    packet += (uint64_t)0;
    packet += 0;
    const float cull = (float)draw.GetCullFaceDir();
    unsigned int cullWord;
    memcpy(&cullWord, &cull, sizeof(cullWord));
    packet += cullWord | ((unsigned int)draw.GetDoCullFace() << 5);
    // q1..56 are one contiguous, owned float copy after q0. No REF to the
    // renderer's mutable RoadContext is allowed.
    packet.Add((const float*)&RoadContext, sizeof(RoadContext) / sizeof(float));
    packet.CloseUnpack();

    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, kClipToGsDepthOffset, Packet::kSingleBuff);
    packet += 0.0f;
    packet += 0.0f;
    packet += 0.0f;
    packet += draw.GetContextDepthScale() + draw.GetDepthOffset();
    packet.CloseUnpack();

    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, kVertexXfrm, Packet::kSingleBuff);
    packet += draw.GetVertexXform();
    packet.CloseUnpack();

    // Restore the ordinary unlit raster contract after all road inputs and
    // before entry; q78 remains sacrificial, q79 begins the first input half.
    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, kGifTag, Packet::kSingleBuff);
    packet += BuildGiftag(GL_TRIANGLES);
    const cpu_vec_xyz& scales = draw.GetContextClipScales();
    packet += scales.x;
    packet += scales.y;
    packet += scales.z;
    packet += draw.GetDoClipping() ? 1 : 0;
    packet += draw.GetClipNear();
    packet += 0.0f;
    packet += 0.0f;
    packet += 0.0f;
    packet += (uint64_t)0;
    packet += (uint64_t)0;
    packet.CloseUnpack();
    packet.Mscal(0);
    packet.Flushe();
    packet.Base(kDoubleBufBase);
    packet.Offset(kDoubleBufOffset);
    packet.CloseTag();
    CacheRendererState();
    pglCountSubmission(PGL_SUBMIT_FULL_CONTEXTS);
#if PGL_SUBMISSION_METRICS
    pglCountSubmission(PGL_SUBMIT_CONTEXT_BYTES, packet.GetByteLength() - start);
#endif
}

void CClipRoadX2RRenderer::DrawRoadQuads(const PGLRoadQuad* quads, int count)
{
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    static const float independentAdc[16] = { 1024.0f };
    pglCountSubmission(PGL_SUBMIT_BLOCKS);
    while (count > 0) {
        const int batch = count > 32 ? 32 : count;
        packet.Cnt();
        packet.Stcycl(1, 1);
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 5, Packet::kDoubleBuff);
        packet.Add((const float*)quads, (unsigned int)batch * 12u);
        packet.CloseUnpack();
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 0, Packet::kDoubleBuff);
        packet += batch;
        packet += 0;
        packet += (uint64_t)0;
        packet.Add(independentAdc, 16);
        packet.CloseUnpack();
        packet.Mscnt();
        packet.Pad128();
        packet.CloseTag();
        pglCountSubmission(PGL_SUBMIT_BUFFERS);
        pglCountSubmission(PGL_SUBMIT_EDGE_BYTES, (unsigned int)batch * sizeof(PGLRoadQuad));
        quads += batch;
        count -= batch;
    }
}

void CClipRoadX2RRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    (void)block;
    // The only supported entrance has already admitted and copied the whole
    // material. A generic array call cannot supply the private context ABI.
    fprintf(stderr, "ps2gl: road renderer requires pglDrawRoadQuads\n");
    abort();
}
#endif

extern "C" void pglRegisterRoadRenderer(void)
{
#if PGL_CITY_ROADS_VU1
    if (pGLContext) CClipRoadX2RRenderer::Register();
#endif
}
