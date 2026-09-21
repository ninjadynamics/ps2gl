/* Owned compact road input and one ordered sky-then-view VU program.
 * No source descriptor or mutable context pointer survives this API call.
 */
#include "GL/ps2gl.h"
#include "ps2gl/x2r_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/metrics.h"
#include "ps2gl/owned_payload.h"
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
typedef char RoadGiftagSize[sizeof(tGifTag) == 16u ? 1 : -1];
typedef char RoadMatrixBytes[sizeof(cpu_mat_44) == 16u * sizeof(float) ? 1 : -1];

#if PGL_CITY_ROADS_VU1 || PGL_CITY_POOLS_VU1 || PGL_CITY_BILLBOARDS_VU1 || PGL_CITY_BILLBOARD_CORNER_ALPHA
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
    : CClipRoadX2RRenderer((void*)vsmGeneralClipRoadX2R_CodeStart,
          (u8*)vsmGeneralClipRoadX2R_CodeEnd - (u8*)vsmGeneralClipRoadX2R_CodeStart,
          "road sky/view, owned compact quads", PGL_CLIP_ROAD_X2R_PROP)
{
}

CClipRoadX2RRenderer::CClipRoadX2RRenderer(void* code, int codeSize,
    const char* name, uint64_t prop, unsigned int contextFirstQuad)
    : CLinearRenderer(code, codeSize, 3, 3, 5, 96, name)
    , ContextFirstQuad(contextFirstQuad)
    , HasRoadContext(false)
    , RoadContextUnchanged(false)
    , SourcePrefixUnchanged(false)
    , RetainedPacket(NULL)
    , RetainedBase(NULL)
    , RetainedEnd(NULL)
    , RetainedFrame(0)
    , RetainedContextValid(false)
{
    // The public direct API admits all ordinary state; custom selection masks
    // it out so stale client-array formats cannot affect this private ABI.
    Capabilities = prop;
    Requirements = prop;
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

bool CClipRoadX2RRenderer::MatchesRoadContext(const PGLRoadContext& context) const
{
    return MatchesSourceContext(&context);
}

bool CClipRoadX2RRenderer::MatchesSourceContext(const void* context) const
{
#if PGL_ROAD_CONTEXT_REUSE
    const unsigned int offset = ContextFirstQuad * 16u;
#if PGL_SOURCE_CONTEXT_TAIL_PATCH
    // A material normally changes this tail. Compare it first so that the
    // prefix is examined only once, below or by SetSourceContext on a miss.
    const unsigned int prefix = offsetof(PGLRoadContext, color) - offset;
    return HasRoadContext
        && memcmp(RoadContext.color, (const char*)context + prefix, 32u) == 0
        && memcmp((const char*)&RoadContext + offset, context, prefix) == 0;
#else
    return HasRoadContext && memcmp((const char*)&RoadContext + offset,
        context, sizeof(RoadContext) - offset) == 0;
#endif
#else
    (void)context;
    return false;
#endif
}

void CClipRoadX2RRenderer::SetRoadContext(const PGLRoadContext& context, bool unchanged)
{
    SetSourceContext(&context, unchanged);
}

void CClipRoadX2RRenderer::SetSourceContext(const void* context, bool unchanged)
{
    RoadContextUnchanged = unchanged;
    const unsigned int offset = ContextFirstQuad * 16u;
#if PGL_SOURCE_CONTEXT_TAIL_PATCH
    const unsigned int prefix = offsetof(PGLRoadContext, color) - offset;
    // Capture equality before updating the cached public context. This is
    // only an EE byte proof; InitContext separately proves live VU ownership.
    SourcePrefixUnchanged = unchanged || (HasRoadContext
        && memcmp((const char*)&RoadContext + offset, context, prefix) == 0);
    if (SourcePrefixUnchanged) {
        if (!unchanged) memcpy(RoadContext.color, (const char*)context + prefix, 32u);
    } else
#endif
    if (!unchanged) memcpy((char*)&RoadContext + offset,
        context, sizeof(RoadContext) - offset);
    HasRoadContext = true;
    pGLContext->SetRendererContextChanged(true);
}

void CClipRoadX2RRenderer::GetRasterContextKey(uint32_t* key)
{
    CImmDrawContext& draw = pGLContext->GetImmDrawContext();
    const cpu_mat_44& transform = draw.GetVertexXform();
    // Object-byte copies preserve signed zero and avoid float/integer aliasing.
    memcpy(key, &transform, 16u * sizeof(float));
    const cpu_vec_xyz& scales = draw.GetContextClipScales();
    const float scalars[5] = { draw.GetContextDepthScale() + draw.GetDepthOffset(),
        scales.x, scales.y, scales.z, draw.GetClipNear() };
    memcpy(key + 16, scalars, sizeof(scalars));
    const float cull = (float)draw.GetCullFaceDir();
    memcpy(key + 21, &cull, sizeof(cull));
    key[21] |= (unsigned int)draw.GetDoCullFace() << 5;
    key[22] = draw.GetDoClipping() ? 1u : 0u;
    const tGifTag tag = BuildGiftag(GL_TRIANGLES);
    memcpy(key + 23, &tag, sizeof(tag));
}

void CClipRoadX2RRenderer::Load()
{
    // Generic Load also executes MSCAL0. This program must not latch its
    // context until InitContext has installed every road and standard field.
    pglInvalidateX2BasePrefix();
    pglInvalidateUnlitContextDelta();
    RetainedContextValid = false;
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
#if PGL_ROAD_CONTEXT_REUSE || PGL_SOURCE_CONTEXT_TAIL_PATCH
    uint32_t rasterKey[27];
    GetRasterContextKey(rasterKey);
    // This is an ordered-chain proof, not a cross-frame VU RAM cache. No
    // intervening packet command may have overwritten context or VIF state.
    // A material bind can change GS settings without changing these inputs;
    // SyncGsContext still fences and sends that material after this returns.
    if (RetainedContextValid
#if PGL_SOURCE_CONTEXT_TAIL_PATCH
        && SourcePrefixUnchanged
#else
        && RoadContextUnchanged
#endif
        && RetainedPacket == &packet && RetainedBase == packet.GetBase()
        && RetainedEnd == packet.GetNextPtr()
        && RetainedFrame == pGLContext->GetFrameNumber()
        && &pGLContext->GetImmGeomManager().GetRendererManager().GetCurRenderer() == this
        && memcmp(RetainedRaster, rasterKey, sizeof(rasterKey)) == 0) {
#if PGL_ROAD_CONTEXT_REUSE
        if (RoadContextUnchanged) {
            CacheRendererState();
            pglCountSubmission(PGL_SUBMIT_DELTA_CONTEXTS);
            return;
        }
#endif
#if PGL_SOURCE_CONTEXT_TAIL_PATCH
        if (SourcePrefixUnchanged) {
#if PGL_SUBMISSION_METRICS
            const unsigned int start = packet.GetByteLength();
#endif
            // Every source program reloads q55..56 after --cont. Its entry
            // matrix (and X2R near value) remains live across MSCNT; the exact
            // raster key above proves those values need no MSCAL0 reload.
            // Keep TOP/DBF progressing and fence the prior context readers.
            packet.Cnt();
            packet.Stcycl(1, 1);
            packet.Stmod(Vifs::AddModes::kNone);
            packet.Flush();
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32, 55, Packet::kSingleBuff);
            pglAddOwnedPayload(packet, RoadContext.color, 8u);
            packet.CloseUnpack();
            packet.CloseTag();
            CacheRendererState();
            pglCountSubmission(PGL_SUBMIT_DELTA_CONTEXTS);
#if PGL_SUBMISSION_METRICS
            pglCountSubmission(PGL_SUBMIT_CONTEXT_BYTES, packet.GetByteLength() - start);
#endif
            return;
        }
#endif
    }
#endif
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
    // Roads/pools own q1..56. Billboards only read q49..56, leaving the
    // unused sky-plane slots untouched. No REF to mutable renderer storage.
    if (ContextFirstQuad) {
        packet.CloseUnpack();
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32,
            ContextFirstQuad + 1u, Packet::kSingleBuff);
    }
    if (ContextFirstQuad == 48u) {
        // The public billboard context reserves q54.w=0. This private flag
        // belongs only to X2B/X2A; retain the untouched public context cache.
        // Copy once into the open packet, then override only its owned word.
        // Add does not publish/send this payload before CloseUnpack below.
        float* const billboardContext = pglAddOwnedPayload(packet,
            (const float*)&RoadContext + ContextFirstQuad * 4u, 32u);
        const uint32_t reuse = PGL_CITY_BILLBOARD_CORNER_REUSE ? 1u : 0u;
        memcpy((char*)billboardContext + 5u * 16u + 12u, &reuse, sizeof(reuse));
    } else {
        pglAddOwnedPayload(packet, (const float*)&RoadContext + ContextFirstQuad * 4u,
            (56u - ContextFirstQuad) * 4u);
    }
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
#if PGL_ROAD_CONTEXT_REUSE || PGL_SOURCE_CONTEXT_TAIL_PATCH
    memcpy(RetainedRaster, rasterKey, sizeof(rasterKey));
    RetainedContextValid = true;
#endif
    pglCountSubmission(PGL_SUBMIT_FULL_CONTEXTS);
#if PGL_SUBMISSION_METRICS
    pglCountSubmission(PGL_SUBMIT_CONTEXT_BYTES, packet.GetByteLength() - start);
#endif
}

void CClipRoadX2RRenderer::DrawRoadQuads(const PGLRoadQuad* quads, int count)
{
    DrawCompactGroundQuads((const float*)quads, count, 12, 32);
}

void CClipRoadX2RRenderer::DrawCompactGroundQuads(const float* quads, int count,
    int floatsPerQuad, int quadsPerBuffer)
{
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
#if !PGL_ROAD_HEADER_COMPACT
    static const float independentAdc[16] = { 1024.0f };
#endif
    pglCountSubmission(PGL_SUBMIT_BLOCKS);
#if PGL_ROAD_HEADER_COMPACT
    // A whole material already fits the normal <=65000q frame chain. One
    // CNT can therefore own all activations; MSCNT and alternating TOP halves
    // retain exactly the same VIF/VU producer-consumer synchronization.
    packet.Cnt();
    packet.Stcycl(1, 1);
#endif
    while (count > 0) {
        const int batch = count > quadsPerBuffer ? quadsPerBuffer : count;
#if !PGL_ROAD_HEADER_COMPACT
        packet.Cnt();
        packet.Stcycl(1, 1);
#endif
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 5, Packet::kDoubleBuff);
        pglAddOwnedPayload(packet, quads,
            (unsigned int)batch * (unsigned int)floatsPerQuad);
        pglCloseOwnedV4Unpack(packet,
            (unsigned int)batch * (unsigned int)floatsPerQuad / 4u);
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 0, Packet::kDoubleBuff);
        packet += batch;
        packet += 0;
        packet += (uint64_t)0;
#if !PGL_ROAD_HEADER_COMPACT
        packet.Add(independentAdc, 16);
#endif
        pglCloseOwnedV4Unpack(packet, PGL_ROAD_HEADER_COMPACT ? 1u : 5u);
        packet.Mscnt();
        packet.Pad128();
#if !PGL_ROAD_HEADER_COMPACT
        packet.CloseTag();
#endif
        pglCountSubmission(PGL_SUBMIT_BUFFERS);
        pglCountSubmission(PGL_SUBMIT_EDGE_BYTES,
            (unsigned int)batch * (unsigned int)floatsPerQuad * sizeof(float));
        quads += batch * floatsPerQuad;
        count -= batch;
    }
#if PGL_ROAD_HEADER_COMPACT
    packet.CloseTag();
#endif
#if PGL_ROAD_CONTEXT_REUSE || PGL_SOURCE_CONTEXT_TAIL_PATCH
    RetainedPacket = &packet;
    RetainedBase = packet.GetBase();
    RetainedEnd = packet.GetNextPtr();
    RetainedFrame = pGLContext->GetFrameNumber();
#endif
}

void CClipRoadX2RRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    (void)block;
    // The only supported entrance has already admitted and copied the whole
    // material. A generic array call cannot supply the private context ABI.
    fprintf(stderr, "ps2gl: compact renderer requires its owned descriptor API\n");
    abort();
}
#endif

extern "C" void pglRegisterRoadRenderer(void)
{
#if PGL_CITY_ROADS_VU1
    if (pGLContext) CClipRoadX2RRenderer::Register();
#endif
}

extern "C" unsigned int pglGetRoadSubmissionOptions(void)
{
#if PGL_CITY_ROADS_VU1
    return (PGL_ROAD_CONTEXT_REUSE ? 1u : 0u)
        | (PGL_ROAD_HEADER_COMPACT ? 2u : 0u)
        | (PGL_COMPACT_QWORD_COPY ? 4u : 0u)
        | (PGL_COMPACT_FIXED_UNPACK_COUNT ? 8u : 0u);
#else
    return 0;
#endif
}

extern "C" unsigned int pglGetSourceContextSubmissionOptions(void)
{
#if PGL_CITY_ROADS_VU1 || PGL_CITY_POOLS_VU1 || PGL_CITY_BILLBOARDS_VU1 || PGL_CITY_BILLBOARD_CORNER_ALPHA
    return (PGL_SOURCE_CONTEXT_TAIL_PATCH ? 1u : 0u)
        | (PGL_SOURCE_FIXED_BATCH_COUNT ? 2u : 0u);
#else
    return 0;
#endif
}
