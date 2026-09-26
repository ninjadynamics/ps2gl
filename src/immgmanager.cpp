/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include <stdio.h>
#include <limits.h>
#include <string.h>

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
#include "ps2gl/x2r_renderer.h"
#include "ps2gl/x2p_renderer.h"
#include "ps2gl/x2b_renderer.h"
#include "ps2gl/x2e_renderer.h"

using namespace ArrayType;

static const unsigned int immediateMapping = Core::MemMappings::Normal;

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

bool CImmGeomManager::TryTriangle3D(const float* xyz)
{
    if (!InsideBeginEnd || !xyz || Geometry.GetNewPrimType() != GL_TRIANGLES ||
        !CurVertexBuf->CanReserveWords(12) ||
        !CurNormalBuf->CanReserveWords(9) ||
        !CurTexCoordBuf->CanReserveWords(6))
        return false;

    float* const p = (float*)CurVertexBuf->ReserveWords(12);
    float* const n = (float*)CurNormalBuf->ReserveWords(9);
    float* const t = (float*)CurTexCoordBuf->ReserveWords(6);
    const cpu_vec_xyz normal = GetCurNormal();
    const float* const uv = GetCurTexCoord();
    // Keep all normal/UV words even when this particular renderer does not
    // consume them. The surrounding EndGeom owns format, colors and draws.
    p[0] = xyz[0]; p[1] = xyz[1]; p[2] = xyz[2]; p[3] = 1.0f;
    p[4] = xyz[3]; p[5] = xyz[4]; p[6] = xyz[5]; p[7] = 1.0f;
    p[8] = xyz[6]; p[9] = xyz[7]; p[10] = xyz[8]; p[11] = 1.0f;
    for (int i = 0; i < 9; i += 3) {
        n[i] = normal.x; n[i + 1] = normal.y; n[i + 2] = normal.z;
    }
    for (int i = 0; i < 6; i += 2) {
        t[i] = uv[0]; t[i + 1] = uv[1];
    }
    Geometry.AddVertices(3);
    Geometry.AddNormals(3);
    Geometry.AddTexCoords(3);
    return true;
}

GLboolean pglTryTriangle3D(const GLfloat* xyz)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryTriangle3D(xyz) ? GL_TRUE : GL_FALSE;
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
    return GL_TRUE;
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

inline __attribute__((always_inline))
void CImmGeomManager::CommitTexturedQuadArrays(const float* vertices, const float* texcoords,
    int count, int wordsPerVertex)
{
    // Only the NEW block is described here. CommitNewGeom must still drain
    // any OLD block with its cached renderer state before syncing this one.
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
    // The stock quad programs read XYZ and supply homogeneous W themselves.
    // Do not infer the next renderer from flags while a change is pending.
    if (Prim == GL_QUADS && RendererManager.CanReuseUnlitQuadRenderer())
        wordsPerVertex = 3;
    float* const vertices = (float*)CurVertexBuf->ReserveWords(count * 4 * wordsPerVertex);
    float* const texcoords = (float*)CurTexCoordBuf->ReserveWords(count * 8);
    const float* q;
    if (wordsPerVertex == 3)
        q = WriteTexturedQuads2D<3>(quads, count, corners, vertices, texcoords);
    else
        q = WriteTexturedQuads2D<4>(quads, count, corners, vertices, texcoords);

    CommitTexturedQuadArrays(vertices, texcoords, count, wordsPerVertex);
    CurTexCoord[0] = q[-2];
    CurTexCoord[1] = q[-1];
    return true;
}

static bool HudFiniteFloat(float value)
{
    unsigned int bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static bool HudAffineColumn(cpu_vec_4 column, float w)
{
    return column.w == w && HudFiniteFloat(column.x)
        && HudFiniteFloat(column.y) && HudFiniteFloat(column.z)
        && HudFiniteFloat(column.w);
}

static bool HudAffineMatrix(const cpu_mat_44& matrix)
{
    return HudAffineColumn(matrix.get_col0(), 0.0f)
        && HudAffineColumn(matrix.get_col1(), 0.0f)
        && HudAffineColumn(matrix.get_col2(), 0.0f)
        && HudAffineColumn(matrix.get_col3(), 1.0f);
}

bool CImmGeomManager::CanDrawColoredHud2DState() const
{
    const GLenum prim = PGL_UNLIT_TEX_TRIANGLES;
    if (InsideBeginEnd || !RendererManager.CanSelectColoredHudRenderer()
        || (prim & 0x7fffffffu) >= kMaxUserPrimTypes
        || GetUserPrimRequirements(prim) != PGL_UNLIT_TEX_TRI_PROP
        || GetUserPrimReqMask(prim) != ~(uint64_t)0xffffffff
        || GLContext.GetImmLighting().GetLightingEnabled()
        || GLContext.GetMaterialManager().GetColorMaterialEnabled()
        || !GLContext.GetTexManager().GetTexEnabled())
        return false;

    CImmDrawContext& draw = GLContext.GetImmDrawContext();
    if (draw.GetFogEnabled() || draw.GetDoCullFace() || draw.GetDoClipping()
        || draw.GetEdgeAAEnabled() || draw.GetPolygonMode() != GL_FILL
        || !HudFiniteFloat(draw.GetDepthOffset()))
        return false;
    return true;
}

bool CImmGeomManager::CanDrawColoredHud2D() const
{
    if (!CanDrawColoredHud2DState()) return false;
    CImmDrawContext& draw = GLContext.GetImmDrawContext();
    // The stock unlit shader still multiplies zero global ambient by the
    // material ambient. Non-finite material RGB cannot share the RGBA path.
    const cpu_vec_4 ambient = GLContext.GetMaterialManager().GetImmMaterial().GetAmbient();
    if (!HudFiniteFloat(ambient.x) || !HudFiniteFloat(ambient.y)
        || !HudFiniteFloat(ambient.z)) return false;

    // Exact affine rows make W=1 under BOTH programs, including the stock
    // quad's shared Q path. Read the forward stacks, never their lazy inverses.
    // The last check also rejects overflow in their GS-scaled product. This
    // only materializes the existing pure transform cache, as drawing would;
    // it changes no GL state, packet, renderer selection or geometry lifetime.
    return HudAffineMatrix(GLContext.GetModelViewStack().GetTop())
        && HudAffineMatrix(GLContext.GetProjectionStack().GetTop())
        && HudAffineMatrix(draw.GetVertexXform());
}

GLboolean pglCanDrawColoredHud2D(void)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().CanDrawColoredHud2D() ? GL_TRUE : GL_FALSE;
}

bool CImmGeomManager::TryDrawColoredHud2DArrays(const float* vertices,
    const float* texcoords, const float* colors, int vertexCount)
{
    return TryDrawColoredArrays(vertices, texcoords, colors, vertexCount, 6);
}

bool CImmGeomManager::TryDrawColoredArrays(const float* vertices,
    const float* texcoords, const float* colors, int vertexCount, int vertexMultiple)
{
    // All rejection precedes PrimChanged/Geometry writes. The pass owner has
    // already admitted its unchanged matrices; do not rescan them per span.
    if (!vertices || !texcoords || !colors || vertexCount <= 0
        || vertexCount > INT_MAX / 16 || vertexCount % vertexMultiple != 0
        || (((uintptr_t)vertices | (uintptr_t)texcoords | (uintptr_t)colors) & 3u)
        || Geometry.GetTotalVertices() > INT_MAX - vertexCount
        || !CanDrawColoredHud2DState()) return false;
    const uintptr_t count = (uintptr_t)vertexCount;
    if ((uintptr_t)vertices > ~(uintptr_t)0 - count * 3u * sizeof(float)
        || (uintptr_t)texcoords > ~(uintptr_t)0 - count * 2u * sizeof(float)
        || (uintptr_t)colors > ~(uintptr_t)0 - count * 4u * sizeof(float))
        return false;

    // Describe only the NEXT block, exactly as DrawArrays does. Existing client
    // descriptors/enables are neither consumed nor changed, even if raylib's
    // initial NORMAL_ARRAY enable survived preceding unlit draws. Current
    // color/normal/UV and immediate buffer cursors are unchanged too.
    const GLenum prim = PGL_UNLIT_TEX_TRIANGLES;
    if (Prim != prim) PrimChanged(prim);
    Geometry.SetPrimType(prim);
    Geometry.SetArrayType(kLinear);
    Geometry.SetVertices(vertices);
    Geometry.SetTexCoords(texcoords);
    Geometry.SetColors(colors);
    Geometry.SetNormals(NULL);
    Geometry.SetVerticesAreValid(true);
    Geometry.SetTexCoordsAreValid(true);
    Geometry.SetColorsAreValid(true);
    Geometry.SetNormalsAreValid(false);
    Geometry.SetWordsPerVertex(3);
    Geometry.SetWordsPerNormal(3);
    Geometry.SetWordsPerTexCoord(2);
    Geometry.SetWordsPerColor(4);
    Geometry.AddVertices(vertexCount);
    Geometry.AddNormals(vertexCount);
    Geometry.AddTexCoords(vertexCount);
    Geometry.AddColors(vertexCount);
    SyncColorMaterial(true);
    CommitNewGeom();
    return true;
}

GLboolean pglTryDrawColoredHud2DArrays(const GLfloat* vertices,
    const GLfloat* texcoords, const GLfloat* colors, GLsizei vertexCount)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawColoredHud2DArrays(
        vertices, texcoords, colors, vertexCount) ? GL_TRUE : GL_FALSE;
}

bool CImmGeomManager::CanDrawColoredTriangles() const
{
    if (!CanDrawColoredHud2DState()) return false;
    const cpu_vec_4 ambient = GLContext.GetMaterialManager().GetImmMaterial().GetAmbient();
    if (!HudFiniteFloat(ambient.x) || !HudFiniteFloat(ambient.y)
        || !HudFiniteFloat(ambient.z)) return false;
    const cpu_mat_44 matrices[3] = {
        GLContext.GetModelViewStack().GetTop(),
        GLContext.GetProjectionStack().GetTop(),
        GLContext.GetImmDrawContext().GetVertexXform()
    };
    for (unsigned int i = 0; i < 3; ++i) {
        const cpu_vec_4 columns[4] = {
            matrices[i].get_col0(), matrices[i].get_col1(),
            matrices[i].get_col2(), matrices[i].get_col3()
        };
        for (unsigned int j = 0; j < 4; ++j)
            if (!HudFiniteFloat(columns[j].x) || !HudFiniteFloat(columns[j].y)
                || !HudFiniteFloat(columns[j].z) || !HudFiniteFloat(columns[j].w))
                return false;
    }
    return true;
}

bool CImmGeomManager::TryDrawColoredTriangleArrays(const float* vertices,
    const float* texcoords, const float* colors, int vertexCount)
{
    return TryDrawColoredArrays(vertices, texcoords, colors, vertexCount, 3);
}

GLboolean pglCanDrawColoredTriangles(void)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().CanDrawColoredTriangles() ? GL_TRUE : GL_FALSE;
}

GLboolean pglUsesColoredTriArrays(void)
{
    return GL_TRUE;
}

GLboolean pglTryDrawColoredTriangleArrays(const GLfloat* vertices,
    const GLfloat* texcoords, const GLfloat* colors, GLsizei vertexCount)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawColoredTriangleArrays(
        vertices, texcoords, colors, vertexCount) ? GL_TRUE : GL_FALSE;
}

static unsigned int wallDescriptorArraysAccepted, wallDescriptorArraysRejected;

bool CImmGeomManager::DrawWallDescriptorArrays(GLenum primitive,
    const float* geometry, const float* colors, int descriptorCount)
{
    const bool pairedColors = primitive == PGL_CLIP_TRIANGLES_X2C;
    const bool cornerFog = primitive == PGL_CLIP_TRIANGLES_X2H;
    const uint64_t requirements = pairedColors ? PGL_CLIP_TRI_X2C_PROP
        : cornerFog ? PGL_CLIP_TRI_X2H_PROP : PGL_CLIP_TRI_X2Q_PROP;
    if ((primitive != PGL_CLIP_TRIANGLES_X2Q && !pairedColors && !cornerFog) || InsideBeginEnd ||
        !geometry || !colors || descriptorCount <= 0 || descriptorCount > INT_MAX / 64 ||
        (((uintptr_t)geometry | (uintptr_t)colors) & 3u) ||
        !RendererManager.CanSelectWallDescriptorRenderer(requirements) ||
        GetUserPrimRequirements(primitive) != requirements ||
        GetUserPrimReqMask(primitive) != ~(uint64_t)0xffffffff ||
        GLContext.GetImmLighting().GetLightingEnabled() ||
        GLContext.GetMaterialManager().GetColorMaterialEnabled() ||
        !GLContext.GetTexManager().GetTexEnabled()) return false;

    const int elements = descriptorCount * 4;
    const uintptr_t geometryBytes = (uintptr_t)descriptorCount * 64u;
    const uintptr_t colorBytes = (uintptr_t)descriptorCount *
        (pairedColors ? 32u : cornerFog ? 48u : 64u);
    if (Geometry.GetTotalVertices() > INT_MAX - elements ||
        (uintptr_t)geometry > ~(uintptr_t)0 - geometryBytes ||
        (uintptr_t)colors > ~(uintptr_t)0 - colorBytes) return false;

    // This is DrawArrays' NEXT-block contract with known descriptor strides.
    // The original X2Q/C renderer still owns setup, transfer, clipping, fog and
    // both material kicks. Do not alter client arrays or borrow its incidental
    // normal/UV descriptors: those streams are unused by this renderer.
    if (Prim != primitive) PrimChanged(primitive);
    Geometry.SetPrimType(primitive);
    Geometry.SetArrayType(kLinear);
    Geometry.SetVertices(geometry);
    Geometry.SetColors(colors);
    Geometry.SetNormals(NULL);
    Geometry.SetTexCoords(NULL);
    Geometry.SetVerticesAreValid(true);
    Geometry.SetColorsAreValid(true);
    Geometry.SetNormalsAreValid(false);
    Geometry.SetTexCoordsAreValid(false);
    Geometry.SetWordsPerVertex(4);
    Geometry.SetWordsPerColor(4);
    Geometry.SetWordsPerNormal(3);
    Geometry.SetWordsPerTexCoord(2);
    Geometry.AddVertices(elements);
    Geometry.AddColors(elements);
    Geometry.AddNormals(elements);
    Geometry.AddTexCoords(elements);
    SyncColorMaterial(true);
    CommitNewGeom();
    return true;
}

GLboolean pglDrawWallDescriptorArrays(GLenum primitive, const GLfloat* geometry,
    const GLfloat* colors, GLsizei descriptorCount)
{
    const bool accepted = pGLContext && !pGLContext->InDListDef() &&
        pGLContext->GetImmGeomManager().DrawWallDescriptorArrays(
            primitive, geometry, colors, descriptorCount);
    if (accepted) ++wallDescriptorArraysAccepted;
    else ++wallDescriptorArraysRejected;
    return accepted ? GL_TRUE : GL_FALSE;
}

extern "C" void pglGetWindowPreparationStats(unsigned int*, unsigned int*);

void pglGetWallPreparationStats(unsigned int* texturePrefixReused, unsigned int* drawTailReused,
    unsigned int* directAccepted, unsigned int* directRejected)
{
    pglGetWindowPreparationStats(texturePrefixReused, drawTailReused);
    if (directAccepted) *directAccepted = wallDescriptorArraysAccepted;
    if (directRejected) *directRejected = wallDescriptorArraysRejected;
}

static bool DirectIdentityColumn(cpu_vec_4 column, float x, float y, float z, float w)
{
    return column.x == x && column.y == y && column.z == z && column.w == w;
}

static bool DirectFiniteColumn(cpu_vec_4 column)
{
    return HudFiniteFloat(column.x) && HudFiniteFloat(column.y)
        && HudFiniteFloat(column.z) && HudFiniteFloat(column.w);
}

static bool DirectTextureValid(const CMMTexture* texture)
{
    if (!texture) return false;
    const GS::tPSM psm = texture->GetPSM();
    if (psm != GS::kPsm32 && psm != GS::kPsm24 && psm != GS::kPsm16
        && psm != GS::kPsm16s && psm != GS::kPsm8 && psm != GS::kPsm8h)
        return false;
    return texture->GetW() && texture->GetH()
        && texture->GetW() <= 1024u && texture->GetH() <= 1024u
        && ((psm != GS::kPsm8 && psm != GS::kPsm8h) || texture->GetOwnClut());
}

static bool RoadContextValid(const PGLRoadContext& context)
{
    const float* values = (const float*)&context;
    for (unsigned int i = 0; i < sizeof(context) / sizeof(float); ++i)
        if (!HudFiniteFloat(values[i])) return false;
    if (context.eyeIncircle[3] <= 0.0f
        || context.skyCenterScale[2] <= 0.0f || context.skyCenterScale[3] <= 0.0f
        || context.clipParams[0] <= 0.0f || context.clipParams[1] <= 0.0f
        || context.clipParams[2] != 1.0e-6f || context.clipParams[3] != 0.0f
        || context.right[3] != 0.0f || context.up[3] != 0.0f
        || context.forward[3] != 0.0f || context.sourceY[1] != 0.0f
        || context.sourceY[2] != 0.0f || context.sourceY[3] != 0.0f)
        return false;
    for (int i = 0; i < 4; ++i)
        if (context.color[i] < 0.0f || context.color[i] > 1.0f) return false;
    for (int i = 0; i < 24; ++i)
        if (context.sourcePlanes[i][3] != 0.0f) return false;
    return true;
}

bool CImmGeomManager::DrawRoadQuads(const PGLRoadContext* context,
    const PGLRoadQuad* quads, int count)
{
    // No old block may be flushed after reservation: its packet footprint and
    // borrowed state belong to the preceding material. The caller drains it
    // before this API, exactly as it does before changing X2 window pairs.
    if (!context || !quads || count <= 0 || count > INT_MAX / 48
        || (((uintptr_t)context | (uintptr_t)quads) & 3u)
        || (uintptr_t)quads > ~(uintptr_t)0 - (uintptr_t)count * sizeof(*quads)
        || (uintptr_t)context > ~(uintptr_t)0 - sizeof(*context)
        || InsideBeginEnd || Geometry.IsPending() || GLContext.InDListDef()
        || !GLContext.UsesNormalFramePacket()
        || !RendererManager.CanSelectRoadRenderer()
        || GetUserPrimRequirements(PGL_CLIP_ROAD_QUADS_X2R) != PGL_CLIP_ROAD_X2R_PROP
        || GetUserPrimReqMask(PGL_CLIP_ROAD_QUADS_X2R) != ~(uint64_t)0xffffffff
        || GLContext.GetImmLighting().GetLightingEnabled()
        || !GLContext.GetTexManager().GetTexEnabled()) return false;

    CClipRoadX2RRenderer* renderer = RendererManager.GetRoadRenderer();
    CImmDrawContext& draw = GLContext.GetImmDrawContext();
    // An exact copy of the last admitted source context is already finite
    // and well formed. This does not imply its VU retention, checked later.
    const bool sameRoadContext = renderer->MatchesRoadContext(*context);
    if (!renderer->IsCodeValid() || (!sameRoadContext && !RoadContextValid(*context))
        || draw.GetFogEnabled() || draw.GetDoCullFace() || draw.GetEdgeAAEnabled()
        || draw.GetPolygonMode() != GL_FILL || !HudFiniteFloat(draw.GetDepthOffset())
        || context->clipParams[0] != draw.GetClipNear()) return false;

    const cpu_mat_44& model = GLContext.GetModelViewStack().GetTop();
    if (!DirectIdentityColumn(model.get_col0(), 1, 0, 0, 0)
        || !DirectIdentityColumn(model.get_col1(), 0, 1, 0, 0)
        || !DirectIdentityColumn(model.get_col2(), 0, 0, 1, 0)
        || !DirectIdentityColumn(model.get_col3(), 0, 0, 0, 1)) return false;
    const cpu_mat_44& xform = draw.GetVertexXform();
    if (!DirectFiniteColumn(xform.get_col0()) || !DirectFiniteColumn(xform.get_col1())
        || !DirectFiniteColumn(xform.get_col2()) || !DirectFiniteColumn(xform.get_col3()))
        return false;

    // At <=1024x1024 and <=32bpp an image has <=9 IMAGE chunks: <=34
    // chain qwords, plus one <=256-entry CLUT, 8 texture and 14 draw settings.
    // 256q covers these, both GS fences, <=8 MPG tags, full 72q context and
    // command padding. Each activation needs <=16q overhead beyond its 3q
    // descriptors. Keep another 16q for the ordinary EndGeometry trailer.
    if (!DirectTextureValid(&GLContext.GetTexManager().GetCurTexture())) return false;
    const uint64_t batches = ((uint64_t)count + 31u) / 32u;
    const uint64_t words = ((uint64_t)count * 3u + batches * 16u + 272u) * 4u;
    CVifSCDmaPacket& packet = GLContext.GetVif1Packet();
    if (words > UINT_MAX || !packet.GetTTE() || packet.HasOpenTag()
        || !packet.CanReserveWords((unsigned int)words)) return false;

    // Admission is complete. All following operations append this material
    // without a fallible partial publication or a pending borrowed block.
    renderer->SetRoadContext(*context, sameRoadContext);
    PrimChanged(PGL_CLIP_ROAD_QUADS_X2R);
    DrawingLinearArray();
    // The road program reads q55, not fixed-function material/light slots.
    // Preserve an inherited COLOR_MATERIAL enable; no color array is supplied.
    SyncColorMaterial(false);
    SyncRenderer();
    SyncRendererContext(PGL_CLIP_ROAD_QUADS_X2R);
    SyncGsContext();
    renderer->DrawRoadQuads(quads, count);
    return true;
}

GLboolean pglDrawRoadQuads(const PGLRoadContext* context,
    const PGLRoadQuad* quads, GLsizei count)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawRoadQuads(context, quads, count)
        ? GL_TRUE : GL_FALSE;
}

static bool BillboardContextValid(const PGLBillboardContext& context)
{
    const float* values = (const float*)&context;
    for (unsigned int i = 0; i < sizeof(context) / sizeof(float); ++i)
        if (!HudFiniteFloat(values[i])) return false;
    return context.right[3] == 0.0f && context.up[3] == 0.0f
        && context.forward[3] == 0.0f && context.eye[3] == 0.0f
        && context.projection[0] == 0.0f && context.projection[1] == 0.0f
        && context.projection[2] > 0.0f && context.projection[3] > 0.0f
        && context.clip[0] > 0.0f && context.clip[1] > 0.0f
        && context.clip[2] == 1.0e-6f && context.clip[3] == 0.0f
        && context.axisU[3] == 0.0f && context.axisV[3] == 0.0f;
}

bool CImmGeomManager::DrawSourceViewQuads(const void* context, const void* quads,
    int count, unsigned int format)
{
    CClipRoadX2RRenderer* renderer = NULL;
    unsigned int quadBytes, contextBytes, batchLimit;
    GLenum primitive;
    uint64_t prop;
    bool validContext;
    float near;
    if (format == 2) {
        if (!context || !RendererManager.CanSelectBillboardAlphaRenderer()) return false;
        renderer = RendererManager.GetBillboardAlphaRenderer();
        quadBytes = sizeof(PGLBillboardAlphaQuad);
        contextBytes = sizeof(PGLBillboardContext);
        batchLimit = 24;
        primitive = PGL_CLIP_BILLBOARD_QUADS_X2A;
        prop = PGL_CLIP_BILLBOARD_X2A_PROP;
    } else if (format == 1) {
        if (!context || !RendererManager.CanSelectBillboardRenderer()) return false;
        renderer = RendererManager.GetBillboardRenderer();
        quadBytes = sizeof(PGLBillboardQuad);
        contextBytes = sizeof(PGLBillboardContext);
        batchLimit = 32;
        primitive = PGL_CLIP_BILLBOARD_QUADS_X2B;
        prop = PGL_CLIP_BILLBOARD_X2B_PROP;
    } else if (format == 0) {
        if (!context || !RendererManager.CanSelectPoolRenderer()) return false;
        renderer = RendererManager.GetPoolRenderer();
        quadBytes = sizeof(PGLPoolQuad);
        contextBytes = sizeof(PGLPoolContext);
        batchLimit = 24;
        primitive = PGL_CLIP_POOL_QUADS_X2P;
        prop = PGL_CLIP_POOL_X2P_PROP;
    } else return false;
    // No old block may be flushed after reservation: its packet footprint and
    // borrowed state belong to the preceding material. The caller drains it
    // before this API, exactly as it does before changing X2 window pairs.
    if (!context || !quads || count <= 0 || count > INT_MAX / (int)quadBytes
        || (((uintptr_t)context | (uintptr_t)quads) & 3u)
        || (uintptr_t)quads > ~(uintptr_t)0 - (uintptr_t)count * quadBytes
        || (uintptr_t)context > ~(uintptr_t)0 - contextBytes
        || InsideBeginEnd || Geometry.IsPending() || GLContext.InDListDef()
        || !GLContext.UsesNormalFramePacket()
        || GetUserPrimRequirements(primitive) != prop
        || GetUserPrimReqMask(primitive) != ~(uint64_t)0xffffffff
        || GLContext.GetImmLighting().GetLightingEnabled()
        || !GLContext.GetTexManager().GetTexEnabled()) return false;

    CImmDrawContext& draw = GLContext.GetImmDrawContext();
    // An exact copy of the last admitted source context is already finite
    // and well formed. This does not imply its VU retention, checked later.
    const bool sameRoadContext = renderer->MatchesSourceContext(context);
    if (format != 0) {
        const PGLBillboardContext& source = *(const PGLBillboardContext*)context;
        validContext = sameRoadContext || BillboardContextValid(source);
        near = source.clip[0];
    } else {
        const PGLPoolContext& source = *(const PGLPoolContext*)context;
        validContext = sameRoadContext || RoadContextValid(source);
        near = source.clipParams[0];
    }
    if (!renderer->IsCodeValid() || !validContext
        || draw.GetFogEnabled() || draw.GetDoCullFace() || draw.GetEdgeAAEnabled()
        || draw.GetDoClipping()
        || draw.GetPolygonMode() != GL_FILL || !HudFiniteFloat(draw.GetDepthOffset())
        || near != draw.GetClipNear()) return false;

    const cpu_mat_44& model = GLContext.GetModelViewStack().GetTop();
    if (!DirectIdentityColumn(model.get_col0(), 1, 0, 0, 0)
        || !DirectIdentityColumn(model.get_col1(), 0, 1, 0, 0)
        || !DirectIdentityColumn(model.get_col2(), 0, 0, 1, 0)
        || !DirectIdentityColumn(model.get_col3(), 0, 0, 0, 1)) return false;
    const cpu_mat_44& xform = draw.GetVertexXform();
    if (!DirectFiniteColumn(xform.get_col0()) || !DirectFiniteColumn(xform.get_col1())
        || !DirectFiniteColumn(xform.get_col2()) || !DirectFiniteColumn(xform.get_col3()))
        return false;

    // At <=1024x1024 and <=32bpp an image has <=9 IMAGE chunks: <=34
    // chain qwords, plus one <=256-entry CLUT, 8 texture and 14 draw settings.
    // 256q covers these, both GS fences, <=8 MPG tags, full 72q context and
    // command padding. Each activation needs <=16q overhead beyond its source
    // descriptors. Keep another 16q for the ordinary EndGeometry trailer.
    if (!DirectTextureValid(&GLContext.GetTexManager().GetCurTexture())) return false;
    // The count guard above limits count to INT_MAX/48 or INT_MAX/64.
    // Both numerators fit unsigned int; these formats use only 32 or 24.
    // Keep the following footprint and alias arithmetic at its original width.
    const uint64_t batches = batchLimit == 32u
        ? ((unsigned int)count + 31u) / 32u
        : ((unsigned int)count + 23u) / 24u;
    const uint64_t words = ((uint64_t)count * (quadBytes / 16u) + batches * 16u + 272u) * 4u;
    CVifSCDmaPacket& packet = GLContext.GetVif1Packet();
    if (words > UINT_MAX || !packet.GetTTE() || packet.HasOpenTag()
        || !packet.CanReserveWords((unsigned int)words)) return false;

    // Inputs may not alias writes made before the owned descriptor copy.
    // Include cached/uncached aliases, as in the paired decal API.
    const uintptr_t writeBegin = (uintptr_t)Core::MakePtrNormal(packet.GetNextPtr());
    const uint64_t writeEnd = (uint64_t)writeBegin + words * sizeof(uint32_t);
    const uintptr_t quadBegin = (uintptr_t)Core::MakePtrNormal(quads);
    const uint64_t quadEnd = (uint64_t)quadBegin + (uint64_t)count * quadBytes;
    const uintptr_t contextBegin = (uintptr_t)Core::MakePtrNormal(context);
    const uint64_t contextEnd = (uint64_t)contextBegin + contextBytes;
    if (((uint64_t)quadBegin < writeEnd && quadEnd > writeBegin)
        || ((uint64_t)contextBegin < writeEnd && contextEnd > writeBegin)) return false;

    // Admission is complete. All following operations append this material
    // without a fallible partial publication or a pending borrowed block.
    renderer->SetSourceContext(context, sameRoadContext);
    PrimChanged(primitive);
    DrawingLinearArray();
    // These programs read descriptor RGBA, not material/light slots.
    // Preserve an inherited COLOR_MATERIAL enable; no color array is supplied.
    SyncColorMaterial(false);
    SyncRenderer();
    SyncRendererContext(primitive);
    SyncGsContext();
    renderer->DrawCompactGroundQuads((const float*)quads, count, quadBytes / 4u, batchLimit);
    return true;
}

bool CImmGeomManager::DrawPoolQuads(const PGLPoolContext* context,
    const PGLPoolQuad* quads, int count)
{
    return DrawSourceViewQuads(context, quads, count, 0);
}

GLboolean pglDrawPoolQuads(const PGLPoolContext* context,
    const PGLPoolQuad* quads, GLsizei count)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawPoolQuads(context, quads, count)
        ? GL_TRUE : GL_FALSE;
}

bool CImmGeomManager::DrawBillboardQuads(const PGLBillboardContext* context,
    const PGLBillboardQuad* quads, int count)
{
    return DrawSourceViewQuads(context, quads, count, 1);
}

GLboolean pglDrawBillboardQuads(const PGLBillboardContext* context,
    const PGLBillboardQuad* quads, GLsizei count)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawBillboardQuads(context, quads, count)
        ? GL_TRUE : GL_FALSE;
}

bool CImmGeomManager::DrawBillboardAlphaQuads(const PGLBillboardContext* context,
    const PGLBillboardAlphaQuad* quads, int count)
{
    return DrawSourceViewQuads(context, quads, count, 2);
}

GLboolean pglDrawBillboardAlphaQuads(const PGLBillboardContext* context,
    const PGLBillboardAlphaQuad* quads, GLsizei count)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawBillboardAlphaQuads(context, quads, count)
        ? GL_TRUE : GL_FALSE;
}

static bool DecalContextValid(const PGLDecalContext& context, const CImmDrawContext& draw)
{
    const float* values = (const float*)&context;
    for (unsigned int i = 0; i < sizeof(context) / sizeof(float); ++i)
        if (!HudFiniteFloat(values[i])) return false;
    float raster[3];
    draw.GetRasterScale(raster);
    if (context.raster[0] != raster[0] || context.raster[1] != raster[1]
        || context.raster[2] != raster[2] || context.raster[0] <= 0.0f
        || context.raster[1] >= 0.0f || context.raster[3] <= 0.0f
        || (context.inverse[3] != 65535.0f && context.inverse[3] != 16777215.0f)
        || context.inverse[3] != -2.0f * raster[2]
        || context.inverse[0] <= 0.0f || context.inverse[1] >= 0.0f
        || context.inverse[2] <= 0.0f || context.depth[0] <= 0.0f
        || context.depth[1] < 0.0f || context.depth[2] < context.depth[1]
        || context.depth[3] != 0.0f
        || context.clip[0] != (float)draw.GetFBWidth() - 0.0625f
        || context.clip[1] != (float)draw.GetFBHeight() - 0.0625f
        || context.clip[2] != 0.0f || context.clip[3] != 0.0f) return false;
    return true;
}

bool CImmGeomManager::DrawDecalQuads(const PGLDecalContext* context,
    const PGLDecalQuad* quads, int count, GLuint baseTexture, GLuint glowTexture)
{
    const PGLDecalRun run = { quads, count, PGL_DECAL_RUN_QUADS };
    return DrawDecalRuns(context, &run, 1, baseTexture, glowTexture);
}

bool CImmGeomManager::DrawDecalRuns(const PGLDecalContext* context,
    const PGLDecalRun* runs, int runCount, GLuint baseTexture, GLuint glowTexture)
{
    return DrawDecalRunsRegionV(context, runs, NULL, runCount, baseTexture, glowTexture);
}

static void DirectDecalRegionV(CMMTexture* texture, const PGLDecalRegionV& region)
{
    // Admission proved a resident context-1 64x128 packed atlas (RGBA32 or its
    // PSMT8 twin, whose palette is pinned in the same pack) and the bounds.
    // No pending Geometry exists in this direct API. The next SyncGsContext
    // fences preceding VU output and owns a complete settings copy, including
    // CLAMP_1/TEX1_1/MIPTBP1_1/2_1. No microcode texture prefix replaces it.
    texture->ClearRegion();
    texture->SetWrapMode(GS::TexWrapMode::kRepeat, GS::TexWrapMode::kClamp);
    texture->SetRegion(0, (uint32_t)region.min_v, 64,
        (uint32_t)(region.max_v - region.min_v + 1));
}

bool CImmGeomManager::DrawDecalRunsRegionV(const PGLDecalContext* context,
    const PGLDecalRun* runs, const PGLDecalRegionV* regions, int runCount,
    GLuint baseTexture, GLuint glowTexture)
{
    return DrawDecalRunsSampling(context, runs, regions, NULL, runCount,
        baseTexture, glowTexture);
}

bool CImmGeomManager::DrawDecalRunsMaterials(const PGLDecalContext* context,
    const PGLDecalRun* runs, const unsigned char* const* materials, int runCount,
    GLuint baseTexture, GLuint glowTexture)
{
    if (!materials || !(pglGetDecalSubmissionOptions() & 256u)) return false;
    return DrawDecalRunsSampling(context, runs, NULL, materials, runCount,
        baseTexture, glowTexture);
}

bool CImmGeomManager::DrawDecalRunsSampling(const PGLDecalContext* context,
    const PGLDecalRun* runs, const PGLDecalRegionV* regions,
    const unsigned char* const* materials, int runCount,
    GLuint baseTexture, GLuint glowTexture)
{
    // Admit the complete pair before any packet, texture or material mutation.
    // Generated descriptor values/headroom are an explicit caller contract.
    if (!context || !runs || (regions && materials) || runCount <= 0 || runCount > 512
        || (((uintptr_t)context | (uintptr_t)runs) & 3u)
        || (uintptr_t)runs > ~(uintptr_t)0 - (uintptr_t)runCount * sizeof(*runs)
        || (uintptr_t)context > ~(uintptr_t)0 - sizeof(*context)
        || InsideBeginEnd || Geometry.IsPending() || GLContext.InDListDef()
        || !GLContext.UsesNormalFramePacket()
        || !RendererManager.CanSelectDecalRenderer()
        || GetUserPrimRequirements(PGL_CLIP_DECAL_QUADS_X2E) != PGL_CLIP_DECAL_X2E_PROP
        || GetUserPrimReqMask(PGL_CLIP_DECAL_QUADS_X2E) != ~(uint64_t)0xffffffff
        || GLContext.GetImmLighting().GetLightingEnabled()
        || !GLContext.GetTexManager().GetTexEnabled()) return false;

    if (regions && (((uintptr_t)regions & 3u)
        || (uintptr_t)regions > ~(uintptr_t)0 - (uintptr_t)runCount * sizeof(*regions)))
        return false;
    if (materials && (((uintptr_t)materials & 3u)
        || (uintptr_t)materials > ~(uintptr_t)0
            - (uintptr_t)runCount * sizeof(*materials))) return false;

    uint64_t records = 0;
    uint64_t batches = 0;
    for (int i = 0; i < runCount; ++i) {
        const PGLDecalRun& run = runs[i];
        if (!run.records || run.count <= 0 || run.count > INT_MAX / 144
            || ((uintptr_t)run.records & 3u)
            || (uintptr_t)run.records > ~(uintptr_t)0
                - (uintptr_t)run.count * sizeof(PGLDecalQuad)
            || (run.format != PGL_DECAL_RUN_QUADS
                && run.format != PGL_DECAL_RUN_NDC_TRIANGLES))
            return false;
        if (regions && (regions[i].min_v < 0
            || regions[i].max_v < regions[i].min_v || regions[i].max_v >= 128))
            return false;
        if (materials) {
            if (!materials[i] || (uintptr_t)materials[i] > ~(uintptr_t)0
                - (uintptr_t)run.count) return false;
            for (int j = 0; j < run.count; ++j) {
                if (materials[i][j] > 3u) return false;
            }
        }
        records += (unsigned int)run.count;
        batches += ((unsigned int)run.count + 15u) / 16u;
    }

    CClipDecalX2ERenderer* renderer = RendererManager.GetDecalRenderer();
    CImmDrawContext& draw = GLContext.GetImmDrawContext();
    if (!renderer->IsCodeValid() || !DecalContextValid(*context, draw)
        || draw.GetDoCullFace() || draw.GetEdgeAAEnabled() || draw.GetDoClipping()
        || draw.GetAlphaTestEnabled() || draw.GetBlendEnabled()
        || draw.GetPolygonMode() != GL_FILL || draw.GetDepthOffset() != 0.0f
        || draw.GetClipNear() != 1.0f || !draw.GetDepthTestEnabled()
        || draw.GetDrawEnv().GetDepthWriteEnabled()
        || !draw.GetDrawEnv().HasDepthTestPassMode(GS::ZTest::kGEqual)) return false;

    const cpu_mat_44& model = GLContext.GetModelViewStack().GetTop();
    const cpu_mat_44& projection = GLContext.GetProjectionStack().GetTop();
    if (!DirectIdentityColumn(model.get_col0(), 1, 0, 0, 0)
        || !DirectIdentityColumn(model.get_col1(), 0, 1, 0, 0)
        || !DirectIdentityColumn(model.get_col2(), 0, 0, 1, 0)
        || !DirectIdentityColumn(model.get_col3(), 0, 0, 0, 1)
        || !DirectIdentityColumn(projection.get_col0(), 1, 0, 0, 0)
        || !DirectIdentityColumn(projection.get_col1(), 0, 1, 0, 0)
        || !DirectIdentityColumn(projection.get_col2(), 0, 0, 1, 0)
        || !DirectIdentityColumn(projection.get_col3(), 0, 0, 0, 1)) return false;
    const cpu_mat_44& xform = draw.GetVertexXform();
    if (!DirectFiniteColumn(xform.get_col0()) || !DirectFiniteColumn(xform.get_col1())
        || !DirectFiniteColumn(xform.get_col2()) || !DirectFiniteColumn(xform.get_col3()))
        return false;
    CTexManager& textures = GLContext.GetTexManager();
    if (!DirectTextureValid(textures.FindNamedTexture(baseTexture))
        || (glowTexture && !DirectTextureValid(textures.FindNamedTexture(glowTexture))))
        return false;
    CMMTexture* regionTextures[2] = { NULL, NULL };
    unsigned int regionPasses = 0;
    if (regions || materials) {
        const GLuint names[2] = { baseTexture, glowTexture };
        for (unsigned int material = 0; material < 2; ++material) {
            if (names[material] && pgl_texture_has_packed_atlas(names[material])) {
                regionTextures[material] = textures.FindNamedTexture(names[material]);
                if (regionTextures[material]->GetContext() != GS::kContext1) return false;
                ++regionPasses;
            }
        }
        if (!regionPasses) return false;
    }

    // Retain the two-copy bound even when glow reuses the owned base payload:
    // each sweep needs <=9q/descriptor and <=16q/activation. A deliberately
    // conservative 512q/sweep covers texture IMAGE/CLUT chains, GS settings,
    // MPG tags, full context and padding; 16q reserves EndGeometry's trailer.
    // Both textures were checked before this reservation, so a later lookup
    // or incompatible second material cannot cause partial fallback.
    const uint64_t passes = glowTexture ? 2u : 1u;
    // Additional region changes need only an ordered resident packed-atlas
    // texture sync (RGBA32, or PSMT8 whose CLUT is pinned in the same pack):
    // seven GS registers + GIF tag, CNT/VIF padding and the VIF fence.
    // 32q bounds each change; the original 512q/sweep owns its first sync.
    // The packed-owner proof excludes image/CLUT uploads on these changes.
    const uint64_t regionWords = (uint64_t)(regions ? regionPasses : 0u) *
        (unsigned int)(runCount - 1) * 32u * 4u;
    const uint64_t words = (passes * (records * 9u + batches * 16u + 512u)
        + 16u) * 4u + regionWords;
    CVifSCDmaPacket& packet = GLContext.GetVif1Packet();
    if (words > UINT_MAX || !packet.GetTTE() || packet.HasOpenTag()
        || !packet.CanReserveWords((unsigned int)words)) return false;

    // Stack-local metadata belongs to this one synchronous base/glow pair.
    // It cannot survive into another call, packet, frame or renderer epoch.
    // Payload bytes themselves live in the append-only normal frame packet.
    if (glowTexture && batches > CClipDecalX2ERenderer::kMaxOwnedPayloadBatches)
        return false;
    const float* ownedPayloads[CClipDecalX2ERenderer::kMaxOwnedPayloadBatches];
    // Reject either input aliasing the future reserved write range, including
    // cached/uncached aliases. Earlier committed packet bytes remain safe.
    const uintptr_t writeBegin = (uintptr_t)Core::MakePtrNormal(packet.GetNextPtr());
    const uint64_t writeEnd = (uint64_t)writeBegin + words * sizeof(uint32_t);
    const uintptr_t runBegin = (uintptr_t)Core::MakePtrNormal(runs);
    const uint64_t runEnd = (uint64_t)runBegin + (uint64_t)runCount * sizeof(*runs);
    const uintptr_t contextBegin = (uintptr_t)Core::MakePtrNormal(context);
    const uint64_t contextEnd = (uint64_t)contextBegin + sizeof(*context);
    if (((uint64_t)runBegin < writeEnd && runEnd > writeBegin)
        || ((uint64_t)contextBegin < writeEnd && contextEnd > writeBegin)) return false;
    if (regions) {
        const uintptr_t regionBegin = (uintptr_t)Core::MakePtrNormal(regions);
        const uint64_t regionEnd = (uint64_t)regionBegin
            + (uint64_t)runCount * sizeof(*regions);
        if ((uint64_t)regionBegin < writeEnd && regionEnd > writeBegin) return false;
    }
    if (materials) {
        const uintptr_t materialBegin = (uintptr_t)Core::MakePtrNormal(materials);
        const uint64_t materialEnd = (uint64_t)materialBegin
            + (uint64_t)runCount * sizeof(*materials);
        if ((uint64_t)materialBegin < writeEnd && materialEnd > writeBegin) return false;
    }
    for (int i = 0; i < runCount; ++i) {
        const uintptr_t sourceBegin = (uintptr_t)Core::MakePtrNormal(runs[i].records);
        const uint64_t sourceEnd = (uint64_t)sourceBegin
            + (uint64_t)runs[i].count * sizeof(PGLDecalQuad);
        if ((uint64_t)sourceBegin < writeEnd && sourceEnd > writeBegin) return false;
        if (materials) {
            const uintptr_t materialBegin = (uintptr_t)Core::MakePtrNormal(materials[i]);
            const uint64_t materialEnd = (uint64_t)materialBegin + runs[i].count;
            if ((uint64_t)materialBegin < writeEnd && materialEnd > writeBegin) return false;
        }
    }

    for (unsigned int pass = 0; pass < (unsigned int)passes; ++pass) {
        if (pass) {
            draw.SetFogEnabled(false);
            draw.SetBlendEnabled(true);
            draw.SetBlendMode(GL_SRC_ALPHA, GL_ONE);
        }
        textures.BindTexture(pass ? glowTexture : baseTexture);
        if (regionTextures[pass]) {
            PGLDecalRegionV firstRegion;
            if (regions) firstRegion = regions[0];
            else {
                firstRegion.min_v = (int)materials[0][0] * 32;
                firstRegion.max_v = firstRegion.min_v + 31;
            }
            DirectDecalRegionV(regionTextures[pass], firstRegion);
            GLContext.TextureChanged();
        }
        renderer->SetDecalContext(*context, pass != 0,
            materials && regionTextures[pass]);
        PrimChanged(PGL_CLIP_DECAL_QUADS_X2E);
        DrawingLinearArray();
        SyncColorMaterial(false);
        SyncRenderer();
        SyncRendererContext(PGL_CLIP_DECAL_QUADS_X2E);
        // The normal GS synchronization fences the complete preceding base
        // sweep before replacing its texture or enabling additive blending.
        SyncGsContext();
        unsigned int batchOffset = 0;
        for (int i = 0; i < runCount; ++i) {
            const PGLDecalRun& run = runs[i];
            if (i && regions && regionTextures[pass]
                && (regions[i].min_v != regions[i - 1].min_v
                    || regions[i].max_v != regions[i - 1].max_v)) {
                DirectDecalRegionV(regionTextures[pass], regions[i]);
                GLContext.TextureChanged();
                SyncGsContext();
            }
            renderer->DrawDecalRecords(run.records, run.count, run.format,
                glowTexture ? ownedPayloads + batchOffset : NULL, pass != 0,
                materials && regionTextures[pass] ? materials[i] : NULL);
            batchOffset += ((unsigned int)run.count + 15u) / 16u;
        }
        // PATH1 changed CLAMP without changing the texture object's sampler.
        // Force the next GS texture use (including the glow sweep) to restore
        // its complete cached settings after the normal completion fence.
        if (materials && regionTextures[pass]) GLContext.TextureChanged();
    }
    return true;
}

GLboolean pglDrawDecalRuns(const PGLDecalContext* context,
    const PGLDecalRun* runs, GLsizei runCount, GLuint baseTexture, GLuint glowTexture)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawDecalRuns(
        context, runs, runCount, baseTexture, glowTexture) ? GL_TRUE : GL_FALSE;
}

GLboolean pglDrawDecalRunsRegionV(const PGLDecalContext* context,
    const PGLDecalRun* runs, const PGLDecalRegionV* regions, GLsizei runCount,
    GLuint baseTexture, GLuint glowTexture)
{
    if (!pGLContext || !regions) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawDecalRunsRegionV(
        context, runs, regions, runCount, baseTexture, glowTexture) ? GL_TRUE : GL_FALSE;
}

GLboolean pglDrawDecalRunsMaterials(const PGLDecalContext* context,
    const PGLDecalRun* runs, const unsigned char* const* materials, GLsizei runCount,
    GLuint baseTexture, GLuint glowTexture)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawDecalRunsMaterials(
        context, runs, materials, runCount, baseTexture, glowTexture) ? GL_TRUE : GL_FALSE;
}

GLboolean pglDrawDecalQuads(const PGLDecalContext* context,
    const PGLDecalQuad* quads, GLsizei count, GLuint baseTexture, GLuint glowTexture)
{
    if (!pGLContext) return GL_FALSE;
    return pGLContext->GetImmGeomManager().DrawDecalQuads(
        context, quads, count, baseTexture, glowTexture) ? GL_TRUE : GL_FALSE;
}

bool CImmGeomManager::TryDrawTexturedQuads2DArrays(const float* vertices, const float* texcoords,
    int count, int wordsPerVertex)
{
    // Borrow only the already-selected stock uniform-color quad route. Do
    // not predict or select a NEXT renderer while OLD geometry is pending.
    // Match the copied API's linked input width on every accepted call.
    if (InsideBeginEnd || !vertices || !texcoords || count <= 0 || count > INT_MAX / 64 ||
        (((uintptr_t)vertices | (uintptr_t)texcoords) & 3u) != 0 ||
        wordsPerVertex != 3 ||
        Prim != GL_QUADS || !RendererManager.CanReuseUnlitQuadRenderer() ||
        GLContext.GetImmLighting().GetLightingEnabled() ||
        GLContext.GetMaterialManager().GetColorMaterialEnabled() ||
        !GLContext.GetTexManager().GetTexEnabled())
        return false;

    const uintptr_t vertexBytes = (uintptr_t)count * 4u * wordsPerVertex * sizeof(float);
    const uintptr_t texcoordBytes = (uintptr_t)count * 8u * sizeof(float);
    if ((uintptr_t)vertices > ~(uintptr_t)0 - vertexBytes ||
        (uintptr_t)texcoords > ~(uintptr_t)0 - texcoordBytes)
        return false;

    // No client-array state or immediate buffer cursor changes. These spans
    // can be referenced later by VIF DMA; the caller owns their full lifetime
    // and cache writeback, including when they remain pending past this call.
    CommitTexturedQuadArrays(vertices, texcoords, count, wordsPerVertex);
    CurTexCoord[0] = texcoords[count * 8 - 2];
    CurTexCoord[1] = texcoords[count * 8 - 1];
    return true;
}

GLboolean pglTryDrawTexturedQuads2DArrays(const GLfloat* vertices, const GLfloat* texcoords,
    GLsizei count, GLint wordsPerVertex)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawTexturedQuads2DArrays(
        vertices, texcoords, count, wordsPerVertex) ? GL_TRUE : GL_FALSE;
}

GLboolean pglTryDrawTexturedQuads2D(const GLfloat* quads, GLsizei count)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawTexturedQuads2D(quads, count)
        ? GL_TRUE : GL_FALSE;
}

GLboolean pglTryDrawTexturedQuadCorners2D(const GLfloat* quads, GLsizei count)
{
    if (!pGLContext || pGLContext->InDListDef()) return GL_FALSE;
    return pGLContext->GetImmGeomManager().TryDrawTexturedQuads2D(quads, count, true)
        ? GL_TRUE : GL_FALSE;
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

    Geometry.AdjustNewGeomPtrs(first, mode == PGL_CLIP_TRIANGLES_X2C);

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
            CImmDrawContext& draw = GLContext.GetImmDrawContext();
            draw.GetDrawEnv().SendSettingsForBlend(packet, draw.GetBlendEnabled(),
                pglZeroAlphaDiscardOwner == &GLContext && !draw.GetEdgeAAEnabled());
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
    if (pGLContext) pGLContext->GeometryFlushPoint();
}
