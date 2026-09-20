/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_renderermanager_h
#define ps2gl_renderermanager_h

// PLIN
// #include "eestruct.h"

#include "ps2s/vif.h"

#include "GL/gl.h"
#include "GL/ps2gl.h"

#include "ps2gl/base_renderer.h"
#include "ps2gl/renderer.h"

/********************************************
 * CRendererManager
 */

class CGLContext;
class CImmGeomManager;
class CVifSCDmaPacket;
class CGeometryBlock;
class CRenderer;
class CUnlitTexTriRenderer;
class CClipTriX2Renderer;
class CClipQuadX2FRenderer;
class CClipRoadX2RRenderer;
class CClipPoolX2PRenderer;
class CClipBillboardX2BRenderer;
class CClipBillboardAlphaX2ARenderer;
class CClipDecalX2ERenderer;

typedef enum { kDirectional,
    kPoint,
    kSpot } tLightType;

typedef struct {
    uint64_t capabilities;
    uint64_t requirements;
    CRenderer* renderer;
    bool preservesX2Base;
} tRenderer;

class CRendererManager {
    CGLContext& GLContext;

    CRendererProps RendererRequirements;
    bool RendererReqsHaveChanged;
    uint64_t CurUserPrimReqs, CurUserPrimReqMask;

    static const int kMaxDefaultRenderers = 64;
    static const int kMaxUserRenderers    = PGL_MAX_CUSTOM_RENDERERS;
    tRenderer DefaultRenderers[kMaxDefaultRenderers];
    tRenderer UserRenderers[kMaxUserRenderers];
    int NumDefaultRenderers, NumUserRenderers;
    const tRenderer *CurrentRenderer, *NewRenderer;
    bool ColoredHudRendererRegistered;
    bool WallQuadRendererRegistered;
    bool WallColorRendererRegistered;
    CClipRoadX2RRenderer* RoadRenderer;
    CClipPoolX2PRenderer* PoolRenderer;
    CClipBillboardX2BRenderer* BillboardRenderer;
    CClipBillboardAlphaX2ARenderer* BillboardAlphaRenderer;
    CClipDecalX2ERenderer* DecalRenderer;
    CClipQuadX2FRenderer* SourceQuadRenderer;

    void RegisterDefaultRenderer(CRenderer* renderer);

public:
    CRendererManager(CGLContext& context);

    void RegisterUserRenderer(CRenderer* renderer);
    void RegisterUnlitTexTriRenderer(CUnlitTexTriRenderer* renderer);
    void RegisterX2Renderer(CClipTriX2Renderer* renderer);
    void RegisterSourceQuadRenderer(CClipQuadX2FRenderer* renderer);
    CClipQuadX2FRenderer* GetSourceQuadRenderer() const { return SourceQuadRenderer; }
    bool CanSelectWallDescriptorRenderer(bool pairedColors) const
    {
        return (pairedColors ? WallColorRendererRegistered : WallQuadRendererRegistered)
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    void RegisterRoadRenderer(CClipRoadX2RRenderer* renderer);
    CClipRoadX2RRenderer* GetRoadRenderer() const { return RoadRenderer; }
    bool CanSelectRoadRenderer() const
    {
        return RoadRenderer != NULL
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    void RegisterPoolRenderer(CClipPoolX2PRenderer* renderer);
    CClipPoolX2PRenderer* GetPoolRenderer() const { return PoolRenderer; }
    bool CanSelectPoolRenderer() const
    {
        return PoolRenderer != NULL
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    void RegisterDecalRenderer(CClipDecalX2ERenderer* renderer);
    void RegisterBillboardRenderer(CClipBillboardX2BRenderer* renderer);
    void RegisterBillboardAlphaRenderer(CClipBillboardAlphaX2ARenderer* renderer);
    CClipBillboardAlphaX2ARenderer* GetBillboardAlphaRenderer() const { return BillboardAlphaRenderer; }
    bool CanSelectBillboardAlphaRenderer() const
    {
        return BillboardAlphaRenderer != NULL
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    CClipBillboardX2BRenderer* GetBillboardRenderer() const { return BillboardRenderer; }
    bool CanSelectBillboardRenderer() const
    {
        return BillboardRenderer != NULL
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    CClipDecalX2ERenderer* GetDecalRenderer() const { return DecalRenderer; }
    bool CanSelectDecalRenderer() const
    {
        return DecalRenderer != NULL
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }
    bool CanSelectColoredHudRenderer() const
    {
        // PrimChanged removes the previous primitive's requirements before
        // adding the new ones. Other custom state must stay out of this path.
        return ColoredHudRendererRegistered
            && (((uint64_t)RendererRequirements & ~CurUserPrimReqs)
                & ~(uint64_t)0xffffffff) == 0;
    }

    bool UpdateNewRenderer();
    void MakeNewRendererCurrent();
    void LoadRenderer(CVifSCDmaPacket& packet);

    CRenderer& GetCurRenderer() { return *(CurrentRenderer->renderer); }
    CRendererProps GetRendererReqs() const { return RendererRequirements; }

    bool IsCurRendererCustom() const { return ((uint32_t)CurrentRenderer >= (uint32_t)UserRenderers); }

    // No prediction or selection side effects: a pending change, custom
    // owner or different primitive retains the caller's four-word input.
    bool CanReuseUnlitQuadRenderer() const
    {
        return CurrentRenderer != NULL && !RendererReqsHaveChanged && !IsCurRendererCustom()
            && RendererRequirements.PrimType == RendererProps::kQuads
            && RendererRequirements.ArrayAccess == RendererProps::kLinear
            && RendererRequirements.Lighting == 0
            && RendererRequirements.PerVtxMaterial == RendererProps::kNoMaterial;
    }

    // state updates

    void EnableCustom(uint64_t flag);
    void DisableCustom(uint64_t flag);

    void NumLightsChanged(tLightType type, int num);
    void PrimChanged(unsigned int prim);
    void TexEnabledChanged(bool enabled);
    void LightingEnabledChanged(bool enabled);
    void SpecularEnabledChanged(bool enabled);
    void PerVtxMaterialChanged(RendererProps::tPerVtxMaterial matType);
    void ClippingEnabledChanged(bool enabled);
    void CullFaceEnabledChanged(bool enabled);
    void ArrayAccessChanged(RendererProps::tArrayAccess accessType);
};

#endif // ps2gl_renderermanager_h
