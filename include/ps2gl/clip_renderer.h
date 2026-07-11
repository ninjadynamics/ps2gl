/* HyperSolar VU1 near-plane clip renderer (tri lists).

   A user renderer wrapping vu1/general_clip_tri.vcl, selected through the
   PGL_CLIP_TRIANGLES custom prim type (see GL/ps2gl.h). Phase 1 is a 1:1
   clone of the stock "linear, tris, no specular" renderer under its own
   microcode so the toolchain + registration loop is proven before any clip
   logic lands; phases 2/3 give the microcode true near-plane classify/clip
   with 1:N output. */

#ifndef clip_renderer_h
#define clip_renderer_h

#include "ps2gl/linear_renderer.h"
#include "ps2gl/renderer.h"

class CClipTriRenderer : public CLinearRenderer {
public:
    CClipTriRenderer();

    static void Register();

    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

class CMMTexture;

/* The DUAL-CONTEXT city renderer (vu1/general_clip_tri_x2.vcl): per-vertex
   color through the 5-plane S-H clip, then ONE compound XGKICK per buffer —
   wall prims on GS context 1 (live ps2gl state: texture, blend, test; giftag
   EOP=0) immediately followed by the additive window prims on GS context 2
   (PRIM.CTXT=1: window texture + additive ALPHA_2 + alpha-test TEST_2,
   programmed ONCE per pass; giftag EOP=1 closes the kick). The GS's two
   persistent contexts exist exactly so intermixed prims can switch state via
   PRIM.CTXT without repeating register loads (GS manual pp. 47-49); this
   replaces the old per-micro-buffer settings prefixes + double kick (~206
   state blocks, TEXFLUSHes and extra kicks per frame at dense city load).
   Selected through PGL_CLIP_TRIANGLES_X2. */
class CClipTriX2Renderer : public CLinearRenderer {
    // The pair for the block being drawn is read LIVE in BuildPrefixes.
    // That is only correct because SetWindowTexture submits any pending block
    // BEFORE the pair changes (see its comment) — so a block is always built
    // against the GL/GS state it was submitted with.
    CMMTexture* WinTex;
    float WinColor[4]; // 0..1 floats as passed to pglClipX2SetWindowTexture

    // Context-2 program status. GS context 2 is NOT ours alone: glClear draws
    // through a kContext2 CDrawEnv + CSprite (clear.cpp) and stomps it every
    // frame. SetWindowTexture() disarms; the first BuildPrefixes after it
    // re-programs ctx2 down the chain. Correct because the game re-calls
    // pglClipX2SetWindowTexture for every x2 pass and nothing clears
    // mid-pass; the live FRAME/ZBUF/XYOFFSET/SCISSOR this mirrors are stable
    // within a pass (layout switches happen between frames).
    bool Ctx2Armed;

    // per-draw staging block, unpacked per buffer (double-buffered) into the
    // vcl's STAGING zone — VU-READ only, never kicked in place (VIF races GIF
    // path 1; kicked qwords must be VU-written):
    //   Pfx[0]  window color const (floats, rgb GS 0..128 range;
    //           x < 0 = window kick disabled, wall-only mode)
    //   Pfx[1]  window prim giftag TEMPLATE: ABE=1, CTXT=1 (context 2),
    //           EOP=1 — the vcl patches NLOOP in and writes it after the last
    //           wall vert to close the compound kick.
    // Size this from the transfer count (packet.Add(Pfx, 2)) and keep in sync
    // with the 2q staging block in vu1/general_clip_tri_x2.vcl. (A shortfall
    // here was a 32-byte heap smasher once — the Pfx[21] incident.)
    uint128_t Pfx[2] __attribute__((aligned(16)));

    // The context-2 settings block DIRECT-sent down the chain when
    // !Ctx2Armed: giftag + 15 A+D regs (the window texture's 7 settings regs
    // address-rewritten ctx1 -> ctx2, additive ALPHA_2, TEST_2, live
    // FRAME_2/ZBUF_2/XYOFFSET_2/SCISSOR_2/FBA_2 mirrors of the draw env, and
    // the ctx1 TEST_1 ATE-off pin for the wall pass).
    // Same transfer-count sizing rule as Pfx (packet.Add(Ctx2, 16)).
    uint128_t Ctx2[16] __attribute__((aligned(16)));

    void BuildPrefixes(CVifSCDmaPacket& packet, CGeometryBlock& block);
    void XferPrefixes(CVifSCDmaPacket& packet);
    void DrawBlockX2(CVifSCDmaPacket& packet, CGeometryBlock& block, int maxVertsPerBuffer);

public:
    CClipTriX2Renderer();

    static void Register();

    void SetWindowTexture(unsigned int texId, float r, float g, float b, float a);

    virtual void InitContext(GLenum primType, uint32_t rcChanges, bool userRcChanged);
    virtual void DrawLinearArrays(CGeometryBlock& block);
};

#endif // clip_renderer_h
