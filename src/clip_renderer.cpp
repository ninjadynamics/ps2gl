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
 * CClipTriX2Renderer — the double-kick city renderer
 */

// VU-mem unpack target: the STAGING zone of general_clip_tri_x2.vcl's
// layout (the microcode copies it to the kick sites — VIF-written qwords
// must never be GIF-read). Changing either side means re-checking the
// whole layout.
#define kX2PfxOff 178 // [win color][pad][wall 10q][win 10q][win giftag] = 23q

CClipTriX2Renderer::CClipTriX2Renderer()
    : CLinearRenderer(mVsmAddr(GeneralClipTriX2), mVsmSize(GeneralClipTriX2), 4, 3,
          kInputStart, 120,
          "clip x2, city walls+windows")
    , WinTex(NULL)
{
    // 120 quads = 30 verts of 4q input (pos, [normal unused], stq, color);
    // the microcode owns the rest of its buffer layout (planes + 3q-vert
    // polygon scratch + TWO prefixed output packets — see the .vcl header).
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
    CMMTexture& wall = tm.GetCurTexture();

    // GS TEST for the two kicks, both derived from ps2gl's LIVE drawenv value �
    // TEST also carries ZTE/ZTST and the dest-alpha test, so building it from
    // scratch would silently disable depth testing.
    //   wall kick:   alpha test OFF (ATE=0), everything else as ps2gl set it.
    //   window kick: ATE=1, ATST=GREATER, AREF=0, AFAIL=KEEP. Window gap texels
    //     sample alpha exactly 0 (16-bit 5551 + TEXA ta0=0), and an additive
    //     blend of As=0 is Cs*0 + Cd = Cd � a framebuffer read-modify-write that
    //     cannot change a bit. Discarding them is bit-identical and skips the
    //     RMW (the GS is fill-bound: the window kick's second rasterization
    //     measured 2.26 ms of `other`). Lit texels give A = At*Ag>>7 = Ag
    //     (ta1=0x80 identity), so a fading building keeps every lit texel while
    //     Ag >= 1; at Ag == 0 the add is zero anyway. Mip levels key alpha at
    //     ANY coverage (ps2_mip16_cache_build), so distant dimmed windows pass.
    // ATE:1 | ATST:3 | AREF:8 | AFAIL:2 = the low 14 bits.
    const uint64_t testBase = pGLContext->GetImmDrawContext().GetDrawEnv().GetTestReg();
    const uint64_t wallTest = testBase & ~(uint64_t)1;         // ATE = 0
    const uint64_t winTest  = (testBase & ~(uint64_t)0x3fff)   // clear ATE/ATST/AREF/AFAIL
        | (uint64_t)1                                          // ATE  = 1
        | ((uint64_t)6 << 1);                                  // ATST = GREATER (AREF=0, AFAIL=KEEP)

    // Pfx[0..1]: [win color const][pad]. Pfx[2..9]: the wall settings block �
    // the texture's OWN giftag + 7 A+D regs (TEXFLUSH/CLAMP/TEX1/TEX0/TEXA/
    // MIPTBP1/2), copied wholesale so punch-through TEXA + custom mip
    // MIPTBPs ride verbatim � plus a NORMAL-blend ALPHA_1 (NLOOP grown to
    // 8): the previous buffer's window kick leaves ADDITIVE alpha, and the
    // fade overlay draws its walls with ABE=1 (inert for the opaque main
    // city). EOP cleared: the wall prim follows in-kick.
    float* wc = (float*)&Pfx[0];
    if (WinTex) {
        // x128 like the vcl's fmt_color: textured GS modulate identity is
        // 128 (GetMaxColorValue) � x255 was 2x overbright. Only rgb is
        // consumed; window verts take their alpha from the wall verts' pv
        // alpha (the fade animation).
        wc[0] = WinColor[0] * 128.0f;
        wc[1] = WinColor[1] * 128.0f;
        wc[2] = WinColor[2] * 128.0f;
        wc[3] = WinColor[3] * 128.0f;
    } else {
        wc[0] = -1.0f; // window kick disabled (wall-only mode)
        wc[1] = wc[2] = wc[3] = 0.0f;
    }
    memset(&Pfx[1], 0, 16);
    memcpy(&Pfx[2], wall.GetSettingsBlock(), 8 * 16);
    {
        tGifTag* bg = (tGifTag*)&Pfx[2];
        bg->EOP   = 0;
        bg->NLOOP = 9;
        // ALPHA_1 = Cv = (Cs - Cd) * As + Cd  (glBlendFunc(SRC_ALPHA,
        // ONE_MINUS_SRC_ALPHA), the fade overlay's blend):
        // a=Cs(0) b=Cd(1) c=As(0) d=Cd(1) -> 0x44
        uint64_t* bq = (uint64_t*)&Pfx[10];
        bq[0] = 0x44;
        bq[1] = GS::RegAddrs::alpha_1;
        uint64_t* bt = (uint64_t*)&Pfx[11];
        bt[0] = wallTest;
        bt[1] = GS::RegAddrs::test_1;
    }

    // Pfx[12..19]: the window settings block, NLOOP grown to 9 for the
    // appended additive ALPHA_1 + TEST_1. Pfx[22]: the window prim giftag
    // template. Only built when the window kick is on.
    if (WinTex) {
        // Path ordering (mirrors SyncGsContext's texture send): wait for
        // path 1 to drain before any path-2 texture/clut upload, or the
        // new data can beat the PREVIOUS buffers' kicks that still sample
        // the old contents ("the new texture settings might beat the
        // geometry to the gs").
        packet.Cnt();
        packet.Flush().Nop();
        packet.CloseTag();

        // Mirror UseCurTexture's bind work for a texture ps2gl never sees
        // draw-bound in x2 mode. PSMT8 first: the clut upload + TEX0.cb �
        // and it MUST precede the settings-block copy below (SetClut writes
        // TEX0). Then Use(), NOT Load(): game-managed textures (pal8/mip16
        // custom uploads) have no pImageMem and XferImage=false � a raw
        // Load() stores through NULL (the stage-3 TLB crash); Use() guards
        // it and its chain-level settings resend is harmless.
        GS::tPSM psm = WinTex->GetPSM();
        if (psm == GS::kPsm8 || psm == GS::kPsm8h) {
            CMMClut* clut = WinTex->GetOwnClut();
            mErrorIf(clut == NULL, "x2 PSMT8 window texture needs its own clut");
            if (clut) {
                clut->Load(packet);
                WinTex->SetClut(*clut);
            }
        }
        WinTex->Use(packet);
        memcpy(&Pfx[12], WinTex->GetSettingsBlock(), 8 * 16);
        tGifTag* wg = (tGifTag*)&Pfx[12];
        wg->EOP   = 0;
        wg->NLOOP = 9;

        // ALPHA_1 = Cv = (Cs - 0) * As + Cd  (glBlendFunc(SRC_ALPHA, ONE)):
        // a=Cs(0) b=0(2) c=As(0) d=Cd(1) -> 0x48
        uint64_t* q = (uint64_t*)&Pfx[20];
        q[0] = 0x48;
        q[1] = GS::RegAddrs::alpha_1;
        uint64_t* wt = (uint64_t*)&Pfx[21];
        wt[0] = winTest;
        wt[1] = GS::RegAddrs::test_1;

        // window prim tag: the wall's giftag with ABE forced on (bit 6 of
        // PRIM: prim_type:3, iip:1, tme:1, fge:1, abe:1). NLOOP patched on
        // VU1 with the emitted count; EOP=1 ends the window kick.
        tGifTag tag = BuildGiftag(GL_TRIANGLES);
        tag.PRIM |= 0x40;
        *(tGifTag*)&Pfx[22] = tag;
    } else {
        memset(&Pfx[12], 0, 11 * 16);
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
        packet.Add(Pfx, 23);
        packet.CloseUnpack(23);
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

// Copy of CLinearRenderer::DrawBlock with ONE change: the prefix blocks are
// unpacked into both output packets before every buffer kick (XferPrefixes
// before each FinishBuffer). Keep in sync with linear_renderer.cpp.
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
