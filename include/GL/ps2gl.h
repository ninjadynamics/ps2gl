/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_h
#define ps2gl_h

#include "GL/gl.h"

/* Aligned client-array transfers use the identical CNT/STMASK/REF/UNPACK
 * packet without running the generic unaligned-head/tail preparation. */
#ifndef PGL_ALIGNED_VECTOR_TRANSFER
#define PGL_ALIGNED_VECTOR_TRANSFER 1
#endif
#if PGL_ALIGNED_VECTOR_TRANSFER != 0 && PGL_ALIGNED_VECTOR_TRANSFER != 1
#error "PGL_ALIGNED_VECTOR_TRANSFER must be 0 or 1"
#endif

/* Whole-renderer A/B: CPU construction of the two normal frame DMA chains.
 * 0 preserves UCAB; 1 uses cached memory with the existing Send() writeback.
 * Geometry, immediate packets, display lists and VU programs are unchanged.
 * This is a library build setting: rebuild ps2gl EE objects after changing it.
 */
#ifndef PGL_CACHED_FRAME_PACKETS
#define PGL_CACHED_FRAME_PACKETS 1
#endif
#if PGL_CACHED_FRAME_PACKETS != 0 && PGL_CACHED_FRAME_PACKETS != 1
#error "PGL_CACHED_FRAME_PACKETS must be 0 or 1"
#endif

/* Defer immediate glLoadMatrixf inversion until an inverse is consumed.
 * Concat retains the original inverse multiplication order; display-list
 * recording remains eager. Independent library-build A/B, no matrix rounding
 * change. Rebuild all ps2gl EE objects when changing this class-layout gate. */
#ifndef PGL_LAZY_MATRIX_INVERSE
#define PGL_LAZY_MATRIX_INVERSE 1
#endif
#if PGL_LAZY_MATRIX_INVERSE != 0 && PGL_LAZY_MATRIX_INVERSE != 1
#error "PGL_LAZY_MATRIX_INVERSE must be 0 or 1"
#endif

/********************************************
 * types
 */

typedef long long pgl64_t;
typedef unsigned long long pglU64_t;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

// immBufferVertexSize is the size in vertices of the buffers used to store
// glBegin/glEnd geometry. there are currently 2 sets of buffers:
// vertex, normal, tex coord, and color buffers.
extern int pglInit(int immBufferVertexSize, int immDrawBufferQwordSize);
extern int pglHasLibraryBeenInitted(void);
extern void pglFinish(void);

extern void pglWaitForVU1(void);
extern void pglWaitForVSync(void);
extern void pglSwapBuffers(void);

// gs memory allocation

extern void pglPrintGsMemAllocation(void);
extern void pglGetGsMemInfo(int* total, int* used, int* largestFreeSlot);
extern int pglHasGsMemBeenInitted(void);

// HyperSolar: hand the CURRENT texture's image buffer (the pointer last passed to
// glTexImage2D) to ps2gl to free() on glDeleteTextures. glTexImage2D stores the
// caller's pointer without owning it, so a caller that allocs-and-forgets leaks it
// (raylib4ps2 rlLoadTexturePS2). Call right after glTexImage2D, before unbinding.
extern void pglTexImageTakeOwnership(void);

/* HyperSolar: cached-dlist render-packet sizing (qwords). The packet holds DMA
   tags + VIF codes only (vertex data rides REF tags), so the estimate is
   size = verts*qwPerVert + strips*qwPerStrip + qwFlat. Library defaults
   (2 / 48 / 512, ~3x measured need) apply if never called; call BEFORE the
   first display list is played to supersede them. The stock code reserved
   14.3 qw/vert ("a pitiful hack") — 1.4 MB for one game's model dlists. */
extern void pglSetDListPacketSizing(int qwPerVert, int qwPerStrip, int qwFlat);

/* Reserve exact attribute counts in a newly opened, still-empty display list.
   Returns zero on allocation failure; no geometry may be emitted in that case.
   Counts include every glBegin/glEnd block in the list. Existing clients that
   do not reserve retain the default fixed-capacity attribute buffers. */
extern int pglReserveDListGeometry(unsigned int vertices, unsigned int normals,
    unsigned int texCoords, unsigned int colors);

// gs mem slots

typedef unsigned int pgl_slot_handle_t;

extern pgl_slot_handle_t pglAddGsMemSlot(int startingPage, int pageLength, unsigned int pixelMode);
extern void pglLockGsMemSlot(pgl_slot_handle_t slot_handle);
extern void pglUnlockGsMemSlot(pgl_slot_handle_t slot_handle);
extern void pglRemoveAllGsMemSlots();

// gs mem areas

typedef unsigned int pgl_area_handle_t;

extern pgl_area_handle_t pglCreateGsMemArea(int width, int height, unsigned int pix_format);
extern void pglDestroyGsMemArea(pgl_area_handle_t mem_area);

extern void pglAllocGsMemArea(pgl_area_handle_t mem_area);
extern void pglFreeGsMemArea(pgl_area_handle_t mem_area);

extern void pglSetGsMemAreaWordAddr(pgl_area_handle_t mem_area, unsigned int addr);

extern void pglBindGsMemAreaToSlot(pgl_area_handle_t mem_area, pgl_slot_handle_t mem_slot);
extern void pglUnbindGsMemArea(pgl_area_handle_t mem_area);

extern void pglLockGsMemArea(pgl_area_handle_t mem_area);
extern void pglUnlockGsMemArea(pgl_area_handle_t mem_area);

extern int pglGsMemAreaIsAllocated(pgl_area_handle_t mem_area);
extern unsigned int pglGetGsMemAreaWordAddr(pgl_area_handle_t mem_area);

// display and draw management

extern void pglSetDisplayBuffers(int interlaced,
    pgl_area_handle_t frame0_mem, pgl_area_handle_t frame1_mem);
extern void pglSetDrawBuffers(int interlaced,
    pgl_area_handle_t frame0_mem, pgl_area_handle_t frame1_mem,
    pgl_area_handle_t depth_mem);

/* Runtime video-mode reconfigure (reuses the current frame buffers). Pair with
   SetGsCrt(). interlaced: 1 NTSC/PAL, 0 480p. overscan_mode: 0 NTSC,1 PAL,2 DTV.
   screen_x / screen_y: horizontal / vertical raster shift for centering (display
   pixels; may be negative -- the GS DX/DY field truncation handles the wrap). */
extern void pglSetVideoMode(int interlaced, int overscan_mode, int screen_x, int screen_y);

/* Live raster re-center (Screen Pos): push ONLY the DISPLAY register for the
   current mode, leaving frame buffer / background alone (no overscan-border
   flash). Use pglSetVideoMode for a full mode change. */
extern void pglSetDisplayOffset(int screen_x, int screen_y);

/* Flicker filter (interlace softening): blend display read circuit 1 (offset one
   scanline) over RC2 with a constant alpha -- a 2-tap vertical low-pass that
   stabilizes 448i output. enable: 0/1. alpha: RC1 (neighbor-line) weight 0..255.
   Only stores state; re-issue pglSetVideoMode to apply. Interlaced modes only. */
extern void pglSetFlickerFilter(int enable, int alpha);

/* Centered viewport squish for overscan "screen fit". sx/sy are fractions
   (0 < s <= 1, 1.0 = full frame); scales by the draw buffer's own dims so it is
   correct in interlaced (half-height) modes. Persists across pglSetVideoMode. */
extern void pglSetViewportScale(float sx, float sy);

/* Immediate draws only: constant GS depth offset, default 0. Positive units
   move depth nearer (ps2gl reverses Z). Does not move geometry, alter clipping,
   or implement glPolygonOffset's slope term. Flushes pending draws on change;
   callers must restore 0 afterwards and keep biased depth within its range. */
extern void pglSetDepthOffset(float units);
/* Read the live NDC-to-GS diagonal scale (signed X/Y/Z, before center offsets).
   Includes the active draw-buffer size, screen fit and depth format. */
extern void pglGetRasterScale(float out[3]);

// textures

void pglTextureFromGsMemArea(pgl_area_handle_t tex_area_handle);

void pglBindTextureToSlot(GLuint texId, pgl_slot_handle_t mem_slot);
void pglFreeTexture(GLuint texId);

// geometry

void pglNormalPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* ptr);
void pglDrawIndexedArrays(GLenum primType,
    int numIndices, const unsigned char* indices,
    int numVertices);

void pglBeginImmediateGeometry(void);
void pglEndImmediateGeometry(void);
void pglRenderImmediateGeometry(void);
void pglFinishRenderingImmediateGeometry(int forceImmediateStop);

void pglBeginGeometry(void);
void pglEndGeometry(void);
void pglRenderGeometry(void);
void pglFinishRenderingGeometry(int forceImmediateStop);

void pglSetRenderingFinishedCallback(void (*cb)(void));

/* Read-only, nonblocking diagnostic snapshot, safe in an interrupt handler.
 * pendingSignals: bit 0 normal / bit 1 immediate submitted chains awaiting
 * their GS SIGNAL; the other outputs are each channel's wrapping count of
 * handled SIGNALs. All pointers must be non-NULL. A concurrent completion may
 * be seen on the next snapshot; this is not a synchronization/fence API. */
void pglGetRenderProgress(unsigned int* pendingSignals,
    unsigned int* normalAcknowledged, unsigned int* immediateAcknowledged);

// general

void pglEnable(GLenum cap);
void pglDisable(GLenum cap);

void pglSetInterlacingOffset(float yPixels);

const char* pglGetCurRendererName();

// custom renderers

void pglBeginRendererDefs();
void pglRegisterRenderer(void* renderer);
void pglEndRendererDefs();

// custom prim types

void pglRegisterCustomPrimType(GLenum primType,
    pglU64_t requirements,
    pglU64_t rendererReqMask,
    int mergeContiguous);

/* HyperSolar VU1 near-plane clip renderer (tri lists). Register once after
   pglInit(), then draw tri lists with glDrawArrays(PGL_CLIP_TRIANGLES, ...).
   UNLIT constant-color path (glColor with lighting off); tris with any vert
   at eye depth < the clip near plane are dropped on VU1 regardless of
   PGL_CLIPPING (behind-camera verts corrupt the GS if drawn raw); the side
   planes stay stock guard-band ADC-cull. Set the near plane to match the
   projection with pglSetClipNear (default 1.0, eye units). */
#define PGL_CLIP_TRIANGLES ((GLenum)0x80000000 | 0)
#define PGL_CLIP_TRI_PROP ((pglU64_t)1 << 32)
void pglRegisterClipTriRenderer(void);
void pglSetClipNear(float near_z);

/* HyperSolar VU1 paired city renderer (tri lists): transforms + clips
   each vertex once, then emits TWO prims in one compound kick — the opaque wall
   (PER-VERTEX color from the color array, current bound texture, ABE=0)
   and the additive window overlay (constant color, the window texture,
   ABE=1) on positionally identical verts. Call order per draw:
     pglClipX2SetWindowTexture(winTex, r,g,b,a);   // 0..1 floats
     glBindTexture(GL_TEXTURE_2D, baseTex);        // AFTER: see below
     glDrawArrays(PGL_CLIP_TRIANGLES_X2, ...);
   Changing the pair flushes any prior pending block before rebinding winTex
   (the bind keeps it resident against the GS LRU), so the base texture must
   be bound afterwards. The caller must also flush after the FINAL paired
   draw before a foreign path changes texture/state. winTex = 0 disables
   the window kick (wall-only mode, for bisects). Window alpha scales the
   additive add through GS ALPHA.FIX and remains independent of wall alpha
   (which may carry a vertex-fog coefficient). PSMT8 window textures are
   supported when they carry their OWN clut (the HyperSolar
   per-texture-palette path); sampling is whatever mode the texture
   last drew with (kModulate).
   Near plane shared with pglSetClipNear. */
#define PGL_CLIP_TRIANGLES_X2 ((GLenum)0x80000000 | 1)
#define PGL_CLIP_TRI_X2_PROP ((pglU64_t)1 << 33)
  void pglRegisterClipTriX2Renderer(void);

  /* Unlit textured float-RGBA triangles already clipped by the caller to
     positive W and the GS guard band. No near-plane splitting is performed.
     XYZ float3/4, UV float2 or explicit STQ float3, RGBA float4 in0..1 are
     required; texturing must be enabled (white texture for untextured effects).
     Normals and fixed-function lighting/color-material flags are ignored.
     RGB/alpha use the textured GS128 scale; optional fog uses source alpha.
     Existing GS blend/depth state and PGL guard/backface culling are honored.
     Client arrays must survive asynchronous DMA; flush before foreign state.
     Register once after pglInit. Generic lighted renderers remain unchanged. */
  #define PGL_UNLIT_TEX_TRIANGLES ((GLenum)0x80000000 | 3)
  #define PGL_UNLIT_TEX_TRI_PROP ((pglU64_t)1 << 35)
  void pglRegisterUnlitTexTriRenderer(void);

/* P3 DESCRIPTOR variant of the x2 renderer: walls travel as compact
   parametric descriptors and VU1 reconstructs the vertices, then the same
   dual-context wall+window compound kick runs. Contract per descriptor
   (one wall rectangle):
     GEO   = 3 "vertices" via glVertexPointer(4, GL_FLOAT, 0, geo):
             [ax az bx bz] [y0 y1 uL uR] [vB 0 0 0]  (q2 .yzw reserved:
             the per-face fade id will ride .y at game integration)
     COLOR = 3 byte-vectors via glColorPointer(4, GL_UNSIGNED_BYTE, 0, col):
             [footRGBA] [topRGBA] [pad]              (0..255 each)
     glDrawArrays(PGL_CLIP_TRIANGLES_X2D, firstDesc * 3, numDescs * 3)
   Corners BL(ax,y0,az) BR(bx,y0,bz) TR(bx,y1,bz) TL(ax,y1,az); tris
   (BL,BR,TR)(BL,TR,TL); UV v is top-anchored (0 at y1, vB at y0), u runs
   uL..uR from a to b. Foot color paints BL/BR, top color TR/TL (the wall
   gradient). Window pair via pglClipX2DSetWindowTexture; X2 and X2D keep
   independent pending-block and context-2 state. */
#define PGL_CLIP_TRIANGLES_X2D ((GLenum)0x80000000 | 2)
#define PGL_CLIP_TRI_X2D_PROP ((pglU64_t)1 << 34)
void pglRegisterClipTriX2DRenderer(void);
void pglClipX2DSetWindowTexture(GLuint texId, float r, float g, float b, float a);
void pglClipX2SetWindowTexture(GLuint texId, float r, float g, float b, float a);

/* Exact vertical wall corners, including trapezoids, with arbitrary per-corner
   UV and FLOAT RGBA. No color normalization or geometry arithmetic occurs in
   the decoder. It expands (A,B,C,A,C,D) then enters the unchanged X2 body.
     GEO = 4 elements via glVertexPointer(4, GL_FLOAT, 0, geo):
           [Ax Az Bx Bz] [Ay By Cy Dy] [uA vA uB vB] [uC vC uD vD]
     COLOR = 4 elements via glColorPointer(4, GL_FLOAT, 0, colors):
             [A.rgba] [B.rgba] [C.rgba] [D.rgba]
     glDrawArrays(PGL_CLIP_TRIANGLES_X2Q, firstDesc*4, numDescs*4)
   D shares A's X/Z and C shares B's X/Z exactly. The caller must qualify that
   contract and preserve the repeated A/C attributes before packing. Colors
   have the same semantic owner as X2 (wall alpha may carry fog keep); paired
   windows retain their independent constant color/alpha and exact shared
   geometry. Source arrays remain DMA-live through frame completion. Set the
   window pair before binding the base texture, and flush the final draw just
   like X2/X2D. Both older primitives and their wire formats remain unchanged. */
#define PGL_CLIP_TRIANGLES_X2Q ((GLenum)0x80000000 | 4)
#define PGL_CLIP_TRI_X2Q_PROP ((pglU64_t)1 << 36)
void pglRegisterClipTriX2QRenderer(void);
void pglClipX2QSetWindowTexture(GLuint texId, float r, float g, float b, float a);

/* Compact camera-facing glow quads, independent of the paired-wall state.
   Register once after pglInit; call SetAxes with the source-space U/V axes.
   GEO glVertexPointer(4,GL_FLOAT): [center.xyz,half][u0,v0,u1,v1].
   COL glColorPointer(4,GL_FLOAT): [normalized RGBA][reserved].
   Draw count is two elements per complete glow. Sources remain immutable
   through chain completion. The decoder emits A,B,C,A,C,D and tail-calls
   the unchanged X2 clip/output body; there is no second material kick.
   Caller owns texture/blend/depth and disables fog (alpha remains opacity).
   Floor-boundary intersections and uncertain near/side admission stay on the
   existing caller fallback. This is not a generic GL quad ABI. */
#define PGL_CLIP_GLOW_QUADS_X2G ((GLenum)0x80000000 | 5)
#define PGL_CLIP_GLOW_X2G_PROP ((pglU64_t)1 << 37)
void pglRegisterClipGlowX2GRenderer(void);
void pglClipX2GSetAxes(float ux, float uy, float uz, float vx, float vy, float vz);

// custom state

void pglEnableCustom(pglU64_t flag);
void pglDisableCustom(pglU64_t flag);

#ifdef __cplusplus
}
#endif // __cplusplus

// "capabilities" (things that can be passed to pglEnable/pglDisable

#define PGL_CLIPPING 2
#define PGL_EDGE_AA 3

// for pglFinishRendering

#define PGL_FORCE_IMMEDIATE_STOP 1
#define PGL_DONT_FORCE_IMMEDIATE_STOP 0

// for pglSetDrawBuffers / pglSetDisplayBuffers

#define PGL_NONINTERLACED 0
#define PGL_INTERLACED 1

// custom prim types

#define PGL_DONT_MERGE_CONTIGUOUS 0
#define PGL_MERGE_CONTIGUOUS 1

// various limits

#define PGL_MAX_CUSTOM_RENDERERS 64
#define PGL_MAX_CUSTOM_PRIM_TYPES 32

#endif // ps2gl_h
