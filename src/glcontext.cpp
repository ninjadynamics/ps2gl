/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "dma.h"
#include "graph.h"
#include "kernel.h"

#include "GL/ps2gl.h"

#include "ps2s/displayenv.h"
#include "ps2s/drawenv.h"
#include "ps2s/gsmem.h"
#include "ps2s/math.h"
#include "ps2s/packet.h"
#include "ps2s/ps2stuff.h"
#include "ps2s/texture.h"
#include "ps2s/types.h"

#include "ps2gl/displaycontext.h"
#include "ps2gl/dlgmanager.h"
#include "ps2gl/dlist.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/gmanager.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/lighting.h"
#include "ps2gl/material.h"
#include "ps2gl/matrix.h"
#include "ps2gl/texture.h"

#include "ee_regs.h"

/********************************************
 * globals
 */

/********************************************
 * CGLContext
 */

// static members

CVifSCDmaPacket *CGLContext::CurPacket, *CGLContext::LastPacket,
    *CGLContext::Vif1Packet = NULL, *CGLContext::SavedVif1Packet = NULL,
    *CGLContext::ImmVif1Packet;

int CGLContext::RenderingFinishedSemaId          = -1;
int CGLContext::ImmediateRenderingFinishedSemaId = -1;
int CGLContext::VsyncSemaId                      = -1;

CGLContext::tRenderingFinishedCallback CGLContext::RenderingFinishedCallback = NULL;

// Submission is owned by the main thread; completion by the GS interrupt.
// Separate counters avoid a shared pending-bit read/modify/write race. They
// describe our end-of-chain SIGNALs, not mere VIF DMA idleness or CPU returns.
static volatile unsigned int NormalChainsSubmitted = 0, NormalChainsCompleted = 0;
static volatile unsigned int ImmediateChainsSubmitted = 0, ImmediateChainsCompleted = 0;

static unsigned int DrawEnvLastFrame, DrawEnvHighWater, DrawEnvOver100Frames;
static unsigned int DrawEnvGrowths, DrawEnvHeapBytes;
static void (*DrawEnvHeapObserver)(unsigned int, unsigned int);

#if PGL_FRAME_PHASE_METRICS
static bool FramePhaseEnabled;
static PGLFramePhaseStats FramePhases[PGL_FRAME_PHASE_COUNT];

static inline unsigned int FramePhaseClock()
{
    unsigned int cycles;
    // Read-only CP0 Count, independent of PCR0/1 and the raylib system timer.
    // The compiler barrier brackets the measured work; no timer reset/fence.
    asm volatile("mfc0 %0, $9" : "=r"(cycles) : : "memory");
    return cycles;
}

class CFramePhaseScope {
    unsigned int Start;
    const unsigned int Phase;
    const bool Enabled;
public:
    explicit CFramePhaseScope(unsigned int phase)
        : Start(0), Phase(phase), Enabled(FramePhaseEnabled)
    {
        if (Enabled) Start = FramePhaseClock();
    }
    ~CFramePhaseScope()
    {
        if (!Enabled) return;
        const unsigned int elapsed = FramePhaseClock() - Start;
        PGLFramePhaseStats& stats = FramePhases[Phase];
        stats.cycles += elapsed;
        ++stats.calls;
        if (elapsed > stats.maxCycles) stats.maxCycles = elapsed;
    }
};
#define PGL_FRAME_SCOPE(phase) CFramePhaseScope phaseScope(phase)
#else
#define PGL_FRAME_SCOPE(phase) ((void)0)
#endif

extern "C" GLboolean pglSetFramePhaseMetrics(GLboolean enabled)
{
#if PGL_FRAME_PHASE_METRICS
    memset(FramePhases, 0, sizeof(FramePhases));
    FramePhaseEnabled = enabled != GL_FALSE;
    return FramePhaseEnabled ? GL_TRUE : GL_FALSE;
#else
    (void)enabled;
    return GL_FALSE;
#endif
}

extern "C" GLboolean pglTakeFramePhaseMetrics(PGLFramePhaseStats phases[PGL_FRAME_PHASE_COUNT])
{
    if (!phases) return GL_FALSE;
#if PGL_FRAME_PHASE_METRICS
    memcpy(phases, FramePhases, sizeof(FramePhases));
    memset(FramePhases, 0, sizeof(FramePhases));
    return FramePhaseEnabled ? GL_TRUE : GL_FALSE;
#else
    memset(phases, 0, PGL_FRAME_PHASE_COUNT * sizeof(*phases));
    return GL_FALSE;
#endif
}

extern "C" void pglSetDrawEnvHeapObserver(void (*observer)(unsigned int, unsigned int))
{
    if (DrawEnvHeapObserver == observer) return;
    DrawEnvHeapObserver = observer;
    if (observer && DrawEnvHeapBytes) observer(0, DrawEnvHeapBytes);
}

extern "C" void pglGetDrawEnvStats(unsigned int* lastFrame, unsigned int* highWater,
    unsigned int* over100Frames, unsigned int* growths, unsigned int* heapBytes)
{
    if (lastFrame) *lastFrame = DrawEnvLastFrame;
    if (highWater) *highWater = DrawEnvHighWater;
    if (over100Frames) *over100Frames = DrawEnvOver100Frames;
    if (growths) *growths = DrawEnvGrowths;
    if (heapBytes) *heapBytes = DrawEnvHeapBytes;
}

void CGLContext::GrowDrawEnvPtrBank()
{
    // The old debug-only assertion allowed entry101 to overwrite the next
    // bank, or CurDrawEnvPtrs itself. Failure must stop before any write even
    // in release builds. Limit both signed counts and allocation arithmetic.
    if (NumCurDrawEnvPtrs < 0 || NumCurDrawEnvPtrs != CurDrawEnvCapacity
        || CurDrawEnvCapacity <= 0 || CurDrawEnvCapacity > INT_MAX / 2
        || (size_t)CurDrawEnvCapacity > ((size_t)-1) / (2 * sizeof(void*))) {
        fputs("ps2gl: invalid draw-environment pointer capacity\n", stderr);
        abort();
    }
    const int newCapacity = CurDrawEnvCapacity * 2;
    const bool oldIsHeap = CurDrawEnvPtrs != DrawEnvPtrs0 && CurDrawEnvPtrs != DrawEnvPtrs1;
    const size_t oldBankBytes = oldIsHeap ? (size_t)CurDrawEnvCapacity * sizeof(void*) : 0;
    const size_t newBankBytes = (size_t)newCapacity * sizeof(void*);
    if (oldBankBytes > DrawEnvHeapBytes
        || newBankBytes > UINT_MAX - (DrawEnvHeapBytes - oldBankBytes)) {
        fputs("ps2gl: draw-environment heap accounting overflow\n", stderr);
        abort();
    }
    void** grown = (void**)malloc(newBankBytes);
    if (!grown) {
        fputs("ps2gl: draw-environment pointer allocation failed\n", stderr);
        abort();
    }
    memcpy(grown, CurDrawEnvPtrs, (size_t)NumCurDrawEnvPtrs * sizeof(void*));
    if (oldIsHeap) free(CurDrawEnvPtrs);
    CurDrawEnvPtrs = grown;
    CurDrawEnvCapacity = newCapacity;
    const unsigned int oldHeapBytes = DrawEnvHeapBytes;
    DrawEnvHeapBytes = (unsigned int)(DrawEnvHeapBytes - oldBankBytes + newBankBytes);
    ++DrawEnvGrowths;
    if (DrawEnvHeapObserver) DrawEnvHeapObserver(oldHeapBytes, DrawEnvHeapBytes);
}

CGLContext::CGLContext(int immBufferQwordSize, int immDrawBufferQwordSize)
    : StateChangesArePushed(false)
    , IsCurrentFieldEven(true)
    , CurrentFrameNumber(0)
    , CurBuffer(0)
{
    NormalChainsSubmitted = NormalChainsCompleted = 0;
    ImmediateChainsSubmitted = ImmediateChainsCompleted = 0;
#if PGL_FRAME_PHASE_METRICS
    memset(FramePhases, 0, sizeof(FramePhases));
#endif
    pglInvalidateX2BasePrefix();
    // Cached construction avoids UCAB's single read-only cache line being
    // invalidated by every store before tag/UNPACK backpatches read it again.
    // The asynchronous frame-buffer ownership remains unchanged.
    // RenderGeometry must retain Send()'s full cache writeback before DMA.
    const unsigned int framePacketMapping = Core::MemMappings::Normal;
    CurPacket = new CVifSCDmaPacket(kDmaPacketMaxQwordLength, DMAC::Channels::vif1,
        Packet::kXferTags, framePacketMapping);
    LastPacket = new CVifSCDmaPacket(kDmaPacketMaxQwordLength, DMAC::Channels::vif1,
        Packet::kXferTags, framePacketMapping);
    Vif1Packet = CurPacket;

    ImmVif1Packet = new CVifSCDmaPacket(immDrawBufferQwordSize, DMAC::Channels::vif1,
        Packet::kXferTags, Core::MemMappings::UncachedAccl);

    CurDrawEnvPtrs     = DrawEnvPtrs0;
    LastDrawEnvPtrs    = DrawEnvPtrs1;
    NumCurDrawEnvPtrs  = 0;
    NumLastDrawEnvPtrs = 0;
    CurDrawEnvCapacity = LastDrawEnvCapacity = kMaxDrawEnvChanges;
    DrawEnvLastFrame = DrawEnvHighWater = DrawEnvOver100Frames = 0;
    DrawEnvGrowths = DrawEnvHeapBytes = 0;

    ImmGManager   = new CImmGeomManager(*this, immBufferQwordSize);
    DListGManager = new CDListGeomManager(*this);
    CurGManager   = ImmGManager;

    ProjectionMatStack = new CImmMatrixStack(*this);
    ModelViewMatStack  = new CImmMatrixStack(*this);
    DListMatStack      = new CDListMatrixStack(*this);
    CurMatrixStack     = ModelViewMatStack;
    SavedCurMatStack   = NULL;

    ImmLighting   = new CImmLighting(*this);
    DListLighting = new CDListLighting(*this);
    CurLighting   = ImmLighting;
    // defaults
    CLight& light = ImmLighting->GetLight(0);
    light.SetDiffuse(cpu_vec_xyzw(1.0f, 1.0f, 1.0f, 1.0f));
    light.SetSpecular(cpu_vec_xyzw(1.0f, 1.0f, 1.0f, 1.0f));

    MaterialManager = new CMaterialManager(*this);
    DListManager    = new CDListManager;
    TexManager      = new CTexManager(*this);

    ImmDrawContext   = new CImmDrawContext(*this);
    DListDrawContext = new CDListDrawContext(*this);
    CurDrawContext   = ImmDrawContext;

    DisplayContext = new CDisplayContext(*this);

    SetRendererContextChanged(true);
    SetGsContextChanged(true);
    SetRendererPropsChanged(true);

    // util
    NumBuffersToBeFreed[0] = NumBuffersToBeFreed[1] = 0;

    GS::Init();

    // create a few semaphores

    struct t_ee_sema newSemaphore    = { 0, 1, 0 }; // but maxCount doesn't work?
    VsyncSemaId                      = CreateSema(&newSemaphore);
    RenderingFinishedSemaId          = CreateSema(&newSemaphore);
    ImmediateRenderingFinishedSemaId = CreateSema(&newSemaphore);
    mErrorIf(VsyncSemaId == -1
            || RenderingFinishedSemaId == -1
            || ImmediateRenderingFinishedSemaId == -1,
        "Failed to create ps2gl semaphores.");

    // add an interrupt handler for gs "signal" exceptions

    AddIntcHandler(INTC_GS, CGLContext::GsIntHandler, 0 /*first handler*/);
    EnableIntc(INTC_GS);
    // clear any signal/vsync exceptions and wait for the next
    *(volatile unsigned int*)GS::ControlRegs::csr = 9;
    // enable signal and vsync exceptions
    *(volatile unsigned int*)GS::ControlRegs::imr = 0x7600;

    // Mask bugged tag mismatch error
    WR_EE_VIF1_ERR(2);
}

CGLContext::~CGLContext()
{
    if (CurDrawEnvPtrs != DrawEnvPtrs0 && CurDrawEnvPtrs != DrawEnvPtrs1)
        free(CurDrawEnvPtrs);
    if (LastDrawEnvPtrs != DrawEnvPtrs0 && LastDrawEnvPtrs != DrawEnvPtrs1)
        free(LastDrawEnvPtrs);
    const unsigned int oldHeapBytes = DrawEnvHeapBytes;
    DrawEnvHeapBytes = 0;
    if (DrawEnvHeapObserver && oldHeapBytes) DrawEnvHeapObserver(oldHeapBytes, 0);

    delete CurPacket;
    delete LastPacket;

    delete ImmGManager;
    delete DListGManager;

    delete ProjectionMatStack;
    delete ModelViewMatStack;
    delete DListMatStack;

    delete ImmLighting;
    delete DListLighting;

    delete MaterialManager;
    delete DListManager;
    delete TexManager;

    delete ImmDrawContext;
    delete DListDrawContext;

    delete DisplayContext;
}

/********************************************
 * display lists
 */

void CGLContext::BeginDListDef(unsigned int listID, GLenum mode)
{
    DListManager->NewList(listID, mode);

    PushStateChanges();

    // not so sure about these two, but let's be cautious
    SetRendererContextChanged(true);
    SetGsContextChanged(true);
    // definately need this to force an update - indexed/linear arrays
    SetRendererPropsChanged(true);

    MaterialManager->BeginDListDef();
    TexManager->BeginDListDef();
    DListGManager->BeginDListDef();

    CurLighting      = DListLighting;
    CurGManager      = DListGManager;
    SavedCurMatStack = CurMatrixStack;
    CurMatrixStack   = DListMatStack;
    CurDrawContext   = DListDrawContext;
}

void CGLContext::EndDListDef()
{
    DListGManager->EndDListDef();
    MaterialManager->EndDListDef();
    TexManager->EndDListDef();

    CurLighting    = ImmLighting;
    CurGManager    = ImmGManager;
    CurMatrixStack = SavedCurMatStack;
    CurDrawContext = ImmDrawContext;

    PopStateChanges();

    DListManager->EndList();
}

/********************************************
 * matrix mode
 */

class CSetMatrixModeCmd : public CDListCmd {
    GLenum Mode;

public:
    CSetMatrixModeCmd(GLenum mode)
        : Mode(mode)
    {
    }
    CDListCmd* Play()
    {
        glMatrixMode(Mode);
        return CDListCmd::GetNextCmd(this);
    }
};

void CGLContext::SetMatrixMode(GLenum mode)
{
    if (InDListDef()) {
        DListManager->GetOpenDList() += CSetMatrixModeCmd(mode);
    } else {
        switch (mode) {
        case GL_MODELVIEW:
            CurMatrixStack = ModelViewMatStack;
            break;
        case GL_PROJECTION:
            CurMatrixStack = ProjectionMatStack;
            break;
        default:
            mNotImplemented();
        }
    }
}

/********************************************
 * immediate geometry
 */

void CGLContext::BeginImmediateGeometry()
{
    //     mErrorIf( InDListDef == true,
    //  	     "pglBeginImmediateGeom can't be called in a display list definition." );

    // flush any pending geometry
    GetImmGeomManager().Flush();

    PushVif1Packet();
    SetVif1Packet(*ImmVif1Packet);

    pglInvalidateUnlitContextDelta();
    GS::CTexEnv::InvalidateTextureSync(); // a reset chain contains no prior texture proof
    ImmVif1Packet->Reset();
}

void CGLContext::EndImmediateGeometry()
{
    mAssert(Vif1Packet == ImmVif1Packet);

    EndVif1Packet(2);

    PopVif1Packet();
}

void CGLContext::RenderImmediateGeometry()
{
    ImmVif1Packet->End();
    ImmVif1Packet->Pad128();
    ImmVif1Packet->CloseTag();

    ++ImmediateChainsSubmitted;
    ImmVif1Packet->Send();
}

void CGLContext::FinishRenderingImmediateGeometry(bool forceImmediateStop)
{
    mWarnIf(forceImmediateStop, "Interrupting currently rendering dma chain not supported yet");
    mNotImplemented();
}

/********************************************
 * normal geometry
 */

void CGLContext::BeginGeometry()
{
    pglInvalidateX2BasePrefix();
    pglInvalidateUnlitContextDelta();
    // reset packets that will be drawn to during this frame

    GS::CTexEnv::InvalidateTextureSync();
    CurPacket->Reset();
}

void CGLContext::EndGeometry()
{
    PGL_FRAME_SCOPE(PGL_FRAME_END);
    EndVif1Packet(1);
}

void CGLContext::EndVif1Packet(unsigned short signalNum)
{
    //printf("%s(%d)\n", __FUNCTION__, signalNum);

    // flush any pending geometry
    GetImmGeomManager().Flush();

    // end current packet
    // write our id to the signal register and trigger an
    // exception on the core when this dma chain reaches the end

    tGifTag giftag;
    giftag.NLOOP = 1;
    giftag.EOP   = 1;
    giftag.PRE   = 0;
    giftag.FLG   = 0; // packed
    giftag.NREG  = 1;
    giftag.REGS0 = 0xe; // a+d

    Vif1Packet->End();
    Vif1Packet->Flush();
    Vif1Packet->OpenDirect();
    {
        *Vif1Packet += giftag;
        *Vif1Packet += Ps2glSignalId | signalNum;
        *Vif1Packet += (uint64_t)0x60; // signal
    }
    Vif1Packet->CloseDirect();
    Vif1Packet->CloseTag();
}

void CGLContext::RenderGeometry()
{
    PGL_FRAME_SCOPE(PGL_FRAME_SEND);
    //printf("%s\n", __FUNCTION__);

    // make sure the semaphore we'll signal on completion is zero now
    while (PollSema(RenderingFinishedSemaId) != -1)
        ;

    ++NormalChainsSubmitted;
    LastPacket->Send();
}

int CGLContext::GsIntHandler(int cause)
{
    // Drain EVERY unmasked event (SIGNAL bit 0 | VSYNC bit 3, per the IMR set
    // in the ctor) before returning. The GS holds its INTC line asserted while
    // ANY unmasked CSR event bit is set, and the EE INTC latches EDGES only —
    // returning with one still pending means the line never drops, no edge is
    // ever latched again, GS interrupts die, and the next WaitSema on this
    // path sleeps forever (the intermittent EE hard freeze on chain-heavy
    // frames, whose end-of-chain SIGNAL drifts across vblank). Servicing only
    // the bits seen in ONE read at entry is not enough: an event that sets
    // DURING the handler (after the read) also keeps the line high — so loop
    // until CSR reads clean, which proves the line is low and the next event
    // makes a fresh edge.
    uint32_t csr;
    while ((csr = *(volatile uint32_t*)GS::ControlRegs::csr) & 9) {
        // signal interrupt?
        if (csr & 1) {
            // is it one of ours?
            uint64_t sigLblId = *(volatile uint64_t*)GS::ControlRegs::siglblid;
            if ((uint16_t)(sigLblId >> 16) == GetPs2glSignalId()) {
                switch (sigLblId & 0xffff) {
                case 1:
                    ++NormalChainsCompleted;
                    iSignalSema(RenderingFinishedSemaId);
                    if (RenderingFinishedCallback != NULL)
                        RenderingFinishedCallback();
                    break;
                case 2:
                    ++ImmediateChainsCompleted;
                    iSignalSema(ImmediateRenderingFinishedSemaId);
                    break;
                default:
                    mError("Unknown signal");
                }

                // clear our signal id
                sigLblId &= ~0xffffffff;
                *(volatile uint64_t*)GS::ControlRegs::siglblid = sigLblId;
            }
            // clear the exception unconditionally — a SIGNAL event left
            // pending holds the interrupt line high and kills all future
            // GS interrupts
            *(volatile unsigned int*)GS::ControlRegs::csr = 1;
        }
        // vsync interrupt? (NOT else — both may be pending together)
        if (csr & 8) {
            iSignalSema(VsyncSemaId);
            // clear the exception and wait for the next
            *(volatile unsigned int*)GS::ControlRegs::csr = 8;
        }
    }

    ExitHandler();

    return 0;
}

void CGLContext::FinishRenderingGeometry(bool forceImmediateStop)
{
    PGL_FRAME_SCOPE(PGL_FRAME_FINISH);
    //printf("%s(%d)\n", __FUNCTION__, forceImmediateStop);

    mWarnIf(forceImmediateStop, "Interrupting currently rendering dma chain not supported yet");
    WaitSema(RenderingFinishedSemaId);
}

void CGLContext::WaitForVSync()
{
    PGL_FRAME_SCOPE(PGL_FRAME_VSYNC);
    //printf("%s\n", __FUNCTION__);

    // wait for beginning of v-sync
    WaitSema(VsyncSemaId);
    // sometimes if we miss a frame the semaphore gets incremented
    // more than once (because maxCount is ignored?) which causes the next
    // call to WaitForVSync to fall through immediately, which is kinda bad,
    // so make sure the count is zero after waiting.
    while (PollSema(VsyncSemaId) != -1)
        ;
    // sceGsSyncV(0);
    uint32_t csr       = *(volatile uint32_t*)GS::ControlRegs::csr;
    IsCurrentFieldEven = (bool)((csr >> 13) & 1);
}

void CGLContext::SwapBuffers()
{
    PGL_FRAME_SCOPE(PGL_FRAME_SWAP);
    pglInvalidateX2BasePrefix();
    //printf("%s\n", __FUNCTION__);

    // switch packet ptrs

    CVifSCDmaPacket* tempPkt = CurPacket;
    CurPacket                = LastPacket;
    LastPacket               = tempPkt;
    Vif1Packet               = CurPacket;

    // switch drawenv ptrs

    DrawEnvLastFrame = (unsigned int)NumCurDrawEnvPtrs;
    if (DrawEnvLastFrame > DrawEnvHighWater) DrawEnvHighWater = DrawEnvLastFrame;
    if (NumCurDrawEnvPtrs > kMaxDrawEnvChanges) ++DrawEnvOver100Frames;
    void** tempDEPtrs  = CurDrawEnvPtrs;
    CurDrawEnvPtrs     = LastDrawEnvPtrs;
    LastDrawEnvPtrs    = tempDEPtrs;
    const int tempDECapacity = CurDrawEnvCapacity;
    CurDrawEnvCapacity = LastDrawEnvCapacity;
    LastDrawEnvCapacity = tempDECapacity;
    NumLastDrawEnvPtrs = NumCurDrawEnvPtrs;
    NumCurDrawEnvPtrs  = 0;

    // tell some modules that it's time to flip

    GetImmGeomManager().SwapBuffers();
    GetDListManager().SwapBuffers();
    GetDisplayContext().SwapBuffers();
    GetImmDrawContext().SwapBuffers(IsCurrentFieldEven);

    // free memory that was waiting til end of frame
    FreeWaitingBuffersAndSwap();

    CurrentFrameNumber++;
}

void CGLContext::FreeWaitingBuffersAndSwap()
{
    //printf("%s\n", __FUNCTION__);

    CurBuffer = 1 - CurBuffer;

    for (int i = 0; i < NumBuffersToBeFreed[CurBuffer]; i++) {
        free(BuffersToBeFreed[CurBuffer][i]);
    }

    NumBuffersToBeFreed[CurBuffer] = 0;
}

/********************************************
 * ps2gl C interface
 */

/// global pointer to the GLContext
CGLContext* pGLContext = NULL;

/**
 * @addtogroup pgl_api pgl* API
 *
 * The pgl* functions affect the behavior of ps2gl and
 * provide access to ps2 features not well-suited to the gl api.
 *
 * The recommended way to use the ps2gl library is for the app
 * to use the pgl* functions to configure the library.  Alternatively,
 * for a quick start try the [very incomplete] glut implementation, which
 * will set things up using default values.
 *
 * @{
 */

/**
 * Initialize the ps2gl library.  You must call this before any other pgl* or gl* functions!
 * (When using glut, it will be called in glutInit() if the ps2gl library was not
 * initialized by the app previously.)
 * Also, the application is now responsible for resetting the machine, including the
 * display mode (putting the gs into the right output state, i.e., resolution and interlaced),
 * usually this just means calling sceGsResetGraph.
 * @param immBufferVertexSize ps2gl uses fixed-size internal buffers to store geometry;
 * this argument tells the library how much space to allocate.
 */
int pglInit(int immBufferVertexSize, int immDrawBufferQwordSize)
{
    // Canary: proves the locally-built ps2gl fork is linked (not the toolchain
    // prebuilt). Stamped with the build timestamp by the Makefile's `ps2gl`
    // target. pglInit() is the library entry point, so this prints once.
    printf("[ CANARY ] Welcome to MODIFIED LOCAL ps2gl! [2026.09.20 13:27]\n");
    printf("[PS2-PACKETS] normal=cached\n");
    printf("[PS2-STACK] lazy-inverse=%d aligned-xfer=%d direct-tags=%d\n",
        1, PGL_ALIGNED_VECTOR_TRANSFER,
        PS2S_DIRECT_PACKET_TAGS);

    ps2sInit();
    pGLContext = new CGLContext(immBufferVertexSize, immDrawBufferQwordSize);

    return true;
}

/**
 * Has pglInit() been called?
 * @return 1 if pglInit has been called, 0 otherwise
 */
int pglHasLibraryBeenInitted(void)
{
    return (pGLContext != NULL);
}

/**
 * Do any necessary clean up when finished using ps2gl.
 */
void pglFinish(void)
{
    if (pGLContext)
        delete pGLContext;
    ps2sFinish();
}

/**
 * Wait for dma transfers to vif1 to end.  Polls cop0, so it should not slow down
 * the transfer, unlike sceGsSyncPath().
 *
 * This call is for convenience only -- there is no need to call it if the app
 * can manage on its own.
 */
void pglWaitForVU1(void)
{
    dma_channel_wait(DMAC::Channels::vif1, 1000000);
}

/**
 * Wait for the vertical retrace.  Note that this call is <b>required</b>
 * for the interlacing to work properly.  (Called by glut.)
 */
void pglWaitForVSync(void)
{
    pGLContext->WaitForVSync();
}

/**
 * Signals the end of the current rendering loop and swaps anything
 * double-buffered (display, draw buffers).
 *
 * Note that this call is <b>required</b>.  (Called by glut.)
 */
void pglSwapBuffers(void)
{
    mErrorIf(pGLContext == NULL, "You need to call pglInit()");

    pGLContext->SwapBuffers();
}

/**
 * Set a function to be called back when rendering finishes.  <b>This
 * function will be called from the interrupt handler; be careful!</b>
 * @param a pointer to the callback function or NULL to clear
 */
void pglSetRenderingFinishedCallback(void (*cb)(void))
{
    pGLContext->SetRenderingFinishedCallback(cb);
}

void pglGetRenderProgress(unsigned int* pendingSignals,
    unsigned int* normalAcknowledged, unsigned int* immediateAcknowledged)
{
    const unsigned int normalDone = NormalChainsCompleted;
    const unsigned int immediateDone = ImmediateChainsCompleted;
    *pendingSignals = (NormalChainsSubmitted != normalDone ? 1u : 0u)
        | (ImmediateChainsSubmitted != immediateDone ? 2u : 0u);
    *normalAcknowledged = normalDone;
    *immediateAcknowledged = immediateDone;
}

/********************************************
 * immediate geometry
 */

void pglBeginImmediateGeometry(void)
{
    pGLContext->BeginImmediateGeometry();
}
void pglEndImmediateGeometry(void)
{
    pGLContext->EndImmediateGeometry();
}
void pglRenderImmediateGeometry(void)
{
    pGLContext->RenderImmediateGeometry();
}
void pglFinishRenderingImmediateGeometry(int forceImmediateStop)
{
    pGLContext->FinishRenderingImmediateGeometry((bool)forceImmediateStop);
}

/********************************************
 * normal geometry
 */

void pglBeginGeometry(void)
{
    pGLContext->BeginGeometry();
}
void pglEndGeometry(void)
{
    pGLContext->EndGeometry();
}
void pglRenderGeometry(void)
{
    pGLContext->RenderGeometry();
}
void pglFinishRenderingGeometry(int forceImmediateStop)
{
    pGLContext->FinishRenderingGeometry((bool)forceImmediateStop);
}

/********************************************
 * enable / disable
 */

void pglEnable(GLenum cap)
{
    switch (cap) {
    case PGL_CLIPPING:
        pGLContext->GetDrawContext().SetDoClipping(true);
        break;
    case PGL_EDGE_AA:
        pGLContext->GetDrawContext().SetEdgeAAEnabled(true);
        break;
    default:
        mError("Unknown option passed to pglEnable()");
    }
}

void pglDisable(GLenum cap)
{
    switch (cap) {
    case PGL_CLIPPING:
        pGLContext->GetDrawContext().SetDoClipping(false);
        break;
    case PGL_EDGE_AA:
        pGLContext->GetDrawContext().SetEdgeAAEnabled(false);
        break;
    default:
        mError("Unknown option passed to pglDisable()");
    }
}

/**
 * @}
 */

/********************************************
 * gl interface
 */

void glEnable(GLenum cap)
{
    GL_FUNC_DEBUG("%s(0x%x)\n", __FUNCTION__, cap);

    CLighting& lighting = pGLContext->GetLighting();

    switch (cap) {
    case GL_LIGHT0:
    case GL_LIGHT1:
    case GL_LIGHT2:
    case GL_LIGHT3:
    case GL_LIGHT4:
    case GL_LIGHT5:
    case GL_LIGHT6:
    case GL_LIGHT7:
        lighting.GetLight(0x7 & cap).SetEnabled(true);
        break;
    case GL_LIGHTING:
        lighting.SetLightingEnabled(true);
        break;

    case GL_BLEND:
        pGLContext->GetDrawContext().SetBlendEnabled(true);
        break;

    case GL_COLOR_MATERIAL:
        pGLContext->GetMaterialManager().SetUseColorMaterial(true);
        break;
    case GL_RESCALE_NORMAL:
        pGLContext->GetDrawContext().SetRescaleNormals(true);
        break;

    case GL_TEXTURE_2D:
        pGLContext->GetTexManager().SetTexEnabled(true);
        break;

    case GL_NORMALIZE:
        pGLContext->GetGeomManager().SetDoNormalize(true);
        break;

    case GL_CULL_FACE:
        pGLContext->GetDrawContext().SetDoCullFace(true);
        break;

    case GL_ALPHA_TEST:
        pGLContext->GetDrawContext().SetAlphaTestEnabled(true);
        break;

    case GL_DEPTH_TEST:
        pGLContext->GetDrawContext().SetDepthTestEnabled(true);
        break;

    case GL_FOG:
        // immediate context only (no dlist recording of fog state)
        pGLContext->GetImmDrawContext().SetFogEnabled(true);
        break;

    default:
        mNotImplemented();
        break;
    }
}

void glDisable(GLenum cap)
{
    GL_FUNC_DEBUG("%s(0x%x)\n", __FUNCTION__, cap);

    switch (cap) {
    case GL_LIGHT0:
    case GL_LIGHT1:
    case GL_LIGHT2:
    case GL_LIGHT3:
    case GL_LIGHT4:
    case GL_LIGHT5:
    case GL_LIGHT6:
    case GL_LIGHT7:
        pGLContext->GetLighting().GetLight(0x7 & cap).SetEnabled(false);
        break;
    case GL_LIGHTING:
        pGLContext->GetLighting().SetLightingEnabled(false);
        break;

    case GL_BLEND:
        pGLContext->GetDrawContext().SetBlendEnabled(false);
        break;

    case GL_COLOR_MATERIAL:
        pGLContext->GetMaterialManager().SetUseColorMaterial(false);
        break;
    case GL_RESCALE_NORMAL:
        pGLContext->GetDrawContext().SetRescaleNormals(false);
        break;

    case GL_TEXTURE_2D:
        pGLContext->GetTexManager().SetTexEnabled(false);
        break;

    case GL_NORMALIZE:
        pGLContext->GetGeomManager().SetDoNormalize(false);
        break;

    case GL_CULL_FACE:
        pGLContext->GetDrawContext().SetDoCullFace(false);
        break;

    case GL_ALPHA_TEST:
        pGLContext->GetDrawContext().SetAlphaTestEnabled(false);
        break;

    case GL_DEPTH_TEST:
        pGLContext->GetDrawContext().SetDepthTestEnabled(false);
        break;

    case GL_FOG:
        pGLContext->GetImmDrawContext().SetFogEnabled(false);
        break;

    default:
        mNotImplemented();
    }
}

void glHint(GLenum target, GLenum mode)
{
    GL_FUNC_DEBUG("%s(0x%x,0x%x)\n", __FUNCTION__, target, mode);

    mNotImplemented();
}

void glGetFloatv(GLenum pname, GLfloat* params)
{
    GL_FUNC_DEBUG("%s(0x%x,...)\n", __FUNCTION__, pname);

    switch (pname) {
    case GL_MODELVIEW_MATRIX:
        memcpy(params, &(pGLContext->GetModelViewStack().GetTop()), 16 * 4);
        break;
    case GL_PROJECTION_MATRIX:
        memcpy(params, &(pGLContext->GetProjectionStack().GetTop()), 16 * 4);
        break;
    default:
        mNotImplemented("pname %d", pname);
        break;
    }
}

void glGetIntegerv(GLenum pname, int* params)
{
    GL_FUNC_DEBUG("%s(0x%x,...)\n", __FUNCTION__, pname);

    mNotImplemented();
}

GLenum glGetError(void)
{
    GL_FUNC_DEBUG("%s()\n", __FUNCTION__);

    mWarn("glGetError does nothing");

    return 0;
}

const GLubyte* glGetString(GLenum name)
{
    GL_FUNC_DEBUG("%s(0x%x)\n", __FUNCTION__, name);

    mNotImplemented();
    return (GLubyte*)"not implemented";
}
