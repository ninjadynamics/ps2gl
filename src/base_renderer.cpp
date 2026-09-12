/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include "ps2s/cpu_matrix.h"
#include "ps2s/math.h"
#include "ps2s/packet.h"

#include <stdlib.h>
#include <string.h>

#include "ps2gl/base_renderer.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/lighting.h"
#include "ps2gl/material.h"
#include "ps2gl/matrix.h"
#include "ps2gl/metrics.h"
#include "ps2gl/texture.h"

// All context offsets in this translation unit use the zero-based layout.
// Define its base before either the full writer or the delta writer uses it.
#define kContextStart 0
#include "vu1_context.h"

extern "C" unsigned int pglGetContextOptimizationFlags(void)
{
    return PGL_SKIP_REDUNDANT_DRAW_STATE | (PGL_SPARSE_UNLIT_CONTEXT << 1)
        | (PGL_SPARSE_CLIP_CONTEXT << 2) | (PGL_DEFER_MATRIX_CONCAT_INVERSE << 3)
        | (PS2S_MATRIX_SCALAR_KERNEL << 4) | (PS2S_MATRIX_EE_COP1 << 5)
        | (PGL_LAZY_MATRIX_INVERSE << 6) | (PS2S_MATRIX_VU0 << 7)
        | (PGL_SKIP_REDUNDANT_COLOR << 8) | (PGL_SKIP_REDUNDANT_BLEND_ALPHA << 9)
        | (PGL_SKIP_REDUNDANT_TEXTURE_SYNC << 10) | (PGL_UNLIT_CONTEXT_DELTA << 11)
        | (PGL_COLOR4UB_LUT << 12) | (PGL_BULK_QUAD_CORNERS << 13)
        | (PGL_CLIP_CONTEXT_DELTA << 14) | (PGL_CONTEXT_COEFFICIENT_CACHE << 15)
        | (PGL_BULK_QUAD_XYZ3 << 16) | (PGL_FUSED_TRIANGLE3D << 17)
        | (PGL_SPARSE_LIGHT_CONTEXT << 18) | (PGL_LIT_MATERIAL_DELTA << 19)
        | (PGL_UNLIT_DELTA_SPECIALIZE << 20) | (PGL_FLAT_QUAD_PACKETS << 21)
        | (PGL_INDEPENDENT_PRIM_HEADER << 22) | (PGL_SUBMISSION_METRICS << 23)
        | (PGL_TRANSFER_FORMAT_CACHE << 24) | (PGL_UNLIT_GS_CONTEXT_DELTA << 25)
        | (PGL_BORROWED_QUAD_ARRAYS << 26);
}

#if PGL_UNLIT_CONTEXT_DELTA || PGL_CLIP_CONTEXT_DELTA || PGL_LIT_MATERIAL_DELTA
// A proof about the last context written to this ordered VIF chain, not a
// cross-frame cache of VU RAM. Frame/reset/program transitions invalidate it.
static const CBaseRenderer* unlitContextOwner;
static const CVifSCDmaPacket* unlitContextPacket;
static GLenum unlitContextPrim;
static bool unlitContextOriginalClip;
static bool unlitContextLightingEnabled;
static bool unlitContextTexEnabled;
#if PGL_UNLIT_GS_CONTEXT_DELTA && PGL_UNLIT_CONTEXT_DELTA
// The GS can need fresh texture/draw settings while these VU context inputs
// remain unchanged. Capture only after a full stock unlit context, not after
// the established material/transform deltas. All words are explicitly set;
// raw float bits also distinguish signed zero without struct padding.
static const unsigned int kUnlitGsContextKeyWords = 15;
static uint32_t unlitGsContextKey[kUnlitGsContextKeyWords];
static bool unlitGsContextValid;

static void GetUnlitGsContextKey(const CImmDrawContext& draw, uint32_t* key)
{
    key[0] = draw.GetFBWidth();
    key[1] = draw.GetFBHeight();
    key[2] = draw.GetDepthBits();
    key[3] = draw.GetDoCullFace();
    key[4] = draw.GetCullFaceDir();
    key[5] = draw.GetPolygonMode();
    key[6] = draw.GetDoSmoothShading();
    key[7] = draw.GetBlendEnabled();
    key[8] = draw.GetEdgeAAEnabled();
    key[9] = draw.GetFogEnabled();
    key[10] = draw.GetDoClipping();
    const float params[4] = { draw.GetDepthOffset(), draw.GetFogStart(),
        draw.GetFogEnd(), draw.GetClipNear() };
    memcpy(key + 11, params, sizeof(params));
}
#endif
#endif

extern "C" void pglInvalidateUnlitContextDelta(void)
{
#if PGL_UNLIT_CONTEXT_DELTA || PGL_CLIP_CONTEXT_DELTA || PGL_LIT_MATERIAL_DELTA
    unlitContextOwner = NULL;
    unlitContextPacket = NULL;
#if PGL_UNLIT_GS_CONTEXT_DELTA && PGL_UNLIT_CONTEXT_DELTA
    unlitGsContextValid = false;
#endif
#endif
}

#if PGL_UNLIT_CONTEXT_DELTA || PGL_CLIP_CONTEXT_DELTA || PGL_LIT_MATERIAL_DELTA
bool CBaseRenderer::CanUseUnlitContextDelta(const CVifSCDmaPacket& packet,
    GLenum primType, uint32_t changes, bool userChanged, bool originalClip) const
{
    const uint32_t allowed = RendererCtxtFlags::Xform | RendererCtxtFlags::CurMaterial;
#if PGL_UNLIT_DELTA_SPECIALIZE
    // These checks need no GL-state pointer chasing. In particular, a full
    // context or a new owner cannot reuse anything regardless of lighting.
    if (changes == 0 || (changes & ~allowed) != 0 || userChanged
        || unlitContextOwner != this || unlitContextPacket != &packet
        || unlitContextOriginalClip != originalClip || unlitContextPrim != primType)
        return false;
#endif
    CGLContext& context = *pGLContext;
    const bool doLighting = context.GetImmLighting().GetLightingEnabled();
    if (doLighting) {
        // A modelview change also changes normal/light transforms. Only the
        // exact material span is independent of the previously lit context.
        if (!PGL_LIT_MATERIAL_DELTA || originalClip
            || changes != RendererCtxtFlags::CurMaterial)
            return false;
    } else if (originalClip ? !PGL_CLIP_CONTEXT_DELTA : !PGL_UNLIT_CONTEXT_DELTA) {
        return false;
    }
    // Unknown/general invalidations (including the legacy 0xff full marker)
    // are never interpreted as a partial update. Any GS change also falls
    // back: draw-buffer/layout transitions can change clip/depth fields.
    return
#if !PGL_UNLIT_DELTA_SPECIALIZE
        changes != 0 && (changes & ~allowed) == 0 && !userChanged
        && unlitContextOwner == this && unlitContextPacket == &packet
        && unlitContextOriginalClip == originalClip
        && unlitContextPrim == primType &&
#endif
        !context.InDListDef()
        && context.GetGsContextChanged() == 0
        && unlitContextLightingEnabled == doLighting
        && unlitContextTexEnabled == context.GetTexManager().GetTexEnabled()
        && context.GetImmGeomManager().GetRendererManager().IsCurRendererCustom() == originalClip;
}

#if PGL_UNLIT_GS_CONTEXT_DELTA && PGL_UNLIT_CONTEXT_DELTA
void CBaseRenderer::CacheUnlitGsContext()
{
    CGLContext& context = *pGLContext;
    unlitGsContextValid = !context.InDListDef()
        && !context.GetImmLighting().GetLightingEnabled()
        && !context.GetImmGeomManager().GetRendererManager().IsCurRendererCustom();
    if (unlitGsContextValid)
        GetUnlitGsContextKey(context.GetImmDrawContext(), unlitGsContextKey);
}

bool CBaseRenderer::CanUseUnlitGsContextDelta(const CVifSCDmaPacket& packet,
    GLenum primType, uint32_t changes, bool userChanged) const
{
    const uint32_t allowed = RendererCtxtFlags::Xform | RendererCtxtFlags::CurMaterial
        | RendererCtxtFlags::TexEnabled;
    if (!unlitGsContextValid || changes == 0 || (changes & ~allowed) != 0
        || userChanged || unlitContextOwner != this || unlitContextPacket != &packet
        || unlitContextOriginalClip || unlitContextPrim != primType
        || unlitContextLightingEnabled)
        return false;

    CGLContext& context = *pGLContext;
    const uint32_t gsChanges = context.GetGsContextChanged();
    if ((gsChanges & ~(GsCtxtFlags::Texture | GsCtxtFlags::DrawEnv)) != 0
        || (gsChanges == 0 && !(changes & RendererCtxtFlags::TexEnabled))
        || context.InDListDef() || context.GetImmLighting().GetLightingEnabled()
        || unlitContextTexEnabled != context.GetTexManager().GetTexEnabled()
        || context.GetImmGeomManager().GetRendererManager().IsCurRendererCustom())
        return false;

    // q0 culling, q57 depth, q75 PRIM, q76 clip and q77 fog inputs must match.
    // q58..65 are still updated by the original material/transform writer.
    // Texture identities/modes and TEST/ALPHA/FRAME/XYOFFSET settings belong
    // to SyncGsContext, whose packets and ordering remain unconditional.
    uint32_t key[kUnlitGsContextKeyWords];
    GetUnlitGsContextKey(context.GetImmDrawContext(), key);
    return memcmp(key, unlitGsContextKey, sizeof(key)) == 0;
}
#endif

#if PGL_UNLIT_DELTA_SPECIALIZE
template <bool Lighting>
inline __attribute__((always_inline))
void CBaseRenderer::AddSpecializedContextDelta(CVifSCDmaPacket& packet, uint32_t changes)
#else
void CBaseRenderer::AddUnlitContextDelta(CVifSCDmaPacket& packet, uint32_t changes)
#endif
{
#if !defined(kContextStart) || kMaterialEmission != 58 || kMaterialAmbient != 59 || kMaterialDiffuse != 60 || \
    kMaterialSpecular != 61 || kVertexXfrm != 62
#error "Review the stock unlit delta spans against the VU context ABI"
#endif
    CGLContext& context = *pGLContext;
    const bool materialChanged = (changes & RendererCtxtFlags::CurMaterial) != 0;
    const bool xformChanged = (changes & RendererCtxtFlags::Xform) != 0;
    packet.Stcycl(1, 1);
    packet.Flush();
    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32,
        materialChanged ? kMaterialEmission : kVertexXfrm, Packet::kSingleBuff);
    if (materialChanged) {
        // Same operations/operand order as the full context. Keep all
        // material words, including alpha and currently unread specular.
        CImmMaterial& material = context.GetMaterialManager().GetImmMaterial();
        const float maxColorValue = GetMaxColorValue(context.GetTexManager().GetTexEnabled());
#if PGL_UNLIT_DELTA_SPECIALIZE
        const bool doLighting = Lighting;
#elif PGL_LIT_MATERIAL_DELTA
        const bool doLighting = context.GetImmLighting().GetLightingEnabled();
#else
        const bool doLighting = false;
#endif
        cpu_vec_4 emission;
        if (doLighting)
            emission = material.GetEmission() * maxColorValue;
        else
            emission = context.GetMaterialManager().GetCurColor() * maxColorValue;
        packet += emission;
        packet += material.GetAmbient();
        cpu_vec_4 matDiffuse = material.GetDiffuse();
        if (!doLighting)
            matDiffuse[3] = context.GetMaterialManager().GetCurColor()[3];
        packet += matDiffuse;
        packet += material.GetSpecular();
    }
    if (xformChanged) packet += context.GetImmDrawContext().GetVertexXform();
    packet.CloseUnpack();
}

#if PGL_UNLIT_DELTA_SPECIALIZE
#if PGL_LIT_MATERIAL_DELTA
// Keep the lit copy out of the unlit writer's instruction footprint. Both
// instantiate the same packet source; the restart stays in CLinearRenderer.
__attribute__((noinline))
void CBaseRenderer::AddLitMaterialContextDelta(CVifSCDmaPacket& packet, uint32_t changes)
{
    AddSpecializedContextDelta<true>(packet, changes);
}
#endif

void CBaseRenderer::AddUnlitContextDelta(CVifSCDmaPacket& packet, uint32_t changes)
{
#if PGL_LIT_MATERIAL_DELTA
    // CanUse has just matched this proof to the current lighting mode. Use
    // that validated bit, rather than walking the lighting owner again.
    if (unlitContextLightingEnabled) {
        AddLitMaterialContextDelta(packet, changes);
        return;
    }
#endif
    AddSpecializedContextDelta<false>(packet, changes);
}
#endif

void CBaseRenderer::NoteUnlitContext(const CVifSCDmaPacket& packet, GLenum primType,
    bool originalClip)
{
    CGLContext& context = *pGLContext;
    const bool doLighting = context.GetImmLighting().GetLightingEnabled();
    const bool enabled = doLighting ? (PGL_LIT_MATERIAL_DELTA && !originalClip)
        : (originalClip ? PGL_CLIP_CONTEXT_DELTA : PGL_UNLIT_CONTEXT_DELTA);
    if (!enabled || context.InDListDef()
        || context.GetImmGeomManager().GetRendererManager().IsCurRendererCustom() != originalClip) {
        pglInvalidateUnlitContextDelta();
        return;
    }
    unlitContextOwner = this;
    unlitContextPacket = &packet;
    unlitContextPrim = primType;
    unlitContextOriginalClip = originalClip;
    unlitContextLightingEnabled = doLighting;
    unlitContextTexEnabled = context.GetTexManager().GetTexEnabled();
}
#endif

void CBaseRenderer::GetUnpackAttribs(int numWords, unsigned int& mode, Vifs::tMask& mask)
{

    if (numWords == 3) {
        Vifs::tMask vec3Mask = { 0, 0, 0, 1,
            0, 0, 0, 1,
            0, 0, 0, 1,
            0, 0, 0, 1 };
        mode = Vifs::UnpackModes::v3_32;
        mask = vec3Mask;
    } else if (numWords == 4) {
        Vifs::tMask vec4Mask = { 0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0 };
        mode = Vifs::UnpackModes::v4_32;
        mask = vec4Mask;
    } else if (numWords == 2) {
        Vifs::tMask vec2Mask = { 0, 0, 1, 1,
            0, 0, 1, 1,
            0, 0, 1, 1,
            0, 0, 1, 1 };
        mode = Vifs::UnpackModes::v2_32;
        mask = vec2Mask;
    } else if (numWords == 1) {
        // PACKED BYTE vector (4 x u8 = 1 word): GL_UNSIGNED_BYTE color arrays.
        // NOT a "1-component" vector — the VIF expands V4-8 to a full qword.
        //
        // This case was MISSING, and its absence is the root cause of the
        // 2026-07-10 byte-color "position streak" incident: mError compiles to
        // NOTHING in release (debug_macros.h), so a 1-word array fell through
        // the else leaving `mode`/`mask` — which alias the caller's member
        // vars — holding the PREVIOUS draw's v4_32 values. The VIF then read 4
        // words per element where 1 was supplied, desynced the chain, and every
        // later unpack landed shifted: displaced positions, no crash, no log.
        Vifs::tMask byteMask = { 0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0 };
        mode = Vifs::UnpackModes::v4_8;
        mask = byteMask;
    } else {
        mError("shouldn't get here (you're probably calling glDrawArrays"
               "without setting one of the pointers)");
    }
}

/**
 * Caches some data frequently used by XferBlock(), sets up row register.
 * The parameters wordsPerNormal, wordsPerTex, and wordsPerColor should be
 * zero if the application has not given normals, texture coords, or colors.
 */
void CBaseRenderer::InitXferBlock(CVifSCDmaPacket& packet,
    int wordsPerVertex, int wordsPerNormal,
    int wordsPerTex, int wordsPerColor)
{
    CImmGeomManager& gmanager = pGLContext->GetImmGeomManager();

    NormalBuf   = &gmanager.GetNormalBuf();
    TexCoordBuf = &gmanager.GetTexCoordBuf();

    CurNormal             = gmanager.GetCurNormal();
    const float* texCoord = gmanager.GetCurTexCoord();
    CurTexCoord[0]        = texCoord[0];
    CurTexCoord[1]        = texCoord[1];

    // get unpack modes/masks

    // The format is a pure function of width, independent of GL/VU state.
    // Constructors initialize every cached width to zero (not a valid VIF
    // format), forcing first-use setup. Keep all four attributes initialized:
    // custom renderers can change XferNormals/XferColors AFTER InitXferBlock.
#if PGL_TRANSFER_FORMAT_CACHE
    if (WordsPerVertex != wordsPerVertex) {
#endif
    WordsPerVertex = wordsPerVertex;
    GetUnpackAttribs(WordsPerVertex, VertexUnpackMode, VertexUnpackMask);
#if PGL_TRANSFER_FORMAT_CACHE
    }
    if (WordsPerNormal != ((wordsPerNormal > 0) ? wordsPerNormal : 3)) {
#endif

    WordsPerNormal = (wordsPerNormal > 0) ? wordsPerNormal : 3;
    GetUnpackAttribs(WordsPerNormal, NormalUnpackMode, NormalUnpackMask);
#if PGL_TRANSFER_FORMAT_CACHE
    }
    if (WordsPerTexCoord != ((wordsPerTex > 0) ? wordsPerTex : 2)) {
#endif

    WordsPerTexCoord = (wordsPerTex > 0) ? wordsPerTex : 2;
    GetUnpackAttribs(WordsPerTexCoord, TexCoordUnpackMode, TexCoordUnpackMask);
#if PGL_TRANSFER_FORMAT_CACHE
    }
    if (WordsPerColor != ((wordsPerColor > 0) ? wordsPerColor : 3)) {
#endif

    WordsPerColor = (wordsPerColor > 0) ? wordsPerColor : 3;
    GetUnpackAttribs(WordsPerColor, ColorUnpackMode, ColorUnpackMask);
#if PGL_TRANSFER_FORMAT_CACHE
    }
#endif

    // set up the row register to expand vectors with fewer than 4 elements

    packet.Cnt();
    {
        // w is 256 to remind me that this is not used as the vertex w but
        // is necessary to clear any adc bits set for strips, otherwise they
        // accumulate..
        static const float row[4] = { 0.0f, 0.0f, 1.0f, 256.0f };
        packet.Strow(row);

        packet.Pad128();
    }
    packet.CloseTag();
}

/**
 * Transfers a block of geometry to vu0/vu1 using <i>packet</i>, where
 * "geometry" means vertices and zero or more normals,
 * texture coordinates, and colors.
 * <b>Note that you MUST set the vif1 write mode correctly before calling
 * XferBlock!!</b> (e.g., Stcycl(1, vu1QuadsPerVert))
 * normals, texCoords, and colors should be NULL if not provided.
 * @param vu1Offset offset into vu1 memory in quadwords
 * @param firstElement the starting "offset" into the vertex, normal, etc.
 *  arrays (for example: this would be "2" to start draw from the 3rd element)
 */
void CBaseRenderer::XferBlock(CVifSCDmaPacket& packet,
    const void* vertices, const void* normals,
    const void* texCoords, const void* colors,
    int vu1Offset, int firstElement, int numToAdd)
{
    //
    // vertices
    //

    if (XferVertices) {
        mErrorIf(vertices == NULL, "Tried to render an array with no vertices!");
        XferVectors(packet, (unsigned int*)vertices,
            firstElement, numToAdd,
            WordsPerVertex, VertexUnpackMask, VertexUnpackMode,
            vu1Offset);
    }

    //
    // normals
    //

    int firstNormal = firstElement;
    if (XferNormals && normals == NULL) {
        // no normals given, so use the current normal..
        // I hate to actually write every normal into the packet,
        // but I can't use the vif to expand the data because I
        // need it to interleave the vertices, normals, etc..
        CDmaPacket& normalBuf = *NormalBuf;
        normals               = (void*)normalBuf.GetNextPtr();
        firstNormal           = 0;

        for (int i = 0; i < numToAdd; i++)
            normalBuf += CurNormal;
    }

    if (XferNormals)
        XferVectors(packet, (unsigned int*)normals,
            firstNormal, numToAdd,
            WordsPerNormal, NormalUnpackMask, NormalUnpackMode,
            vu1Offset + 1);

    //
    // tex coords
    //

    int firstTexCoord = firstElement;
    if (XferTexCoords && texCoords == NULL) {
        // no tex coords given, so use the current value..
        // see note above for normals
        CDmaPacket& texCoordBuf = *TexCoordBuf;
        texCoords               = (void*)texCoordBuf.GetNextPtr();
        firstTexCoord           = 0;

        for (int i = 0; i < numToAdd; i++) {
            texCoordBuf += CurTexCoord[0];
            texCoordBuf += CurTexCoord[1];
        }
    }
    if (XferTexCoords)
        XferVectors(packet, (unsigned int*)texCoords,
            firstTexCoord, numToAdd,
            WordsPerTexCoord, TexCoordUnpackMask, TexCoordUnpackMode,
            vu1Offset + 2);

    //
    // colors
    //

    int firstColor = firstElement;
    if (colors != NULL && XferColors) {
        XferVectors(packet, (unsigned int*)colors,
            firstColor, numToAdd,
            WordsPerColor, ColorUnpackMask, ColorUnpackMode,
            vu1Offset + 3);
    }
}

void CBaseRenderer::AddVu1RendererContext(CVifSCDmaPacket& packet, GLenum primType, int vu1Offset,
    bool sparseUnlit)
{
#if PGL_UNLIT_CONTEXT_DELTA || PGL_CLIP_CONTEXT_DELTA || PGL_LIT_MATERIAL_DELTA
    pglInvalidateUnlitContextDelta();
#endif
    CGLContext& glContext = *pGLContext;
    CImmLighting& lighting = glContext.GetImmLighting();
    const bool doLighting = lighting.GetLightingEnabled();
    sparseUnlit = sparseUnlit && !doLighting;
#if kNumLights != 0 || kGlobalAmbient != 57 || kVertexXfrm != 62 || kGifTag != 75
#error "Review the stock unlit context spans against the VU context ABI"
#endif
#if PGL_SPARSE_LIGHT_CONTEXT && (kLight0Base != 9 || kLightStructSize != 6)
#error "Review the contiguous light-record prefix against the VU context ABI"
#endif

    packet.Stcycl(1, 1);
    packet.Flush();
    packet.Pad96();
    packet.OpenUnpack(Vifs::UnpackModes::v4_32, vu1Offset, Packet::kSingleBuff);
    {
        // find light pointers
        tLightPtrs lightPtrs[8];
        tLightPtrs *nextDir, *nextPt, *nextSpot;
        nextDir = nextPt = nextSpot = &lightPtrs[0];
        int numDirs, numPts, numSpots;
        numDirs = numPts = numSpots = 0;
#if PGL_SPARSE_LIGHT_CONTEXT
        int lightRecordCount = 0;
#else
        const int lightRecordCount = 8;
#endif
        for (int i = 0; !sparseUnlit && i < 8; i++) {
            CImmLight& light = lighting.GetImmLight(i);
            if (light.IsEnabled()) {
#if PGL_SPARSE_LIGHT_CONTEXT
                lightRecordCount = i + 1;
#endif
                int lightBase = kLight0Base + vu1Offset;
                if (light.IsDirectional()) {
                    nextDir->dir = lightBase + i * kLightStructSize;
                    nextDir++;
                    numDirs++;
                } else if (light.IsPoint()) {
                    nextPt->point = lightBase + i * kLightStructSize;
                    nextPt++;
                    numPts++;
                } else if (light.IsSpot()) {
                    nextSpot->spot = lightBase + i * kLightStructSize;
                    nextSpot++;
                    numSpots++;
                }
            }
        }
#if PGL_SPARSE_LIGHT_CONTEXT
        // Stock light loops dereference only the enabled pointers counted in
        // q0. Keep the original contiguous prefix (including holes) and omit
        // just its disabled tail; arbitrary/custom contexts retain all eight
        // records. This is a fresh upload, not a cross-draw lighting cache.
        if (!doLighting || glContext.InDListDef()
            || glContext.GetImmGeomManager().GetRendererManager().IsCurRendererCustom())
            lightRecordCount = 8;
#endif

        // transpose of object to world space xfrm (for light directions)
        cpu_mat_44 objToWorldXfrmTrans;
        float normalScale = 1.0f;
        CImmDrawContext& drawContext = glContext.GetImmDrawContext();
        if (!sparseUnlit) {
            objToWorldXfrmTrans = glContext.GetModelViewStack().GetTop();
            // clear any translations.. should be doing a 3x3 transpose..
            objToWorldXfrmTrans.set_col3(cpu_vec_xyzw(0, 0, 0, 1));
            objToWorldXfrmTrans = objToWorldXfrmTrans.transpose();
            // do we need to rescale normals?
            cpu_mat_44 normalRescale;
            normalRescale.set_identity();
            if (drawContext.GetRescaleNormals()) {
                cpu_vec_xyzw fake_normal(1, 0, 0, 0);
                fake_normal = objToWorldXfrmTrans * fake_normal;
                normalScale = 1.0f / fake_normal.length();
                normalRescale.set_scale(cpu_vec_xyz(normalScale, normalScale, normalScale));
            }
            objToWorldXfrmTrans = normalRescale * objToWorldXfrmTrans;
        }

        // num lights
        if (doLighting) {
            packet += numDirs;
            packet += numPts;
            packet += numSpots;
        } else {
            packet += (uint64_t)0;
            packet += 0;
        }

        // backface culling multiplier -- this is 1.0f or -1.0f, the 6th bit
        // also turns on/off culling
        float bfc_mult = (float)drawContext.GetCullFaceDir();
        unsigned int bfc_word;
        asm(" ## nop ## "
            : "=r"(bfc_word)
            : "0"(bfc_mult));
        bool do_culling = drawContext.GetDoCullFace() && (primType > GL_LINE_STRIP);
        packet += bfc_word | (unsigned int)do_culling << 5;

        float maxColorValue = GetMaxColorValue(glContext.GetTexManager().GetTexEnabled());

        if (sparseUnlit) {
            // All stock linear programs branch around lighting with q0.xyz=0.
            // Keep their exact material/fog/primitive fields; X2's smaller
            // custom context is NOT sufficient for these programs.
            packet.CloseUnpack();
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32,
                vu1Offset + kGlobalAmbient, Packet::kSingleBuff);
        } else {
            // light pointers
            packet.Add(&lightPtrs[0], 8);

            // add light info
            for (int i = 0; i < lightRecordCount; i++) {
                CImmLight& light = lighting.GetImmLight(i);
                packet += light.GetAmbient() * maxColorValue;
                packet += light.GetDiffuse() * maxColorValue;
                packet += light.GetSpecular() * maxColorValue;

                if (light.IsDirectional())
                    packet += light.GetPosition();
                else {
                    packet += light.GetPosition();
                }

                packet += light.GetSpotDir();

                // attenuation coeffs for positional light sources
                // because we're doing lighting calculations in object space,
                // we need to adjust the attenuation of positional light sources
                // and all lighting directions to take into account scaling
                packet += light.GetConstantAtten();
                packet += light.GetLinearAtten() * 1.0f / normalScale;
                packet += light.GetQuadAtten() * 1.0f / normalScale;
                packet += 0; // padding
            }
#if PGL_SPARSE_LIGHT_CONTEXT
            if (lightRecordCount < 8) {
                // The light pointers and every live record keep their exact
                // VU addresses. Resume at q57 so ambient/material/xforms/tag
                // retain their original words, order and synchronization.
                packet.CloseUnpack();
                packet.Pad96();
                packet.OpenUnpack(Vifs::UnpackModes::v4_32,
                    vu1Offset + kGlobalAmbient, Packet::kSingleBuff);
            }
#endif
        }

        // global ambient
        cpu_vec_4 globalAmb;
        if (doLighting)
            globalAmb = lighting.GetGlobalAmbient() * maxColorValue;
        else
            globalAmb = cpu_vec_4(0, 0, 0, 0);
        // Read the float object as floats. The old uint32_t* cast violates
        // strict aliasing: optimized EE code discarded the initialization and
        // uploaded stale stack bytes as ambient RGB, tinting unlit geometry.
        packet += globalAmb.x;
        packet += globalAmb.y;
        packet += globalAmb.z;

        // stick in the offset to convert clip space depth value to GS
#if PGL_CONTEXT_COEFFICIENT_CACHE
        float depthClipToGs = drawContext.GetContextDepthScale();
#else
        float depthClipToGs = (float)((1 << drawContext.GetDepthBits()) - 1) / 2.0f;
#endif
        // Both classic and X2 add this AFTER perspective division. Bias only
        // the output depth; depthClipToGs below still describes clipping.
        packet += depthClipToGs + drawContext.GetDepthOffset();

        // cur material

        CImmMaterial& material = glContext.GetMaterialManager().GetImmMaterial();

        // add emissive component
        cpu_vec_4 emission;
        if (doLighting)
            emission = material.GetEmission() * maxColorValue;
        else
            emission = glContext.GetMaterialManager().GetCurColor() * maxColorValue;
        packet += emission;

        // ambient
        packet += material.GetAmbient();

        // diffuse
        cpu_vec_4 matDiffuse = material.GetDiffuse();
        // the alpha value is set to the alpha of the diffuse in the renderers;
        // this should be the current color alpha if lighting is disabled
        if (!doLighting)
            matDiffuse[3] = glContext.GetMaterialManager().GetCurColor()[3];
        packet += matDiffuse;

        // specular
        packet += material.GetSpecular();

        // vertex xform
        packet += drawContext.GetVertexXform();

        if (sparseUnlit) {
            // No zero-light output uses q66..74. Preserve the VIF FLUSH above
            // and the final giftag write, so old buffers finish before reuse.
            packet.CloseUnpack();
            packet.Pad96();
            packet.OpenUnpack(Vifs::UnpackModes::v4_32,
                vu1Offset + kGifTag, Packet::kSingleBuff);
        } else {
            // fixed vertToEye vector for non-local specular
            cpu_vec_xyzw vertToEye(0.0f, 0.0f, 1.0f, 0.0f);
            packet += objToWorldXfrmTrans * vertToEye;

            // transpose of object to world space transform
            packet += objToWorldXfrmTrans;

            // world to object space xfrm (for light positions)
            cpu_mat_44 worldToObjXfrm = glContext.GetModelViewStack().GetInvTop();
            packet += worldToObjXfrm;
        }

        // giftag - this is down at the bottom to make sure that when switching
        // primitives the last buffer will have a chance to copy the giftag before
        // it is overwritten with the new one
        GLenum newPrimType = drawContext.GetPolygonMode();
        if (newPrimType == GL_FILL)
            newPrimType = primType;
        newPrimType &= 0xff;
        tGifTag giftag = BuildGiftag(newPrimType);
        packet += giftag;

        // add info used by clipping code
        // first the dimensions of the framebuffer
#if PGL_CONTEXT_COEFFICIENT_CACHE
        const cpu_vec_xyz& clipScales = drawContext.GetContextClipScales();
        packet += clipScales.x;
        packet += clipScales.y;
        packet += clipScales.z;
#else
        float xClip = (float)2048.0f / (drawContext.GetFBWidth() * 0.5f * 2.0f);
        packet += Math::Max(xClip, 1.0f);
        float yClip = (float)2048.0f / (drawContext.GetFBHeight() * 0.5f * 2.0f);
        packet += Math::Max(yClip, 1.0f);
        float depthClip = 2048.0f / depthClipToGs;
        // FIXME: maybe these 2048's should be 2047.5s...
        depthClip *= 1.003f; // round up a bit for fp error (????)
        packet += depthClip;
#endif
        // enable/disable clipping
        packet += (drawContext.GetDoClipping()) ? 1 : 0;

        // GS hardware fog params (kFogParams, see vu1/geometry.i fog_coef):
        // x unused, y = F clamp max, z = 255/(far - near), w = eye far Z
        float fogEnd   = drawContext.GetFogEnd();
#if PGL_CONTEXT_COEFFICIENT_CACHE
        float fogScale = drawContext.GetContextFogScale();
#else
        float fogStart = drawContext.GetFogStart();
        float fogScale = (fogEnd > fogStart) ? 255.0f / (fogEnd - fogStart) : 0.0f;
#endif
        // x rides the eye-space near plane for the VU1 clip renderer
        // (pglSetClipNear); the stock renderers never read it
        packet += drawContext.GetClipNear();
        packet += 254.0f;
        packet += fogScale;
        packet += fogEnd;

        // kFogPad guard qword (see vu1_context.h): sacrificial slot between the
        // fog params and the double-buffer base; content never read.
        packet += 0.0f;
        packet += 0.0f;
        packet += 0.0f;
        packet += 0.0f;
    }
    packet.CloseUnpack();
}

tGifTag
CBaseRenderer::BuildGiftag(GLenum primType)
{
    CGLContext& glContext = *pGLContext;

    primType &= 0x7; // convert from GL #define to gs prim number
    CImmDrawContext& drawContext = glContext.GetImmDrawContext();
    bool smoothShading           = drawContext.GetDoSmoothShading();
    bool useTexture              = glContext.GetTexManager().GetTexEnabled();
    bool alpha                   = drawContext.GetBlendEnabled();
    bool edgeAA                  = drawContext.GetEdgeAAEnabled();
    unsigned int nreg            = OutputQuadsPerVert;

    // GS edge anti-aliasing smooths polygon/line edges.  It does not filter
    // textures; texture shimmer still has to be diagnosed in the STQ path.
    // fge (verified live on HW, red-FOGCOL probe 2026-07-02): renderers whose
    // microcode computes a real per-vertex F (fog_coef) honor it; the others
    // still carry the strip-ADC filler in the F bits, so only enable GL_FOG
    // around draws that route to fog-aware renderers.
    bool fog = drawContext.GetFogEnabled();
    GS::tPrim prim = { prim_type : primType, iip : smoothShading, tme : useTexture, fge : fog, abe : alpha, aa1 : edgeAA, fst : 0, ctxt : 0, fix : 0 };
    tGifTag giftag = { NLOOP : 0, EOP : 1, pad0 : 0, id : 0, PRE : 1, PRIM : *(uint64_t*)&prim, FLG : 0, NREG : nreg, REGS0 : 2, REGS1 : 1, REGS2 : 4 };
    return giftag;
}

void CBaseRenderer::CacheRendererState()
{
    XferNormals   = pGLContext->GetImmLighting().GetLightingEnabled();
    XferTexCoords = pGLContext->GetTexManager().GetTexEnabled();
    XferColors    = pGLContext->GetMaterialManager().GetColorMaterialEnabled();
}

void CBaseRenderer::Load()
{
#if PGL_UNLIT_CONTEXT_DELTA || PGL_CLIP_CONTEXT_DELTA || PGL_LIT_MATERIAL_DELTA
    pglInvalidateUnlitContextDelta();
#endif
    unsigned int size64     = MicrocodePacketSize / 8;
    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    const u64* code         = (const u64*)MicrocodePacket;
    unsigned int addr64     = 0;

    mErrorIf((unsigned int)code & 0xf, "code not & 0xf");
    mErrorIf(MicrocodePacketSize & 0xf, "size not & 0xf");

    while (size64 > 0) {
        // Total send size
        unsigned int sendSize64 = (size64 > 256) ? 256 : size64;

        // Add send code command (VIF_CMD_MPG)
        packet.Ref(code, sendSize64 / 2);
        packet.Pad96();
        packet.Mpg(sendSize64 & 0xff, addr64);

        code += sendSize64;
        size64 -= sendSize64;
        addr64 += sendSize64;
    }
    packet.Cnt();
    packet.Mscal(0);
    packet.Pad128();
    packet.CloseTag();

    pglAddToMetric(kMetricsRendererUpload);
}

void CBaseRenderer::XferVectors(CVifSCDmaPacket& packet, unsigned int* dataStart,
    int startOffset, int numVectors, int wordsPerVec,
    Vifs::tMask unpackMask, uint32_t unpackMode,
    int vu1MemOffset)
{
    // find number of words to prepend with a cnt

    unsigned int* vecDataStart = dataStart + startOffset * wordsPerVec;
    unsigned int* vecDataEnd   = vecDataStart + numVectors * wordsPerVec;

    mAssert(numVectors > 0);
    mErrorIf((unsigned int)vecDataStart & (4 - 1),
        "XferVectors only works with word-aligned data");

#if PGL_ALIGNED_VECTOR_TRANSFER
    if ((((uintptr_t)vecDataStart | (uintptr_t)vecDataEnd) & 15u) == 0u) {
        /* Same packet as the generic path with both edge counts zero.
         * Keep the mask, double-buffer bit, REF alias and UNPACK count; the
         * source still belongs to its frame until DMA completes. */
        packet.Cnt();
        packet.Stmask(unpackMask);
        packet.Pad128();
        packet.CloseTag();
        packet.Ref(Core::MakePtrNormal(vecDataStart),
            (numVectors * wordsPerVec) / 4);
        packet.Nop();
        packet.OpenUnpack(unpackMode, vu1MemOffset,
            VifDoubleBuffered, Packet::kMasked);
        packet.CloseUnpack(numVectors);
        pglCountSubmission(PGL_SUBMIT_REF_BYTES, numVectors * wordsPerVec * 4u);
        return;
    }
#endif

    int numWordsToPrepend      = 0;
    unsigned int* refXferStart = vecDataStart;
    while ((unsigned int)refXferStart & (16 - 1)) {
        numWordsToPrepend++;
        refXferStart++;
        if (refXferStart == vecDataEnd)
            break;
    }
    int numWordsToAppend     = 0;
    unsigned int* refXferEnd = vecDataEnd;
    while (((unsigned int)refXferEnd & (16 - 1)) && refXferEnd > refXferStart) {
        numWordsToAppend++;
        refXferEnd--;
    }
    int numQuadsInRefXfer = ((unsigned int)refXferEnd - (unsigned int)refXferStart) / 16;
    pglCountSubmission(PGL_SUBMIT_REF_BYTES, numQuadsInRefXfer * 16u);
    pglCountSubmission(PGL_SUBMIT_EDGE_BYTES, (numWordsToPrepend + numWordsToAppend) * 4u);

    packet.Cnt();
    {
        // set mask to expand vectors appropriately
        packet.Stmask(unpackMask);

        // prepend
        if (numWordsToPrepend > 1) {
            // either 2 or 3 words to prepend
            packet.Nop().Nop();
            if (numWordsToPrepend == 2)
                packet.Nop();

            packet.OpenUnpack(unpackMode,
                vu1MemOffset,
                VifDoubleBuffered,
                Packet::kMasked);
            packet.CloseUnpack(numVectors);

            if (numWordsToPrepend == 3)
                packet += *vecDataStart;
        }

        packet.Pad128();
    }
    packet.CloseTag();

    // xfer qword block of vectors
    packet.Ref(Core::MakePtrNormal(refXferStart), numQuadsInRefXfer);
    {
        // either 0 words to prepend or 1 word left to prepend
        if (numWordsToPrepend == 0)
            packet.Nop();
        if (numWordsToPrepend <= 1) {
            packet.OpenUnpack(unpackMode,
                vu1MemOffset,
                VifDoubleBuffered,
                Packet::kMasked);
            packet.CloseUnpack(numVectors);
        }
        if (numWordsToPrepend == 1)
            packet += *vecDataStart;
        else if (numWordsToPrepend == 2)
            packet.Add(vecDataStart, 2);
        else if (numWordsToPrepend == 3)
            packet.Add(&vecDataStart[1], 2);
    }

    // xfer any remaining vectors
    if (numWordsToAppend > 0) {
        packet.Cnt();
        {
            packet.Add(refXferEnd, numWordsToAppend);
            packet.Pad128();
        }
        packet.CloseTag();
    }
}
