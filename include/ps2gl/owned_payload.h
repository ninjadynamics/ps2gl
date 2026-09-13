/* Exact owned compact payload copies. Keep source-chain bounds/TTE handling in
 * CSCDmaPacket::Add; this does not borrow application storage or publish refs.
 */
#ifndef PS2GL_OWNED_PAYLOAD_H
#define PS2GL_OWNED_PAYLOAD_H

#include "GL/ps2gl.h"
#include "ps2s/packet.h"
#include <stdint.h>

/* A struct retains may_alias through Add's template deduction. A typedef of
 * uint128_t can lose its alias attribute when reduced to the canonical type.
 * No float arithmetic/conversion is allowed while copying descriptor bits.
 */
struct __attribute__((aligned(16), may_alias)) PglOwnedQword {
    uint128_t bits;
};
typedef char PglOwnedQwordSize[sizeof(PglOwnedQword) == 16u ? 1 : -1];

static inline float* pglAddOwnedPayload(CVifSCDmaPacket& packet,
    const float* source, unsigned int floatCount)
{
#if PGL_COMPACT_QWORD_COPY
    if ((((uintptr_t)source | (uintptr_t)packet.GetNextPtr()) & 15u) == 0u
        && (floatCount & 3u) == 0u) {
        return reinterpret_cast<float*>(packet.Add(
            reinterpret_cast<const PglOwnedQword*>(source), floatCount >> 2));
    }
#endif
    // Public APIs require only float alignment; keep that contract intact.
    return packet.Add(source, floatCount);
}

#endif
