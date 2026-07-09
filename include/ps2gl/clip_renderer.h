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

/* The DOUBLE-KICK city renderer (vu1/general_clip_tri_x2.vcl): per-vertex
   color through the 5-plane S-H clip, then two XGKICKs per buffer — opaque
   wall (pv color, base texture) + additive window overlay (constant color,
   window texture) from the same transformed verts. Selected through
   PGL_CLIP_TRIANGLES_X2. See PS2/plans/todo/VU1_X2_PLAN.md. */
class CClipTriX2Renderer : public CLinearRenderer {
    // The pair for the block being drawn is read LIVE in BuildPrefixes.
    // That is only correct because SetWindowTexture submits any pending block
    // BEFORE the pair changes (see its comment) — so a block is always built
    // against the GL/GS state it was submitted with.
    CMMTexture* WinTex;
    float WinColor[4]; // 0..1 floats as passed to pglClipX2SetWindowTexture

    // per-draw prefix block, unpacked ONCE per buffer into the STAGING zone
    // (vu 178..200) — the microcode copies it to the kick sites (VIF-written
    // qwords must never be GIF-read: VIF races the GIF, the city flicker):
    //   Pfx[0..1]   [win color][pad]
    //   Pfx[2..11]  wall texture settings giftag + 9 A+D regs, normal-blend ALPHA_1
    //   Pfx[12..21] win  texture settings giftag + 9 A+D regs, additive ALPHA_1
    //   Pfx[22]     window prim giftag template, ABE=1
    // MUST be 23: kX2PfxOff's unpack sends 23q and the wall-only branch memsets
    // Pfx[12..22]. At 21 that memset wrote 32 bytes of zeros past the end of this
    // heap-allocated object, onto the next malloc chunk's header — a corrupt free
    // list that only surfaced later, inside mallinfo(). Keep in sync with the 23q
    // block in vu1/general_clip_tri_x2.vcl and packet.Add(Pfx, 23).
    uint128_t Pfx[23] __attribute__((aligned(16)));

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
