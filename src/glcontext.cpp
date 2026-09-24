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
int CGLContext::RasterFinishedSemaId             = -1;
int CGLContext::PresentationSemaId               = -1;

CGLContext::tRenderingFinishedCallback CGLContext::RenderingFinishedCallback = NULL;

// Submission is owned by the main thread; completion by the GS interrupt.
// Separate counters avoid a shared pending-bit read/modify/write race. They
// describe our end-of-chain SIGNALs, not mere VIF DMA idleness or CPU returns.
static volatile unsigned int NormalChainsSubmitted = 0, NormalChainsCompleted = 0;
static unsigned int NormalContextEpoch = 0;
static volatile unsigned int ImmediateChainsSubmitted = 0, ImmediateChainsCompleted = 0;
// Only normal chains emit FINISH. One normal chain may be outstanding, so a
// raster event cannot acknowledge an immediate upload or another frame.
static volatile unsigned int NormalFramesFinished = 0;

// Main publishes an immutable DISPFB pair; the IRQ publishes its sequence only
// after the MMIO writes. Semaphores wake waiters, never establish ownership.
static uint64_t PresentationFB1, PresentationFB2;
static bool PresentationHasBuffers;
static volatile unsigned int PresentationRequested = 0, PresentationCompleted = 0;
static volatile unsigned int PresentationIntervals = 0;
static volatile bool PresentationFieldIsEven;
static unsigned int PresentationNormalSequence;
static unsigned int PresentationCadence = 1;
static bool PresentationEnabled;
// A completion IRQ can finish the same blank's publication after VSINT was
// serviced. The request, hardware phase and short-lived Count witness prevent
// that permission surviving into another field or a newly queued frame.
static bool PresentationApertureOpen;
static unsigned int PresentationApertureRequest, PresentationApertureClock;
static unsigned int PresentationAperturePhase;
static bool PresentationApertureFieldIsEven;
// EE Timer1: count BUSCLK/256, reset at the VBLANK rising edge in hardware.
// The GS IRQ may itself be late, so a latched VSINT is not a phase test.
// The audited NTSC/PAL/480p timing model puts blanking at >=1.430 ms. Admit
// the first 1.0 ms (576 ticks), leaving >=0.430 ms for two privileged MMIO
// stores that bypass the GS drawing FIFO. The earlier 0.5 ms cutoff could
// discard a completed frame despite remaining blank time. GS VSINT itself
// arrives about 0.19-0.26 ms after the hardware edge. No added wait or delay.
// These mode timings come from hardware-tested PCSX2, not a Sony guarantee;
// do not extend this API to custom modes without reviewing their timing.
// Timing provenance/limits: PS2_SCREEN_TEARING_INVESTIGATION.md.
static const unsigned int PresentationTimerMode = 0x9e;
static const unsigned int PresentationEarlyBlankTicks = 288;
static const unsigned int PresentationBlankTicks = PGL_PRESENT_EXTENDED_BLANK ? 576 : 288;
static bool PresentationTimerOwned;
static unsigned int SavedPresentationTimerMode, SavedPresentationTimerCount;

static inline unsigned int ReadEeCycleCount()
{
    unsigned int cycles;
    // EE Core User's Manual p70: CP0 Count advances every CPU cycle. Reading
    // it dates the aperture; never reset it or use elapsed time as completion.
    asm volatile("mfc0 %0, $9" : "=r"(cycles) : : "memory");
    return cycles;
}

static unsigned int DrawEnvLastFrame, DrawEnvHighWater, DrawEnvOver100Frames;
static unsigned int DrawEnvGrowths, DrawEnvHeapBytes;
static void (*DrawEnvHeapObserver)(unsigned int, unsigned int);

#if PGL_FRAME_PHASE_METRICS
static volatile bool FramePhaseEnabled;
static PGLFramePhaseStats FramePhases[PGL_FRAME_PHASE_COUNT];
static PGLPresentationStats PresentationStats;
// Close each observed display interval at the next VSINT. Counting at its
// start would incorrectly classify a later same-blank completion as a repeat.
// Keep the open interval across reporting snapshots; neither path changes pacing.
static bool PresentationFieldObserved, PresentationFieldPublished;
static unsigned int PresentationRepeatHistory, PresentationRepeatCount;
static unsigned int PresentationHistoryFields;
#define PGL_PRESENT_COUNT(field) do { if (FramePhaseEnabled) ++PresentationStats.field; } while (0)

#if PGL_PRESENT_TIMELINE_METRICS
static const unsigned int PresentationTraceMaxCycles = 294912000u;
static PGLPresentationTimingStats PresentationTimingStats;
static unsigned int PresentationTimingReport;
#if PGL_PRESENT_PIPE_METRICS
static PGLPresentationPipelineSample PresentationPipelineWorst;
static PGLPresentationPipelineSample PresentationPipelineTaken;
#endif
#if PGL_PRESENT_SEND_METRICS
static volatile PGLPresentationSendSample PresentationSendWorst;
static PGLPresentationSendSample PresentationSendTaken;
#endif
static bool PreviousPresentationClockValid;
static unsigned int PreviousPresentationClock;
static struct {
    bool active, signalSeen, finishSeen, edgeSeen, badAge;
    unsigned int request, normal, queueClock, readyClock;
    unsigned int observedEdges;
    PGLPresentationTimingSample sample;
#if PGL_PRESENT_SEND_METRICS
    volatile unsigned int sendCycles, sendFlags;
#endif
#if PGL_PRESENT_PIPE_METRICS
    PGLPresentationPipelineSample pipeline;
#endif
} PresentationTiming;

static void ResetPresentationTimingRecord()
{
    if (FramePhaseEnabled && PresentationTiming.active)
        ++PresentationTimingStats.discarded;
    PresentationTiming.active = false;
    PreviousPresentationClockValid = false;
#if PGL_PRESENT_SEND_METRICS
    PresentationTiming.sendFlags = 0;
#endif
#if PGL_PRESENT_PIPE_METRICS
    PresentationTiming.pipeline.flags = 0;
#endif
}

static void ArmPresentationTiming(unsigned int request)
{
    PresentationTiming.active = false;
#if PGL_PRESENT_SEND_METRICS
    PresentationTiming.sendFlags = 0;
#endif
    // Bootstrap presents an already retired image; it is not a DMA sample.
    // Intentional multi-refresh pacing must not enter the missed cohort.
    if (!FramePhaseEnabled || !PresentationEnabled || PresentationIntervals != 1)
        return;
    PresentationTiming.signalSeen = PresentationTiming.finishSeen = false;
    PresentationTiming.edgeSeen = PresentationTiming.badAge = false;
    PresentationTiming.observedEdges = 0;
    PresentationTiming.request = request;
    PresentationTiming.normal = NormalChainsSubmitted;
    PresentationTiming.queueClock = ReadEeCycleCount();
    memset(&PresentationTiming.sample, 0, sizeof(PresentationTiming.sample));
#if PGL_PRESENT_PIPE_METRICS
    PresentationTiming.pipeline.flags = 0;
#endif
    PGLPresentationTimingSample& sample = PresentationTiming.sample;
    sample.sequence = NormalChainsSubmitted;
    sample.queuePhase = *R_EE_T1_COUNT;
    if (PreviousPresentationClockValid) {
        const unsigned int elapsed = PresentationTiming.queueClock - PreviousPresentationClock;
        if (elapsed < PresentationTraceMaxCycles) {
            sample.flags |= PGL_PRESENT_TRACE_HAS_POST;
            sample.postPresentToQueueCycles = elapsed;
        }
    }
    PresentationTiming.active = true;
}

#if PGL_PRESENT_SEND_METRICS
static void ObservePresentationSendReturn(unsigned int request,
    unsigned int sequence, unsigned int queueClock, unsigned int clock)
{
    const unsigned int elapsed = clock - queueClock;
    if (PresentationTiming.request != request || PresentationTiming.normal != sequence
        || elapsed >= PresentationTraceMaxCycles)
        return;
    // IRQ may already have completed this record. Publish pending data before
    // validity, then repair the matching retained worst if it copied invalid.
    // No next normal submission or timing Take can run until this main-thread
    // Send call returns, so a matching worst cannot be replaced underneath us.
    PresentationTiming.sendCycles = elapsed;
    asm volatile("" : : : "memory");
    PresentationTiming.sendFlags = PGL_PRESENT_SEND_VALID;
    asm volatile("" : : : "memory");
    if (PresentationTimingStats.worstValid
        && PresentationSendWorst.sequence == sequence) {
        PresentationSendWorst.queueToSendReturnCycles = elapsed;
        PresentationSendWorst.flags = PGL_PRESENT_SEND_VALID;
    }
}
#endif

static void ObservePresentationCompletion(bool raster, unsigned int clock)
{
    if (!FramePhaseEnabled || !PresentationTiming.active
        || PresentationTiming.request != PresentationRequested
        || PresentationTiming.normal != NormalChainsSubmitted)
        return;
    PGLPresentationTimingSample& sample = PresentationTiming.sample;
    if (raster) {
        if (PresentationTiming.finishSeen) return;
        PresentationTiming.finishSeen = true;
        sample.queueToFinishCycles = clock - PresentationTiming.queueClock;
    } else {
        if (PresentationTiming.signalSeen) return;
        PresentationTiming.signalSeen = true;
        sample.queueToSignalCycles = clock - PresentationTiming.queueClock;
    }
    if (PresentationTiming.signalSeen && PresentationTiming.finishSeen) {
        PresentationTiming.readyClock = clock;
        sample.queueToReadyCycles = clock - PresentationTiming.queueClock;
        sample.readyPhase = *R_EE_T1_COUNT;
    }
}

static void ObservePresentationEdge(unsigned int clock, unsigned int phase,
    unsigned int timerMode)
{
    if (!FramePhaseEnabled || !PresentationTiming.active
        || PresentationTiming.request != PresentationRequested)
        return;
    if (++PresentationTiming.observedEdges >= 64)
        PresentationTiming.badAge = true;
    if (PresentationTiming.edgeSeen) {
        // This same request survived its previous serviced opportunity.
        // Initial not-ready + later same-blank rescue does not reach here.
        ++PresentationTiming.sample.missedEdges;
        return;
    }
    PresentationTiming.edgeSeen = true;
    PGLPresentationTimingSample& sample = PresentationTiming.sample;
    sample.edgePhase = phase;
#if PGL_PRESENT_PIPE_METRICS
    if (!PresentationTiming.signalSeen || !PresentationTiming.finishSeen) {
        PGLPresentationPipelineSample& pipeline = PresentationTiming.pipeline;
        pipeline.sequence = sample.sequence;
        pipeline.flags = PGL_PRESENT_PIPE_VALID
            | (PresentationTiming.signalSeen ? PGL_PRESENT_PIPE_SIGNAL_SEEN : 0)
            | (PresentationTiming.finishSeen ? PGL_PRESENT_PIPE_FINISH_SEEN : 0);
        // EE manual pp21-23, 73-74, 124, 143-144, 161/164: word-readable
        // status, no read-clear. Never read VIF CODE/NUM or GIF TAG/CNT here.
        // These three sequential reads observe the first serviced edge only;
        // no waits, event acknowledgements or presentation decisions change.
        pipeline.vifStatus = *(volatile unsigned int*)0x10003c00;
        pipeline.gifStatus = *(volatile unsigned int*)0x10003020;
        pipeline.dmaControl = *(volatile unsigned int*)0x10009000;
    }
#endif
    const unsigned int age = clock - PresentationTiming.queueClock;
    if (age >= PresentationTraceMaxCycles) {
        PresentationTiming.badAge = true;
    } else if (timerMode == PresentationTimerMode && phase < PresentationBlankTicks) {
        sample.flags |= PGL_PRESENT_TRACE_HAS_BUDGET;
        sample.budgetCycles = age + (PresentationBlankTicks - phase) * 512u;
    }
}

static void RecordPresentationRejection(unsigned int mask)
{
    if (!FramePhaseEnabled) return;
    for (unsigned int bit = 0; bit < 5; ++bit) {
        if (mask & (1u << bit)) ++PresentationTimingStats.rejectReasons[bit];
    }
    if (PresentationTiming.active && PresentationTiming.request == PresentationRequested)
        PresentationTiming.sample.rejectMask |= mask;
}

static unsigned int PresentationTimingBin(unsigned int cycles)
{
    return cycles < 294912u ? 0u : cycles < 884736u ? 1u : 2u;
}

static void CompletePresentationTiming(unsigned int clock)
{
    if (!FramePhaseEnabled) return;
    PreviousPresentationClock = clock;
    PreviousPresentationClockValid = true;
    if (!PresentationTiming.active) return;
    const unsigned int total = clock - PresentationTiming.queueClock;
    const bool valid = PresentationTiming.request == PresentationRequested
        && PresentationTiming.normal == PresentationNormalSequence
        && PresentationTiming.signalSeen && PresentationTiming.finishSeen
        && PresentationTiming.edgeSeen && !PresentationTiming.badAge
        && total < PresentationTraceMaxCycles;
    PresentationTiming.active = false;
    if (!valid) {
        ++PresentationTimingStats.discarded;
        return;
    }
    PGLPresentationTimingSample& sample = PresentationTiming.sample;
    sample.readyToPresentCycles = clock - PresentationTiming.readyClock;
    const unsigned int groupIndex = sample.missedEdges != 0 ? 1u : 0u;
    PGLPresentationTimingGroup& group = PresentationTimingStats.groups[groupIndex];
    ++group.samples;
    group.queueToSignalCycles += sample.queueToSignalCycles;
    group.queueToFinishCycles += sample.queueToFinishCycles;
    group.queueToReadyCycles += sample.queueToReadyCycles;
    group.readyToPresentCycles += sample.readyToPresentCycles;
    if (sample.queueToReadyCycles > group.maxReadyCycles)
        group.maxReadyCycles = sample.queueToReadyCycles;
    if (sample.readyToPresentCycles > group.maxWaitCycles)
        group.maxWaitCycles = sample.readyToPresentCycles;
    if (sample.flags & PGL_PRESENT_TRACE_HAS_POST) {
        ++group.postPresentSamples;
        group.postPresentToQueueCycles += sample.postPresentToQueueCycles;
    }
    if (sample.flags & PGL_PRESENT_TRACE_HAS_BUDGET) {
        ++group.budgetSamples;
        group.budgetCycles += sample.budgetCycles;
    }
    if (groupIndex != 0) {
        if (sample.flags & PGL_PRESENT_TRACE_HAS_BUDGET) {
            ++PresentationTimingStats.missedJoint[3u * PresentationTimingBin(sample.budgetCycles)
                + PresentationTimingBin(sample.queueToReadyCycles)];
        } else {
            ++PresentationTimingStats.unknownBudget;
        }
        if (!PresentationTimingStats.worstValid
            || total > PresentationTimingStats.worst.queueToReadyCycles
                + PresentationTimingStats.worst.readyToPresentCycles) {
            PresentationTimingStats.worst = sample;
            PresentationTimingStats.worstValid = 1;
#if PGL_PRESENT_SEND_METRICS
            PresentationSendWorst.sequence = sample.sequence;
            PresentationSendWorst.queueToSendReturnCycles = PresentationTiming.sendCycles;
            PresentationSendWorst.flags = PresentationTiming.sendFlags;
#endif
#if PGL_PRESENT_PIPE_METRICS
            PresentationPipelineWorst = PresentationTiming.pipeline;
#endif
        }
    }
}
#endif

class CFramePhaseScope {
    unsigned int Start;
    const unsigned int Phase;
    const bool Enabled;
public:
    explicit CFramePhaseScope(unsigned int phase)
        : Start(0), Phase(phase), Enabled(FramePhaseEnabled)
    {
        if (Enabled) Start = ReadEeCycleCount();
    }
    ~CFramePhaseScope()
    {
        if (!Enabled) return;
        const unsigned int elapsed = ReadEeCycleCount() - Start;
        PGLFramePhaseStats& stats = FramePhases[Phase];
        stats.cycles += elapsed;
        ++stats.calls;
        if (elapsed > stats.maxCycles) stats.maxCycles = elapsed;
    }
};
#define PGL_FRAME_SCOPE(phase) CFramePhaseScope phaseScope(phase)
#else
#define PGL_FRAME_SCOPE(phase) ((void)0)
#define PGL_PRESENT_COUNT(field) ((void)0)
#endif

extern "C" GLboolean pglSetFramePhaseMetrics(GLboolean enabled)
{
#if PGL_FRAME_PHASE_METRICS
    const int interruptsEnabled = DIntr();
    memset(FramePhases, 0, sizeof(FramePhases));
    memset(&PresentationStats, 0, sizeof(PresentationStats));
    PresentationFieldObserved = PresentationFieldPublished = false;
    PresentationRepeatHistory = PresentationRepeatCount = PresentationHistoryFields = 0;
#if PGL_PRESENT_TIMELINE_METRICS
    ResetPresentationTimingRecord();
    memset(&PresentationTimingStats, 0, sizeof(PresentationTimingStats));
#if PGL_PRESENT_PIPE_METRICS
    PresentationPipelineWorst.flags = 0;
    PresentationPipelineTaken.flags = 0;
#endif
#if PGL_PRESENT_SEND_METRICS
    PresentationSendWorst.flags = 0;
    PresentationSendTaken.flags = 0;
#endif
#endif
    FramePhaseEnabled = enabled != GL_FALSE;
    if (interruptsEnabled) EIntr();
    return enabled != GL_FALSE ? GL_TRUE : GL_FALSE;
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

extern "C" GLboolean pglTakePresentationMetrics(PGLPresentationStats* stats)
{
    if (!stats) return GL_FALSE;
#if PGL_FRAME_PHASE_METRICS
    const int interruptsEnabled = DIntr();
    memcpy(stats, &PresentationStats, sizeof(*stats));
    stats->blankTicks = PresentationBlankTicks;
    memset(&PresentationStats, 0, sizeof(PresentationStats));
    const bool enabled = FramePhaseEnabled;
    if (interruptsEnabled) EIntr();
    return enabled ? GL_TRUE : GL_FALSE;
#else
    memset(stats, 0, sizeof(*stats));
    return GL_FALSE;
#endif
}

extern "C" GLboolean pglTakePresentationTimingMetrics(PGLPresentationTimingStats* stats)
{
    if (!stats) return GL_FALSE;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
    const int interruptsEnabled = DIntr();
    memcpy(stats, &PresentationTimingStats, sizeof(*stats));
    stats->report = ++PresentationTimingReport;
#if PGL_PRESENT_PIPE_METRICS
    PresentationPipelineTaken.flags = 0;
    if (stats->worstValid && (PresentationPipelineWorst.flags & PGL_PRESENT_PIPE_VALID)) {
        PresentationPipelineTaken = PresentationPipelineWorst;
        PresentationPipelineTaken.report = stats->report;
    }
    PresentationPipelineWorst.flags = 0;
#endif
#if PGL_PRESENT_SEND_METRICS
    PresentationSendTaken.flags = 0;
    if (stats->worstValid && (PresentationSendWorst.flags & PGL_PRESENT_SEND_VALID)
        && PresentationSendWorst.sequence == stats->worst.sequence) {
        PresentationSendTaken.sequence = PresentationSendWorst.sequence;
        PresentationSendTaken.flags = PresentationSendWorst.flags;
        PresentationSendTaken.queueToSendReturnCycles = PresentationSendWorst.queueToSendReturnCycles;
        PresentationSendTaken.report = stats->report;
    }
    PresentationSendWorst.flags = 0;
#endif
    memset(&PresentationTimingStats, 0, sizeof(PresentationTimingStats));
    const bool enabled = FramePhaseEnabled;
    if (interruptsEnabled) EIntr();
    return enabled ? GL_TRUE : GL_FALSE;
#else
    memset(stats, 0, sizeof(*stats));
    return GL_FALSE;
#endif
}

extern "C" unsigned int pglGetPresentationPipelineOptions(void)
{
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_PIPE_METRICS
    return 1u;
#else
    return 0u;
#endif
}

extern "C" GLboolean pglTakePresentationPipelineMetrics(unsigned int report,
    unsigned int sequence, PGLPresentationPipelineSample* sample)
{
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_PIPE_METRICS
    // Only the main-thread timing Take/reset writes this sealed copy. IRQs
    // continue collecting into their separate pending/worst records.
    if (!sample || !(PresentationPipelineTaken.flags & PGL_PRESENT_PIPE_VALID)
        || PresentationPipelineTaken.report != report
        || PresentationPipelineTaken.sequence != sequence)
        return GL_FALSE;
    *sample = PresentationPipelineTaken;
    PresentationPipelineTaken.flags = 0;
    return GL_TRUE;
#else
    (void)report;
    (void)sequence;
    (void)sample;
    return GL_FALSE;
#endif
}

extern "C" unsigned int pglGetPresentationSendOptions(void)
{
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_SEND_METRICS
    return 1u;
#else
    return 0u;
#endif
}

extern "C" GLboolean pglTakePresentationSendMetrics(unsigned int report,
    unsigned int sequence, PGLPresentationSendSample* sample)
{
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_SEND_METRICS
    // IRQ never writes the copy sealed by the existing timing Take exclusion.
    if (!sample || !(PresentationSendTaken.flags & PGL_PRESENT_SEND_VALID)
        || PresentationSendTaken.report != report
        || PresentationSendTaken.sequence != sequence)
        return GL_FALSE;
    *sample = PresentationSendTaken;
    PresentationSendTaken.flags = 0;
    return GL_TRUE;
#else
    (void)report;
    (void)sequence;
    (void)sample;
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
    if (++NormalContextEpoch == 0) ++NormalContextEpoch;
    ImmediateChainsSubmitted = ImmediateChainsCompleted = 0;
    NormalFramesFinished = 0;
    PresentationRequested = PresentationCompleted = 0;
    PresentationIntervals = 0;
    PresentationNormalSequence = 0;
    PresentationCadence = 1;
    PresentationEnabled = false;
    PresentationApertureOpen = false;
    PresentationTimerOwned = false;
#if PGL_FRAME_PHASE_METRICS
    FramePhaseEnabled = false;
    memset(FramePhases, 0, sizeof(FramePhases));
    memset(&PresentationStats, 0, sizeof(PresentationStats));
    PresentationFieldObserved = PresentationFieldPublished = false;
    PresentationRepeatHistory = PresentationRepeatCount = PresentationHistoryFields = 0;
#if PGL_PRESENT_TIMELINE_METRICS
    ResetPresentationTimingRecord();
    memset(&PresentationTimingStats, 0, sizeof(PresentationTimingStats));
    PresentationTimingReport = 0;
#if PGL_PRESENT_SEND_METRICS
    PresentationSendWorst.flags = 0;
    PresentationSendTaken.flags = 0;
#endif
#if PGL_PRESENT_PIPE_METRICS
    PresentationPipelineWorst.flags = 0;
    PresentationPipelineTaken.flags = 0;
#endif
#endif
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
    RasterFinishedSemaId             = CreateSema(&newSemaphore);
    PresentationSemaId               = CreateSema(&newSemaphore);
    mErrorIf(VsyncSemaId == -1
            || RenderingFinishedSemaId == -1
            || ImmediateRenderingFinishedSemaId == -1
            || RasterFinishedSemaId == -1 || PresentationSemaId == -1,
        "Failed to create ps2gl semaphores.");

    // add an interrupt handler for gs "signal" exceptions

    AddIntcHandler(INTC_GS, CGLContext::GsIntHandler, 0 /*first handler*/);
    EnableIntc(INTC_GS);
    // Clear and enable SIGNAL, FINISH and VSYNC events. FINISH is observed
    // independently: a following SIGNAL does not wait for raster completion.
    *(volatile unsigned int*)GS::ControlRegs::csr = 11;
    *(volatile unsigned int*)GS::ControlRegs::imr = 0x7400;

    // Mask bugged tag mismatch error
    WR_EE_VIF1_ERR(2);
}

CGLContext::~CGLContext()
{
    WaitForPresentation();
    if (PresentationTimerOwned) {
        *R_EE_T1_MODE = 0;
        *R_EE_T1_COUNT = SavedPresentationTimerCount;
        *R_EE_T1_MODE = SavedPresentationTimerMode;
        PresentationTimerOwned = false;
    }
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

    tGifTag giftag = {};
    giftag.NLOOP = signalNum == 1 ? 2 : 1;
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
        if (signalNum == 1) {
            *Vif1Packet += (uint64_t)0;
            *Vif1Packet += (uint64_t)0x61; // FINISH: raster event, not a command stall
        }
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
    while (PollSema(RasterFinishedSemaId) != -1)
        ;

    // The previous frame must have retired before this untagged FINISH event
    // is reassigned. The game normally already waited in the presentation path.
    if (NormalChainsCompleted != NormalChainsSubmitted
        || NormalFramesFinished != NormalChainsSubmitted
        || PresentationCompleted != PresentationRequested) {
        fputs("ps2gl: normal frame submitted before completion\n", stderr);
        abort();
    }
    *(volatile unsigned int*)GS::ControlRegs::csr = 2;
    ++NormalChainsSubmitted;
    // Arm the frame that is ABOUT TO render, before returning to next-frame
    // EE preparation. Waiting until EndDrawing would miss a perfectly usable
    // vblank whenever that preparation crosses its boundary.
    if (PresentationEnabled)
        QueuePresentation(PresentationCadence);
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_SEND_METRICS
    const bool captureSend = FramePhaseEnabled && PresentationTiming.active;
    const unsigned int sendRequest = PresentationRequested;
    const unsigned int sendSequence = NormalChainsSubmitted;
    const unsigned int sendQueueClock = PresentationTiming.queueClock;
#endif
    LastPacket->Send();
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS && PGL_PRESENT_SEND_METRICS
    if (captureSend)
        ObservePresentationSendReturn(sendRequest, sendSequence, sendQueueClock, ReadEeCycleCount());
#endif
}

void CGLContext::TryPresent(bool atVsync)
{
    if (PresentationCompleted == PresentationRequested
        || !PresentationApertureOpen
        || PresentationApertureRequest != PresentationRequested
        || PresentationIntervals != 0
        || NormalChainsCompleted != PresentationNormalSequence
        || NormalFramesFinished != PresentationNormalSequence)
        return;

    // Timer1 resets before the GS FIELD/VSINT transition. A saved permit plus
    // a low timer value alone could therefore mistake the NEXT blank for this
    // one. The Count deadline and nondecreasing hardware phase both must hold.
    // One BUSCLK/256 tick is 512 EE cycles. The deadline merely expires a
    // phase permit; owned SIGNAL and FINISH above establish raster readiness.
    const unsigned int clock = ReadEeCycleCount();
    const unsigned int elapsed = clock - PresentationApertureClock;
    const unsigned int phase = *R_EE_T1_COUNT;
    const uint32_t currentCsr = *(volatile uint32_t*)GS::ControlRegs::csr;
    const bool fieldIsEven = (currentCsr & (1u << 13)) != 0;
    const unsigned int timerMode = *R_EE_T1_MODE & 0x3ff;
    if (timerMode != PresentationTimerMode
        || phase < PresentationAperturePhase || phase >= PresentationBlankTicks
        || elapsed >= (PresentationBlankTicks - PresentationAperturePhase) * 512u
        || fieldIsEven != PresentationApertureFieldIsEven) {
        PGL_PRESENT_COUNT(expiredReady);
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
        if (FramePhaseEnabled) {
            const unsigned int reasons = (timerMode != PresentationTimerMode ? 1u : 0u)
                | (phase < PresentationAperturePhase ? 2u : 0u)
                | (phase >= PresentationBlankTicks ? 4u : 0u)
                | (elapsed >= (PresentationBlankTicks - PresentationAperturePhase) * 512u ? 8u : 0u)
                | (fieldIsEven != PresentationApertureFieldIsEven ? 16u : 0u);
            RecordPresentationRejection(reasons);
        }
#endif
        PresentationApertureOpen = false;
        return;
    }

    asm volatile("" ::: "memory");
    if (PresentationHasBuffers) {
        *(volatile uint64_t*)GS::ControlRegs::dispfb1 = PresentationFB1;
        *(volatile uint64_t*)GS::ControlRegs::dispfb2 = PresentationFB2;
    }
    PresentationFieldIsEven = fieldIsEven;
    PresentationApertureOpen = false;
    PGL_PRESENT_COUNT(presented);
    if (phase >= PresentationEarlyBlankTicks)
        PGL_PRESENT_COUNT(extendedBlank);
#if PGL_FRAME_PHASE_METRICS
    if (FramePhaseEnabled) PresentationFieldPublished = true;
#endif
    if (atVsync)
        PGL_PRESENT_COUNT(atVsync);
    else
        PGL_PRESENT_COUNT(afterCompletion);
    asm volatile("sync.l" ::: "memory");
    PresentationCompleted = PresentationRequested;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
    CompletePresentationTiming(clock);
#endif
    iSignalSema(PresentationSemaId);
}

int CGLContext::GsIntHandler(int cause)
{
    // Drain EVERY unmasked event (SIGNAL bit 0 | FINISH bit 1 | VSYNC bit 3).
    // FINISH and SIGNAL can arrive in either order; neither may consume the
    // other's acknowledgement. Keep the existing drain-until-clean invariant.
    // The GS holds its INTC line asserted while any unmasked event is set.
    // Original simultaneous-event failure mechanism: the EE INTC latches EDGES only —
    // returning with one still pending means the line never drops, no edge is
    // ever latched again, GS interrupts die, and the next WaitSema on this
    // path sleeps forever (the intermittent EE hard freeze on chain-heavy
    // frames, whose end-of-chain SIGNAL drifts across vblank). Servicing only
    // the bits seen in ONE read at entry is not enough: an event that sets
    // DURING the handler (after the read) also keeps the line high — so loop
    // until CSR reads clean, which proves the line is low and the next event
    // makes a fresh edge.
    uint32_t csr;
    while ((csr = *(volatile uint32_t*)GS::ControlRegs::csr) & 11) {
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
        // One observation time for both bits if they were latched together;
        // the order of the handler's branches is not GS execution order.
        const unsigned int completionClock = FramePhaseEnabled
            && PresentationTiming.active && (csr & 3) ? ReadEeCycleCount() : 0;
#endif
        if (csr & 2) {
            *(volatile unsigned int*)GS::ControlRegs::csr = 2;
            if (NormalFramesFinished != NormalChainsSubmitted) {
                NormalFramesFinished = NormalChainsSubmitted;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
                ObservePresentationCompletion(true, completionClock);
#endif
                iSignalSema(RasterFinishedSemaId);
            }
        }
        // signal interrupt?
        if (csr & 1) {
            // is it one of ours?
            uint64_t sigLblId = *(volatile uint64_t*)GS::ControlRegs::siglblid;
            if ((uint16_t)(sigLblId >> 16) == GetPs2glSignalId()) {
                switch (sigLblId & 0xffff) {
                case 1:
                    ++NormalChainsCompleted;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
                    ObservePresentationCompletion(false, completionClock);
#endif
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
#if PGL_FRAME_PHASE_METRICS
            if (FramePhaseEnabled && PresentationEnabled) {
                if (PresentationFieldObserved) {
                    ++PresentationStats.displayFields;
                    const unsigned int repeated = PresentationFieldPublished ? 0u : 1u;
                    PresentationStats.repeatedFields += repeated;
                    // A bounded observed-cadence history exposes clustered
                    // misses without logging every IRQ or changing pacing.
                    PresentationRepeatCount -= (PresentationRepeatHistory >> 29) & 1u;
                    PresentationRepeatHistory = ((PresentationRepeatHistory << 1) | repeated)
                        & 0x3fffffffu;
                    PresentationRepeatCount += repeated;
                    if (PresentationHistoryFields < 30)
                        ++PresentationHistoryFields;
                    if (PresentationHistoryFields == 30) {
                        ++PresentationStats.windows30;
                        if (PresentationRepeatCount > PresentationStats.worstRepeat30)
                            PresentationStats.worstRepeat30 = PresentationRepeatCount;
                    }
                }
                PresentationFieldObserved = true;
                PresentationFieldPublished = false;
                if (PresentationCompleted == PresentationRequested)
                    ++PresentationStats.emptyQueueEdges;
            }
#endif
            PresentationApertureOpen = false;
            if (PresentationCompleted != PresentationRequested) {
                // Date the same-blank permit before sampling its remaining
                // budget, so setup work cannot extend the admission deadline.
                const unsigned int apertureClock = ReadEeCycleCount();
                const unsigned int phase = *R_EE_T1_COUNT;
                const unsigned int timerMode = *R_EE_T1_MODE & 0x3ff;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
                ObservePresentationEdge(apertureClock, phase, timerMode);
#endif
                if (timerMode == PresentationTimerMode
                    && phase < PresentationBlankTicks) {
                    if (PresentationIntervals != 0)
                        --PresentationIntervals;
                    PresentationApertureRequest = PresentationRequested;
                    PresentationApertureClock = apertureClock;
                    PresentationAperturePhase = phase;
                    const uint32_t currentCsr = *(volatile uint32_t*)GS::ControlRegs::csr;
                    PresentationApertureFieldIsEven = (currentCsr & (1u << 13)) != 0;
                    PresentationApertureOpen = true;
                    if (PresentationIntervals == 0
                        && (NormalChainsCompleted != PresentationNormalSequence
                            || NormalFramesFinished != PresentationNormalSequence))
                        PGL_PRESENT_COUNT(notReadyEdges);
                } else {
                    PGL_PRESENT_COUNT(lateEdges);
                }
            }
            iSignalSema(VsyncSemaId);
            // clear the exception and wait for the next
            *(volatile unsigned int*)GS::ControlRegs::csr = 8;
        }
        // A FINISH/SIGNAL that arrives just after VSINT can still use that
        // admitted blank. Never demand a second refresh solely for IRQ order.
        TryPresent((csr & 8) != 0);
    }

    ExitHandler();

    return 0;
}

void CGLContext::FinishRenderingGeometry(bool forceImmediateStop)
{
    PGL_FRAME_SCOPE(PGL_FRAME_FINISH);
    //printf("%s(%d)\n", __FUNCTION__, forceImmediateStop);

    mWarnIf(forceImmediateStop, "Interrupting currently rendering dma chain not supported yet");
    const unsigned int submitted = NormalChainsSubmitted;
    while (NormalChainsCompleted != submitted)
        WaitSema(RenderingFinishedSemaId);
    while (NormalFramesFinished != submitted)
        WaitSema(RasterFinishedSemaId);
}

void CGLContext::WaitForVSync()
{
    PGL_FRAME_SCOPE(PGL_FRAME_VSYNC);
    //printf("%s\n", __FUNCTION__);

    // Standalone wait (e.g. a layout switch), not permission for a later
    // thread-side flip. Presentation uses the atomic wait+publish path below.
    const int interruptsEnabled = DIntr();
    PresentationApertureOpen = false;
    *(volatile unsigned int*)GS::ControlRegs::csr = 8;
    while (PollSema(VsyncSemaId) != -1)
        ;
    if (interruptsEnabled) EIntr();
    WaitSema(VsyncSemaId);
    // sceGsSyncV(0);
    uint32_t csr       = *(volatile uint32_t*)GS::ControlRegs::csr;
    IsCurrentFieldEven = (bool)((csr >> 13) & 1);
}

void CGLContext::QueuePresentation(unsigned int intervals)
{
    if (PresentationCompleted != PresentationRequested) {
        fputs("ps2gl: overwriting an outstanding presentation\n", stderr);
        abort();
    }
    while (PollSema(PresentationSemaId) != -1)
        ;
    PresentationHasBuffers = GetDisplayContext().PrepareBufferSwap(
        &PresentationFB1, &PresentationFB2);
    const unsigned int request = PresentationRequested + 1;
    const int interruptsEnabled = DIntr();
    if (!PresentationTimerOwned) {
        // The game uses SDK Timer2; legacy ps2gl examples may use Timer1.
        // Reserve it only for this opt-in API and reject an active owner.
        SavedPresentationTimerMode = *R_EE_T1_MODE & 0x3ff;
        SavedPresentationTimerCount = *R_EE_T1_COUNT;
        if (SavedPresentationTimerMode & 0x80) {
            if (interruptsEnabled) EIntr();
            fputs("ps2gl: synchronized presentation requires free EE Timer1\n", stderr);
            abort();
        }
        *R_EE_T1_MODE = 0;
        *R_EE_T1_COUNT = 0xffff;
        *R_EE_T1_MODE = PresentationTimerMode | 0xc00; // clear old event flags
        PresentationTimerOwned = true;
    }
    // A new submission needs a future edge, not stale credit. This happens at
    // DMA submission, NOT after the CPU has spent a frame preparing more work.
    PresentationApertureOpen = false;
    *(volatile unsigned int*)GS::ControlRegs::csr = 8;
    PresentationNormalSequence = NormalChainsSubmitted;
    PresentationIntervals = intervals ? intervals : 1;
    asm volatile("sync.l" ::: "memory");
    PresentationRequested = request;
#if PGL_FRAME_PHASE_METRICS && PGL_PRESENT_TIMELINE_METRICS
    ArmPresentationTiming(request);
#endif
    PGL_PRESENT_COUNT(queued);
    if (interruptsEnabled) EIntr();
    // FlushCache/Send and all packet preparation run with normal IRQ delivery.
}

void CGLContext::WaitForPresentation()
{
    PGL_FRAME_SCOPE(PGL_FRAME_VSYNC);
    const unsigned int request = PresentationRequested;
    if (PresentationCompleted == request)
        PGL_PRESENT_COUNT(joinReady);
    else
        PGL_PRESENT_COUNT(joinWait);
    while (PresentationCompleted != request)
        WaitSema(PresentationSemaId);
}

void CGLContext::ResetPresentation()
{
    WaitForPresentation();
    // Replacing the display buffers establishes a new initial front. The next
    // swap must bootstrap that pair, not consume an old layout's completed ack.
    const int interruptsEnabled = DIntr();
    PresentationApertureOpen = false;
    PresentationEnabled = false;
#if PGL_FRAME_PHASE_METRICS
    PresentationFieldObserved = PresentationFieldPublished = false;
    PresentationRepeatHistory = PresentationRepeatCount = PresentationHistoryFields = 0;
#if PGL_PRESENT_TIMELINE_METRICS
    ResetPresentationTimingRecord();
#endif
#endif
    if (interruptsEnabled) EIntr();
}

void CGLContext::SwapBuffersOnVSync(unsigned int intervals)
{
    if (NormalChainsCompleted != NormalChainsSubmitted
        || NormalFramesFinished != NormalChainsSubmitted)
        FinishRenderingGeometry(false);
    PresentationCadence = intervals ? intervals : 1;
    if (!PresentationEnabled) {
        QueuePresentation(PresentationCadence);
        PresentationEnabled = true;
    }
    // Normally the ISR has already displayed this frame during EE preparation.
    // Consume that exact acknowledgement; do not request a second vblank.
    WaitForPresentation();
    IsCurrentFieldEven = PresentationFieldIsEven;
    // Only CPU bookkeeping follows acknowledgement. No second display write,
    // and no packet/heap/manager work runs in interrupt context.
    SwapBuffers(true);
}

void CGLContext::SwapBuffers(bool displayAlreadyPresented)
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
    GetDisplayContext().SwapBuffers(displayAlreadyPresented);
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
    printf("[ CANARY ] Welcome to MODIFIED LOCAL ps2gl! [2026.09.24 09:05]\n");
    printf("[PS2-PACKETS] normal=cached\n");
    printf("[PS2-STACK] lazy-inverse=%d aligned-xfer=%d direct-tags=%d\n",
        1, 1,
        1);

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

void pglSwapBuffersOnVSync(unsigned int intervals)
{
    mErrorIf(pGLContext == NULL, "You need to call pglInit()");
    pGLContext->SwapBuffersOnVSync(intervals);
}

void pglWaitForPresentation(void)
{
    mErrorIf(pGLContext == NULL, "You need to call pglInit()");
    pGLContext->WaitForPresentation();
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

void pglGetNextNormalSubmission(unsigned int* sequence, unsigned int* contextEpoch)
{
    *sequence = NormalChainsSubmitted + 1;
    *contextEpoch = NormalContextEpoch;
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
