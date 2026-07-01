/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include "GL/ps2gl.h"

#include "ps2gl/displaycontext.h"
#include "ps2gl/dlist.h"

#include "ps2s/displayenv.h"

CDisplayContext::CDisplayContext(CGLContext& context)
    : GLContext(context)
    , Frame0Mem(NULL)
    , Frame1Mem(NULL)
    , CurFrameMem(NULL)
    , LastFrameMem(NULL)
    , DisplayEnv(NULL)
    , DisplayIsDblBuffered(true)
    , DisplayIsInterlaced(true)
    , FlickerEnabled(false)
    , FlickerAlpha(0)
{
    DisplayEnv = new GS::CDisplayEnv;
}

// Flicker filter: point read circuit 1 at the same frame buffer as RC2 but one
// line lower (DBY=1), matching DISPLAY1 geometry to DISPLAY2, and const-alpha
// blend the two -- a 2-tap vertical low-pass on scanout. FlickerAlpha is RC1's
// (neighbor-line) weight. When disabled, RC1 is turned off and the blend is
// restored to the constructor default (RC1 alpha, RC1 off => no effect). RC1's
// frame-buffer ADDRESS is repointed per swap in SwapBuffers; here we set the
// initial address (Frame0Mem, matching the SetFB2 calls) plus DBY/geometry/PMODE.
void CDisplayContext::ApplyFlicker(bool interlaced, int width, int height, int screenX, int screenY)
{
    if (FlickerEnabled) {
        DisplayEnv->SetFB1(Frame0Mem->GetWordAddr(), width, 0, 1, Frame0Mem->GetPixFormat());
        if (interlaced)
            DisplayEnv->SetDisplay1(width, height * 2, screenX, screenY, 4, 1);
        else
            DisplayEnv->SetDisplay1(width, height, screenX, screenY, 2, 1);
        DisplayEnv->SetUseReadCircuit1(true);
        DisplayEnv->BlendRC1WithRC2();
        DisplayEnv->BlendUsingConstAlpha(FlickerAlpha);
    } else {
        DisplayEnv->SetUseReadCircuit1(false);
        DisplayEnv->BlendUsingRC1Alpha();
    }
}

void CDisplayContext::SetFlickerFilter(bool enable, int alpha)
{
    FlickerEnabled = enable;
    FlickerAlpha   = (unsigned char)alpha;
}

CDisplayContext::~CDisplayContext()
{
    // don't delete the frame mem areas -- they are created/destroyed by
    // the app

    delete DisplayEnv;
}

void CDisplayContext::SetDisplayBuffers(bool interlaced,
    GS::CMemArea* frame0Mem, GS::CMemArea* frame1Mem)
{
    Frame0Mem = frame0Mem;
    Frame1Mem = frame1Mem;

    DisplayIsDblBuffered = (frame0Mem && frame1Mem);
    DisplayIsInterlaced  = interlaced;

    // "current" means the frame being drawn to by the loop of code executing on the core
    // "last" will be the frame being displayed if drawing immediately, or the frame not
    // displayed now if building up a packet to be sent next frame.
    CurFrameMem  = Frame0Mem;
    LastFrameMem = Frame1Mem;

    int width = frame0Mem->GetWidth(), height = frame0Mem->GetHeight();
    int displayHeight = (DisplayIsInterlaced) ? height * 2 : height;

    DisplayEnv->SetFB2(frame0Mem->GetWordAddr(), width, 0, 0, frame0Mem->GetPixFormat());
    DisplayEnv->SetDisplay2(width, displayHeight);
    ApplyFlicker(DisplayIsInterlaced, width, height, 0, 0);
    DisplayEnv->SendSettings();
}

void CDisplayContext::SetVideoMode(bool interlaced, int overscanMode, int screenX, int screenY)
{
    if (!Frame0Mem)
        return;

    DisplayIsInterlaced = interlaced;

    // overscanMode 0/1/2 maps 1:1 onto GS::DisplayModes ntsc/pal/dtv, which sets
    // the per-mode DX/DY overscan base used below.
    DisplayEnv->SetDisplayMode((GS::tDisplayMode)overscanMode);

    int width  = Frame0Mem->GetWidth();
    int height = Frame0Mem->GetHeight();
    DisplayEnv->SetFB2(Frame0Mem->GetWordAddr(), width, 0, 0, Frame0Mem->GetPixFormat());

    if (interlaced)
        // Interlaced (NTSC/PAL): a half-height field buffer fills the visible
        // area via interlace (display height = buffer height x2, magV x1).
        // magH x4 = the NTSC/PAL dot clock (DW = width*4).
        DisplayEnv->SetDisplay2(width, height * 2, screenX, screenY, 4, 1);
    else
        // Progressive (480p): show the FULL-height buffer 1:1 (magV x1, was x2 to
        // line-double a field buffer for fake 480p). magH x2, NOT x4: the DTV
        // 480p scan clock is ~2x NTSC, so magH x4 makes the image exactly TWICE
        // too wide on real hardware (PCSX2 normalizes it, so it only shows on a
        // GS). DW = width*2, MAGH = 1. See PS2/VRAM_480P_PLAN.md.
        DisplayEnv->SetDisplay2(width, height, screenX, screenY, 2, 1);

    ApplyFlicker(interlaced, width, height, screenX, screenY);
    DisplayEnv->SendSettings();
}

void CDisplayContext::SetDisplayOffset(int screenX, int screenY)
{
    if (!Frame0Mem)
        return;

    // Recompute the DISPLAY register for the CURRENT mode (interlace + the
    // overscan/magnification SetVideoMode already programmed) with the new
    // raster offset, then push ONLY that register -- no PMODE/DISPFB/BGCOLOR
    // rewrite, so the overscan border doesn't flash while re-centering.
    int width  = Frame0Mem->GetWidth();
    int height = Frame0Mem->GetHeight();
    if (DisplayIsInterlaced)
        DisplayEnv->SetDisplay2(width, height * 2, screenX, screenY, 4, 1);
    else
        DisplayEnv->SetDisplay2(width, height, screenX, screenY, 2, 1);

    // With the flicker filter on, RC1 must re-center alongside RC2 or the two
    // circuits pan apart into a misaligned double image. Mirror DISPLAY1 and
    // push it too (still no PMODE/DISPFB rewrite, so no border flash).
    if (FlickerEnabled) {
        if (DisplayIsInterlaced)
            DisplayEnv->SetDisplay1(width, height * 2, screenX, screenY, 4, 1);
        else
            DisplayEnv->SetDisplay1(width, height, screenX, screenY, 2, 1);
        DisplayEnv->SendDisplayPos1();
    }

    DisplayEnv->SendDisplayPos();
}

void CDisplayContext::SwapBuffers()
{
    // flip frame buffer ptrs
    if (DisplayIsDblBuffered) {
        GS::CMemArea* temp = CurFrameMem;
        CurFrameMem        = LastFrameMem;
        LastFrameMem       = temp;

        // display the last completed frame (which is frame n-2 because we're not
        // drawing immediately but building up a packet)
        // remember this is immediately sent, not delayed through a packet
        DisplayEnv->SetFB2Addr(CurFrameMem->GetWordAddr());
        // Flicker filter: RC1 shares RC2's buffer, so it must follow the flip
        // too (else the two circuits blend two DIFFERENT frames -> ghosting).
        // The one-line DBY offset lives in DISPFB1 and survives SetFB1Addr.
        if (FlickerEnabled)
            DisplayEnv->SetFB1Addr(CurFrameMem->GetWordAddr());
        DisplayEnv->SendSettings();
    }
}

/********************************************
 * ps2gl C interface
 */

/**
 * @addtogroup pgl_api
 * @{
 */

/**
 * Tell ps2gl what areas in GS ram to display.
 * @param interlaced PGL_INTERLACED or PGL_NONINTERLACED
 * @param frame0_mem the first area if double-buffered, otherwise the only area
 * @param frame1_mem the second area if double-buffered, otherwise NULL
 */
void pglSetDisplayBuffers(int interlaced, pgl_area_handle_t frame0_mem, pgl_area_handle_t frame1_mem)
{
    pGLContext->GetDisplayContext().SetDisplayBuffers(interlaced,
        reinterpret_cast<GS::CMemArea*>(frame0_mem),
        reinterpret_cast<GS::CMemArea*>(frame1_mem));
}

/**
 * Reconfigure the display for a runtime video-mode change (NTSC / PAL / 480p)
 * without re-creating the frame buffers. Pair it with SetGsCrt() for the scan
 * timing. interlaced: 1 for NTSC/PAL, 0 for 480p. overscan_mode: 0 NTSC, 1 PAL,
 * 2 DTV. screen_y: vertical shift for centering.
 */
void pglSetVideoMode(int interlaced, int overscan_mode, int screen_x, int screen_y)
{
    pGLContext->GetDisplayContext().SetVideoMode(interlaced != 0, overscan_mode, screen_x, screen_y);
}

void pglSetDisplayOffset(int screen_x, int screen_y)
{
    pGLContext->GetDisplayContext().SetDisplayOffset(screen_x, screen_y);
}

/**
 * Flicker filter (interlace softening): blend read circuit 1 (offset one line)
 * over RC2 with a constant alpha. Only stores the state -- re-issue
 * pglSetVideoMode to apply it. alpha is RC1's neighbor-line weight (0..255).
 */
void pglSetFlickerFilter(int enable, int alpha)
{
    pGLContext->GetDisplayContext().SetFlickerFilter(enable != 0, alpha);
}

/** @} */ // pgl_api

/********************************************
 * gl api
 */

void glPixelStorei(GLenum pname, int param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mNotImplemented();
}

void glReadPixels(int x, int y, int width, int height,
    GLenum format, GLenum type, void* pixels)
{
    GL_FUNC_DEBUG("%s(%d,%d,%d,%d,...)\n", __FUNCTION__, x, y, width, height);

    mNotImplemented();
}

void glViewport(GLint x, GLint y,
    GLsizei width, GLsizei height)
{
    GL_FUNC_DEBUG("%s(%d,%d,%d,%d)\n", __FUNCTION__, x, y, width, height);

    // Intentionally a no-op: raylib4ps2 calls this with the LOGICAL framebuffer
    // size (e.g. 640x448), but in interlaced modes the GS draw buffer is a
    // half-height field buffer -- honoring it here would double the vertical
    // scale and shred all geometry. Screen Fit goes through pglSetViewportScale
    // instead, which scales by the draw context's own Width/Height.
    mNotImplemented();
}
