/* HyperSolar VU1 near-plane clip renderer (tri lists) + the double-kick
   city renderer (x2). See clip_renderer.h. */

#include <string.h>

#include "ps2s/drawenv.h"
#include "ps2s/math.h"

#include "GL/ps2gl.h"

#include "ps2gl/clip_renderer.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
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
 * CClipTriX2Renderer — the dual-context city renderer
 */

// VU-mem unpack target: the STAGING zone of general_clip_tri_x2.vcl's
// layout (VU-READ only — the microcode builds the kicked qwords itself;
// VIF-written qwords must never be GIF-read). Changing either side means
// re-checking the whole layout.
#define kX2PfxOff 178 // [win color][win prim giftag template] = 2q

CClipTriX2Renderer::CClipTriX2Renderer()
    : CLinearRenderer(mVsmAddr(GeneralClipTriX2), mVsmSize(GeneralClipTriX2), 4, 3,
          kInputStart, 120,
          "clip x2, city walls+windows")
    , WinTex(NULL)
    , Ctx2Armed(false)
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

void CClipTriX2Renderer::Register()
{
    pX2Renderer = new CClipTriX2Renderer;
    pglRegisterRenderer(pX2Renderer);

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
    // Re-program GS context 2 on the next x2 draw: the pair changed, and
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
    CLinearRenderer::InitContext(GL_TRIANGLES, rcChanges, userRcChanged);
}

void CClipTriX2Renderer::BuildPrefixes(CVifSCDmaPacket& packet, CGeometryBlock& block)
{
    // Live state is correct here ONLY because every x2 draw is flushed while its
    // own bindings are still current: SetWindowTexture flushes on a pair change,
    // and the GAME flushes after its final x2 draw of a pass. Drop either half
    // and a pending block reads the next drawer's texture (see SetWindowTexture).
    CTexManager& tm = pGLContext->GetTexManager();
    mErrorIf(!tm.GetTexEnabled(), "the x2 renderer needs texturing enabled (city walls)");

    // Pfx[0]: the window color const. x128 like the vcl's fmt_color: textured
    // GS modulate identity is 128 (GetMaxColorValue) — x255 was 2x overbright.
    // Only rgb is consumed; window verts take their alpha from the wall verts'
    // pv alpha (the fade animation). x < 0 = window kick disabled (wall-only).
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
        tag.PRIM |= 0x40 | 0x200;
        *(tGifTag*)&Pfx[1] = tag;
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
        // Path ordering (mirrors SyncGsContext's texture send): wait for
        // path 1 to drain before the path-2 sends below — prior buffers'
        // kicks may still be drawing with the OLD ctx2 state / texture data.
        packet.Cnt();
        packet.Flush().Nop();
        packet.CloseTag();

        // PSMT8 window: clut upload + TEX0.cb — MUST precede the settings
        // copy below (SetClut writes TEX0).
        GS::tPSM psm = WinTex->GetPSM();
        if (psm == GS::kPsm8 || psm == GS::kPsm8h) {
            CMMClut* clut = WinTex->GetOwnClut();
            mErrorIf(clut == NULL, "x2 PSMT8 window texture needs its own clut");
            if (clut) {
                clut->Load(packet);
                WinTex->SetClut(*clut);
            }
        }
        // The upload half of Use() ONLY: its ctx1 settings send would stomp
        // the wall texture's live TEX0_1 (see LoadIfDirty's comment). Inert
        // for game-managed textures (XferImage=false).
        WinTex->LoadIfDirty(packet);

        // Ctx2[0..7]: the window texture's OWN settings block (giftag + 7
        // A+D regs), register addresses rewritten to context 2 — so
        // punch-through TEXA + custom mip MIPTBPs ride verbatim. TEXA is
        // global (safe with the x2 pairing: a PSMT8 wall takes alpha from
        // its CLUT, only the PSMCT16 window reads TEXA); TEXFLUSH stripped
        // to NOP (the real upload path above kept its flush — a TEX0
        // retarget to resident data needs none, GS manual pp. 43/51/63/131).
        memcpy(&Ctx2[0], WinTex->GetSettingsBlock(), 8 * 16);
        tGifTag* cg = (tGifTag*)&Ctx2[0];
        cg->NLOOP = 15;
        uint64_t* tq = (uint64_t*)&Ctx2[1];
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
        const GS::CDrawEnv& de = pGLContext->GetImmDrawContext().GetDrawEnv();
        const uint64_t testBase = de.GetTestReg();
        const uint64_t winTest = (testBase & ~(uint64_t)0x3fff) // clear ATE/ATST/AREF/AFAIL
            | (uint64_t)1                                       // ATE  = 1
            | ((uint64_t)6 << 1);                               // ATST = GREATER (AREF=0, AFAIL=KEEP)
        uint64_t* rq = (uint64_t*)&Ctx2[8];
        // ALPHA_2 = Cv = (Cs - 0) * As + Cd  (glBlendFunc(SRC_ALPHA, ONE)):
        // a=Cs(0) b=0(2) c=As(0) d=Cd(1) -> 0x48
        rq[0] = 0x48;
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

        // DIRECT-send the block down GIF path 2 (same qword-alignment nop
        // trick as CTexEnv::SendSettings).
        packet.Cnt();
        {
            packet.Nop();
            if (!packet.GetTTE())
                packet.Nop().Nop();
            packet.OpenDirect();
            packet.Add(Ctx2, 16);
            packet.CloseDirect();
        }
        packet.CloseTag();

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

    // per-buffer vert cap: multiple of SIX (the 3n-1 splitter lesson)
    int maxVertsPerBuffer = InputGeomBufSize / InputQuadsPerVert;
    if (maxVertsPerBuffer > 256)
        maxVertsPerBuffer = 256;
    maxVertsPerBuffer -= 3;
    maxVertsPerBuffer -= maxVertsPerBuffer % 6;

    DrawBlockX2(packet, block, maxVertsPerBuffer);
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
