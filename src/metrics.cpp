//
// ps2gl metrics module
//
//  Written by Stefan Boberg
//

#include "ps2gl/metrics.h"
#include "ps2gl/glcontext.h"
#include <string.h>

Metric_t g_Metrics[kMetricsCount];

void pglResetMetrics()
{
    memset(g_Metrics, 0, sizeof(g_Metrics));
}

#if PGL_SUBMISSION_METRICS
bool g_PglSubmissionSampleActive;
unsigned int g_PglSubmissionSample[PGL_SUBMIT_COUNT];
static unsigned int submissionMetricStart[2];
static unsigned int submissionPacketStart;
static const CVifSCDmaPacket* submissionPacket;
static const MetricsEnum submissionMetricKeys[2] = {
    kMetricsTextureUploadCount, kMetricsClutUploadCount
};
#endif

extern "C" GLboolean pglBeginSubmissionSample(void)
{
#if PGL_SUBMISSION_METRICS
    if (!pGLContext || g_PglSubmissionSampleActive) return GL_FALSE;
    memset(g_PglSubmissionSample, 0, sizeof(g_PglSubmissionSample));
    for (int i = 0; i < 2; ++i)
        submissionMetricStart[i] = (unsigned int)pglGetMetric(submissionMetricKeys[i]);
    submissionPacketStart = pGLContext->GetVif1Packet().GetByteLength();
    submissionPacket = &pGLContext->GetVif1Packet();
    g_PglSubmissionSampleActive = true;
    return GL_TRUE;
#else
    return GL_FALSE;
#endif
}

extern "C" GLboolean pglReadSubmissionSample(unsigned int values[PGL_SUBMIT_COUNT])
{
    if (!values) return GL_FALSE;
#if PGL_SUBMISSION_METRICS
    if (g_PglSubmissionSampleActive && pGLContext
        && submissionPacket == &pGLContext->GetVif1Packet()
        && submissionPacket->GetByteLength() >= submissionPacketStart) {
        memcpy(values, g_PglSubmissionSample, sizeof(g_PglSubmissionSample));
        for (int i = 0; i < 2; ++i)
            values[PGL_SUBMIT_TEXTURE_UPLOADS + i] =
                (unsigned int)pglGetMetric(submissionMetricKeys[i]) - submissionMetricStart[i];
        values[PGL_SUBMIT_PACKET_BYTES] =
            pGLContext->GetVif1Packet().GetByteLength() - submissionPacketStart;
        return GL_TRUE;
    }
#endif
    memset(values, 0, PGL_SUBMIT_COUNT * sizeof(*values));
    return GL_FALSE;
}

extern "C" void pglEndSubmissionSample(void)
{
#if PGL_SUBMISSION_METRICS
    g_PglSubmissionSampleActive = false;
#endif
}
