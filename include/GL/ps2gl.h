/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_h
#define ps2gl_h

#include "GL/gl.h"

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
