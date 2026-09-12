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

/* Immediate attribute buffers only; keep the existing full-cache writeback
 * in source-chain Send(), double buffering and append-only frame lifetime.
 * Independent of PGL_CACHED_FRAME_PACKETS. Rebuild ps2gl EE after changing. */
#ifndef PGL_CACHED_IMMEDIATE_GEOMETRY
#define PGL_CACHED_IMMEDIATE_GEOMETRY 1
#endif
#if PGL_CACHED_IMMEDIATE_GEOMETRY != 0 && PGL_CACHED_IMMEDIATE_GEOMETRY != 1
#error "PGL_CACHED_IMMEDIATE_GEOMETRY must be 0 or 1"
#endif

/* Suppress unchanged immediate depth/write-mask and lighting setters.
 * Compare the authoritative draw environment, including depth-disable side
 * effects; never discard a recorded display-list state command. */
#ifndef PGL_SKIP_REDUNDANT_DRAW_STATE
#define PGL_SKIP_REDUNDANT_DRAW_STATE 1
#endif
#if PGL_SKIP_REDUNDANT_DRAW_STATE != 0 && PGL_SKIP_REDUNDANT_DRAW_STATE != 1
#error "PGL_SKIP_REDUNDANT_DRAW_STATE must be 0 or 1"
#endif

/* Keep immediate vertex colors, but avoid rebuilding an unchanged constant
 * color/material context. Display-list recording and custom renderers retain
 * their original invalidation contract. Rebuild ps2gl EE after changing. */
#ifndef PGL_SKIP_REDUNDANT_COLOR
#define PGL_SKIP_REDUNDANT_COLOR 1
#endif
#if PGL_SKIP_REDUNDANT_COLOR != 0 && PGL_SKIP_REDUNDANT_COLOR != 1
#error "PGL_SKIP_REDUNDANT_COLOR must be 0 or 1"
#endif

/* Compare authoritative quantized GS fields before re-sending an unchanged
 * blend/alpha-test setup. Preserve explicit reassertion after custom kicks. */
#ifndef PGL_SKIP_REDUNDANT_BLEND_ALPHA
#define PGL_SKIP_REDUNDANT_BLEND_ALPHA 1
#endif
#if PGL_SKIP_REDUNDANT_BLEND_ALPHA != 0 && PGL_SKIP_REDUNDANT_BLEND_ALPHA != 1
#error "PGL_SKIP_REDUNDANT_BLEND_ALPHA must be 0 or 1"
#endif

/* Reuse an identical managed texture synchronization at its actual draw
 * boundary. Uploads, CLUT/global state writes and custom contexts invalidate
 * the proof. Bind commands and required TEXFLUSH operations are retained.
 * Initial integration needs ps2stuff and all ps2gl EE objects. Later gate
 * changes need all ps2gl EE objects; ps2stuff's write serial remains active. */
#ifndef PGL_SKIP_REDUNDANT_TEXTURE_SYNC
#define PGL_SKIP_REDUNDANT_TEXTURE_SYNC 1
#endif
#if PGL_SKIP_REDUNDANT_TEXTURE_SYNC != 0 && PGL_SKIP_REDUNDANT_TEXTURE_SYNC != 1
#error "PGL_SKIP_REDUNDANT_TEXTURE_SYNC must be 0 or 1"
#endif

/* Stock linear renderers with lighting OFF: upload their live context spans
 * without preparing eight unused lights and lighting-only matrices. Custom
 * and indexed renderers retain their existing context contract. EE-only A/B. */
#ifndef PGL_SPARSE_UNLIT_CONTEXT
#define PGL_SPARSE_UNLIT_CONTEXT 1
#endif
#if PGL_SPARSE_UNLIT_CONTEXT != 0 && PGL_SPARSE_UNLIT_CONTEXT != 1
#error "PGL_SPARSE_UNLIT_CONTEXT must be 0 or 1"
#endif

/* Explicit opt-in for the proven July PGL_CLIP_TRIANGLES VU program only,
 * with GL lighting OFF. Other custom programs keep their own contracts. */
#ifndef PGL_SPARSE_CLIP_CONTEXT
#define PGL_SPARSE_CLIP_CONTEXT 1
#endif
#if PGL_SPARSE_CLIP_CONTEXT != 0 && PGL_SPARSE_CLIP_CONTEXT != 1
#error "PGL_SPARSE_CLIP_CONTEXT must be 0 or 1"
#endif

/* Reuse a verified stock-unlit context; upload only changed material/xform
 * spans. Renderer/frame transitions invalidate the proof. EE-only A/B. */
#ifndef PGL_UNLIT_CONTEXT_DELTA
#define PGL_UNLIT_CONTEXT_DELTA 1
#endif
#if PGL_UNLIT_CONTEXT_DELTA != 0 && PGL_UNLIT_CONTEXT_DELTA != 1
#error "PGL_UNLIT_CONTEXT_DELTA must be 0 or 1"
#endif

/* Same stock lit renderer: update only q58..61 for material-only changes.
 * Lighting/texture ownership, transforms, lights, GS and renderer changes
 * retain a full context upload and the original microprogram restart. */
#ifndef PGL_LIT_MATERIAL_DELTA
#define PGL_LIT_MATERIAL_DELTA 1
#endif
#if PGL_LIT_MATERIAL_DELTA != 0 && PGL_LIT_MATERIAL_DELTA != 1
#error "PGL_LIT_MATERIAL_DELTA must be 0 or 1"
#endif

/* Keep the established unlit delta writer specialized and reject invalid
 * reuse before touching lighting state. Same context words and restarts. */
#ifndef PGL_UNLIT_DELTA_SPECIALIZE
#define PGL_UNLIT_DELTA_SPECIALIZE 1
#endif
#if PGL_UNLIT_DELTA_SPECIALIZE != 0 && PGL_UNLIT_DELTA_SPECIALIZE != 1
#error "PGL_UNLIT_DELTA_SPECIALIZE must be 0 or 1"
#endif

/* Skip arbitrary-strip bookkeeping for one stock, uniform textured quad
 * array. Same VU chunk boundaries, REF transfers and microprogram. */
#ifndef PGL_FLAT_QUAD_PACKETS
#define PGL_FLAT_QUAD_PACKETS 1
#endif
/* Independent primitives have an immutable no-restart ADC header. */
#ifndef PGL_INDEPENDENT_PRIM_HEADER
#define PGL_INDEPENDENT_PRIM_HEADER 1
#endif
/* Recompute VIF unpack masks only when that renderer's input width changes.
 * Existing format fields own the cache; row/fallback values stay live. */
#ifndef PGL_TRANSFER_FORMAT_CACHE
#define PGL_TRANSFER_FORMAT_CACHE 1
#endif
/* Pure GS texture/draw-environment changes need not replace a compatible
 * stock unlit VU context. GS synchronization itself remains mandatory. */
#ifndef PGL_UNLIT_GS_CONTEXT_DELTA
#define PGL_UNLIT_GS_CONTEXT_DELTA 1
#endif
/* Borrow caller-owned, immutable uniform quad arrays through packet finish.
 * Eligibility failures leave state unchanged and retain the copying API. */
#ifndef PGL_BORROWED_QUAD_ARRAYS
#define PGL_BORROWED_QUAD_ARRAYS 1
#endif
/* Optional scoped CPU submission counters; no clocks, waits or per-vertex
 * hooks. The application explicitly samples a small subset of frames. */
#ifndef PGL_SUBMISSION_METRICS
#define PGL_SUBMISSION_METRICS 1
#endif
#if (PGL_FLAT_QUAD_PACKETS != 0 && PGL_FLAT_QUAD_PACKETS != 1) || \
    (PGL_INDEPENDENT_PRIM_HEADER != 0 && PGL_INDEPENDENT_PRIM_HEADER != 1) || \
    (PGL_TRANSFER_FORMAT_CACHE != 0 && PGL_TRANSFER_FORMAT_CACHE != 1) || \
    (PGL_UNLIT_GS_CONTEXT_DELTA != 0 && PGL_UNLIT_GS_CONTEXT_DELTA != 1) || \
    (PGL_BORROWED_QUAD_ARRAYS != 0 && PGL_BORROWED_QUAD_ARRAYS != 1) || \
    (PGL_SUBMISSION_METRICS != 0 && PGL_SUBMISSION_METRICS != 1)
#error "Packet optimization/metrics switches must be 0 or 1"
#endif

/* Extend the proven context-delta spans to the original GeneralClipTri
 * renderer only. Other custom VU programs retain their existing contracts. */
#ifndef PGL_CLIP_CONTEXT_DELTA
#define PGL_CLIP_CONTEXT_DELTA 1
#endif
#if PGL_CLIP_CONTEXT_DELTA != 0 && PGL_CLIP_CONTEXT_DELTA != 1
#error "PGL_CLIP_CONTEXT_DELTA must be 0 or 1"
#endif

/* Cache scalar depth/clip/fog coefficients at the owning state boundaries.
 * Original arithmetic and uploaded context words remain unchanged. */
#ifndef PGL_CONTEXT_COEFFICIENT_CACHE
#define PGL_CONTEXT_COEFFICIENT_CACHE 1
#endif
#if PGL_CONTEXT_COEFFICIENT_CACHE != 0 && PGL_CONTEXT_COEFFICIENT_CACHE != 1
#error "PGL_CONTEXT_COEFFICIENT_CACHE must be 0 or 1"
#endif

/* Exact byte-to-float color mapping, replacing four software double divides.
 * The public float values and color/material/display-list behavior stay the
 * same. Rebuild ps2gl EE after changing either gate below. */
#ifndef PGL_COLOR4UB_LUT
#define PGL_COLOR4UB_LUT 1
#endif
#if PGL_COLOR4UB_LUT != 0 && PGL_COLOR4UB_LUT != 1
#error "PGL_COLOR4UB_LUT must be 0 or 1"
#endif

/* Caller-ordered 2D quad corners copied into existing immediate buffers.
 * Keeps the stock GL_QUADS renderer, diagonal and final current UV. */
#ifndef PGL_BULK_QUAD_CORNERS
#define PGL_BULK_QUAD_CORNERS 1
#endif
#if PGL_BULK_QUAD_CORNERS != 0 && PGL_BULK_QUAD_CORNERS != 1
#error "PGL_BULK_QUAD_CORNERS must be 0 or 1"
#endif

/* Omit unused source W in stable stock unlit quad runs. The existing VIF
 * XYZ unpack and quad shader still produce the same homogeneous position.
 * Unknown/custom/changing renderer ownership keeps the XYZW source path. */
#ifndef PGL_BULK_QUAD_XYZ3
#define PGL_BULK_QUAD_XYZ3 1
#endif
#if PGL_BULK_QUAD_XYZ3 != 0 && PGL_BULK_QUAD_XYZ3 != 1
#error "PGL_BULK_QUAD_XYZ3 must be 0 or 1"
#endif

/* Three glVertex3f submissions with identical current attributes, appended
 * transactionally to the existing immediate GL_TRIANGLES stream. */
#ifndef PGL_FUSED_TRIANGLE3D
#define PGL_FUSED_TRIANGLE3D 1
#endif
#if PGL_FUSED_TRIANGLE3D != 0 && PGL_FUSED_TRIANGLE3D != 1
#error "PGL_FUSED_TRIANGLE3D must be 0 or 1"
#endif

/* Stock lit programs upload light records through the highest enabled slot,
 * then resume at global ambient. Custom programs retain complete records. */
#ifndef PGL_SPARSE_LIGHT_CONTEXT
#define PGL_SPARSE_LIGHT_CONTEXT 1
#endif
#if PGL_SPARSE_LIGHT_CONTEXT != 0 && PGL_SPARSE_LIGHT_CONTEXT != 1
#error "PGL_SPARSE_LIGHT_CONTEXT must be 0 or 1"
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

/* Defer immediate inverse concatenation until GetInvTop consumes it. Store
 * the original ordered operands, never invert the composed forward matrix.
 * A fixed journal falls back eagerly when full; display-list recording stays
 * eager. Independent of lazy glLoadMatrixf, no VU changes. Rebuild every EE
 * consumer of internal ps2gl headers after changing this class-layout gate. */
#ifndef PGL_DEFER_MATRIX_CONCAT_INVERSE
#define PGL_DEFER_MATRIX_CONCAT_INVERSE 1
#endif
#if PGL_DEFER_MATRIX_CONCAT_INVERSE != 0 && PGL_DEFER_MATRIX_CONCAT_INVERSE != 1
#error "PGL_DEFER_MATRIX_CONCAT_INVERSE must be 0 or 1"
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
/* Same four glTexCoord2f/glVertex2f pairs, BL/TL/TR/BR, z=0 and w=1.
 * Current normal/color and all draw state are retained; current UV ends at
 * (u1,v1). Returns false WITHOUT changes outside immediate GL_QUADS, during
 * display-list recording, or if the complete append cannot fit. */
extern GLboolean pglTryTexturedQuad2D(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1,
    GLfloat u0, GLfloat v0, GLfloat u1, GLfloat v1);
/* Three ordered XYZ positions (nine floats) inside immediate GL_TRIANGLES.
 * Copies the current normal/UV for every vertex and supplies W=1, exactly
 * like three glVertex3f calls; current attributes/color stream are untouched.
 * No draw or flush is added. Returns false without changes outside this
 * primitive, during display-list recording, or when any complete span fails
 * capacity preflight. The caller retains its original three-call fallback. */
extern GLboolean pglTryTriangle3D(const GLfloat* xyz);
/* Complete uniform-color quad run, OUTSIDE glBegin/glEnd. Eight floats per
 * quad: x0,y0,x1,y1,u0,v0,u1,v1. Copies into the existing frame-owned immediate
 * buffers; uses the same GL_QUADS renderer and BL/TL/TR/BR order. Requires
 * texture enabled, lighting/color-material disabled. No client-array mutation.
 * Rejection leaves all state/cursors unchanged; success leaves current UV at
 * the last quad's (u1,v1), with current normal/color untouched. */
extern GLboolean pglTryDrawTexturedQuads2D(const GLfloat* quads, GLsizei count);
/* Same uniform quad draw, borrowing ordered XYZ/XYZW and UV arrays instead
 * of expanding/copying descriptors. Count is QUADS. Arrays must be4-byte
 * aligned and remain immutable through DMA completion, with caller cache
 * writeback before submission of the frame. No client-array state mutation.
 * Requires the already-selected stable stock unlit quad renderer and its
 * linked bulk vertex width (XYZ3 gate ON:3, OFF:4). Rejection is untouched.
 * The final current UV is read from the last input vertex. */
extern GLboolean pglTryDrawTexturedQuads2DArrays(const GLfloat* vertices,
    const GLfloat* texcoords, GLsizei count, GLint wordsPerVertex);
/* Same admission/lifetime contract as the rectangle run above, but each
 * quad is four caller-ordered {x,y,u,v} corners (16 floats). No sorting or
 * rectangle reconstruction; z=0,w=1 and the original diagonal are retained.
 * The final current UV is the fourth corner of the last quad. Gate OFF
 * returns false without modifying anything so callers use immediate mode. */
extern GLboolean pglTryDrawTexturedQuadCorners2D(const GLfloat* quads, GLsizei count);
/* Internal frame/renderer ownership fence for the context-delta proof. */
extern void pglInvalidateUnlitContextDelta(void);
/* Actual linked library setting, not the caller's header default. */
extern GLboolean pglUsesCachedImmediateGeometry(void);
/* Linked-library context A/B settings: bit0=unchanged draw-state suppression,
 * bit1=stock unlit sparse context, bit2=unlit PGL_CLIP_TRIANGLES context,
 * bit3=deferred inverse concat, bit4=scalar matrix kernel, bit5=EE matrix
 * kernel (takes precedence over bit4), bit6=lazy glLoadMatrixf inverse,
 * bit7=VU0 matrix kernel (takes precedence over bits4/5),
 * bit8=unchanged color/material suppression, bit9=unchanged blend/alpha test,
 * bit10=identical resident managed texture synchronization reuse,
 * bit11=stock unlit context delta, bit12=exact byte-color lookup,
 * bit13=caller-ordered immediate quad-corner runs,
 * bit14=original GeneralClipTri context delta,
 * bit15=state-owned scalar context coefficients,
 * bit16=XYZ source packing for stable stock unlit quad runs,
 * bit17=fused immediate triangle position/current-attribute appends,
 * bit18=stock lit context prefix ending at the highest enabled light slot,
 * bit19=stock lit material-only context update,
 * bit20=separate unlit/lit delta writers and early common rejection,
 * bit21=single-array uniform textured quad packet builder,
 * bit22=immutable independent-primitive ADC header,
 * bit23=scoped CPU submission metrics available,
 * bit24=renderer-owned VIF transfer-format reuse,
 * bit25=stock unlit context reuse across compatible GS state changes,
 * bit26=caller-owned immutable uniform quad arrays.
 * Query once per report, not per vertex. */
extern unsigned int pglGetContextOptimizationFlags(void);
/* Cumulative unsigned counters within one explicit, non-nestable sample.
 * Read does not flush or mutate rendering. Packet bytes refer to the normal
 * VIF chain, excluding REF payload; context bytes are a subset of that chain.
 * Cached dlist replay is not counted as fresh packet construction. Keep a
 * sample within one frame/packet; Read returns false if its owner changed. */
enum {
    PGL_SUBMIT_XYZ3, PGL_SUBMIT_XYZ4, PGL_SUBMIT_BLOCKS,
    PGL_SUBMIT_FLAT_BLOCKS, PGL_SUBMIT_BUFFERS,
    PGL_SUBMIT_FULL_CONTEXTS, PGL_SUBMIT_DELTA_CONTEXTS,
    PGL_SUBMIT_REF_BYTES, PGL_SUBMIT_EDGE_BYTES,
    PGL_SUBMIT_TEXTURE_SYNCS, PGL_SUBMIT_TEXTURE_REUSES,
    PGL_SUBMIT_TEXTURE_UPLOADS, PGL_SUBMIT_CLUT_UPLOADS,
    PGL_SUBMIT_PROGRAM_LOADS,
    PGL_SUBMIT_CONTEXT_BYTES, PGL_SUBMIT_PACKET_BYTES, PGL_SUBMIT_COUNT
};
extern GLboolean pglBeginSubmissionSample(void);
extern GLboolean pglReadSubmissionSample(unsigned int values[PGL_SUBMIT_COUNT]);
extern void pglEndSubmissionSample(void);
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
  /* Admission for replacing ordinary uniform-color z=0 HUD quads with the
     existing colored triangle primitive. Call outside Begin/End and display
     list recording, after binding/enabling the intended texture and 2D state.
     Client-array descriptors/enables are irrelevant to this direct-array API.
     Requires the registered builtin renderer,
     no other custom state, lighting/color-material/fog/cull/clipping/edge-AA
     off, polygon FILL, and finite affine modelview/projection/raster transform
     with exact homogeneous W=1. No packet/GL state changes or drain; the pure
     vertex-transform cache may be materialized. The caller still owns finite,
     GS-safe input coordinates, 0..1 RGBA (identical at all vertices of each
     quad), original quad triangles 0,1,3 / 1,3,2, and array DMA lifetime.
     A game-side gate must also be honored by the caller. Recheck after any
     relevant state/matrix change; do not retain the result across HUD passes. */
  GLboolean pglCanDrawColoredHud2D(void);
  /* Submit an admitted HUD span as borrowed XYZ3/UV2/RGBA4 float arrays, each
     at least 4-byte aligned. vertexCount is positive and divisible by six.
     Requires a successful pglCanDrawColoredHud2D in this pass and unchanged
     matrices/material/2D admission state; texture and depth-write changes are
     allowed between drained spans. The cheap state guards are repeated, but
     matrices/vertex contents are not rescanned. FALSE consumes nothing, so the
     caller may use its existing fallback. TRUE keeps inputs borrowed through
     normal frame/DMA completion: preserve them and normal cache writeback.
     Client descriptors/enables and current color/normal/UV stay unchanged.
     Flush pending construction before foreign state or texture teardown. */
  GLboolean pglTryDrawColoredHud2DArrays(const GLfloat* vertices,
      const GLfloat* texcoords, const GLfloat* colors, GLsizei vertexCount);

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
