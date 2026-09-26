/* HyperSolar VU1 near-plane clip renderer (tri lists) + the double-kick
   city renderer (x2). See clip_renderer.h. */

#include <stdio.h>
#include <string.h>

#include "ps2s/drawenv.h"
#include "ps2s/math.h"

#include "GL/ps2gl.h"

#include "ps2gl/clip_renderer.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/lighting.h"
#include "ps2gl/metrics.h"
#include "ps2gl/texture.h"

#include "vu1_mem_linear.h"

#define VU_FUNCTIONS(name)        \
    void vsm##name##_CodeStart(); \
    void vsm##name##_CodeEnd()

#define mVsmAddr(name) ((void*)vsm##name##_CodeStart)
#define mVsmSize(name) ((u8*)vsm##name##_CodeEnd - (u8*)vsm##name##_CodeStart)

extern "C" {
VU_FUNCTIONS(GeneralClipTri);
VU_FUNCTIONS(GeneralClipTriX2);
VU_FUNCTIONS(GeneralClipTriX2DDecode);
}

using namespace RendererProps;

// This is a proof about preceding writes in one ordered VIF packet, not a
// hardware-completion flag or a cache carried across frames. X2/Q/D/G share
// the same immutable base image; their decoders start immediately after it.
static const CGLContext* x2BaseContext;
static const CVifSCDmaPacket* x2BasePacket;
static const void* x2BaseImage;
static unsigned int x2BaseBytes;
static unsigned int x2BaseSkippedUploads, x2BaseSkippedBytes;

extern "C" void pglInvalidateX2BasePrefix(void)
{
    x2BaseContext = NULL;
    x2BasePacket = NULL;
    x2BaseImage = NULL;
    x2BaseBytes = 0;
}

extern "C" void pglGetX2BaseReuseStats(unsigned int* uploads, unsigned int* bytes)
{
    if (uploads) *uploads = x2BaseSkippedUploads;
    if (bytes) *bytes = x2BaseSkippedBytes;
}

static unsigned int x2WindowAttempts, x2WindowReused, x2WindowFull, x2WindowPins;
static unsigned int x2WindowTexturePrefixReused, x2WindowDrawTailReused;

extern "C" void pglGetWindowPreparationStats(unsigned int* texturePrefixReused,
    unsigned int* drawTailReused)
{
    if (texturePrefixReused) *texturePrefixReused = x2WindowTexturePrefixReused;
    if (drawTailReused) *drawTailReused = x2WindowDrawTailReused;
}

extern "C" void pglGetWindowContextStats(unsigned int* attempts,
    unsigned int* reused, unsigned int* full, unsigned int* globalPins)
{
    if (attempts) *attempts = x2WindowAttempts;
    if (reused) *reused = x2WindowReused;
    if (full) *full = x2WindowFull;
    if (globalPins) *globalPins = x2WindowPins;
}

static const CGLContext* x2WindowContext;
static const CClipTriX2Renderer* x2WindowOwner;
static const CMMTexture* x2WindowTexture;
static const CVifSCDmaPacket* x2WindowPacket;
static const uint128_t* x2WindowPacketBase;
static unsigned int x2WindowFrame, x2WindowSerial, x2WindowPacketBytes;
static uint128_t x2WindowSettings[16] __attribute__((aligned(16)));

static bool X2WindowHasDirectColor(const CMMTexture& texture)
{
    // Indexed TEX0 sends can reload the shared CLUT or alter its CBP latches
    // even when all register bytes repeat. Keep those on the original path.
    const GS::tPSM psm = texture.GetPSM();
    return psm == GS::kPsm16 || psm == GS::kPsm16s ||
        psm == GS::kPsm24 || psm == GS::kPsm32;
}

static uint64_t X2WindowSettingValue(const uint128_t* settings, int quad)
{
    uint64_t value;
    memcpy(&value, (const unsigned char*)settings + quad * 16, sizeof(value));
    return value;
}

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
    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_XYZ2,
        PGL_CLIP_TRI_PROP,
        ~(pglU64_t)0xffffffff,
        PGL_MERGE_CONTIGUOUS);
}

extern "C" GLboolean pglCanDrawClipTriXYZ2(void)
{
    return !pGLContext->InDListDef()
        && !pGLContext->GetImmLighting().GetLightingEnabled()
        && !pGLContext->GetImmDrawContext().GetFogEnabled()
        && pGLContext->GetImmDrawContext().GetPolygonMode() == GL_FILL
        && pGLContext->GetImmDrawContext().GetDepthOffset() == 0.0f
        && pGLContext->GetImmDrawContext().GetDepthBits() == 24;
}

void CClipTriRenderer::InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged)
{
    if (primType == PGL_CLIP_TRIANGLES_XYZ2) {
        // This encoding owns a full context. Stock material/transform deltas
        // contain an unscaled Z row and must never inherit or update it.
        pglInvalidateUnlitContextDelta();
        if (!pglCanDrawClipTriXYZ2()) {
            printf("ps2gl: XYZ2 clip triangles require immediate unlit, fog-off, unbiased Z24 GL_FILL; draw rejected\n");
            return;
        }
#if kContextStart != 0 || kClipToGsDepthOffset != 57 || kVertexXfrm != 62 || kGifTag != 75
#error "Review the XYZ2 context patches against the GeneralClipTri ABI"
#endif
        CImmDrawContext& draw = pGLContext->GetImmDrawContext();
        CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
#if PGL_SUBMISSION_METRICS
        const unsigned int contextStart = packet.GetByteLength();
#endif
        const float encodingScale = 1.0f / 16.0f;
        const cpu_mat_44& source = draw.GetVertexXform();
        cpu_vec_4 c0 = source.get_col0();
        cpu_vec_4 c1 = source.get_col1();
        cpu_vec_4 c2 = source.get_col2();
        cpu_vec_4 c3 = source.get_col3();
        c0.z *= encodingScale;
        c1.z *= encodingScale;
        c2.z *= encodingScale;
        c3.z *= encodingScale;
        const cpu_mat_44 encoded(c0, c1, c2, c3);
        const cpu_vec_4 offset(0.0f, 0.0f, 0.0f,
            (draw.GetContextDepthScale() + draw.GetDepthOffset()) * encodingScale);
        tGifTag giftag = BuildGiftag(GL_TRIANGLES);
        giftag.REGS2 = 5; // PACKED XYZ2: full integer Z, same bit-111 ADC.

        packet.Cnt();
        AddVu1RendererContext(packet, GL_TRIANGLES, kContextStart, true);
        // The stock writer supplies every material, clipping and fog input.
        // Its VIF FLUSH protects prior draws. Patch only the encoding words
        // before MSCAL loads them; never edit the shared matrix or VU image.
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32,
            kClipToGsDepthOffset, Packet::kSingleBuff);
        packet += offset;
        packet.CloseUnpack();
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32,
            kVertexXfrm, Packet::kSingleBuff);
        packet += encoded;
        packet.CloseUnpack();
        packet.Pad96();
        packet.OpenUnpack(Vifs::UnpackModes::v4_32,
            kGifTag, Packet::kSingleBuff);
        packet += giftag;
        packet.CloseUnpack();
        packet.Mscal(0);
        packet.Flushe();
        packet.Base(kDoubleBufBase);
        packet.Offset(kDoubleBufOffset);
        packet.CloseTag();

        // Scaling only Z and its offset by 1/16 makes the existing FTOI4.z
        // emit integer Z for XYZ2. X/Y/W, near/side tests, STQ, fog arithmetic,
        // fan topology and the three-qword output stride remain unchanged.
        CacheRendererState();
        pglCountSubmission(PGL_SUBMIT_FULL_CONTEXTS);
#if PGL_SUBMISSION_METRICS
        pglCountSubmission(PGL_SUBMIT_CONTEXT_BYTES, packet.GetByteLength() - contextStart);
#endif
        return;
    }
    // The context/giftag builder doesn't know the custom prim enum; the
    // microcode consumes plain tri lists (tri-strip prim + ADC on the first
    // two verts of each tri, like general_nospec_tri), which is exactly what
    // GL_TRIANGLES builds.
    // Only the original GeneralClipTri program has this uniform-color layout.
    // Its q58..61 material and q62..65 transform words match the stock unlit
    // writer. Reuse the exact delta/restart path; X2 and arbitrary custom
    // programs remain excluded. A prior full upload establishes ownership.
    if (TryUnlitContextDelta(GL_TRIANGLES, rcChanges, userRcChanged, true)) return;
    if (!pGLContext->GetImmLighting().GetLightingEnabled()) {
        // The original GeneralClipTri VU program reads q0,57..60,62..65,
        // 75..77. The generic unlit spans retain every one plus guard q78.
        // Keep its existing transform, near-plane math, fan emission and
        // uniform color/fog. Do not substitute X2's vertex-alpha semantics.
        InitLinearContext(GL_TRIANGLES, true);
        NoteUnlitContext(pGLContext->GetVif1Packet(), GL_TRIANGLES, true);
        return;
    }
    CLinearRenderer::InitContext(GL_TRIANGLES, rcChanges, userRcChanged);
    NoteUnlitContext(pGLContext->GetVif1Packet(), GL_TRIANGLES, true);
}

void CClipTriRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    if (block.GetPrimType() == PGL_CLIP_TRIANGLES_XYZ2 && !pglCanDrawClipTriXYZ2())
        return; // InitContext diagnoses invalid admission even in release builds.
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
 * CClipTriX2Renderer — the dual-context city renderer
 */

// VU-mem unpack target: the STAGING zone of general_clip_tri_x2.vcl's
// layout (VU-READ only — the microcode builds the kicked qwords itself;
// VIF-written qwords must never be GIF-read). Changing either side means
// re-checking the whole layout.
#define kX2PfxOff 178 // [win color][win prim giftag template] = 2q

// shared body for X2 + X2D (same caps/input contract, different microcode
// and custom-prim property bit)
CClipTriX2Renderer::CClipTriX2Renderer(void* mcode, int mcodeSize, const char* name, uint64_t prop)
    : CLinearRenderer(mcode, mcodeSize, 4, 3, kInputStart, 120, name)
    , WinTex(NULL)
    , Ctx2Armed(false)
    , Ctx2SourceValid(false)
    , ContextDeltaEligible(false)
    , ContextInputsValid(false)
    , ContextPacket(NULL)
    , ContextPacketBase(NULL)
    , ContextPacketEnd(NULL)
    , ContextFrame(0)
{
    WinColor[0] = WinColor[1] = WinColor[2] = WinColor[3] = 1.0f;

    CRendererProps caps = {
        PrimType : kTriangles,
        Lighting : 1,
        NumDirLights : k3DirLights | k8DirLights,
        NumPtLights : k1PtLight | k2PtLights | k8PtLights,
        Texture : 1,
        Specular : 0,
        PerVtxMaterial : kDiffuse,
        Clipping : kNonClipped | kClipped,
        CullFace : 1,
        TwoSidedLighting : 0,
        ArrayAccess : kLinear
    };

    Capabilities = (uint64_t)caps | prop;
    Requirements = prop;
}

CClipTriX2Renderer::CClipTriX2Renderer()
    : CLinearRenderer(mVsmAddr(GeneralClipTriX2), mVsmSize(GeneralClipTriX2), 4, 3,
          kInputStart, 120,
          "clip x2, city walls+windows")
    , WinTex(NULL)
    , Ctx2Armed(false)
    , Ctx2SourceValid(false)
    , ContextDeltaEligible(true)
    , ContextInputsValid(false)
    , ContextPacket(NULL)
    , ContextPacketBase(NULL)
    , ContextPacketEnd(NULL)
    , ContextFrame(0)
{
    // 120 quads = 30 verts of 4q input (pos, [normal unused], stq, color);
    // the microcode owns the rest of its buffer layout (planes + 3q-vert
    // polygon scratch + ONE compound wall+window output packet — see the
    // .vcl header).
    WinColor[0] = WinColor[1] = WinColor[2] = WinColor[3] = 1.0f;

    CRendererProps caps = {
        PrimType : kTriangles,
        Lighting : 1,
        NumDirLights : k3DirLights | k8DirLights,
        NumPtLights : k1PtLight | k2PtLights | k8PtLights,
        Texture : 1,
        Specular : 0,
        PerVtxMaterial : kDiffuse,
        Clipping : kNonClipped | kClipped,
        CullFace : 1,
        TwoSidedLighting : 0,
        ArrayAccess : kLinear
    };

    Capabilities = (uint64_t)caps | PGL_CLIP_TRI_X2_PROP;
    Requirements = PGL_CLIP_TRI_X2_PROP;
}

static CClipTriX2Renderer* pX2Renderer = NULL;

extern "C" unsigned int pglGetRawX2SubmissionOptions(void)
{
    return PGL_RAW_X2_MULTI_SPAN && pX2Renderer ? 1u : 0u;
}

void CClipTriX2Renderer::Register()
{
    pX2Renderer = new CClipTriX2Renderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2Renderer);

    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2,
        PGL_CLIP_TRI_X2_PROP,
        ~(pglU64_t)0xffffffff, // match on the custom bits only
        PGL_MERGE_CONTIGUOUS);
}

void CClipTriX2Renderer::SetWindowTexture(unsigned int texId, float r, float g, float b, float a)
{
    // Submit any block still pending BEFORE the pair changes: BuildPrefixes
    // reads the LIVE bound textures, and ps2gl would otherwise draw the pending
    // block during the next draw's commit, with the next facade's bindings
    // already current.
    //
    // *** THIS ONLY PROTECTS PAIR CHANGES (facade -> facade). ***
    // The caller MUST ALSO flush after its FINAL x2 draw of a pass. A pending
    // block is drawn at the next glDrawArrays from ANY path, so the last x2
    // block of a pass otherwise takes its TEX0 from whatever the billboards /
    // lamps / HUD had bound: untextured white walls, worst at city load where
    // every building is in the fade pass. Do NOT delete the game's glFlush()
    // calls on the strength of this one — that regression cost an evening.
    //
    // Flushing is cheap: CImmGeomManager::Flush only appends to the DMA chain
    // (XferVectors REFs the vertex arrays rather than copying them). It is not
    // a pipeline drain — deleting the game's calls measured 0.22 ms SLOWER.
    pGLContext->GetGeomManager().Flush();

    WinColor[0] = r;
    WinColor[1] = g;
    WinColor[2] = b;
    WinColor[3] = a;
    // Revalidate GS context 2 on the next x2 draw: the pair changed, and
    // glClear (which draws through a kContext2 CDrawEnv) may have stomped
    // ctx2 since the last pass. Called once per pass, this is also the
    // once-per-frame re-arm.
    Ctx2Armed = false;
    if (texId == 0) {
        WinTex = NULL;
        return;
    }
    // Resolve through BindTexture, NOT GetNamedTexture: the bind registers the
    // texture as in-use with the manager (residency / GS-LRU bookkeeping) and
    // dirties the GS context. In x2 mode the window tile is bound nowhere
    // else, so a pure lookup leaves it evictable. The caller rebinds the base
    // texture right after.
    CTexManager& tm = pGLContext->GetTexManager();
    tm.BindTexture(texId);
    WinTex = &tm.GetCurTexture();
}

void CClipTriX2Renderer::InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged)
{
    (void)primType;
    (void)rcChanges;
    (void)userRcChanged;
    if (ContextDeltaEligible) {
        InitRetainedContext();
        return;
    }
    InitUnlitContext();
}

void CClipTriX2Renderer::BuildContextInputs(uint128_t* inputs)
{
    typedef char X2ContextTypes[sizeof(uint128_t) == 16u && sizeof(tGifTag) == 16u ? 1 : -1];
    (void)sizeof(X2ContextTypes);
    CImmDrawContext& draw = pGLContext->GetImmDrawContext();
    const float cull = (float)draw.GetCullFaceDir();
    unsigned int cullWord;
    memcpy(&cullWord, &cull, sizeof(cullWord));
    const uint32_t cullQuad[4] = {
        0, 0, 0, cullWord | ((unsigned int)draw.GetDoCullFace() << 5)
    };
    const float depth[4] = { 0.0f, 0.0f, 0.0f,
        draw.GetContextDepthScale() + draw.GetDepthOffset() };
    memcpy(inputs, cullQuad, sizeof(cullQuad));
    memcpy(inputs + 1, depth, sizeof(depth));
    typedef char X2ContextMatrixSize[sizeof(cpu_mat_44) == 64u ? 1 : -1];
    (void)sizeof(X2ContextMatrixSize);
    memcpy(inputs + 2, &draw.GetVertexXform(), sizeof(cpu_mat_44));
    GLenum primitive = draw.GetPolygonMode();
    if (primitive == GL_FILL) primitive = GL_TRIANGLES;
    // Build a real tag, then copy its representation: never mutate a GIF
    // bitfield through integer/float staging storage (the short-tag crash).
    const tGifTag tag = BuildGiftag(primitive & 0xff);
    memcpy(inputs + 6, &tag, sizeof(tag));
    const cpu_vec_xyz& scale = draw.GetContextClipScales();
    const struct {
        float x, y, z;
        uint32_t clipping;
    } clip = { scale.x, scale.y, scale.z, draw.GetDoClipping() ? 1u : 0u };
    typedef char X2ContextClipSize[sizeof(clip) == 16u ? 1 : -1];
    (void)sizeof(X2ContextClipSize);
    memcpy(inputs + 7, &clip, sizeof(clip));
    const float near[4] = { draw.GetClipNear(), 0.0f, 0.0f, 0.0f };
    memcpy(inputs + 8, near, sizeof(near));
    inputs[9] = 0;
}

void CClipTriX2Renderer::InitRetainedContext()
{
    // q0,57,62..65,75..78 are read-only to X2 and the opted-in decoders.
    // No opted-in program writes these retained ranges. X2F alone also owns
    // private absolute scratch q1..22; all other scratch stays in the bounded
    // XTOP-derived halves starting at q79. Renderer switches restore context.
#if kBackFaceCullMult != 0 || kClipToGsDepthOffset != 57 || \
    kVertexXfrm != 62 || kGifTag != 75 || kClipInfo != 76 || \
    kFogParams != 77 || kFogPad != 78 || kDoubleBufBase != 79
#error "Review retained X2 context against its VU read/write ABI"
#endif
    pglInvalidateUnlitContextDelta();
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    uint128_t inputs[10] __attribute__((aligned(16)));
    BuildContextInputs(inputs);
    const bool retained = ContextInputsValid && !pGLContext->InDListDef()
        && pGLContext->UsesNormalFramePacket()
        && ContextPacket == &packet && ContextPacketBase == packet.GetBase()
        && ContextPacketEnd == packet.GetNextPtr()
        && ContextFrame == pGLContext->GetFrameNumber()
        && &pGLContext->GetImmGeomManager().GetRendererManager().GetCurRenderer() == this;
#if PGL_SUBMISSION_METRICS
    const unsigned int start = packet.GetByteLength();
#endif
    // Even an unchanged context retains FLUSH and the complete restart. X2
    // latches matrix/near in its PC0 prologue; no timing shortcut is assumed.
    packet.Cnt();
    packet.Stcycl(1, 1);
    packet.Flush();
    static const unsigned int offsets[4] = { 0, 57, 62, 75 };
    static const unsigned int first[4] = { 0, 1, 2, 6 };
    static const unsigned int lengths[4] = { 1, 1, 4, 4 };
    bool skipped = false;
    for (unsigned int range = 0; range < 4; ++range) {
        if (!retained || memcmp(inputs + first[range], ContextInputs + first[range],
                lengths[range] * sizeof(uint128_t)) != 0) {
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32, offsets[range], Packet::kSingleBuff);
            packet.Add(inputs + first[range], lengths[range]);
            packet.CloseUnpack();
        } else {
            skipped = true;
        }
    }
    packet.Mscal(0);
    packet.Flushe();
    packet.Base(kDoubleBufBase);
    packet.Offset(kDoubleBufOffset);
    packet.CloseTag();
    CacheRendererState();
    memcpy(ContextInputs, inputs, sizeof(ContextInputs));
    ContextInputsValid = true;
    // Only the subsequent completed draw can establish an uninterrupted
    // context-to-draw-to-context chain; InitContext alone grants no reuse.
    ContextPacketEnd = NULL;
    pglCountSubmission(skipped ? PGL_SUBMIT_DELTA_CONTEXTS : PGL_SUBMIT_FULL_CONTEXTS);
#if PGL_SUBMISSION_METRICS
    pglCountSubmission(PGL_SUBMIT_CONTEXT_BYTES, packet.GetByteLength() - start);
#endif
}

void CClipTriX2Renderer::RememberContextEnd()
{
    if (ContextDeltaEligible && ContextInputsValid && !pGLContext->InDListDef()
        && pGLContext->UsesNormalFramePacket()) {
        CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
        ContextPacket = &packet;
        ContextPacketBase = packet.GetBase();
        ContextPacketEnd = packet.GetNextPtr();
        ContextFrame = pGLContext->GetFrameNumber();
    }
}

typedef char X2TexturePrefixQwordCheck[
    sizeof(uint128_t) == 16 && sizeof(tGifTag) == 16 && sizeof(uint64_t) == 8 ? 1 : -1];

static inline uint64_t X2TextureRegisterAddress(const uint128_t* settings, int quad)
{
    uint64_t address;
    memcpy(&address, (const unsigned char*)settings + quad * 16 + 8, sizeof(address));
    return address;
}

static inline bool X2TexturePrefixHasFixedLayout(const uint128_t* settings)
{
    // CTexEnv's packed settings ABI is giftag plus these seven A+D records.
    // Compare all address bits; changed/custom layouts retain the old remapper.
    return X2TextureRegisterAddress(settings, 1) == GS::RegAddrs::texflush &&
        X2TextureRegisterAddress(settings, 2) == GS::RegAddrs::clamp_1 &&
        X2TextureRegisterAddress(settings, 3) == GS::RegAddrs::tex1_1 &&
        X2TextureRegisterAddress(settings, 4) == GS::RegAddrs::tex0_1 &&
        X2TextureRegisterAddress(settings, 5) == GS::RegAddrs::texa &&
        X2TextureRegisterAddress(settings, 6) == GS::RegAddrs::miptbp1_1 &&
        X2TextureRegisterAddress(settings, 7) == GS::RegAddrs::miptbp2_1;
}

static inline void X2TexturePrefixAddress(uint128_t* settings, int quad, uint64_t address)
{
    // Deliberate bit copy, not a uint64_t lvalue into a differently typed object.
    memcpy((unsigned char*)settings + quad * 16 + 8, &address, sizeof(address));
}

void CClipTriX2Renderer::BuildWindowContext2Settings()
{
    const GS::CDrawEnv& de = pGLContext->GetImmDrawContext().GetDrawEnv();
    const uint64_t drawSource[6] = {
        de.GetTestReg(), de.GetFrameReg(), de.GetZBufReg(),
        de.GetXYOffsetReg(), de.GetScissorReg(), de.GetFBAReg()
    };
    unsigned int alphaSource;
    memcpy(&alphaSource, &WinColor[3], sizeof(alphaSource));
    // Texture changes and draw-environment changes are independent. In a city
    // material sweep the window tiles change, while the draw tail usually does
    // not. Retain either half only while its complete source key still matches.
    // This is CPU construction reuse, never evidence of resident GS state.
    const bool reuseTexture = Ctx2SourceValid &&
        memcmp(Ctx2SourceTexture, WinTex->GetSettingsBlock(), sizeof(Ctx2SourceTexture)) == 0;
    const bool reuseDraw = Ctx2SourceValid && Ctx2SourceAlpha == alphaSource &&
        memcmp(Ctx2SourceDraw, drawSource, sizeof(drawSource)) == 0;
    if (reuseTexture) ++x2WindowTexturePrefixReused;
    if (reuseDraw) ++x2WindowDrawTailReused;
    if (!reuseTexture) {
        // Ctx2[0..7]: the window texture's OWN settings block (giftag + 7
        // A+D regs), register addresses rewritten to context 2 — so
        // punch-through TEXA + custom mip MIPTBPs ride verbatim. TEXA is
        // global (safe with the x2 pairing: a PSMT8 wall takes alpha from
        // its CLUT, only the PSMCT16 window reads TEXA). Keep this cached
        // prefix's TEXFLUSH as NOP for resident data. A newly uploaded
        // window CLUT restores it in the send copy below before TEX0_2.
        memcpy(&Ctx2[0], WinTex->GetSettingsBlock(), 8 * 16);
        tGifTag contextTag;
        memcpy(&contextTag, &Ctx2[0], sizeof(contextTag));
        contextTag.NLOOP = 15;
        memcpy(&Ctx2[0], &contextTag, sizeof(contextTag));
        if (X2TexturePrefixHasFixedLayout(Ctx2)) {
            // The fixed ABI needs six address stores; TEXA stays global and
            // every texture value remains the original byte-for-byte copy.
            X2TexturePrefixAddress(Ctx2, 1, GS::RegAddrs::nop);
            X2TexturePrefixAddress(Ctx2, 2, GS::RegAddrs::clamp_2);
            X2TexturePrefixAddress(Ctx2, 3, GS::RegAddrs::tex1_2);
            X2TexturePrefixAddress(Ctx2, 4, GS::RegAddrs::tex0_2);
            X2TexturePrefixAddress(Ctx2, 6, GS::RegAddrs::miptbp1_2);
            X2TexturePrefixAddress(Ctx2, 7, GS::RegAddrs::miptbp2_2);
        } else
        {
            uint64_t textureRegisters[14];
            memcpy(textureRegisters, &Ctx2[1], sizeof(textureRegisters));
            uint64_t* tq = textureRegisters;
            for (int i = 0; i < 7; i++, tq += 2) {
                switch (tq[1]) {
                case GS::RegAddrs::texflush: tq[1] = GS::RegAddrs::nop; break;
                case GS::RegAddrs::clamp_1: tq[1] = GS::RegAddrs::clamp_2; break;
                case GS::RegAddrs::tex1_1: tq[1] = GS::RegAddrs::tex1_2; break;
                case GS::RegAddrs::tex0_1: tq[1] = GS::RegAddrs::tex0_2; break;
                case GS::RegAddrs::texa: break; // global
                case GS::RegAddrs::miptbp1_1: tq[1] = GS::RegAddrs::miptbp1_2; break;
                case GS::RegAddrs::miptbp2_1: tq[1] = GS::RegAddrs::miptbp2_2; break;
                default: mError("unexpected reg in the texture settings block");
                }
            }
            memcpy(&Ctx2[1], textureRegisters, sizeof(textureRegisters));
        }

        memcpy(Ctx2SourceTexture, WinTex->GetSettingsBlock(), sizeof(Ctx2SourceTexture));
    }

    if (!reuseDraw) {
        // Ctx2[8..14]: blend/test + live draw-env mirrors. All derived from
        // ps2gl's LIVE values — never built from scratch (TEST also carries
        // ZTE/ZTST; FRAME/ZBUF/XYOFFSET/SCISSOR must match ctx1 exactly or
        // window prims draw shifted / mis-scissored / into the wrong buffer).
        //   Window alpha test: ATE=1, ATST=GREATER, AREF=0, AFAIL=KEEP.
        //   Gap texels sample alpha exactly 0 (16-bit 5551 + TEXA ta0=0) and
        //   an additive blend of As=0 is Cs*0 + Cd = Cd — discarding them is
        //   bit-identical and skips the RMW. Lit texels give A = At*Ag>>7 =
        //   Ag (ta1=0x80 identity), so a fading building keeps every lit
        //   texel while Ag >= 1. Mip levels key alpha at ANY coverage
        //   (ps2_mip16_cache_build), so distant dimmed windows pass.
        const uint64_t testBase = de.GetTestReg();
        const uint64_t winTest = (testBase & ~(uint64_t)0x3fff) // clear ATE/ATST/AREF/AFAIL
            | (uint64_t)1                                       // ATE  = 1
            | ((uint64_t)6 << 1);                               // ATST = GREATER (AREF=0, AFAIL=KEEP)
        uint64_t rq[16];
        // (Cs - 0) * FIX + Cd. Wall A may carry the hardware-fog coefficient,
        // so the paired window's brightness is the explicit constant instead.
        float af = WinColor[3] * 128.0f;
        if (af < 0.0f) af = 0.0f;
        if (af > 128.0f) af = 128.0f;
        const uint64_t fix = (uint64_t)(af + 0.5f);
        rq[0] = 0x68 | (fix << 32); // c=FIX(2)
        rq[1] = GS::RegAddrs::alpha_2;
        rq[2] = winTest;
        rq[3] = GS::RegAddrs::test_2;
        rq[4] = de.GetFrameReg();
        rq[5] = GS::RegAddrs::frame_2;
        // ZMSK=1 (bit 32): the window re-writes its wall's identical Z — the
        // write is pure redundant bandwidth, the z-TEST still occludes.
        rq[6] = de.GetZBufReg() | ((uint64_t)1 << 32);
        rq[7] = GS::RegAddrs::zbuf_2;
        rq[8] = de.GetXYOffsetReg();
        rq[9] = GS::RegAddrs::xyoffset_2;
        rq[10] = de.GetScissorReg();
        rq[11] = GS::RegAddrs::scissor_2;
        rq[12] = de.GetFBAReg();
        rq[13] = GS::RegAddrs::fba_2;
        // TEST_1 pin (proof-grade pixel contract): the wall prims draw with
        // LIVE ctx1 state, and nothing upstream proves ATE is off at city
        // entry (the old per-buffer prefixes forced it). Send the live TEST
        // with ATE cleared — byte-identical to ps2gl's cache whenever ATE
        // was already off (the game's case today), so no state desync; if a
        // future path enters with ATE on, this pins the walls opaque like
        // the old architecture did.
        rq[14] = testBase & ~(uint64_t)1;
        rq[15] = GS::RegAddrs::test_1;
        memcpy(&Ctx2[8], rq, sizeof(rq));
        memcpy(Ctx2SourceDraw, drawSource, sizeof(drawSource));
        Ctx2SourceAlpha = alphaSource;
    }
    Ctx2SourceValid = true;
}

bool CClipTriX2Renderer::TryReuseWindowContext(CVifSCDmaPacket& packet)
{
    const unsigned int serial = GS::CTexEnv::GetContext2WriteSerial();
    if (serial == 0 || serial != x2WindowSerial ||
        x2WindowContext != pGLContext || x2WindowOwner != this ||
        x2WindowTexture != WinTex || x2WindowPacket != &packet ||
        x2WindowPacketBase != packet.GetBase() ||
        x2WindowPacketBytes > packet.GetByteLength() ||
        x2WindowFrame != pGLContext->GetFrameNumber() ||
        pGLContext->InDListDef() || !X2WindowHasDirectColor(*WinTex) ||
        !X2TexturePrefixHasFixedLayout(WinTex->GetSettingsBlock()))
        return false;

    // Build the exact same full payload before comparing it: texture sampling,
    // FIX rounding, window depth/masks and wall TEST_1 all remain authoritative.
    BuildWindowContext2Settings();
    if (memcmp(x2WindowSettings, Ctx2, sizeof(Ctx2)) != 0 ||
        !WinTex->TouchImageIfClean())
        return false;

    const uint64_t texa = X2WindowSettingValue(Ctx2, 5);
    const uint64_t test1 = X2WindowSettingValue(Ctx2, 15);
    pGLContext->GetImmDrawContext().NoteDrawEnvOverride();
    if (!GS::CTexEnv::HasOrderedWindowGlobals(packet, texa, test1)) {
        // Base texture settings normally restore another TEXA between paired
        // spans. Retain the original path-1 completion fence before changing
        // either shared register; only the unchanged context-2 writes vanish.
        packet.Cnt();
        packet.Flush().Nop();
        packet.CloseTag();

        uint128_t pins[3] __attribute__((aligned(16)));
        // Mutate a real tGifTag, then copy its representation. A bitfield store
        // through a cast into uint128_t is undefined: EE -O2 discarded NLOOP=2,
        // leaving a 15-write GIF tag in this two-write DIRECT packet.
        tGifTag pinsTag;
        memcpy(&pinsTag, &Ctx2[0], sizeof(pinsTag));
        pinsTag.NLOOP = 2;
        memcpy(&pins[0], &pinsTag, sizeof(pinsTag));
        memcpy(&pins[1], &Ctx2[5], 16);
        memcpy(&pins[2], &Ctx2[15], 16);
        packet.Cnt();
        packet.Nop();
        if (!packet.GetTTE()) packet.Nop().Nop();
        packet.OpenDirect();
        packet.Add(pins, 3);
        packet.CloseDirect();
        packet.CloseTag();

        // These known writes advance the ordinary texture proof, while the
        // independently tracked context-2 register payload stays unchanged.
        GS::CTexEnv::NoteOrderedTextureSettings(packet, GS::kContext1, texa);
        GS::CTexEnv::NoteOrderedDrawSettings(packet, GS::kContext1, test1);
        ++x2WindowPins;
    }
    x2WindowPacketBytes = packet.GetByteLength();
    ++x2WindowReused;
    Ctx2Armed = true;
    return true;
}

void CClipTriX2Renderer::RememberWindowContext(const CVifSCDmaPacket& packet)
{
    x2WindowSerial = 0;
    if (pGLContext->InDListDef() || !X2WindowHasDirectColor(*WinTex) ||
        !X2TexturePrefixHasFixedLayout(WinTex->GetSettingsBlock()))
        return;

    GS::CTexEnv::NoteOrderedContext2Prefix(packet,
        X2WindowSettingValue(Ctx2, 5), X2WindowSettingValue(Ctx2, 15));
    x2WindowContext = pGLContext;
    x2WindowOwner = this;
    x2WindowTexture = WinTex;
    x2WindowPacket = &packet;
    x2WindowPacketBase = packet.GetBase();
    x2WindowPacketBytes = packet.GetByteLength();
    x2WindowFrame = pGLContext->GetFrameNumber();
    memcpy(x2WindowSettings, Ctx2, sizeof(Ctx2));
    x2WindowSerial = GS::CTexEnv::GetContext2WriteSerial();
}

void CClipTriX2Renderer::BuildPrefixes(CVifSCDmaPacket& packet, CGeometryBlock& block)
{
    // Live state is correct here ONLY because every x2 draw is flushed while its
    // own bindings are still current: SetWindowTexture flushes on a pair change,
    // and the GAME flushes after its final x2 draw of a pass. Drop either half
    // and a pending block reads the next drawer's texture (see SetWindowTexture).
    CTexManager& tm = pGLContext->GetTexManager();
    mErrorIf(!tm.GetTexEnabled(), "the x2 renderer needs texturing enabled (city walls)");
    (void)tm; // mErrorIf compiles out in release

    // Pfx[0]: the window color const. x128 like the vcl's fmt_color: textured
    // GS modulate identity is 128 (GetMaxColorValue) — x255 was 2x overbright.
    // RGBA is consumed by VU1. The window kick uses this constant alpha rather
    // than the wall vertex alpha: city haze deliberately stores its GS fog
    // coefficient in wall A, and leaking that byte into the window would make
    // mode-1 windows disappear at the fully fogged rim. x < 0 disables the
    // window kick (wall-only).
    float* wc = (float*)&Pfx[0];
    if (WinTex) {
        wc[0] = WinColor[0] * 128.0f;
        wc[1] = WinColor[1] * 128.0f;
        wc[2] = WinColor[2] * 128.0f;
        wc[3] = WinColor[3] * 128.0f;

        // Pfx[1]: the window prim giftag TEMPLATE — the wall's giftag with
        // ABE (bit 6 of PRIM) forced on and CTXT (bit 9) selecting GS
        // context 2. NLOOP patched on VU1 with the emitted count; EOP=1
        // (BuildGiftag default) closes the compound kick.
        tGifTag tag = BuildGiftag(GL_TRIANGLES);
        /* Settled wall influence may use alpha-driven GS fog. The paired
           additive window kick must not inherit FGE from the wall giftag. */
        tag.PRIM = (tag.PRIM & ~0x20) | 0x40 | 0x200;
        memcpy(&Pfx[1], &tag, sizeof(tag));
    } else {
        wc[0] = -1.0f; // window kick disabled (wall-only mode)
        wc[1] = wc[2] = wc[3] = 0.0f;
        memset(&Pfx[1], 0, 16);
    }

    // Program GS context 2 once per pass (SetWindowTexture disarms). The
    // wall prims need NOTHING here: they draw on context 1 with ps2gl's own
    // live state (texture, ALPHA_1, TEST_1) — the old per-buffer wall prefix
    // only existed to repair ctx1 after the window kick's writes, and the
    // window no longer touches ctx1.
    if (WinTex && !Ctx2Armed) {
        ++x2WindowAttempts;
        if (TryReuseWindowContext(packet)) return;
        // Path ordering (mirrors SyncGsContext's texture send): wait for
        // path 1 to drain before the path-2 sends below — prior buffers'
        // kicks may still be drawing with the OLD ctx2 state / texture data.
        packet.Cnt();
        packet.Flush().Nop();
        packet.CloseTag();

        // PSMT8 window: clut upload + TEX0.cb — MUST precede the settings
        // copy below (SetClut writes TEX0).
        GS::tPSM psm = WinTex->GetPSM();
        bool windowClutUploaded = false;
        if (psm == GS::kPsm8 || psm == GS::kPsm8h) {
            CMMClut* clut = WinTex->GetOwnClut();
            mErrorIf(clut == NULL, "x2 PSMT8 window texture needs its own clut");
            if (clut) {
                windowClutUploaded = !clut->TouchIfResident();
                clut->Load(packet);
                WinTex->SetClut(*clut);
            }
        }
        // The upload half of Use() ONLY: its ctx1 settings send would stomp
        // the wall texture's live TEX0_1 (see LoadIfDirty's comment). Inert
        // for game-managed textures (XferImage=false).
        WinTex->LoadIfDirty(packet);

        BuildWindowContext2Settings();
        // An unchanged depth/mask setter may deliberately restore the whole
        // environment after this raw ATE pin. Preserve that reassertion even
        // after switching back to an ordinary renderer; CPU equality alone
        // does not prove that the queued GS state matches its shadow.
        pGLContext->GetImmDrawContext().NoteDrawEnvOverride();
        // Context2 has private TEX0 but shares TEXA and the CLUT temporary
        // buffer. Its raw settings bypass CTexEnv::SendSettings.
        GS::CTexEnv::InvalidateTextureSync();

        // DIRECT-send the block down GIF path 2 (same qword-alignment nop
        // trick as CTexEnv::SendSettings).
        packet.Cnt();
        {
            packet.Nop();
            if (!packet.GetTTE())
                packet.Nop().Nop();
            packet.OpenDirect();
            if (windowClutUploaded) {
                // IMAGE upload does not invalidate the GS texture buffer.
                // Restore the source TEXFLUSH before its first CLUT load
                // (GS manual 3.4.7), without changing the resident cache.
                uint128_t uploadContext[16] __attribute__((aligned(16)));
                memcpy(uploadContext, Ctx2, sizeof(Ctx2));
                const uint128_t* source = WinTex->GetSettingsBlock();
                for (int i = 1; i < 8; i++) {
                    if (X2TextureRegisterAddress(source, i) == GS::RegAddrs::texflush)
                        X2TexturePrefixAddress(uploadContext, i, GS::RegAddrs::texflush);
                }
                packet.Add(uploadContext, 16);
            } else {
                packet.Add(Ctx2, 16);
            }
            packet.CloseDirect();
        }
        packet.CloseTag();

        ++x2WindowFull;
        RememberWindowContext(packet);
        Ctx2Armed = true;
    }
    (void)block;
}

void CClipTriX2Renderer::XferPrefixes(CVifSCDmaPacket& packet)
{
    // contiguous unpacks (cl == wl == 1); the next buffer's XferBlock /
    // XferBufferHeader re-establish their own stcycl.
    // ALIGNMENT (hardware-learned): unpack DATA must sit qword-aligned in
    // the chain — packet.Add of uint128s is an sq into the (uncached)
    // packet buffer and faults on a word-aligned cursor ("address store
    // exception"). Same trick as CTexEnv::SendSettings: pad with vifnops
    // so each unpack CODE lands in the last word of a qword. With TTE the
    // Cnt tag itself carries the first two vif words.
    packet.Cnt();
    {
        if (!packet.GetTTE())
            packet.Nop().Nop();
        packet.Stcycl(1, 1);
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, kX2PfxOff, Packet::kDoubleBuff);
        packet.Add(Pfx, 2);
        packet.CloseUnpack(2);
        packet.Pad128();
    }
    packet.CloseTag();
}

void CClipTriX2Renderer::DrawLinearArrays(CGeometryBlock& block)
{
    // custom prim: CommitPrimType leaves these unset (see CClipTriRenderer)
    block.SetNumVertsPerPrim(3);
    block.SetNumVertsToRestartStrip(0);
    block.SetStripsCanBeMerged(true);

    int wordsPerVert   = block.GetWordsPerVertex();
    int wordsPerNormal = (block.GetNormalsAreValid()) ? block.GetWordsPerNormal() : 0;
    int wordsPerTex    = (block.GetTexCoordsAreValid()) ? block.GetWordsPerTexCoord() : 0;
    int wordsPerColor  = (block.GetColorsAreValid()) ? block.GetWordsPerColor() : 0;
    mErrorIf(wordsPerColor == 0, "the x2 renderer needs a color array (wall pv tint)");

    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    InitXferBlock(packet, wordsPerVert, wordsPerNormal, wordsPerTex, wordsPerColor);

    // This renderer is UNLIT-by-contract: per-vertex color is consumed
    // directly and no normal is ever read. CacheRendererState() derives
    // XferNormals from the LIGHTING flag and XferColors from COLOR_MATERIAL
    // — with lighting on but GL_NORMAL_ARRAY disabled, XferBlock's
    // *NormalBuf fallback dereferences NULL (the stage-3 TLB crash,
    // BadVAddr 0x57C); with the lighting bracket dropped, XferColors would
    // silently stop shipping the required color array. Pin both to the
    // renderer's actual input contract.
    XferNormals = false;
    XferColors  = true;

    BuildPrefixes(packet, block);

    if (TryDrawIndependentSpans(packet, block)) {
        RememberContextEnd();
        return;
    }

    // per-buffer vert cap: multiple of SIX (the 3n-1 splitter lesson)
    int maxVertsPerBuffer = InputGeomBufSize / InputQuadsPerVert;
    if (maxVertsPerBuffer > 256)
        maxVertsPerBuffer = 256;
    maxVertsPerBuffer -= 3;
    maxVertsPerBuffer -= maxVertsPerBuffer % 6;

    // Raw X2 owns input q5..124 (30 * 4q), followed by planes at q125.
    // Its existing A=30/B=18 output ping-pong fits every <=18-vertex fan.
    // With no window section, moving an activation boundary preserves the
    // entire material/triangle order. Keep paired wall/window interleaving
    // unchanged, and avoid the generic splitter's odd partial-strip rule.
    if (!WinTex && !pGLContext->InDListDef()
        && MicrocodePacket == mVsmAddr(GeneralClipTriX2)
        && MicrocodePacketSize == mVsmSize(GeneralClipTriX2)
        && InputGeomOffset == 5 && InputGeomBufSize == 120
        && InputQuadsPerVert == 4 && OutputQuadsPerVert == 3
        && kDoubleBufBase == 79 && kDoubleBufSize == 472
        && VifDoubleBuffered && wordsPerVert == 3
        && wordsPerTex == 2 && wordsPerColor == 4
        && block.GetNumStrips() == 1 && !block.StripIsContinued(0)
        && block.GetStripLength(0) > 0 && block.GetStripLength(0) % 3 == 0)
        maxVertsPerBuffer = 30;

    DrawBlockX2(packet, block, maxVertsPerBuffer);
    RememberContextEnd();
}

bool CClipTriX2Renderer::TryDrawIndependentSpans(CVifSCDmaPacket& packet,
    CGeometryBlock& block)
{
#if PGL_RAW_X2_MULTI_SPAN
    // Qualify every source before emitting anything. Continued strips and
    // paired windows keep their original split/order contract. No VU image,
    // memory limit or output-spill rule changes in this packet-only path.
    if (WinTex || pGLContext->InDListDef()
        || MicrocodePacket != mVsmAddr(GeneralClipTriX2)
        || MicrocodePacketSize != mVsmSize(GeneralClipTriX2)
        || InputGeomOffset != 5 || InputGeomBufSize != 120
        || InputQuadsPerVert != 4 || OutputQuadsPerVert != 3
        || kDoubleBufBase != 79 || kDoubleBufSize != 472
        || !VifDoubleBuffered || block.GetArrayType() != ArrayType::kLinear
        || block.GetPrimType() != PGL_CLIP_TRIANGLES_X2
        || block.GetWordsPerVertex() != 3 || block.GetWordsPerTexCoord() != 2
        || block.GetWordsPerColor() != 4 || !block.GetVerticesAreValid()
        || !block.GetTexCoordsAreValid() || !block.GetColorsAreValid()
        || block.GetNumStrips() < 2)
        return false;
    unsigned int total = 0;
    for (int strip = 0; strip < block.GetNumStrips(); ++strip) {
        const int count = block.GetStripLength(strip);
        if (count <= 0 || count % 3 || block.StripIsContinued(strip)
            || !block.GetVertices(strip) || !block.GetTexCoords(strip)
            || !block.GetColors(strip))
            return false;
        if ((unsigned int)count > (unsigned int)block.GetTotalVertices() - total)
            return false;
        total += (unsigned int)count;
    }
    if (total != (unsigned int)block.GetTotalVertices()) return false;

    packet.Cnt();
    packet.Stcycl(1, InputQuadsPerVert);
    packet.Pad128();
    packet.CloseTag();
    int used = 0;
    unsigned short independentOffset = 0;
    for (int strip = 0; strip < block.GetNumStrips(); ++strip) {
        const int count = block.GetStripLength(strip);
        const int remainder = count % 30;
        // Filling every gap can add a transfer without saving an activation
        // (24+24 becomes three transfers for the same two activations).
        // Merge only when this run's complete remainder fits: each merge
        // saves one prefix/header/MSCNT and never adds an XferBlock call.
        if (used && (!remainder || used + remainder > 30)) {
            XferPrefixes(packet);
            FinishBuffer(packet, 0, used, InputQuadsPerVert, 1, &independentOffset);
            used = 0;
        }
        for (int first = 0; first < count;) {
            const int added = Math::Min(count - first, 30 - used);
            XferBlock(packet, block.GetVertices(strip), NULL,
                block.GetTexCoords(strip), block.GetColors(strip),
                InputGeomOffset + used * InputQuadsPerVert, first, added);
            used += added;
            first += added;
            if (used == 30) {
                XferPrefixes(packet);
                FinishBuffer(packet, 0, used, InputQuadsPerVert, 1, &independentOffset);
                used = 0;
            }
        }
    }
    if (used) {
        XferPrefixes(packet);
        FinishBuffer(packet, 0, used, InputQuadsPerVert, 1, &independentOffset);
    }
    return true;
#else
    (void)packet;
    (void)block;
    return false;
#endif
}

// Copy of CLinearRenderer::DrawBlock with ONE change: the 2q staging block
// (window color + window prim giftag template) is unpacked per buffer
// (XferPrefixes before each FinishBuffer). Keep in sync with
// linear_renderer.cpp.
void CClipTriX2Renderer::DrawBlockX2(CVifSCDmaPacket& packet,
    CGeometryBlock& block, int maxVertsPerBuffer)
{
    mErrorIf(block.GetWordsPerVertex() == 2, "2 word vertices not supported");

    packet.Cnt();
    {
        packet.Stcycl(1, InputQuadsPerVert);
        packet.Pad128();
    }
    packet.CloseTag();

    int numVertsToRestart  = block.GetNumVertsToRestartStrip();
    bool stripsCanBeMerged = block.GetStripsCanBeMerged();

    int numVertsXferred   = 0;
    int numStripsInBuffer = 0;
    unsigned short stripOffsets[16];
    bool haveContinued = false;
    const void *normals, *vertices, *texCoords, *colors;
    normals = vertices = texCoords = colors = NULL;
    int vu1BufferOffset = 0, stripIndex = 0, vertsInBlock = 0;
    int adjMaxVertsPerBuffer = maxVertsPerBuffer - (Math::IsOdd(maxVertsPerBuffer - numVertsToRestart));
    for (int curStrip = 0; curStrip < block.GetNumStrips(); curStrip++) {

        int numVertsFirstBuffer, numVertsLastBuffer, numBuffers;
        FindNumBuffers(block.GetStripLength(curStrip),
            numVertsToRestart, numVertsXferred, maxVertsPerBuffer,
            numVertsFirstBuffer, numVertsLastBuffer, numBuffers);

        int numVertsThisBuffer;
        int indexIntoStrip  = 0;
        int vu1QuadsPerVert = InputQuadsPerVert;
        for (int curBuffer = 0;
             curBuffer < numBuffers;
             curBuffer++, indexIntoStrip += numVertsThisBuffer - numVertsToRestart) {

            if (curBuffer == 0)
                numVertsThisBuffer = numVertsFirstBuffer;
            else if (curBuffer == numBuffers - 1)
                numVertsThisBuffer = numVertsLastBuffer;
            else
                numVertsThisBuffer = adjMaxVertsPerBuffer;

            if (!haveContinued) {
                vertices        = (block.GetVerticesAreValid()) ? block.GetVertices(curStrip) : NULL;
                normals         = (block.GetNormalsAreValid()) ? block.GetNormals(curStrip) : NULL;
                texCoords       = (block.GetTexCoordsAreValid()) ? block.GetTexCoords(curStrip) : NULL;
                colors          = (block.GetColorsAreValid()) ? block.GetColors(curStrip) : NULL;
                vu1BufferOffset = InputGeomOffset + numVertsXferred * vu1QuadsPerVert;
                stripIndex      = indexIntoStrip;
                vertsInBlock    = 0;
            }

            if (!block.StripIsContinued(curStrip)
                || curBuffer < numBuffers - 1) {
                XferBlock(packet,
                    vertices, normals, texCoords, colors,
                    vu1BufferOffset,
                    stripIndex, vertsInBlock + numVertsThisBuffer);
                haveContinued = false;
            } else {
                vertsInBlock += numVertsThisBuffer;
                haveContinued = true;
            }

            stripOffsets[numStripsInBuffer++] = numVertsXferred;
            mErrorIf(numStripsInBuffer > 16, "Too many strips in buffer.. this shouldn't happen");
            numVertsXferred += numVertsThisBuffer;

            if (curBuffer < numBuffers - 1) {
                XferPrefixes(packet);
                FinishBuffer(packet, numVertsToRestart, numVertsXferred, vu1QuadsPerVert,
                    numStripsInBuffer, stripOffsets);
                numStripsInBuffer = 0;
                numVertsXferred   = 0;
            }

        } // end buffer loop

        if (!stripsCanBeMerged
            || ((maxVertsPerBuffer - numVertsXferred) <= numVertsToRestart + 1)
            || numStripsInBuffer == 16
            || (curStrip == block.GetNumStrips() - 1)) {
            if (haveContinued) {
                XferBlock(packet,
                    vertices, normals, texCoords, colors,
                    vu1BufferOffset,
                    stripIndex, vertsInBlock);
                haveContinued = false;
            }

            XferPrefixes(packet);
            FinishBuffer(packet, numVertsToRestart, numVertsXferred, vu1QuadsPerVert,
                numStripsInBuffer, stripOffsets);
            numStripsInBuffer = 0;
            numVertsXferred   = 0;
        }

    } // end strip loop
}

/********************************************
 * CClipTriX2DRenderer — separate decoder + exact X2 body (P3)
 */

// VU-mem descriptor staging offsets (see general_clip_tri_x2d_decode.vcl:
// GEO 4 descs x 3q at 101, COL 4 descs x 3 V4-8 vectors at 113)
#define kX2dGeoOff 101
#define kX2dColOff 113

CClipTriX2DRenderer::CClipTriX2DRenderer()
    : CClipTriX2Renderer(mVsmAddr(GeneralClipTriX2), mVsmSize(GeneralClipTriX2),
          "clip x2d, wall descriptors", PGL_CLIP_TRI_X2D_PROP)
    , DecoderCode(mVsmAddr(GeneralClipTriX2DDecode))
    , DecoderCodeSize(mVsmSize(GeneralClipTriX2DDecode))
    , DecoderAddr64(mVsmSize(GeneralClipTriX2) / 8)
    , DescriptorElements(3)
    , DescriptorColorWords(1)
    , DescriptorColorOffset(kX2dColOff)
    , DescriptorColorDivisor(1)
    , DescriptorColorQwords(0)
{
    ContextDeltaEligible = true;
}

CClipTriX2DRenderer::CClipTriX2DRenderer(const void* decoder, int decoderSize,
    const char* name, uint64_t prop, int elements, int colorWords, int colorDivisor,
    int colorQwords)
    : CClipTriX2Renderer(mVsmAddr(GeneralClipTriX2), mVsmSize(GeneralClipTriX2), name, prop)
    , DecoderCode(decoder)
    , DecoderCodeSize(decoderSize)
    , DecoderAddr64(mVsmSize(GeneralClipTriX2) / 8)
    , DescriptorElements(elements)
    , DescriptorColorWords(colorWords)
    , DescriptorColorOffset(kX2dGeoOff + 4 * elements)
    , DescriptorColorDivisor(colorDivisor)
    , DescriptorColorQwords(colorQwords)
{
}

/* Upload one independently assembled VU1 image at an instruction address.
 * This intentionally mirrors CBaseRenderer::Load instead of changing that
 * proven path for every other ps2gl renderer. MPG and MSCAL addresses are
 * 64-bit VU instruction addresses; DMA REF counts are 128-bit qwords. */
static void UploadVu1Range(CVifSCDmaPacket& packet, const void* image,
    int imageBytes, unsigned int addr64)
{
    // Any overlapping upload destroys the proof before its first MPG. Decoder
    // writes at the exact end of the prefix are disjoint and preserve it.
    if (imageBytes > 0 && addr64 < x2BaseBytes / 8u)
        pglInvalidateX2BasePrefix();
    const u64* code = (const u64*)image;
    unsigned int size64 = imageBytes / 8;

    mErrorIf((unsigned int)code & 0xf, "x2d: VU code not 16-byte aligned");
    mErrorIf(imageBytes & 0xf, "x2d: VU code size not 16-byte aligned");

    while (size64 > 0) {
        unsigned int sendSize64 = (size64 > 256) ? 256 : size64;
        packet.Ref(code, sendSize64 / 2);
        packet.Pad96();
        packet.Mpg(sendSize64 & 0xff, addr64);
        code += sendSize64;
        size64 -= sendSize64;
        addr64 += sendSize64;
    }
}

static void LoadX2Base(CVifSCDmaPacket& packet, const void* image, int imageBytes)
{
    const bool canonical = image == mVsmAddr(GeneralClipTriX2)
        && imageBytes == mVsmSize(GeneralClipTriX2) && imageBytes > 0;
    if (canonical && !pGLContext->InDListDef()
        && x2BaseContext == pGLContext && x2BasePacket == &packet
        && x2BaseImage == image && x2BaseBytes == (unsigned int)imageBytes) {
        ++x2BaseSkippedUploads;
        x2BaseSkippedBytes += (unsigned int)imageBytes;
        return;
    }
    UploadVu1Range(packet, image, imageBytes, 0);
    if (canonical && !pGLContext->InDListDef()) {
        x2BaseContext = pGLContext;
        x2BasePacket = &packet;
        x2BaseImage = image;
        x2BaseBytes = (unsigned int)imageBytes;
    }
}

void CClipTriX2Renderer::Load()
{
    ContextInputsValid = false;
    pglInvalidateUnlitContextDelta();
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    LoadX2Base(packet, MicrocodePacket, MicrocodePacketSize);
    // MSCAL retains the previous uploader's microprogram-completion wait and
    // initializes all X2 registers even when its immutable code is resident.
    packet.Cnt();
    packet.Mscal(0);
    packet.Pad128();
    packet.CloseTag();
    pglAddToMetric(kMetricsRendererUpload);
}

void CClipTriX2DRenderer::Load()
{
    ContextInputsValid = false;
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();

    // PC 0..DecoderAddr64-1 is literally the checked-in X2 object. The
    // decoder follows it without concatenation or reassembly, so VCL can
    // never perturb the hardware-green transform/S-H/compound-kick body.
    LoadX2Base(packet, MicrocodePacket, MicrocodePacketSize);
    UploadVu1Range(packet, DecoderCode, DecoderCodeSize, DecoderAddr64);

    packet.Cnt();
    packet.Mscal(0); // run X2's six-instruction initialization prologue
    packet.Pad128();
    packet.CloseTag();

    pglAddToMetric(kMetricsRendererUpload);
}

static CClipTriX2DRenderer* pX2DRenderer = NULL;

void CClipTriX2DRenderer::Register()
{
    pX2DRenderer = new CClipTriX2DRenderer;
    pGLContext->GetImmGeomManager().GetRendererManager().RegisterX2Renderer(pX2DRenderer);

    pglRegisterCustomPrimType(PGL_CLIP_TRIANGLES_X2D,
        PGL_CLIP_TRI_X2D_PROP,
        ~(pglU64_t)0xffffffff, // match on the custom bits only
        PGL_MERGE_CONTIGUOUS);
}

void CClipTriX2DRenderer::DrawLinearArrays(CGeometryBlock& block)
{
    // One complete descriptor is the indivisible source primitive. Both
    // formats keep four descriptors (24 expanded vertices) per activation.
    block.SetNumVertsPerPrim(DescriptorElements);
    block.SetNumVertsToRestartStrip(0);
    block.SetStripsCanBeMerged(true);

    mErrorIf(block.GetWordsPerVertex() != 4,
        "x2d: the GEO stream must be glVertexPointer(4, GL_FLOAT)");
    mErrorIf(!block.GetColorsAreValid() || block.GetWordsPerColor() != DescriptorColorWords,
        "x2d/x2q: COLOR stream does not match the descriptor format");

    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();

    // Unlit-by-contract like X2. The two source streams are transferred
    // directly: float GEO plus byte COL (X2D) or exact float COL (X2Q).
    XferNormals = false;
    XferColors  = true;

    BuildPrefixes(packet, block);

    DrawBlockX2D(packet, block, 4 * DescriptorElements);
    RememberContextEnd();
}

void CClipTriX2DRenderer::FinishBufferX2D(CVifSCDmaPacket& packet, int numElems,
    bool directPacket)
{
    if (directPacket) {
        // Exact X2Q/C only. The unchanged X2 io loop and both decoders read
        // q0.x; neither reads the legacy strip ADC q1..4. Keep one activation
        // and the same two prefix qwords, in a single owned CNT. Scalar header
        // writes need only word alignment; the Pfx copy remains qword aligned.
        packet.Cnt();
        packet.Stcycl(1, 1);
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, kX2PfxOff, Packet::kDoubleBuff);
        packet.Add(Pfx, 2);
        packet.CloseUnpack(2);
        packet.OpenUnpack(Vifs::UnpackModes::v4_32, 0, Packet::kDoubleBuff);
        packet += (numElems / 4) * 6;
        packet += 0;
        packet += 0;
        packet += 0;
        packet.CloseUnpack(1);
        packet.Mscal(DecoderAddr64);
        packet.Pad128();
        packet.CloseTag();
        return;
    }
    XferPrefixes(packet);
    packet.Cnt();
    {
        // Header names the expanded count; the source count stays aligned
        // to the selected format's complete descriptor size.
        XferBufferHeader(packet, 0, (numElems / DescriptorElements) * 6, 0, NULL);
        // XferBufferHeader restores stcycl(1, InputQuadsPerVert) for the
        // vert-array renderers; the descriptor streams unpack contiguously
        packet.Stcycl(1, 1);
        // One activation per buffer is essential: MSCAL and MSCNT each copy
        // TOPS->TOP and toggle DBF. The decoder tail-jumps to X2 PC 6, so a
        // second MSCAL/MSCNT would toggle to the wrong buffer half.
        packet.Mscal(DecoderAddr64);
        packet.Pad128();
    }
    packet.CloseTag();
}

/* The caller proves a whole aligned V4_32 stream and TTE. These are the same
   borrowed REF bytes as XferVectors, with no format expansion or masked lanes;
   one tag carries NOP+UNPACK and no redundant mask-setting CNT is needed. */
static inline void XferWallFloatVectors(CVifSCDmaPacket& packet,
    const void* source, unsigned int qwords, int destination)
{
    packet.Ref(Core::MakePtrNormal(source), qwords);
    packet.Nop();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, destination, Packet::kDoubleBuff);
    packet.CloseUnpack(qwords);
    pglCountSubmission(PGL_SUBMIT_REF_BYTES, qwords * 16u);
}

void CClipTriX2DRenderer::DrawBlockX2D(CVifSCDmaPacket& packet,
    CGeometryBlock& block, int maxElemsPerBuffer)
{
    const bool directPacket = DescriptorElements == 4 && DescriptorColorWords == 4
        && (DescriptorColorDivisor == 1 || DescriptorColorDivisor == 2
            || DescriptorColorQwords == 3)
        && VifDoubleBuffered && packet.GetTTE();
    packet.Cnt();
    {
        packet.Stcycl(1, 1); // both descriptor streams unpack contiguously
        packet.Pad128();
    }
    packet.CloseTag();

    Vifs::tMask noMask;
    *(unsigned int*)&noMask = 0;

    int elemsInBuffer = 0;
    for (int curStrip = 0; curStrip < block.GetNumStrips(); curStrip++) {
        int stripLen = block.GetStripLength(curStrip);
        mErrorIf(stripLen % DescriptorElements,
            "x2d/x2q: draw count must contain complete descriptors");
        unsigned int* geo = (unsigned int*)block.GetVertices(curStrip);
        unsigned int* col = (unsigned int*)block.GetColors(curStrip);
        int idx = 0;
        while (idx < stripLen) {
            int room = maxElemsPerBuffer - elemsInBuffer;
            int n = stripLen - idx;
            if (n > room)
                n = room;
            // Whole descriptor offsets preserve both initial alignments.
            // Unaligned input keeps the existing edge-copy transfer exactly.
            if (directPacket && (((uintptr_t)geo | (uintptr_t)col) & 15u) == 0u) {
                const bool paired = DescriptorColorDivisor == 2;
                XferWallFloatVectors(packet, geo + idx * 4, (unsigned int)n,
                    kX2dGeoOff + elemsInBuffer);
                if (DescriptorColorQwords == 3) {
                    // X2H: three color qwords per four-element wall.
                    XferWallFloatVectors(packet, col + (idx >> 2) * 12,
                        (unsigned int)((n >> 2) * 3),
                        DescriptorColorOffset + (elemsInBuffer >> 2) * 3);
                } else
                XferWallFloatVectors(packet, col + (paired ? idx * 2 : idx * 4),
                    (unsigned int)(paired ? n >> 1 : n),
                    DescriptorColorOffset + (paired ? elemsInBuffer >> 1 : elemsInBuffer));
            } else
            {
                // Geometry keeps one qword per element. X2C has one color
                // qword per two elements; legacy X2D colors use byte UNPACK.
                XferVectors(packet, geo, idx, n, 4, noMask,
                    Vifs::UnpackModes::v4_32, kX2dGeoOff + elemsInBuffer);
                if (DescriptorColorQwords == 3) {
                    XferVectors(packet, col, (idx >> 2) * 3, (n >> 2) * 3, 4, noMask,
                        Vifs::UnpackModes::v4_32,
                        DescriptorColorOffset + (elemsInBuffer >> 2) * 3);
                } else if (DescriptorColorDivisor == 2) {
                    XferVectors(packet, col, idx >> 1, n >> 1, 4, noMask,
                        Vifs::UnpackModes::v4_32, DescriptorColorOffset + (elemsInBuffer >> 1));
                } else {
                    XferVectors(packet, col, idx, n, DescriptorColorWords, noMask,
                        DescriptorColorWords == 1 ? Vifs::UnpackModes::v4_8 : Vifs::UnpackModes::v4_32,
                        DescriptorColorOffset + elemsInBuffer);
                }
            }
            elemsInBuffer += n;
            idx += n;
            if (elemsInBuffer == maxElemsPerBuffer) {
                FinishBufferX2D(packet, elemsInBuffer, directPacket);
                elemsInBuffer = 0;
            }
        }
    }
    if (elemsInBuffer > 0)
        FinishBufferX2D(packet, elemsInBuffer, directPacket);
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

/**
 * Register the double-kick city renderer + PGL_CLIP_TRIANGLES_X2.
 */
void pglRegisterClipTriX2Renderer(void)
{
    CClipTriX2Renderer::Register();
}

/**
 * Set the window overlay texture + constant color (0..1 floats) for the
 * next PGL_CLIP_TRIANGLES_X2 draws. Binds texId internally — rebind the
 * base texture AFTER this call. texId = 0 disables the window kick.
 */
void pglClipX2SetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2Renderer == NULL, "pglRegisterClipTriX2Renderer() first");
    pX2Renderer->SetWindowTexture(texId, r, g, b, a);
}

/* Keep X2 and X2D pair state completely independent.  Calling both setters
 * from the legacy X2 entry point changed the hardware-green X2 path merely by
 * registering X2D; each renderer owns its own pending-block flush, WinTex and
 * context-2 re-arm. */
void pglClipX2DSetWindowTexture(GLuint texId, float r, float g, float b, float a)
{
    mErrorIf(pX2DRenderer == NULL, "pglRegisterClipTriX2DRenderer() first");
    pX2DRenderer->SetWindowTexture(texId, r, g, b, a);
}

/**
 * Register the descriptor variant + PGL_CLIP_TRIANGLES_X2D (contract in
 * GL/ps2gl.h). Register the base x2 renderer too if both paths are used.
 */
void pglRegisterClipTriX2DRenderer(void)
{
    CClipTriX2DRenderer::Register();
}
