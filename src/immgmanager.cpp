/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include <stdio.h>
#include <limits.h>

#include "ps2s/cpu_matrix.h"
#include "ps2s/displayenv.h"
#include "ps2s/math.h"
#include "ps2s/packet.h"

#include "GL/ps2gl.h"
#include "ps2gl/clear.h"
#include "ps2gl/dlist.h"
#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/gmanager.h"
#include "ps2gl/material.h"
#include "ps2gl/lighting.h"
#include "ps2gl/matrix.h"
#include "ps2gl/renderer.h"
#include "ps2gl/texture.h"

using namespace ArrayType;

static const unsigned int immediateMapping = PGL_CACHED_IMMEDIATE_GEOMETRY
    ? Core::MemMappings::Normal : Core::MemMappings::UncachedAccl;

/********************************************
 * CImmGeomManager
 */

CImmGeomManager::CImmGeomManager(CGLContext& context, int immBufferQwordSize)
    : CGeomManager(context)
    , RendererManager(context)
    , VertexBuf0(immBufferQwordSize + immBufferQwordSize % 4,
          DMAC::Channels::vif1, immediateMapping)
    , NormalBuf0(immBufferQwordSize * 4 / 3 + 1 + (immBufferQwordSize * 4 / 3 + 1) % 4,
          DMAC::Channels::vif1, immediateMapping)
    , TexCoordBuf0(immBufferQwordSize / 2 + (immBufferQwordSize / 2) % 4,
          DMAC::Channels::vif1, immediateMapping)
    , ColorBuf0(immBufferQwordSize + immBufferQwordSize % 4,
          DMAC::Channels::vif1, immediateMapping)
    , VertexBuf1(immBufferQwordSize + immBufferQwordSize % 4,
          DMAC::Channels::vif1, immediateMapping)
    , NormalBuf1(immBufferQwordSize * 4 / 3 + 1 + (immBufferQwordSize * 4 / 3 + 1) % 4,
          DMAC::Channels::vif1, immediateMapping)
    , TexCoordBuf1(immBufferQwordSize / 2 + (immBufferQwordSize / 2) % 4,
          DMAC::Channels::vif1, immediateMapping)
    , ColorBuf1(immBufferQwordSize + immBufferQwordSize % 4,
          DMAC::Channels::vif1, immediateMapping)
{
    CurVertexBuf   = &VertexBuf0;
    CurNormalBuf   = &NormalBuf0;
    CurTexCoordBuf = &TexCoordBuf0;
    CurColorBuf    = &ColorBuf0;

    VertArray = new CVertArray;

    RendererManager.ArrayAccessChanged(RendererProps::kLinear);
}

CImmGeomManager::~CImmGeomManager()
{
    delete VertArray;
}

void CImmGeomManager::SwapBuffers()
{
    // flip the geometry buffers
    if (CurVertexBuf == &VertexBuf0)
        CurVertexBuf = &VertexBuf1;
    else
        CurVertexBuf = &VertexBuf0;
    CurVertexBuf->Reset();
    if (CurNormalBuf == &NormalBuf0)
        CurNormalBuf = &NormalBuf1;
    else
        CurNormalBuf = &NormalBuf0;
    CurNormalBuf->Reset();
    if (CurTexCoordBuf == &TexCoordBuf0)
        CurTexCoordBuf = &TexCoordBuf1;
    else
        CurTexCoordBuf = &TexCoordBuf0;
    CurTexCoordBuf->Reset();
    if (CurColorBuf == &ColorBuf0)
        CurColorBuf = &ColorBuf1;
    else
        CurColorBuf = &ColorBuf0;
    CurColorBuf->Reset();
}

/********************************************
 * glBegin/glEnd and related
 */

void CImmGeomManager::BeginGeom(GLenum mode)
{
    if (Prim != mode)
        PrimChanged(mode);

    Geometry.SetPrimType(mode);
    Geometry.SetArrayType(kLinear);

    Geometry.SetNormals(CurNormalBuf->GetNextPtr());
    Geometry.SetVertices(CurVertexBuf->GetNextPtr());
    Geometry.SetTexCoords(CurTexCoordBuf->GetNextPtr());
    Geometry.SetColors(CurColorBuf->GetNextPtr());

    InsideBeginEnd = true;
}

void CImmGeomManager::Vertex(cpu_vec_xyzw newVert)
{
    cpu_vec_xyz normal = GetCurNormal();
    *CurNormalBuf += normal;

    const float* texCoord = GetCurTexCoord();
    *CurTexCoordBuf += texCoord[0];
    *CurTexCoordBuf += texCoord[1];

    *CurVertexBuf += newVert;

    Geometry.AddVertices();
    Geometry.AddNormals();
    Geometry.AddTexCoords();
}

bool CImmGeomManager::TryTexturedQuad2D(float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1)
{
    // Prim describes the last synchronized renderer, not necessarily this
    // glBegin. Test the pending primitive before touching any cursor/state.
    if (!InsideBeginEnd || Geometry.GetNewPrimType() != GL_QUADS ||
        !CurVertexBuf->CanReserveWords(16) ||
        !CurNormalBuf->CanReserveWords(12) ||
        !CurTexCoordBuf->CanReserveWords(8))
        return false;

    float* p = (float*)CurVertexBuf->ReserveWords(16);
    float* n = (float*)CurNormalBuf->ReserveWords(12);
    float* t = (float*)CurTexCoordBuf->ReserveWords(8);
    const cpu_vec_xyz normal = GetCurNormal();
    // Preserve the GL_QUADS diagonal, homogeneous coordinates and attribute
    // streams exactly. No temporary arrays, alternate renderer or early flush.
    p[0] = x0; p[1] = y1; p[2] = 0.0f; p[3] = 1.0f;
    p[4] = x0; p[5] = y0; p[6] = 0.0f; p[7] = 1.0f;
    p[8] = x1; p[9] = y0; p[10] = 0.0f; p[11] = 1.0f;
    p[12] = x1; p[13] = y1; p[14] = 0.0f; p[15] = 1.0f;
    for (int i = 0; i < 12; i += 3) {
        n[i] = normal.x; n[i + 1] = normal.y; n[i + 2] = normal.z;
    }
    t[0] = u0; t[1] = v1; t[2] = u0; t[3] = v0;
    t[4] = u1; t[5] = v0; t[6] = u1; t[7] = v1;
    CurTexCoord[0] = u1;
    CurTexCoord[1] = v1;
    Geometry.AddVertices(4);
    Geometry.AddNormals(4);
    Geometry.AddTexCoords(4);
    return true;
}

GLboolean pglTryTexturedQuad2D(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1,
    GLfloat u0, GLfloat v0, GLfloat u1, GLfloat v1)
{
    // Display-list recording must keep its own manager and command stream.
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryTexturedQuad2D(x0, y0, x1, y1,
        u0, v0, u1, v1) ? GL_TRUE : GL_FALSE;
}

GLboolean pglUsesCachedImmediateGeometry(void)
{
    return PGL_CACHED_IMMEDIATE_GEOMETRY ? GL_TRUE : GL_FALSE;
}

template <int WordsPerVertex>
static inline const float* WriteTexturedQuads2D(const float* quads, int count,
    bool corners, float* p, float* t)
{
    const float* q = quads;
    // Both widths share the exact source mapping. Width branches disappear
    // in each template instance; rectangle/corner layout is unswitched once.
    if (corners) {
        for (int i = 0; i < count; ++i, q += 16, p += 4 * WordsPerVertex, t += 8) {
            p[0] = q[0]; p[1] = q[1]; p[2] = 0.0f;
            p[WordsPerVertex] = q[4]; p[WordsPerVertex + 1] = q[5]; p[WordsPerVertex + 2] = 0.0f;
            p[2 * WordsPerVertex] = q[8]; p[2 * WordsPerVertex + 1] = q[9]; p[2 * WordsPerVertex + 2] = 0.0f;
            p[3 * WordsPerVertex] = q[12]; p[3 * WordsPerVertex + 1] = q[13]; p[3 * WordsPerVertex + 2] = 0.0f;
            if (WordsPerVertex == 4) {
                p[3] = 1.0f; p[7] = 1.0f; p[11] = 1.0f; p[15] = 1.0f;
            }
            t[0] = q[2]; t[1] = q[3]; t[2] = q[6]; t[3] = q[7];
            t[4] = q[10]; t[5] = q[11]; t[6] = q[14]; t[7] = q[15];
        }
    } else {
        for (int i = 0; i < count; ++i, q += 8, p += 4 * WordsPerVertex, t += 8) {
            p[0] = q[0]; p[1] = q[3]; p[2] = 0.0f;
            p[WordsPerVertex] = q[0]; p[WordsPerVertex + 1] = q[1]; p[WordsPerVertex + 2] = 0.0f;
            p[2 * WordsPerVertex] = q[2]; p[2 * WordsPerVertex + 1] = q[1]; p[2 * WordsPerVertex + 2] = 0.0f;
            p[3 * WordsPerVertex] = q[2]; p[3 * WordsPerVertex + 1] = q[3]; p[3 * WordsPerVertex + 2] = 0.0f;
            if (WordsPerVertex == 4) {
                p[3] = 1.0f; p[7] = 1.0f; p[11] = 1.0f; p[15] = 1.0f;
            }
            t[0] = q[4]; t[1] = q[7]; t[2] = q[4]; t[3] = q[5];
            t[4] = q[6]; t[5] = q[5]; t[6] = q[6]; t[7] = q[7];
        }
    }
    return q;
}

bool CImmGeomManager::TryDrawTexturedQuads2D(const float* quads, int count, bool corners)
{
    // Validate the ENTIRE run before reserving or changing renderer state.
    // These absent attributes are not read by the unlit uniform-color path.
    if (InsideBeginEnd || !quads || count <= 0 || count > INT_MAX / 16 ||
        GLContext.GetImmLighting().GetLightingEnabled() ||
        GLContext.GetMaterialManager().GetColorMaterialEnabled() ||
        !GLContext.GetTexManager().GetTexEnabled() ||
        !CurVertexBuf->CanReserveWords(count * 16) ||
        !CurTexCoordBuf->CanReserveWords(count * 8))
        return false;

    int wordsPerVertex = 4;
#if PGL_BULK_QUAD_XYZ3
    // The stock quad programs read XYZ and supply homogeneous W themselves.
    // Do not infer the next renderer from flags while a change is pending.
    if (Prim == GL_QUADS && RendererManager.CanReuseUnlitQuadRenderer())
        wordsPerVertex = 3;
#endif
    float* const vertices = (float*)CurVertexBuf->ReserveWords(count * 4 * wordsPerVertex);
    float* const texcoords = (float*)CurTexCoordBuf->ReserveWords(count * 8);
    const float* q;
#if PGL_BULK_QUAD_XYZ3
    if (wordsPerVertex == 3)
        q = WriteTexturedQuads2D<3>(quads, count, corners, vertices, texcoords);
    else
#endif
        q = WriteTexturedQuads2D<4>(quads, count, corners, vertices, texcoords);

    if (Prim != GL_QUADS) PrimChanged(GL_QUADS);
    Geometry.SetPrimType(GL_QUADS);
    Geometry.SetArrayType(kLinear);
    Geometry.SetVertices(vertices);
    Geometry.SetTexCoords(texcoords);
    Geometry.SetNormals(NULL);
    Geometry.SetColors(NULL);
    Geometry.SetVerticesAreValid(true);
    Geometry.SetTexCoordsAreValid(true);
    Geometry.SetNormalsAreValid(false);
    Geometry.SetColorsAreValid(false);
    Geometry.SetWordsPerVertex(wordsPerVertex);
    Geometry.SetWordsPerNormal(3);
    Geometry.SetWordsPerTexCoord(2);
    Geometry.SetWordsPerColor(4);
    Geometry.AddVertices(count * 4);
    Geometry.AddNormals(count * 4);
    Geometry.AddTexCoords(count * 4);
    Geometry.AddColors(count * 4);
    SyncColorMaterial(false);
    CommitNewGeom();
    CurTexCoord[0] = q[-2];
    CurTexCoord[1] = q[-1];
    return true;
}

GLboolean pglTryDrawTexturedQuads2D(const GLfloat* quads, GLsizei count)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawTexturedQuads2D(quads, count)
        ? GL_TRUE : GL_FALSE;
}

GLboolean pglTryDrawTexturedQuadCorners2D(const GLfloat* quads, GLsizei count)
{
#if PGL_BULK_QUAD_CORNERS
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawTexturedQuads2D(quads, count, true)
        ? GL_TRUE : GL_FALSE;
#else
    (void)quads;
    (void)count;
    return GL_FALSE;
#endif
}

void CImmGeomManager::Normal(cpu_vec_xyz normal)
{
    if (DoNormalize)
        normal.normalize();
    CurNormal = normal;
}

void CImmGeomManager::Color(cpu_vec_xyzw color)
{
    if (InsideBeginEnd) {
        *CurColorBuf += color;
        Geometry.AddColors();
        //add color inside too
        GLContext.GetMaterialManager().Color(color);
    } else {
        GLContext.GetMaterialManager().Color(color);
    }
}

void CImmGeomManager::TexCoord(float u, float v)
{
    CurTexCoord[0] = u;
    CurTexCoord[1] = v;
}

void CImmGeomManager::EndGeom()
{
    InsideBeginEnd = false;

    Geometry.SetVerticesAreValid(true);
    Geometry.SetNormalsAreValid(true);
    Geometry.SetTexCoordsAreValid(true);

    // check colors
    Geometry.SetColorsAreValid(false);
    if (Geometry.GetNumNewColors() > 0) {
        //debug test for raylib
        if(Geometry.GetNumNewVertices() != Geometry.GetNumNewColors())
        {
            //printf("vertices: %d , colors: %d Sorry, but inside glBegin/glEnd you need to specify either one color for each vertex given, or none.\n",Geometry.GetNumNewVertices(),Geometry.GetNumNewColors());
        }
        /*mErrorIf(Geometry.GetNumNewVertices() != Geometry.GetNumNewColors(),
            "Sorry, but inside glBegin/glEnd you need "
            "to specify either one color for each vertex given, or none.");*/
        Geometry.SetColorsAreValid(true);

        SyncColorMaterial(true);
    } else {
        SyncColorMaterial(false);
    }

    Geometry.SetWordsPerVertex(4);
    Geometry.SetWordsPerNormal(3);
    Geometry.SetWordsPerTexCoord(2);
    Geometry.SetWordsPerColor(4);

    CommitNewGeom();
}

/********************************************
 * DrawArrays
 */

void CImmGeomManager::DrawArrays(GLenum mode, int first, int count)
{
    if (Prim != mode)
        PrimChanged(mode);

    Geometry.SetPrimType(mode);
    Geometry.SetArrayType(kLinear);

    Geometry.SetVertices(VertArray->GetVertices());
    Geometry.SetNormals(VertArray->GetNormals());
    Geometry.SetTexCoords(VertArray->GetTexCoords());
    Geometry.SetColors(VertArray->GetColors());

    Geometry.SetVerticesAreValid(VertArray->GetVerticesAreValid());
    Geometry.SetNormalsAreValid(VertArray->GetNormalsAreValid());
    Geometry.SetTexCoordsAreValid(VertArray->GetTexCoordsAreValid());
    Geometry.SetColorsAreValid(VertArray->GetColorsAreValid());

    Geometry.SetWordsPerVertex(VertArray->GetWordsPerVertex());
    Geometry.SetWordsPerNormal(VertArray->GetWordsPerNormal());
    Geometry.SetWordsPerTexCoord(VertArray->GetWordsPerTexCoord());
    Geometry.SetWordsPerColor(VertArray->GetWordsPerColor());

    Geometry.AddVertices(count);
    Geometry.AddNormals(count);
    Geometry.AddTexCoords(count);
    Geometry.AddColors(count);

    Geometry.AdjustNewGeomPtrs(first);

    // do this before sync'ing the vu1 renderer in CommitNewGeom
    SyncColorMaterial(VertArray->GetColors() != NULL);

    CommitNewGeom();
}

void CImmGeomManager::DrawingIndexedArray()
{
    if (!LastArrayAccessIsValid || !LastArrayAccessWasIndexed) {
        GLContext.ArrayAccessChanged();
        RendererManager.ArrayAccessChanged(RendererProps::kIndexed);
        LastArrayAccessIsValid = true;
    }
    LastArrayAccessWasIndexed = true;
}

void CImmGeomManager::DrawIndexedArrays(GLenum primType,
    int numIndices, const unsigned char* indices,
    int numVertices)
{
    /*
   // make sure there's no pending geometry
   Flush();

   // do these before sync'ing the vu1 renderer
   SyncColorMaterial(VertArray->GetColors() != NULL);
   DrawingIndexedArray();

   // now update the renderer and render

   bool rendererChanged = RendererManager.UpdateRenderer();

   if ( rendererChanged ) {
      RendererManager.LoadRenderer(GLContext.GetVif1Packet());
   }
   SyncRendererContext(primType);
   SyncGsContext();

   RendererManager.GetCurRenderer().DrawIndexedArrays( primType, numIndices, indices,
						  numVertices, *VertArray );
   */
    if (Prim != primType)
        PrimChanged(primType);

    Geometry.SetPrimType(primType);
    Geometry.SetArrayType(kIndexed);

    Geometry.SetVertices(VertArray->GetVertices());
    Geometry.SetNormals(VertArray->GetNormals());
    Geometry.SetTexCoords(VertArray->GetTexCoords());
    Geometry.SetColors(VertArray->GetColors());

    Geometry.SetVerticesAreValid(VertArray->GetVerticesAreValid());
    Geometry.SetNormalsAreValid(VertArray->GetNormalsAreValid());
    Geometry.SetTexCoordsAreValid(VertArray->GetTexCoordsAreValid());
    Geometry.SetColorsAreValid(VertArray->GetColorsAreValid());

    Geometry.SetWordsPerVertex(VertArray->GetWordsPerVertex());
    Geometry.SetWordsPerNormal(VertArray->GetWordsPerNormal());
    Geometry.SetWordsPerTexCoord(VertArray->GetWordsPerTexCoord());
    Geometry.SetWordsPerColor(VertArray->GetWordsPerColor());

    Geometry.AddVertices(numVertices);
    Geometry.AddNormals(numVertices);
    Geometry.AddTexCoords(numVertices);
    Geometry.AddColors(numVertices);

    Geometry.SetNumIndices(numIndices);
    Geometry.SetIndices(indices);
    Geometry.SetIStripLengths(NULL);

    // do this before sync'ing the vu1 renderer in CommitNewGeom
    SyncColorMaterial(VertArray->GetColors() != NULL);

    CommitNewGeom();
}

/********************************************
 * common and synchronization code
 */

void CImmGeomManager::DrawingLinearArray()
{
    if (!LastArrayAccessIsValid || LastArrayAccessWasIndexed) {
        GLContext.ArrayAccessChanged();
        RendererManager.ArrayAccessChanged(RendererProps::kLinear);
        LastArrayAccessIsValid = true;
    }
    LastArrayAccessWasIndexed = false;
}

void CImmGeomManager::CommitNewGeom()
{
    // do this before updating the renderer
    if (Geometry.GetNewArrayType() == kLinear)
        DrawingLinearArray();
    else
        DrawingIndexedArray();

    bool doReset         = true;
    bool rendererChanged = RendererManager.UpdateNewRenderer();

    if (Geometry.IsPending()) {

        // FIXME: need to ask the renderer what context changes it cares about/updates

        // if the context hasn't changed, try to merge the new geometry
        // into the current block
        if (GLContext.GetRendererContextChanged() == 0
            && GLContext.GetGsContextChanged() == 0
            && !UserRenderContextChanged
            && !rendererChanged
            && Geometry.MergeNew()) {
            doReset = false;
        } else {
            // couldn't merge; draw the old geometry so we can reset and start a new block
            if (Geometry.GetArrayType() == kLinear)
                RendererManager.GetCurRenderer().DrawLinearArrays(Geometry);
            else
                RendererManager.GetCurRenderer().DrawIndexedArrays(Geometry);
        }
    }

    if (doReset) {
        Geometry.MakeNewValuesCurrent();
        Geometry.ResetNew();

        if (rendererChanged) {
            RendererManager.MakeNewRendererCurrent();
            RendererManager.LoadRenderer(GLContext.GetVif1Packet());
        }
        SyncRendererContext(Geometry.GetPrimType());
        SyncGsContext();
    }
}

void CImmGeomManager::PrimChanged(GLenum primType)
{
    GLContext.PrimChanged();
    RendererManager.PrimChanged(primType);
}

void CImmGeomManager::SyncRenderer()
{
    if (RendererManager.UpdateNewRenderer()) {
        RendererManager.MakeNewRendererCurrent();
        RendererManager.LoadRenderer(GLContext.GetVif1Packet());
    }
}

void CImmGeomManager::SyncRendererContext(GLenum primType)
{
    // resend the rendering context if necessary
    if (GLContext.GetRendererContextChanged()
        || (RendererManager.IsCurRendererCustom() && UserRenderContextChanged)) {
        RendererManager.GetCurRenderer().InitContext(primType,
            GLContext.GetRendererContextChanged(),
            UserRenderContextChanged);

        GLContext.SetRendererContextChanged(false);
        UserRenderContextChanged = false;
        Prim                     = primType;
    }
}

void CImmGeomManager::SyncGsContext()
{
    if (uint32_t changed = GLContext.GetGsContextChanged()) {
        // has the texture changed?
        bool texEnabled         = GLContext.GetTexManager().GetTexEnabled();
        CVifSCDmaPacket& packet = GLContext.GetVif1Packet();
        if (texEnabled
            && changed & GsCtxtFlags::Texture) {
            // we have to wait for all previous buffers to finish, or
            // the new texture settings might beat the geometry to the gs..
            packet.Cnt();
            packet.Flush().Nop();
            packet.CloseTag();
            GLContext.GetTexManager().UseCurTexture(GLContext.GetVif1Packet());
        }

        // has the draw environment changed?
        if (changed & GsCtxtFlags::DrawEnv) {
            // as with textures..
            packet.Cnt();
            packet.Flush().Nop();
            packet.CloseTag();
            // FIXME
            GLContext.AddingDrawEnvToPacket((uint128_t*)GLContext.GetVif1Packet().GetNextPtr() + 1);
            GLContext.GetImmDrawContext().GetDrawEnv().SendSettings(GLContext.GetVif1Packet());
            GLContext.GetImmDrawContext().NoteDrawEnvSubmission();
        }

        GLContext.SetGsContextChanged(false);
    }
}

void CImmGeomManager::SyncColorMaterial(bool pvColorsArePresent)
{
    CMaterialManager& mm = GLContext.GetMaterialManager();
    if (pvColorsArePresent && mm.GetColorMaterialEnabled()) {
        switch (mm.GetColorMaterialMode()) {
        case GL_EMISSION:
            mNotImplemented("Only GL_DIFFUSE can change per-vertex");
            break;
        case GL_AMBIENT:
            mNotImplemented("Only GL_DIFFUSE can change per-vertex");
            break;
        case GL_DIFFUSE:
            // fix later..
            GLContext.PerVtxMaterialChanged();
            RendererManager.PerVtxMaterialChanged(RendererProps::kDiffuse);
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            mNotImplemented("Only GL_DIFFUSE can change per-vertex");
            break;
        case GL_SPECULAR:
            mNotImplemented("Only GL_DIFFUSE can change per-vertex");
            // PerVtxMaterialChanged( PerVtxMaterial::kSpecular );
            break;
        }
    } else {
        RendererManager.PerVtxMaterialChanged(RendererProps::kNoMaterial);
    }
}

void CImmGeomManager::Flush()
{
    if (Geometry.IsPending()) {
        if (Geometry.GetArrayType() == kLinear)
            RendererManager.GetCurRenderer().DrawLinearArrays(Geometry);
        else
            RendererManager.GetCurRenderer().DrawIndexedArrays(Geometry);
        Geometry.Reset();
    }
}
