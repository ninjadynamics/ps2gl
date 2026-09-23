/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_clear_h
#define ps2gl_clear_h

// Independent clear-only A/B. Rebuild ps2gl after changing this switch.
#ifndef PGL_CLEAR_PAGE_STRIPS
#define PGL_CLEAR_PAGE_STRIPS 1
#endif
#if PGL_CLEAR_PAGE_STRIPS != 0 && PGL_CLEAR_PAGE_STRIPS != 1
#error "PGL_CLEAR_PAGE_STRIPS must be 0 or 1"
#endif

#include "GL/gl.h"

#include "ps2s/drawenv.h"

#include "ps2s/sprite.h"

class CVifSCDmaPacket;

/********************************************
 * class def
 */

class CClearEnv {

    GS::CDrawEnv* pDrawEnv;
    CSprite* pSprite;
    int Width, Height;
    GS::tPSM FramePsm, DepthPsm;

    bool CanUsePageStrips() const;

public:
    CClearEnv();
    ~CClearEnv();

    void SetDimensions(int width, int height);
    void SetFrameBufPsm(GS::tPSM psm);
    void SetDepthBufPsm(GS::tPSM psm);
    void SetFrameBufAddr(unsigned int gsWordAddr)
    {
        pDrawEnv->SetFrameBufferAddr(gsWordAddr);
    }
    void SetDepthBufAddr(unsigned int gsWordAddr)
    {
        pDrawEnv->SetDepthBufferAddr(gsWordAddr);
    }

    void SetClearColor(float r, float g, float b, float a)
    {
        pSprite->SetColor((unsigned int)(255.0f * r),
            (unsigned int)(255.0f * g),
            (unsigned int)(255.0f * b),
            (unsigned int)(255.0f * a));
    }

    void SetClearDepth(float depth)
    {
        pSprite->SetDepth(Core::FToI4(depth));
    }

    void ClearBuffers(unsigned int bitMask);
};

#endif // ps2gl_clear_h
