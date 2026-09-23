/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include <string.h>

#include "ps2s/math.h"
#include "ps2s/packet.h"

#include "ps2gl/clear.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

static unsigned int clearPageStripCounts[3];

extern "C" unsigned int pglGetClearPageStripOptions(void)
{
    return PGL_CLEAR_PAGE_STRIPS ? 1u : 0u;
}

extern "C" void pglGetClearPageStripCounts(unsigned int out[3])
{
    if (out) {
        for (unsigned int i = 0; i < 3; ++i)
            out[i] = clearPageStripCounts[i];
    }
}

CClearEnv::CClearEnv()
    : Width(0), Height(0), FramePsm(GS::kPsm32), DepthPsm(GS::kPsmz32)
{
    pDrawEnv = new GS::CDrawEnv(GS::kContext2);
    pDrawEnv->SetDepthTestPassMode(GS::ZTest::kAlways);

    pSprite = new CSprite(GS::kContext2, 0, 0, 0, 0);
    pSprite->SetUseTexture(false);
    unsigned int clearColor[4] = { 0, 0, 0, 0 };
    pSprite->SetColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    pSprite->SetDepth(0);
}

CClearEnv::~CClearEnv()
{
    delete pDrawEnv;
    delete pSprite;
}

void CClearEnv::SetDimensions(int width, int height)
{
    Width = width;
    Height = height;
    pDrawEnv->SetFrameBufferDim(width, height);
    pSprite->SetVertices(0, 0, width, height);
}

void CClearEnv::SetFrameBufPsm(GS::tPSM psm)
{
    FramePsm = psm;
    pDrawEnv->SetFrameBufferPSM(psm);
}

void CClearEnv::SetDepthBufPsm(GS::tPSM psm)
{
    DepthPsm = psm;
    pDrawEnv->SetDepthBufferPSM(psm);
}

bool CClearEnv::CanUsePageStrips() const
{
    // This private sprite has integer XY, constant RGBA/Z, zero XYOFFSET,
    // and TME/FGE/ABE/AA1 off; no public clear setter changes those invariants.
    // Keep partial-page edges and unsupported formats on the original path.
    return Width > 64 && Width <= 2048 && (Width & 63) == 0
        && Height > 0 && Height <= 2048
        && (FramePsm == GS::kPsm32 || FramePsm == GS::kPsm24
            || FramePsm == GS::kPsm16 || FramePsm == GS::kPsm16s)
        && (DepthPsm == GS::kPsmz32 || DepthPsm == GS::kPsmz24
            || DepthPsm == GS::kPsmz16 || DepthPsm == GS::kPsmz16s);
}

void CClearEnv::ClearBuffers(unsigned int bitMask)
{
    ++clearPageStripCounts[0];
    if (bitMask & GL_DEPTH_BUFFER_BIT)
        pDrawEnv->EnableDepthTest();
    else
        pDrawEnv->DisableDepthTest();

    if (bitMask & GL_COLOR_BUFFER_BIT)
        pDrawEnv->SetFrameBufferDrawMask(0);
    else
        pDrawEnv->SetFrameBufferDrawMask(0xffffffff);

    CVifSCDmaPacket& packet = pGLContext->GetVif1Packet();
    pGLContext->AddingDrawEnvToPacket((uint128_t*)pGLContext->GetVif1Packet().GetNextPtr() + 1);
    pDrawEnv->SendSettings(packet);
#if PGL_CLEAR_PAGE_STRIPS
    if (CanUsePageStrips()) {
        // GS Manual p46: top/left inclusive, bottom/right exclusive. These
        // adjacent integer sprites clear each original pixel exactly once.
        // All admitted FB/Z formats have 64-pixel page width (p162); keeping
        // each row within one page avoids the wide-sprite page breaks described
        // in GS Supplement pp6-9. Keep one ordered DIRECT for the whole clear.
        ++clearPageStripCounts[1];
        clearPageStripCounts[2] += Width / 64;
        // CSprite owns six qwords: GIFtag, RGBAQ, NOP, XYZF2, NOP, XYZF2.
        // Keep its exact PRIM/color/vertex words, but send the fixed color
        // once and repeat just the two vertex registers for each strip.
        const uint128_t* source = pSprite->GetPacket().GetBase();
        tGifTag colorTag;
        memcpy(&colorTag, source, sizeof(colorTag));
        tGifTag vertexTag = colorTag;
        colorTag.NREG = 1;
        colorTag.EOP = 0;
        vertexTag.NLOOP = Width / 64;
        vertexTag.NREG = 2;
        vertexTag.PRE = 0;
        vertexTag.REGS0 = colorTag.REGS2;
        vertexTag.REGS1 = colorTag.REGS4;
        uint128_t vertices[2] __attribute__((aligned(16)));
        memcpy(vertices, source + 3, sizeof(vertices[0]));
        memcpy(vertices + 1, source + 5, sizeof(vertices[1]));
        packet.Cnt();
        packet.Nop();
        if (!packet.GetTTE())
            packet.Nop().Nop();
        packet.OpenDirect();
        packet += colorTag;
        packet.Add(source + 1, 1);
        packet += vertexTag;
        for (int x = 0; x < Width; x += 64) {
            const unsigned int left = x << 4;
            const unsigned int right = (x + 64) << 4;
            memcpy(vertices, &left, sizeof(left));
            memcpy(vertices + 1, &right, sizeof(right));
            packet.Add(vertices, 2);
        }
        packet.CloseDirect();
        packet.CloseTag();
        return;
    }
#endif
    pSprite->Draw(packet);
}

/********************************************
 * C gl api
 */

void glClearColor(GLclampf red,
    GLclampf green,
    GLclampf blue,
    GLclampf alpha)
{
    GL_FUNC_DEBUG("%s(%f,%f,%f,%f)\n", __FUNCTION__, red, green, blue, alpha);

    CClearEnv& clearEnv = pGLContext->GetImmDrawContext().GetClearEnv();

    using namespace Math;
    clearEnv.SetClearColor(Clamp(red, 0.0f, 1.0f),
        Clamp(green, 0.0f, 1.0f),
        Clamp(blue, 0.0f, 1.0f),
        Clamp(alpha, 0.0f, 1.0f));
}

void glClearDepth(GLclampd depth)
{
    GL_FUNC_DEBUG("%s(%f)\n", __FUNCTION__, depth);

    CClearEnv& clearEnv = pGLContext->GetImmDrawContext().GetClearEnv();

    clearEnv.SetClearDepth((float)depth);
}

void glClear(GLbitfield mask)
{
    GL_FUNC_DEBUG("%s(0x%x)\n", __FUNCTION__, mask);

    pGLContext->GetImmDrawContext().GetClearEnv().ClearBuffers(mask);
}
