/* Owned compact decal input and the accepted homogeneous depth adapter.
 * No source descriptor or mutable context pointer survives this API call.
 */
#include "GL/ps2gl.h"
#include "ps2gl/x2e_renderer.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/metrics.h"
#include "ps2gl/owned_payload.h"
#include "vu1_mem_linear.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char DecalContextSize[sizeof(PGLDecalContext) == 8u * 16u ? 1 : -1];
typedef char DecalQuadSize[sizeof(PGLDecalQuad) == 9u * 16u ? 1 : -1];
typedef char DecalNdcVertexSize[sizeof(PGLDecalNdcVertex) == 3u * 16u ? 1 : -1];
typedef char DecalNdcTriangleSize[sizeof(PGLDecalNdcTriangle) == sizeof(PGLDecalQuad) ? 1 : -1];
typedef char DecalRasterOffset[offsetof(PGLDecalContext, raster) == 4u * 16u ? 1 : -1];
typedef char DecalInverseOffset[offsetof(PGLDecalContext, inverse) == 5u * 16u ? 1 : -1];
typedef char DecalDepthOffset[offsetof(PGLDecalContext, depth) == 6u * 16u ? 1 : -1];
typedef char DecalClipOffset[offsetof(PGLDecalContext, clip) == 7u * 16u ? 1 : -1];
typedef char DecalColorOffset[offsetof(PGLDecalQuad, colors) == 3u * 16u ? 1 : -1];
typedef char DecalZwOffset[offsetof(PGLDecalQuad, zw) == 5u * 16u ? 1 : -1];
typedef char DecalErrorOffset[offsetof(PGLDecalQuad, depth) == 7u * 16u ? 1 : -1];

#if PGL_CITY_ENTRANCES_VU1
extern "C" {
void vsmGeneralClipDecalX2E_CodeStart();
void vsmGeneralClipDecalX2E_CodeEnd();
}

#if kBackFaceCullMult != 0 || kClipToGsDepthOffset != 57 || \
    kVertexXfrm != 62 || kGifTag != 75 || kClipInfo != 76 || \
    kFogParams != 77 || kFogPad != 78 || kDoubleBufBase != 79 || \
    kDoubleBufOffset != 472 || kInputStart != 5
#error "Review decal context and 472-qword halves against the VU ABI"
#endif

CClipDecalX2ERenderer::CClipDecalX2ERenderer()
    : CLinearRenderer((void*)vsmGeneralClipDecalX2E_CodeStart,
          (u8*)vsmGeneralClipDecalX2E_CodeEnd - (u8*)vsmGeneralClipDecalX2E_CodeStart,
          9, 3, 5, 144, "coplanar decals, owned compact quads")
{
    // The public direct API admits all ordinary state; custom selection masks
    // it out so stale client-array formats cannot affect this private ABI.
    Capabilities = PGL_CLIP_DECAL_X2E_PROP;
    Requirements = PGL_CLIP_DECAL_X2E_PROP;
    memset(&DecalContext, 0, sizeof(DecalContext));
    RegionMaterials = false;
}

bool CClipDecalX2ERenderer::IsCodeValid() const
{
    return MicrocodePacketSize > 0 && MicrocodePacketSize <= 16384
        && (MicrocodePacketSize & 15) == 0
        && ((uintptr_t)MicrocodePacket & 15u) == 0;
}

void CClipDecalX2ERenderer::Register()
{
    CRendererManager& manager = pGLContext->GetImmGeomManager().GetRendererManager();
    if (manager.GetDecalRenderer()) return;
    manager.RegisterDecalRenderer(new CClipDecalX2ERenderer);
    pglRegisterCustomPrimType(PGL_CLIP_DECAL_QUADS_X2E, PGL_CLIP_DECAL_X2E_PROP,
        ~(pglU64_t)0xffffffff, PGL_DONT_MERGE_CONTIGUOUS);
}

void CClipDecalX2ERenderer::SetDecalContext(const PGLDecalContext& context, bool glow,
    bool regionMaterials)
{
    memcpy(&DecalContext, &context, sizeof(DecalContext));
    DecalContext.depth[3] = glow ? 1.0f : 0.0f;
    RegionMaterials = regionMaterials;
    // Public source clip.z/w remain zero. Only this linked module chooses the
    // private decoder path; old applications do not need a new descriptor ABI.
    DecalContext.clip[2] = PGL_DECAL_XY_REUSE ? 1.0f : 0.0f;
    DecalContext.clip[3] = PGL_DECAL_TRIVIAL_ACCEPT
        ? (PGL_DECAL_CLIP_PREFIX_REUSE ? 3.0f : 1.0f) : 0.0f;
    pGLContext->SetRendererContextChanged(true);
}

void CClipDecalX2ERenderer::Load()
{
    // Generic Load also executes MSCAL0. This program must not latch its
    // context until InitContext has installed every decal and standard field.
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

void CClipDecalX2ERenderer::InitContext(GLenum primType, uint32_t rcChanges,
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
    // q1..8 are one contiguous, owned float copy after q0. No REF to the
    // renderer's mutable DecalContext is allowed.
    pglAddOwnedPayload(packet, (const float*)&DecalContext,
        sizeof(DecalContext) / sizeof(float));
    if (RegionMaterials) {
        // Private q9..13 are unused by the ordinary decal program. One
        // PACKED CLAMP_1 primitive precedes each material's geometry tag.
        // EOP=0 keeps it in the same PATH1 packet as the EOP=1 vertex tag.
        packet += (uint64_t)1 | ((uint64_t)1 << 60);
        packet += (uint64_t)0x08;
        for (unsigned int material = 0; material < 4; ++material) {
            const uint64_t minV = material * 32u;
            const uint64_t maxV = minV + 31u;
            // U REPEAT, V REGION_CLAMP. The GS scales V bounds per mip.
            packet += (uint64_t)8 | (minV << 24) | (maxV << 34);
            packet += (uint64_t)0;
        }
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

    // Restore the ordinary unlit raster contract after all decal inputs and
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

void CClipDecalX2ERenderer::DrawDecalRecords(const void* records, int count,
    unsigned int format, const float** ownedPayloads, bool reusePayload,
    const unsigned char* materials)
{
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    const float* source = (const float*)records;
#if !PGL_DECAL_HEADER_COMPACT
    static const float independentAdc[16] = { 1024.0f };
#endif
#if PGL_DECAL_PAYLOAD_REUSE
    unsigned int batchIndex = 0;
#else
    (void)ownedPayloads;
    (void)reusePayload;
#endif
    pglCountSubmission(PGL_SUBMIT_BLOCKS);
#if PGL_DECAL_HEADER_COMPACT
    // Admission reserves the complete material in the <=65000q frame chain.
    // Keep MSCNT/TOP alternation unchanged; only close CNT for a real REF.
    packet.Cnt();
    packet.Stcycl(1, 1);
#endif
    while (count > 0) {
        const int batch = count > 16 ? 16 : count;
#if !PGL_DECAL_HEADER_COMPACT
        packet.Cnt();
        packet.Stcycl(1, 1);
#endif
#if PGL_DECAL_PAYLOAD_REUSE
        if (reusePayload) {
            packet.Pad128();
            packet.CloseTag();
            // This is the exact earlier Add destination, not a main-game
            // scratch pointer or a stride derived from variable GS packets.
            // Normal frame Send flushes these owned bytes before either read.
            packet.Ref(Core::MakePtrNormal(ownedPayloads[batchIndex]),
                (unsigned int)batch * 9u);
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32, 5, Packet::kDoubleBuff);
            packet.CloseUnpack((unsigned int)batch * 9u);
            packet.Cnt();
            pglCountSubmission(PGL_SUBMIT_REF_BYTES,
                (unsigned int)batch * sizeof(PGLDecalQuad));
        } else
#endif
        {
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32, 5, Packet::kDoubleBuff);
#if PGL_DECAL_PAYLOAD_REUSE
            const float* owned = pglAddOwnedPayload(packet,
                source, (unsigned int)batch * 36u);
            if (ownedPayloads) ownedPayloads[batchIndex] = owned;
#else
            pglAddOwnedPayload(packet, source,
                (unsigned int)batch * 36u);
#endif
            pglCloseOwnedV4Unpack(packet, (unsigned int)batch * 9u);
            pglCountSubmission(PGL_SUBMIT_EDGE_BYTES,
                (unsigned int)batch * sizeof(PGLDecalQuad));
        }
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 0, Packet::kDoubleBuff);
        packet += batch;
        packet += format | (materials ? 2u : 0u);
        if (materials) {
            // Each VI load is16bits. Keep eight two-bit cells in each word;
            // format bit1 enables the private material decoder.
            unsigned int low = 0, high = 0;
            for (int i = 0; i < batch; ++i) {
                if (i < 8) low |= (unsigned int)materials[i] << (i * 2);
                else high |= (unsigned int)materials[i] << ((i - 8) * 2);
            }
            packet += low;
            packet += high;
        } else packet += (uint64_t)0;
#if !PGL_DECAL_HEADER_COMPACT
        packet.Add(independentAdc, 16);
#endif
        pglCloseOwnedV4Unpack(packet, PGL_DECAL_HEADER_COMPACT ? 1u : 5u);
        packet.Mscnt();
        packet.Pad128();
#if !PGL_DECAL_HEADER_COMPACT
        packet.CloseTag();
#endif
        pglCountSubmission(PGL_SUBMIT_BUFFERS);
#if PGL_DECAL_PAYLOAD_REUSE
        ++batchIndex;
#endif
        source += batch * 36;
        if (materials) materials += batch;
        count -= batch;
    }
#if PGL_DECAL_HEADER_COMPACT
    packet.CloseTag();
#endif
}

void CClipDecalX2ERenderer::DrawLinearArrays(CGeometryBlock& block)
{
    (void)block;
    // The private pair API has already admitted and copied both whole
    // materials. A generic array call cannot supply the private context ABI.
    fprintf(stderr, "ps2gl: decal renderer requires pglDrawDecalQuads\n");
    abort();
}
#endif

extern "C" void pglRegisterDecalRenderer(void)
{
#if PGL_CITY_ENTRANCES_VU1
    if (pGLContext) CClipDecalX2ERenderer::Register();
#endif
}

extern "C" unsigned int pglGetDecalSubmissionOptions(void)
{
#if PGL_CITY_ENTRANCES_VU1
    return (PGL_DECAL_PAYLOAD_REUSE ? 1u : 0u)
        | (PGL_DECAL_HEADER_COMPACT ? 2u : 0u)
        | (PGL_COMPACT_QWORD_COPY ? 4u : 0u)
        | (PGL_DECAL_XY_REUSE ? 8u : 0u)
        | (PGL_DECAL_PROJECTED_RUNS ? 16u : 0u)
        | (PGL_DECAL_TRIVIAL_ACCEPT ? 32u : 0u)
        | (PGL_DECAL_TRIVIAL_ACCEPT && PGL_DECAL_CLIP_PREFIX_REUSE ? 64u : 0u)
        | (PGL_COMPACT_FIXED_UNPACK_COUNT ? 128u : 0u)
        | 256u; // per-record atlas materials in the ordinary16-record batch
#else
    return 0u;
#endif
}
