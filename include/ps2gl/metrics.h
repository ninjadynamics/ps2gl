//
// ps2gl Metrics Module definitions
//
//  Original author: Stefan Boberg (boberg@team17.com)
//

#ifndef PS2GL_METRICS_H
#define PS2GL_METRICS_H
#include "GL/ps2gl.h"

#define PS2GL_METRICS_ENABLE 1

enum MetricsEnum {
    /// Number of textures uploaded to the GS
    kMetricsTextureUploadCount,

    /// Number of bytes of texture data uploaded to the GS
    kMetricsTextureUploadBytes,

    /// Number of CLUTs uploaded to the GS
    kMetricsClutUploadCount,

    /// Number of VU1 renderer code uploads
    kMetricsRendererUpload,

    /// Number of texture binds (glBindTexture())
    kMetricsBindTexture,

    /// Total number of metrics quantities
    kMetricsCount,
};

typedef unsigned long long Metric_t; // 64-bit integer

extern Metric_t g_Metrics[kMetricsCount];

/** Reset all metric values to zero
  */
extern void pglResetMetrics();

/** Get value of specified metric
  */
inline Metric_t pglGetMetric(MetricsEnum eMetric)
{
    return g_Metrics[eMetric];
}

/** Reset specified metric
  */
inline void pglResetMetric(MetricsEnum eMetric)
{
    g_Metrics[eMetric] = 0;
}

/** Increase metric value by specified amount
  */
inline void pglAddToMetric(MetricsEnum eMetric, Metric_t Value = 1)
{
#if PS2GL_METRICS_ENABLE
    g_Metrics[eMetric] += Value;
#endif
}

#if PGL_SUBMISSION_METRICS
extern bool g_PglSubmissionSampleActive;
extern unsigned int g_PglSubmissionSample[PGL_SUBMIT_COUNT];
#endif

inline void pglCountSubmission(unsigned int metric, unsigned int amount = 1)
{
#if PGL_SUBMISSION_METRICS
    if (g_PglSubmissionSampleActive) g_PglSubmissionSample[metric] += amount;
#else
    (void)metric;
    (void)amount;
#endif
}
#endif
