/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_h
#define ps2gl_h

#include "GL/gl.h"

/* Optional scoped CPU submission counters; no clocks, waits or per-vertex
 * hooks. The application explicitly samples a small subset of frames. */
#ifndef PGL_SUBMISSION_METRICS
#define PGL_SUBMISSION_METRICS 1
#endif
#if PGL_SUBMISSION_METRICS != 0 && PGL_SUBMISSION_METRICS != 1
#error "Packet optimization/metrics switches must be 0 or 1"
#endif

/* Independent raw-X2 triangle runs retain the 30-vertex input ceiling.
 * Rebuild ps2gl after changing this experimental packet-splitting gate. */
#ifndef PGL_RAW_X2_MULTI_SPAN
#define PGL_RAW_X2_MULTI_SPAN 1
#endif
#if PGL_RAW_X2_MULTI_SPAN != 0 && PGL_RAW_X2_MULTI_SPAN != 1
#error "PGL_RAW_X2_MULTI_SPAN must be 0 or 1"
#endif

/* Explicit fully masked depth draws may suppress GS texture sampling while
 * retaining their original renderer, VU inputs and texture state tracking. */
#ifndef PGL_MASKED_DEPTH_NO_TEXTURE
#define PGL_MASKED_DEPTH_NO_TEXTURE 1
#endif
#if PGL_MASKED_DEPTH_NO_TEXTURE != 0 && PGL_MASKED_DEPTH_NO_TEXTURE != 1
#error "PGL_MASKED_DEPTH_NO_TEXTURE must be 0 or 1"
#endif

/* Five frame-boundary scopes, enabled explicitly by the application. No
 * clocks in the vertex loops; quiet applications never read CP0 Count. */
#ifndef PGL_FRAME_PHASE_METRICS
#define PGL_FRAME_PHASE_METRICS 1
#endif
#if PGL_FRAME_PHASE_METRICS != 0 && PGL_FRAME_PHASE_METRICS != 1
#error "PGL_FRAME_PHASE_METRICS must be 0 or 1"
#endif

/* Presentation-only A/B: use the first 1.0 ms of supported NTSC/PAL/480p
 * blanking instead of the initial 0.5 ms cutoff. Completion/ownership checks
 * remain mandatory in both paths. Rebuild ps2gl after changing this switch. */
#ifndef PGL_PRESENT_EXTENDED_BLANK
#define PGL_PRESENT_EXTENDED_BLANK 1
#endif
#if PGL_PRESENT_EXTENDED_BLANK != 0 && PGL_PRESENT_EXTENDED_BLANK != 1
#error "PGL_PRESENT_EXTENDED_BLANK must be 0 or 1"
#endif

/* Correlated normal-frame diagnostics only; never used for pacing. Runtime
 * collection follows pglSetFramePhaseMetrics. OFF removes the IRQ sampling. */
#ifndef PGL_PRESENT_TIMELINE_METRICS
#define PGL_PRESENT_TIMELINE_METRICS 1
#endif
#if PGL_PRESENT_TIMELINE_METRICS != 0 && PGL_PRESENT_TIMELINE_METRICS != 1
#error "PGL_PRESENT_TIMELINE_METRICS must be 0 or 1"
#endif

/* Three read-only status words at the first serviced, unready frame edge.
 * OFF removes all per-frame storage/sampling; rebuild ps2gl for this gate. */
#ifndef PGL_PRESENT_PIPE_METRICS
#define PGL_PRESENT_PIPE_METRICS 0
#endif
#if PGL_PRESENT_PIPE_METRICS != 0 && PGL_PRESENT_PIPE_METRICS != 1
#error "PGL_PRESENT_PIPE_METRICS must be 0 or 1"
#endif

/* One read-only Count stamp after normal packet Send returns. Independent
 * of pipeline snapshots; OFF removes its per-frame storage and hooks. */
#ifndef PGL_PRESENT_SEND_METRICS
#define PGL_PRESENT_SEND_METRICS 0
#endif
#if PGL_PRESENT_SEND_METRICS != 0 && PGL_PRESENT_SEND_METRICS != 1
#error "PGL_PRESENT_SEND_METRICS must be 0 or 1"
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
 * linked bulk vertex width (three floats). Rejection is untouched.
 * The final current UV is read from the last input vertex. */
extern GLboolean pglTryDrawTexturedQuads2DArrays(const GLfloat* vertices,
    const GLfloat* texcoords, GLsizei count, GLint wordsPerVertex);
/* Same admission/lifetime contract as the rectangle run above, but each
 * quad is four caller-ordered {x,y,u,v} corners (16 floats). No sorting or
 * rectangle reconstruction; z=0,w=1 and the original diagonal are retained.
 * The final current UV is the fourth corner of the last quad. Inadmissible
 * calls return false without changes so callers use immediate mode. */
extern GLboolean pglTryDrawTexturedQuadCorners2D(const GLfloat* quads, GLsizei count);
/* Internal frame/renderer ownership fence for the context-delta proof. */
extern void pglInvalidateUnlitContextDelta(void);
/* Invalidate before any external/raw VU1 microprogram upload or cached replay.
 * This proves ordered code residency only, never VU/GS completion. */
extern void pglInvalidateX2BasePrefix(void);
/* Cumulative unsigned skipped-base upload count and bytes. Subtract snapshots
 * modulo unsigned wrap. Read has no rendering side effects; either output may
 * be NULL. Separate from PGL_SUBMIT_COUNT to preserve its existing array ABI. */
extern void pglGetX2BaseReuseStats(unsigned int* uploads, unsigned int* bytes);
/* Cumulative window context opportunities; unsigned subtraction handles wrap. */
extern void pglGetWindowContextStats(unsigned int* attempts, unsigned int* reused,
    unsigned int* full, unsigned int* globalPins);
/* Draw-environment pointer bookkeeping diagnostics, read without rendering
 * side effects. lastFrame/highWater count records at completed swaps;
 * over100Frames/growths are cumulative unsigned counters (subtract modulo
 * wrap). heapBytes counts live grown pointer banks, excluding embedded100.
 * Outputs may be NULL. Counters restart when a context is constructed. */
extern void pglGetDrawEnvStats(unsigned int* lastFrame, unsigned int* highWater,
    unsigned int* over100Frames, unsigned int* growths, unsigned int* heapBytes);
/* Main-thread frame-boundary elapsed CPU cycles. FINISH waits for the previous
 * normal chain's GS SIGNAL and raster FINISH; VSYNC measures a presentation
 * join (or standalone vblank wait); SEND includes cache
 * writeback, existing DMA-channel readiness wait and DMA start, not completion
 * of the submitted chain. These are not GPU execution times.
 * Count increments every EE CPU cycle (EE Core Manual p70); subtraction wraps
 * modulo32, so each returned scope must finish within one Count period.
 * Each phase has its OWN call count; startup and nonstandard clients can make
 * those counts differ. Set resets the window. Take copies and clears it, with
 * no flush/wait/render side effects. Disabled builds return false and zeros. */
enum {
    PGL_FRAME_END, PGL_FRAME_FINISH, PGL_FRAME_VSYNC,
    PGL_FRAME_SWAP, PGL_FRAME_SEND, PGL_FRAME_PHASE_COUNT
};
typedef struct {
    pglU64_t cycles;
    unsigned int calls;
    unsigned int maxCycles;
} PGLFramePhaseStats;
extern GLboolean pglSetFramePhaseMetrics(GLboolean enabled);
extern GLboolean pglTakeFramePhaseMetrics(PGLFramePhaseStats phases[PGL_FRAME_PHASE_COUNT]);
/* Aggregate IRQ/main presentation evidence, enabled with frame-phase metrics.
 * A notReady edge is phase-admitted with its minimum cadence satisfied, but
 * lacks the owned normal SIGNAL or FINISH. lateEdges are rejected VSINTs.
 * The two join counters classify calls, not necessarily distinct frames.
 * displayFields closes the previous serviced refresh, after same-blank rescue;
 * repeatedFields counts those with no publication. emptyQueueEdges observes
 * a refresh with no pending submission. Cadence >1 intentionally repeats.
 * windows30 counts valid rolling 30-observation windows closed in this report;
 * worstRepeat30 is their largest repeat count (undefined if windows30 is zero).
 * History survives snapshots but not metric/layout resets. These are serviced
 * VSINT observations, not physical refresh counts if interrupts coalesce.
 * blankTicks reports the linked library's cutoff at 576 kHz. extendedBlank
 * counts publications beyond the old 288-tick cutoff; expiredReady counts
 * owned, completed requests whose opened phase permit is invalid/expired. */
typedef struct {
    unsigned int queued, presented, atVsync, afterCompletion;
    unsigned int notReadyEdges, lateEdges, joinReady, joinWait;
    unsigned int displayFields, repeatedFields, emptyQueueEdges;
    unsigned int windows30, worstRepeat30;
    unsigned int blankTicks, extendedBlank, expiredReady;
} PGLPresentationStats;
/* Snapshot and reset under interrupt exclusion; no per-frame logging. */
extern GLboolean pglTakePresentationMetrics(PGLPresentationStats* stats);
/* Whole normal-frame samples, accumulated only on their owned publication.
 * Queue is sampled before Send (including cache writeback/DMA readiness).
 * SIGNAL/FINISH are IRQ observations, not hardware execution timestamps.
 * Presentation time is the admission check immediately before DISPFB stores.
 * Durations use read-only CP0 Count cycles; phase uses Timer1 ticks. A pending
 * record survives Take; metric/layout reset invalidates it. Bootstrap and
 * cadence !=1 have no sample. Samples lasting >=1 second are discarded;
 * unobserved full Count wraps cannot be reconstructed from 32-bit stamps.
 * groups[0] publishes at its first serviced opportunity, groups[1] later.
 * Neither initially not-ready nor same-blank rescue alone counts as a miss. */
typedef struct {
    pglU64_t queueToSignalCycles, queueToFinishCycles, queueToReadyCycles;
    pglU64_t readyToPresentCycles, postPresentToQueueCycles, budgetCycles;
    unsigned int samples, postPresentSamples, budgetSamples;
    unsigned int maxReadyCycles, maxWaitCycles;
} PGLPresentationTimingGroup;
enum {
    PGL_PRESENT_TRACE_HAS_POST = 1, PGL_PRESENT_TRACE_HAS_BUDGET = 2
};
typedef struct {
    unsigned int sequence, queuePhase, readyPhase, edgePhase, flags;
    unsigned int queueToSignalCycles, queueToFinishCycles, queueToReadyCycles;
    unsigned int readyToPresentCycles, postPresentToQueueCycles, budgetCycles;
    unsigned int missedEdges, rejectMask;
} PGLPresentationTimingSample;
typedef struct {
    unsigned int report;
    PGLPresentationTimingGroup groups[2];
    /* Overlapping mode/rewind/phase-cutoff/Count-expiry/FIELD rejection events,
     * counted when rejected, independently of completed-frame cohorts. */
    unsigned int rejectReasons[5];
    /* Missed cohort only: budget row, observed-ready latency column;
     * three bins per axis: <1 ms, 1..<3 ms, >=3 ms. */
    unsigned int missedJoint[9], unknownBudget, discarded;
    unsigned int worstValid;
    PGLPresentationTimingSample worst;
} PGLPresentationTimingStats;
extern GLboolean pglTakePresentationTimingMetrics(PGLPresentationTimingStats* stats);
enum {
    PGL_PRESENT_PIPE_VALID = 1,
    PGL_PRESENT_PIPE_SIGNAL_SEEN = 2,
    PGL_PRESENT_PIPE_FINISH_SEEN = 4
};
typedef struct {
    unsigned int report, sequence, flags;
    unsigned int vifStatus, gifStatus, dmaControl;
} PGLPresentationPipelineSample;
/* Sequential VIF1_STAT/GIF_STAT/D1_CHCR reads, not an atomic hardware edge or
 * exclusive GPU timer. A timing Take seals its worst-missed pipeline sample;
 * consume it with the same report/sequence before the next timing Take.
 * Absent samples return false without writing output. Existing timing ABI
 * and flags stay unchanged when this independent diagnostic is OFF. */
extern unsigned int pglGetPresentationPipelineOptions(void);
extern GLboolean pglTakePresentationPipelineMetrics(unsigned int report,
    unsigned int sequence, PGLPresentationPipelineSample* sample);
enum { PGL_PRESENT_SEND_VALID = 1 };
typedef struct {
    unsigned int report, sequence, flags, queueToSendReturnCycles;
} PGLPresentationSendSample;
/* Queue-to-Send-return includes cache writeback, DMA-channel readiness and
 * submission together, not DMA completion or an exclusive cache timer. It
 * may end after a completion IRQ/publication. One extra Count read per owned
 * normal sample; no wait or interrupt masking. A timing Take seals the same
 * worst-missed request; consume by report/sequence before the next Take.
 * Invalid/absent or >=1-second samples return false without writing output. */
extern unsigned int pglGetPresentationSendOptions(void);
extern GLboolean pglTakePresentationSendMetrics(unsigned int report,
    unsigned int sequence, PGLPresentationSendSample* sample);
/* Optional main-thread allocation observer. Successful growth/free reports
 * old/new total requested heap bytes, excluding allocator overhead. Installing
 * a different non-NULL observer reports existing storage as 0 -> heapBytes;
 * re-installing the same observer does nothing. NULL detaches. The callback
 * must not render, destroy the context, or change this observer. */
extern void pglSetDrawEnvHeapObserver(void (*observer)(unsigned int, unsigned int));
/* Compatibility query: immediate geometry always uses cached memory. */
extern GLboolean pglUsesCachedImmediateGeometry(void);
/* Linked-library feature record; promoted paths report constant ON.
 * Bits4/5 retain historical ON values although their superseded EE selectors
 * are removed; bit7 identifies the active VU0 matrix path.
 * bit0=unchanged draw-state suppression,
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
 * bit26=caller-owned immutable uniform quad arrays,
 * bit27=identical X2-family base microprogram prefix reuse,
 * bit28=bounded raw X2 single-material 30-vertex input batches,
 * bit29=owned compact road sky/view clipping packets,
 * bit30=owned complete base/glow decal pair packets.
 * Query once per report, not per vertex. */
extern unsigned int pglGetContextOptimizationFlags(void);

/* Bit 0 reports the linked masked-depth sampling implementation.
 * Begin returns false without changing state when unsupported. On success,
 * keep draw/texture state fixed until End; modelview/source arrays may change.
 * End drains pending geometry before restoring the ordinary GS giftag. */
extern unsigned int pglGetMaskedDepthTextureOptions(void);
/* Cumulative scope attempts, admitted scopes, and emitted TME-off giftags. */
extern void pglGetMaskedDepthTextureCounts(unsigned int out[3]);
/* Bit 0 reports clear strips; cumulative calls, striped clears, sprites. */
extern unsigned int pglGetClearPageStripOptions(void);
extern void pglGetClearPageStripCounts(unsigned int out[3]);
extern GLboolean pglBeginMaskedDepthNoTexture(void);
extern void pglEndMaskedDepthNoTexture(void);
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
// Join the previous frame's queued vblank publication, then recycle its retired
// front buffer and CPU packet ownership. The following pglRenderGeometry arms
// its own publication before EE preparation starts again. intervals>=1 applies
// to that following submission; the first call bootstraps the display once.
// Reserves EE Timer1 for hardware scan-phase admission until pglFinish(). Do
// not use Timer1 for profiling/alarms while this presentation API is active.
extern void pglSwapBuffersOnVSync(unsigned int intervals);
// Retire a queued display publication before replacing framebuffer storage.
// Idempotent; does not rotate CPU packet/draw ownership or schedule a new flip.
extern void pglWaitForPresentation(void);

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
/* Main-thread diagnostic identity only: the next normal RenderGeometry chain
 * and current context lifetime. Neither reserves a chain nor supplies a fence.
 * Associate caller preparation only if no normal submission intervenes. */
void pglGetNextNormalSubmission(unsigned int* sequence, unsigned int* contextEpoch);

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
/* Jet-only full-depth encoding of the same unlit triangle pipeline. Registered
   by pglRegisterClipTriRenderer; no additional VU image. Requires immediate
   Z24 GL_FILL, zero depth offset, lighting/fog disabled, and finite source vertices clipped exactly
   to the positive eye near plane. The caller must keep projected Z in
   [0, 2^31) (near=1/far=2048 and 0.05<=eye W<=far is the intended envelope).
   XYZ2 preserves depth above the Z24 buffer maximum until GS rasterization;
   XYZF2 would discard its upper bits before the GS. Side clipping, geometry,
   STQ and ADC are unchanged. Do not use as a fog-capable general primitive. */
#define PGL_CLIP_TRIANGLES_XYZ2 ((GLenum)0x80000000 | 13)
/* Query before selecting the optional alias; unsupported state keeps the
   caller's ordinary rendering path. Does not validate vertex/range ownership. */
GLboolean pglCanDrawClipTriXYZ2(void);
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

  /* Admission for ordered textured triangles with finite transforms/material,
     fog/lighting/color-material/culling/clipping/AA OFF and polygon fill.
     Unlike the HUD route, perspective is permitted: caller proves positive W
     and GS-safe coordinates for every triangle using its existing guard.
     Uniform RGBA per triangle must use the original glColor4ub float values.
     The borrowed draw requires a positive multiple of three, repeats cheap
     state checks, and preserves client arrays/current attributes. No partial
     consumption on FALSE. Arrays survive normal frame completion; drain
     construction before changing live texture/state. */
  GLboolean pglCanDrawColoredTriangles(void);
  GLboolean pglUsesColoredTriArrays(void);
  GLboolean pglTryDrawColoredTriangleArrays(const GLfloat* vertices,
      const GLfloat* texcoords, const GLfloat* colors, GLsizei vertexCount);

/* P3 DESCRIPTOR variant of the x2 renderer: walls travel as compact
   parametric descriptors and VU1 reconstructs the vertices, then the same
   dual-context wall+window compound kick runs. Contract per descriptor
   (one wall rectangle):
     GEO   = 3 "vertices" via glVertexPointer(4, GL_FLOAT, 0, geo):
             [ax az bx bz] [y0 y1 uL uR] [vB pad pad pad]
             (q2 .yzw are ignored by the decoder and may carry CPU-only facts)
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
/* bit0: exact noncontinued raw-X2 runs, window section disabled, at most30
   vertices per activation. Retain source arrays through frame completion and
   flush the final pending block before changing any owned draw state. */
unsigned int pglGetRawX2SubmissionOptions(void);

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

/* X2Q geometry with exactly shared bottom/top RGB and edge fog. COLOR uses
   glColorPointer(4,GL_FLOAT,0,colors) but packs TWO qwords per wall:
   [bottomRGB,fogA] [topRGB,fogB]. A=(bottom,fogA), B=(bottom,fogB),
   C=(top,fogB), D=(top,fogA). The decoder only copies fields, then enters
   the same X2 body. GEO/draw counts remain four elements per wall; first
   must be a multiple of four. Both source arrays stay DMA-live. */
#define PGL_CLIP_TRIANGLES_X2C ((GLenum)0x80000000 | 9)
#define PGL_CLIP_TRI_X2C_PROP ((pglU64_t)1 << 41)
void pglRegisterClipTriX2CRenderer(void);
void pglClipX2CSetWindowTexture(GLuint texId, float r, float g, float b, float a);
/* bit0 registered X2C colors, bit1 direct float X2Q/C packet specialization,
   bit2 fixed context-2 texture-prefix preparation, bit3 ordered window reuse,
   bit4 pure CPU prefix/tail reuse, bit5 borrowed wall-descriptor arrays,
   bit6 exact retained X2 VU context deltas. */
unsigned int pglGetWallSubmissionOptions(void);
/* Borrow exact X2Q/C descriptors without changing client-array descriptors or
   current attributes. descriptorCount counts walls (4 GEO qwords each; colors
   are 4 qwords for X2Q and 2 for X2C). Caller owns immutable source storage through
   frame DMA completion, sets the window pair/base texture first and glFlushes
   construction before changing their state. Rejection submits nothing. */
GLboolean pglDrawWallDescriptorArrays(GLenum primitive, const GLfloat* geometry,
    const GLfloat* colors, GLsizei descriptorCount);
void pglGetWallPreparationStats(unsigned int* texturePrefixReused, unsigned int* drawTailReused,
    unsigned int* directAccepted, unsigned int* directRejected);

/* Four authored corners ABCD retain the original ABC/ACD diagonal and each
   corner's XYZ3/UV2/floatRGBA4 values. The caller subtracts eye on EE using
   the source operation order, then uses the original rotation-only X2
   modelview. The lane is unlit and textured; it retains X2 clipping, fog/opacity and
   raster-depth semantics. Count and each source span must be multiples of
   four. Arrays are borrowed and immutable until frame DMA completion. */
#define PGL_CLIP_QUADS_X2F ((GLenum)0x80000000 | 12)
#define PGL_CLIP_QUAD_X2F_PROP ((pglU64_t)1 << 44)
void pglRegisterClipQuadX2FRenderer(void);
/* bit0 = registered four-corner source path, bit1 = single-material A60/B36,
   bit2 = shared corner classification dispatch (culling OFF only),
   bit3 = ten source quads per activation (otherwise eight). */
unsigned int pglGetSourceQuadSubmissionOptions(void);

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

/* Dedicated unlit road representation. These contiguous float fields are the
   56-qword VU context at absolute q1..56; all reserved lanes must be zero.
   Planes, basis and source coordinates retain the caller's arithmetic frame.
   q52 = eye.xyz/incircle squared; q53 = sky center X/Z and source px/py;
   q54 = near/NDC limit/1e-6/0; q55 = normalized RGBA; q56 = source Y/0/0/0. */
typedef struct PGLRoadContext {
    GLfloat eyePlanes[24][4];
    GLfloat sourcePlanes[24][4];
    GLfloat right[4], up[4], forward[4];
    GLfloat eyeIncircle[4];
    GLfloat skyCenterScale[4];
    GLfloat clipParams[4];
    GLfloat color[4];
    GLfloat sourceY[4];
} PGLRoadContext;

/* Exact original corners, not a reconstructed parallelogram. UVs are
   A(0,0), B(1,0), C(1,v1), D(0,v1), triangles ABC then ACD. */
typedef struct PGLRoadQuad {
    GLfloat ab[4]; /* Ax, Az, Bx, Bz */
    GLfloat cd[4]; /* Cx, Cz, Dx, Dz */
    GLfloat v[4];  /* v1, 0, 0, 0 */
} PGLRoadQuad;

/* Pool sky clipping uses the road coordinate/plane context. Unlike roads,
   pools then clip the complete polygon against the source SH view planes
   BEFORE fan triangulation; VU projects that already-clipped fan directly.
   sourceY is common to the whole batch, color in the context is reserved
   (set all four lanes to 0). Per-quad UV and color are explicit float values. */
typedef PGLRoadContext PGLPoolContext;
typedef struct PGLPoolQuad {
    GLfloat xz[2][4]; /* Ax,Az,Bx,Bz; Cx,Cz,Dx,Dz */
    GLfloat uv[4];   /* u0,v0,u1,v1: A00 B10 C11 D01 */
    GLfloat color[4];
} PGLPoolQuad;
#define PGL_CLIP_POOL_QUADS_X2P ((GLenum)0x80000000 | 8)
#define PGL_CLIP_POOL_X2P_PROP ((pglU64_t)1 << 40)
void pglRegisterPoolRenderer(void);
unsigned int pglGetPoolSubmissionOptions(void);
/* Same identity-modelview, finite projection and unlit state admission as
   pglDrawRoadQuads; retains caller texture, blend, depth and alpha-test state.
   PGL_CLIPPING must be OFF: this program performs source-view clipping only.
   No pending geometry allowed. FALSE publishes nothing. TRUE owns all source
   bytes in the normal frame packet. Inputs cannot alias future packet writes,
   including cached/uncached aliases. No retained reference to caller storage.
   Caller supplies finite, haze-free pools on the exact common floor plane. */
GLboolean pglDrawPoolQuads(const PGLPoolContext* context,
    const PGLPoolQuad* quads, GLsizei count);

/* q49..56: same source-eye/view contract as pools, without sky planes. */
typedef struct PGLBillboardContext {
    GLfloat right[4], up[4], forward[4], eye[4];
    GLfloat projection[4]; /* 0,0,px,py */
    GLfloat clip[4];       /* near,NDC,epsilon,0 */
    GLfloat axisU[4], axisV[4];
} PGLBillboardContext;
typedef struct PGLBillboardQuad {
    GLfloat centerHalf[4];
    GLfloat uv[4];
    GLfloat color[4];
} PGLBillboardQuad;
/* Same authored corners/UV/RGB; final source alpha is supplied per corner
   before clipping, preserving EE radial haze and its byte/LUT rounding. */
typedef struct PGLBillboardAlphaQuad {
    GLfloat centerHalf[4];
    GLfloat uv[4];
    GLfloat color[4];
    GLfloat alpha[4]; /* A, B, C, D */
} PGLBillboardAlphaQuad;
#define PGL_CLIP_BILLBOARD_QUADS_X2A ((GLenum)0x80000000 | 11)
#define PGL_CLIP_BILLBOARD_X2A_PROP ((pglU64_t)1 << 43)
#define PGL_CLIP_BILLBOARD_QUADS_X2B ((GLenum)0x80000000 | 10)
#define PGL_CLIP_BILLBOARD_X2B_PROP ((pglU64_t)1 << 42)
void pglRegisterBillboardRenderer(void);
/* bit0 uniform-alpha X2B, bit1 independent per-corner-alpha X2A,
 * bit2 exact shared-corner transform/classification reuse. */
unsigned int pglGetBillboardSubmissionOptions(void);
/* Same transactional source ownership/state contract as pglDrawPoolQuads.
   Caller proves finite authored corners and that neither original triangle
   needs source sky-floor clipping. VU reconstructs ((center +/- U*half)
   +/- V*half), then performs source-view clipping before triangulation.
   Haze/fog and PGL_CLIPPING must be OFF; near/side crossings are supported. */
GLboolean pglDrawBillboardQuads(const PGLBillboardContext* context,
    const PGLBillboardQuad* quads, GLsizei count);
/* Same ownership/admission as above; per-corner final source alpha replaces
   color.w before clipping. Source radial haze stays with the caller. */
GLboolean pglDrawBillboardAlphaQuads(const PGLBillboardContext* context,
    const PGLBillboardAlphaQuad* quads, GLsizei count);

#define PGL_CLIP_ROAD_QUADS_X2R ((GLenum)0x80000000 | 6)
#define PGL_CLIP_ROAD_X2R_PROP ((pglU64_t)1 << 38)
void pglRegisterRoadRenderer(void);
/* Linked choices: bit0 retained context, bit1 compact batch headers,
   bit2 aligned qword payload copies, bit3 fixed UNPACK counts
   (the stream choices also apply to pools and cards). */
unsigned int pglGetRoadSubmissionOptions(void);
/* Bit0: retained source context tail patch. Bit1: fixed source batch counts. */
unsigned int pglGetSourceContextSubmissionOptions(void);
/* Count is complete quads in one material, split internally into <=32.
   Call in the normal frame chain, outside Begin/End/display lists, after
   flushing pending geometry. Requires identity modelview, finite context,
   texture enabled, lighting/fog/culling/edge-AA off, FILL. COLOR_MATERIAL
   and client arrays are ignored and preserved. Texture dimensions are 1..1024;
   supported formats are PSM32/24/16/16s/8/8h, with an owned palette for 8/8h.
   Caller owns exact eligible finite convex corner/UV topology and projection;
   this API does not rescan descriptors or change source geometry/material.
   FALSE consumes no inputs and changes no GL state/packet/cursor, apart from
   materializing the pure vertex-transform cache. TRUE copies ALL source and
   context bytes into frame-owned chain storage before return and leaves no
   pending geometry. Texture/depth/blend state and current attributes remain
   caller-owned. No generic glDrawArrays call uses this private primitive. */
GLboolean pglDrawRoadQuads(const PGLRoadContext* context,
    const PGLRoadQuad* quads, GLsizei count);

/* Entrance decal context occupies absolute VU q1..8, in this exact order.
   The caller preflights finite source topology, clipping and biased-depth
   headroom before publication. Matrices retain the accepted EE product order. */
typedef struct PGLDecalContext {
    GLfloat transform[4][4]; /* column-major GS-scaled projection * view */
    GLfloat raster[4];      /* raster X, Y, Z, source near */
    GLfloat inverse[4];     /* 1/rasterX, 1/rasterY, 2/maxDepth, maxDepth */
    GLfloat depth[4];       /* reciprocal-depth coefficient, base/glow bias, pass */
    GLfloat clip[4];        /* source clip guard X/Y, 0, 0 */
} PGLDecalContext;

/* Exact vertical rectangle ABC/ACD; no midpoint reconstruction. Both colors
   and glow alpha are constant along each vertical edge. Z/W and per-source-
   triangle maxima are supplied by the admission computation, not estimated.
   The descriptor is nine qwords (144 bytes), copied inline for each pass. */
typedef struct PGLDecalQuad {
    GLfloat geometry[3][4]; /* Ax/Az/Bx/Bz; ylo/yhi/u0/u1; v0/v1/glowA/glowB */
    GLfloat colors[2][4];   /* normalized RGBA at A/D, B/C; A is base fog keep */
    GLfloat zw[4][2];       /* exact homogeneous clip Z/W at A, B, C, D */
    GLfloat depth[2][4];    /* ABC/ACD allowance, sumAbsZ, sumAbsW, skip(0/1) */
} PGLDecalQuad;

/* Already clipped/projected exceptional triangles, in original fan order.
   Position contains the exact earlier EE base and glow NDC depths; texture
   contains premultiplied STQ and the glow alpha. No reconstruction is used. */
typedef struct PGLDecalNdcVertex {
    GLfloat position[4]; /* xNDC, yNDC, baseZNDC, glowZNDC */
    GLfloat texture[4];  /* S, T, Q, glowAlpha */
    GLfloat color[4];    /* base normalized RGBA; A also carries base fog */
} PGLDecalNdcVertex;

typedef struct PGLDecalNdcTriangle {
    PGLDecalNdcVertex vertex[3];
} PGLDecalNdcTriangle;

#define PGL_DECAL_RUN_QUADS 0u
#define PGL_DECAL_RUN_NDC_TRIANGLES 1u
typedef struct PGLDecalRun {
    const void* records;
    GLsizei count; /* quads or triangles, according to format */
    GLuint format;
} PGLDecalRun;

typedef struct PGLDecalRegionV {
    GLint min_v;
    GLint max_v;
} PGLDecalRegionV;

#define PGL_CLIP_DECAL_QUADS_X2E ((GLenum)0x80000000 | 7)
#define PGL_CLIP_DECAL_X2E_PROP ((pglU64_t)1 << 39)
void pglRegisterDecalRenderer(void);
/* Linked effective options: bit 0 = owned base/glow payload reuse,
   bit 1 = compact header/CNT, bit 2 = aligned qword payload copies,
   bit 3 = exact VU corner XY product reuse,
   bit 4 = ordered compact/projected runs in one decal program,
   bit 5 = exact four-corner all-inside classification,
   bit 6 = reuse the proven plane prefix after a failed quad certificate,
   bit 7 = fixed UNPACK counts,
   bit 8 = per-record mip material CLAMP in the X2E output stream. */
unsigned int pglGetDecalSubmissionOptions(void);
/* Submit the whole base sweep followed by the optional whole glow sweep.
   Call after draining pending geometry, in the normal frame chain, outside
   Begin/End/display lists. Requires identity projection/modelview, texture ON,
   lighting/culling/edge-AA/clipping/alpha-test/blending OFF, FILL, depth LEQUAL
   with writes OFF, depth offset zero and identity clip-near one. Base fog may
   be enabled. COLOR_MATERIAL, client-array descriptions and current attributes
   are ignored and preserved. Both named textures must already exist, have
   dimensions 1..1024 and use PSM32/24/16/16s/8/8h; 8/8h need an owned palette.
   The caller owns finite descriptors, source near/headroom admission and the
   exact rectangle/attribute contract above; this API does not rescan quads.
   FALSE publishes nothing and changes no GL state or cursor, apart from
   materializing the pure vertex-transform cache. Inputs may not overlap the
   reserved future frame-packet write range, including its uncached aliases.
   TRUE owns all descriptor/context bytes before return; the glow pass may
   reference the base pass's owned copy. It leaves no pending geometry;
   the last material remains bound. With glow, fog is OFF and SRC_ALPHA/ONE
   blending ON; without glow, caller's base fog/blend state remains. The caller
   owns the normal pass teardown. No fallback is allowed after a TRUE result. */
GLboolean pglDrawDecalQuads(const PGLDecalContext* context,
    const PGLDecalQuad* quads, GLsizei count, GLuint baseTexture, GLuint glowTexture);
/* Same atomic ownership/state contract as pglDrawDecalQuads, with at most512
   ordered runs. Every projected triangle must already be clipped, finite and
   depth-admitted by the caller, with exact base/glow positions and STQ. The
   API validates ALL run ranges and reserves BOTH materials before mutation.
   Each material replays the complete run order; compact/projected boundaries
   change only the activation header, never the renderer or its GS state.
   FALSE admits no part of the stream; the caller may then use its full
   fallback. Projected records require linked decal option bit4. */
GLboolean pglDrawDecalRuns(const PGLDecalContext* context,
    const PGLDecalRun* runs, GLsizei runCount, GLuint baseTexture, GLuint glowTexture);
/* The same atomic pair with one inclusive base-level V region per run.
   Regions apply only to admitted 64x128 PSM32 packed mip textures; U repeats.
   At least one material must have that pack. All regions/ranges and the
   additional ordered texture settings are admitted before either sweep.
   The GS shifts V bounds for each mip level. Source UVs remain unchanged. */
GLboolean pglDrawDecalRunsRegionV(const PGLDecalContext* context,
    const PGLDecalRun* runs, const PGLDecalRegionV* regions, GLsizei runCount,
    GLuint baseTexture, GLuint glowTexture);
/* Same atomic pair, with one byte per record selecting atlas cell 0..3.
   materials[i] addresses runs[i].count bytes. Each cell covers32 base-level
   rows of the admitted64x128 PSM32 pack. VU1 writes ordered CLAMP_1 settings
   alongside geometry; different cells can share one16-record activation.
   Non-mipped passes ignore cells. All metadata is validated and copied before
   return. No descriptor, geometry, UV or texture-layout change is involved.
   Requires linked decal option bit8; FALSE publishes nothing. */
GLboolean pglDrawDecalRunsMaterials(const PGLDecalContext* context,
    const PGLDecalRun* runs, const unsigned char* const* materials,
    GLsizei runCount, GLuint baseTexture, GLuint glowTexture);
int pgl_texture_has_mips32(unsigned int name);
// Live packed entrance atlas: the RGBA32 pyramid or its exact PSMT8 twin.
int pgl_texture_has_packed_atlas(unsigned int name);
int pgl_texture_region_clamp_v(unsigned int name, int min_v, int max_v);

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
