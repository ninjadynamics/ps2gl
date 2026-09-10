/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include <stdio.h>

#include "GL/ps2gl.h"

#include "ps2s/cpu_matrix.h"
#include "ps2s/displayenv.h"
#include "ps2s/math.h"
#include "ps2s/packet.h"

#include "ps2gl/clear.h"
#include "ps2gl/debug.h"
#include "ps2gl/dlist.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/gmanager.h"
#include "ps2gl/matrix.h"

/********************************************
 * class CVertArray
 */

CVertArray::CVertArray()
{
    Vertices = Normals = TexCoords = Colors = NULL;
    VerticesAreValid = NormalsAreValid = TexCoordsAreValid = ColorsAreValid = false;
    WordsPerVertex = WordsPerTexCoord = WordsPerColor = 0;
    WordsPerNormal                                    = 3; // not set by NormalPointer
}

/********************************************
 * class CGeomManager
 */

// static members

CVertArray* CGeomManager::VertArray;

tUserPrimEntry CGeomManager::UserPrimTypes[kMaxUserPrimTypes];

bool CGeomManager::DoNormalize = false;

CGeomManager::CGeomManager(CGLContext& context)
    : GLContext(context)
    , CurNormal(0.0f, 0.0f, 1.0f)
    , Prim(GL_INVALID_VALUE)
    , InsideBeginEnd(false)
    , LastArrayAccessWasIndexed(false)
    , LastArrayAccessIsValid(false)
    , UserRenderContextChanged(false)
{
    for (unsigned int i               = 0; i < kMaxUserPrimTypes; i++)
        UserPrimTypes[i].requirements = 0xffffffff;
}

/********************************************
 * gl api
 */

/**
 * @defgroup gl_api gl* API
 *
 * Differences between ps2gl gl* functions and the usual ones.
 * (If a gl* function is called with parameters that are
 * unsupported/broken, it should say so.)
 *
 * @{
 */

/**
 * @param size 2, 3, or 4
 * @param type must be GL_FLOAT
 * @param stride must be <b>zero</b>.  Non-zero strides are unsupported and likely
 * to remain so.
 */
void glVertexPointer(GLint size, GLenum type,
    GLsizei stride, const GLvoid* ptr)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    if (stride != 0) {
        mNotImplemented("stride must be 0");
        return;
    }
    if (type != GL_FLOAT) {
        mNotImplemented("type must be float");
        return;
    }

    CVertArray& vertArray = pGLContext->GetGeomManager().GetVertArray();
    vertArray.SetVertices((void*)ptr);
    vertArray.SetWordsPerVertex(size);
}

/**
 * @param type must be GL_FLOAT
 * @param stride must be <b>zero</b>.  Non-zero strides are unsupported and likely
 * to remain so.
 */
void glNormalPointer(GLenum type, GLsizei stride,
    const GLvoid* ptr)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    pglNormalPointer(3, type, stride, ptr);
}

/**
 * @param size 2, 3, or 4
 * @param type must be GL_FLOAT
 * @param stride must be <b>zero</b>.  Non-zero strides are unsupported and likely
 * to remain so.
 */
void glTexCoordPointer(GLint size, GLenum type,
    GLsizei stride, const GLvoid* ptr)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    if (stride != 0) {
        mNotImplemented("stride must be 0");
        return;
    }
    if (type != GL_FLOAT) {
        mNotImplemented("type must be float");
        return;
    }

    CVertArray& vertArray = pGLContext->GetGeomManager().GetVertArray();
    vertArray.SetTexCoords((void*)ptr);
    vertArray.SetWordsPerTexCoord(size);
}

/**
 * @param size 3 or 4
 * @param type must be GL_FLOAT
 * @param stride must be <b>zero</b>.  Non-zero strides are unsupported and likely
 * to remain so.
 */
void glColorPointer(GLint size, GLenum type,
    GLsizei stride, const GLvoid* ptr)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    if (stride != 0) {
        mNotImplemented("stride must be 0");
        return;
    }
    if (type != GL_FLOAT) {
        // HyperSolar X2D ONLY: the wall-descriptor COLOR stream rides
        // GL_UNSIGNED_BYTE (V4-8 unpack, 1 word per element — see
        // PGL_CLIP_TRIANGLES_X2D in GL/ps2gl.h). Every other renderer's
        // color contract stays GL_FLOAT; do not feed byte colors to the
        // classic/lit paths.
        if (type == GL_UNSIGNED_BYTE && size == 4) {
            CVertArray& va = pGLContext->GetGeomManager().GetVertArray();
            va.SetColors((void*)ptr);
            va.SetWordsPerColor(1);
            return;
        }
        mNotImplemented("type must be float");
        return;
    }

    CVertArray& vertArray = pGLContext->GetGeomManager().GetVertArray();
    vertArray.SetColors((void*)ptr);
    vertArray.SetWordsPerColor(size);
}

/**
 * The important thing to remember with DrawArrays() is that <b>array data
 * is not copied (mostly)</b>.  Since the only rendering mode supported now is
 * delayed one frame, this means that the app must <b>double-buffer geometry</b>
 * when it changes.  The "mostly" above is because little bits of the array
 * will become part of the dma chain, so modifying the data referenced by a
 * display list won't work as expected.  (This would be really useful and should
 * be made possible.)
 *
 * There is no limit on strip lengths (make them as long as possible!).
 */
void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.DrawArrays(mode, first, count);
}

/**
 * This is not implemented yet
 */
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mError("glDrawElements is a placeholder ATM and should not be called");
}

/**
 * This is not implemented yet
 */
void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid* pointer)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mError("glInterleavedArrays is a placeholder ATM and should not be called");
}

/**
 * This is not implemented yet
 */
void glArrayElement(GLint i)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mError("glArrayElement is a placeholder ATM and should not be called");
}

/**
 * Flushes the internal geometry buffers.
 */
void glFlush(void)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.Flush();
}

/** @} */ // gl_api

void glEnableClientState(GLenum cap)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    CVertArray& vertArray  = gmanager.GetVertArray();

    switch (cap) {
    case GL_NORMAL_ARRAY:
        vertArray.SetNormalsValid(true);
        break;
    case GL_VERTEX_ARRAY:
        vertArray.SetVerticesValid(true);
        break;
    case GL_COLOR_ARRAY:
        vertArray.SetColorsValid(true);
        break;
    case GL_TEXTURE_COORD_ARRAY:
        vertArray.SetTexCoordsValid(true);
        break;

    case GL_INDEX_ARRAY:
    case GL_EDGE_FLAG_ARRAY:
        mNotImplemented("capability = %d", cap);
        break;
    }
}

void glDisableClientState(GLenum cap)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    CVertArray& vertArray  = gmanager.GetVertArray();

    switch (cap) {
    case GL_NORMAL_ARRAY:
        vertArray.SetNormalsValid(false);
        break;
    case GL_VERTEX_ARRAY:
        vertArray.SetVerticesValid(false);
        break;
    case GL_COLOR_ARRAY:
        vertArray.SetColorsValid(false);
        break;
    case GL_TEXTURE_COORD_ARRAY:
        vertArray.SetTexCoordsValid(false);
        break;

    case GL_INDEX_ARRAY:
    case GL_EDGE_FLAG_ARRAY:
        mNotImplemented("capability = %d", cap);
        break;
    }
}

void glBegin(GLenum mode)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.BeginGeom(mode);
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.Normal(cpu_vec_xyz(x, y, z));
}

void glNormal3fv(const GLfloat* v)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glNormal3f(v[0], v[1], v[2]);
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.Vertex(cpu_vec_xyzw(x, y, z, w));
}

void glVertex4fv(const GLfloat* vertex)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f(vertex[0], vertex[1], vertex[2], vertex[3]);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f(x, y, z, 1.0f);
}

void glVertex3fv(const GLfloat* vertex)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f(vertex[0], vertex[1], vertex[2], 1.0f);
}

void glVertex2f(GLfloat x, GLfloat y)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f(x, y, 0.0f, 1.0f);
}

void glVertex2i(GLint x, GLint y)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f((GLfloat)x, (GLfloat)y, 0.0f, 1.0f);
}

void glVertex2fv(const GLfloat* vertex)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glVertex4f(vertex[0], vertex[1], 0.0f, 1.0f);
}

void glTexCoord2f(GLfloat u, GLfloat v)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.TexCoord(u, v);
}

void glTexCoord2fv(const GLfloat* texCoord)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexCoord2f(texCoord[0], texCoord[1]);
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.Color(cpu_vec_xyzw(red, green, blue, 1.0f));
}

void glColor3fv(const GLfloat* color)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glColor3f(color[0], color[1], color[2]);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.Color(cpu_vec_xyzw(red, green, blue, alpha));
}

#if PGL_COLOR4UB_LUT
// Exact float results of the original (float)byte / 255.0 expression.
// Unsuffixed 255.0 made each channel execute a software double division and
// two conversions on the EE. These constants retain all 256 rounded values;
// do not replace them with byte * reciprocal (a different rounding path).
static const float pglColorByteToFloat[256] = {
    0.0000000000000000f, 0.0039215688593685627f, 0.0078431377187371254f, 0.011764706112444401f,
    0.015686275437474251f, 0.019607843831181526f, 0.023529412224888802f, 0.027450980618596077f,
    0.031372550874948502f, 0.035294119268655777f, 0.039215687662363052f, 0.043137256056070328f,
    0.047058824449777603f, 0.050980392843484879f, 0.054901961237192154f, 0.058823529630899429f,
    0.062745101749897003f, 0.066666670143604279f, 0.070588238537311554f, 0.074509806931018829f,
    0.078431375324726105f, 0.082352943718433380f, 0.086274512112140656f, 0.090196080505847931f,
    0.094117648899555206f, 0.098039217293262482f, 0.10196078568696976f, 0.10588235408067703f,
    0.10980392247438431f, 0.11372549086809158f, 0.11764705926179886f, 0.12156862765550613f,
    0.12549020349979401f, 0.12941177189350128f, 0.13333334028720856f, 0.13725490868091583f,
    0.14117647707462311f, 0.14509804546833038f, 0.14901961386203766f, 0.15294118225574493f,
    0.15686275064945221f, 0.16078431904315948f, 0.16470588743686676f, 0.16862745583057404f,
    0.17254902422428131f, 0.17647059261798859f, 0.18039216101169586f, 0.18431372940540314f,
    0.18823529779911041f, 0.19215686619281769f, 0.19607843458652496f, 0.20000000298023224f,
    0.20392157137393951f, 0.20784313976764679f, 0.21176470816135406f, 0.21568627655506134f,
    0.21960784494876862f, 0.22352941334247589f, 0.22745098173618317f, 0.23137255012989044f,
    0.23529411852359772f, 0.23921568691730499f, 0.24313725531101227f, 0.24705882370471954f,
    0.25098040699958801f, 0.25490197539329529f, 0.25882354378700256f, 0.26274511218070984f,
    0.26666668057441711f, 0.27058824896812439f, 0.27450981736183167f, 0.27843138575553894f,
    0.28235295414924622f, 0.28627452254295349f, 0.29019609093666077f, 0.29411765933036804f,
    0.29803922772407532f, 0.30196079611778259f, 0.30588236451148987f, 0.30980393290519714f,
    0.31372550129890442f, 0.31764706969261169f, 0.32156863808631897f, 0.32549020648002625f,
    0.32941177487373352f, 0.33333334326744080f, 0.33725491166114807f, 0.34117648005485535f,
    0.34509804844856262f, 0.34901961684226990f, 0.35294118523597717f, 0.35686275362968445f,
    0.36078432202339172f, 0.36470589041709900f, 0.36862745881080627f, 0.37254902720451355f,
    0.37647059559822083f, 0.38039216399192810f, 0.38431373238563538f, 0.38823530077934265f,
    0.39215686917304993f, 0.39607843756675720f, 0.40000000596046448f, 0.40392157435417175f,
    0.40784314274787903f, 0.41176471114158630f, 0.41568627953529358f, 0.41960784792900085f,
    0.42352941632270813f, 0.42745098471641541f, 0.43137255311012268f, 0.43529412150382996f,
    0.43921568989753723f, 0.44313725829124451f, 0.44705882668495178f, 0.45098039507865906f,
    0.45490196347236633f, 0.45882353186607361f, 0.46274510025978088f, 0.46666666865348816f,
    0.47058823704719543f, 0.47450980544090271f, 0.47843137383460999f, 0.48235294222831726f,
    0.48627451062202454f, 0.49019607901573181f, 0.49411764740943909f, 0.49803921580314636f,
    0.50196081399917603f, 0.50588238239288330f, 0.50980395078659058f, 0.51372551918029785f,
    0.51764708757400513f, 0.52156865596771240f, 0.52549022436141968f, 0.52941179275512695f,
    0.53333336114883423f, 0.53725492954254150f, 0.54117649793624878f, 0.54509806632995605f,
    0.54901963472366333f, 0.55294120311737061f, 0.55686277151107788f, 0.56078433990478516f,
    0.56470590829849243f, 0.56862747669219971f, 0.57254904508590698f, 0.57647061347961426f,
    0.58039218187332153f, 0.58431375026702881f, 0.58823531866073608f, 0.59215688705444336f,
    0.59607845544815063f, 0.60000002384185791f, 0.60392159223556519f, 0.60784316062927246f,
    0.61176472902297974f, 0.61568629741668701f, 0.61960786581039429f, 0.62352943420410156f,
    0.62745100259780884f, 0.63137257099151611f, 0.63529413938522339f, 0.63921570777893066f,
    0.64313727617263794f, 0.64705884456634521f, 0.65098041296005249f, 0.65490198135375977f,
    0.65882354974746704f, 0.66274511814117432f, 0.66666668653488159f, 0.67058825492858887f,
    0.67450982332229614f, 0.67843139171600342f, 0.68235296010971069f, 0.68627452850341797f,
    0.69019609689712524f, 0.69411766529083252f, 0.69803923368453979f, 0.70196080207824707f,
    0.70588237047195435f, 0.70980393886566162f, 0.71372550725936890f, 0.71764707565307617f,
    0.72156864404678345f, 0.72549021244049072f, 0.72941178083419800f, 0.73333334922790527f,
    0.73725491762161255f, 0.74117648601531982f, 0.74509805440902710f, 0.74901962280273438f,
    0.75294119119644165f, 0.75686275959014893f, 0.76078432798385620f, 0.76470589637756348f,
    0.76862746477127075f, 0.77254903316497803f, 0.77647060155868530f, 0.78039216995239258f,
    0.78431373834609985f, 0.78823530673980713f, 0.79215687513351440f, 0.79607844352722168f,
    0.80000001192092896f, 0.80392158031463623f, 0.80784314870834351f, 0.81176471710205078f,
    0.81568628549575806f, 0.81960785388946533f, 0.82352942228317261f, 0.82745099067687988f,
    0.83137255907058716f, 0.83529412746429443f, 0.83921569585800171f, 0.84313726425170898f,
    0.84705883264541626f, 0.85098040103912354f, 0.85490196943283081f, 0.85882353782653809f,
    0.86274510622024536f, 0.86666667461395264f, 0.87058824300765991f, 0.87450981140136719f,
    0.87843137979507446f, 0.88235294818878174f, 0.88627451658248901f, 0.89019608497619629f,
    0.89411765336990356f, 0.89803922176361084f, 0.90196079015731812f, 0.90588235855102539f,
    0.90980392694473267f, 0.91372549533843994f, 0.91764706373214722f, 0.92156863212585449f,
    0.92549020051956177f, 0.92941176891326904f, 0.93333333730697632f, 0.93725490570068359f,
    0.94117647409439087f, 0.94509804248809814f, 0.94901961088180542f, 0.95294117927551270f,
    0.95686274766921997f, 0.96078431606292725f, 0.96470588445663452f, 0.96862745285034180f,
    0.97254902124404907f, 0.97647058963775635f, 0.98039215803146362f, 0.98431372642517090f,
    0.98823529481887817f, 0.99215686321258545f, 0.99607843160629272f, 1.0000000000000000f,
};
#endif

//raylib need this function
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);
#if PGL_COLOR4UB_LUT
    glColor4f(pglColorByteToFloat[red], pglColorByteToFloat[green],
        pglColorByteToFloat[blue], pglColorByteToFloat[alpha]);
#else
    float r = (float)red/255.0;
    float b = (float)blue/255.0;
    float g = (float)green/255.0;
    float a = (float)alpha/255.0;
    glColor4f(r,g,b,a);
#endif
}

void glColor4fv(const GLfloat* color)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glColor4f(color[0], color[1], color[2], color[3]);
}

void glEnd(void)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CGeomManager& gmanager = pGLContext->GetGeomManager();
    gmanager.EndGeom();
}

/********************************************
 * pgl api
 */

/**
 * @addtogroup pgl_api
 * @{
 */

/**
 * Specify a normal pointer with either 3 or 4 elements.
 * If 4-element normals are specified, the last element (w)
 * will be ignored.
 */
void pglNormalPointer(GLint size, GLenum type,
    GLsizei stride, const GLvoid* ptr)
{
    if (stride != 0) {
        mNotImplemented("stride must be 0");
        return;
    }
    if (type != GL_FLOAT) {
        mNotImplemented("type must be float");
        return;
    }

    CVertArray& vertArray = pGLContext->GetGeomManager().GetVertArray();
    vertArray.SetNormals((void*)ptr);
    vertArray.SetWordsPerNormal(size);
}

void pglDrawIndexedArrays(GLenum primType,
    int numIndices, const unsigned char* indices,
    int numVertices)
{
    pGLContext->GetGeomManager().DrawIndexedArrays(primType, numIndices, indices, numVertices);
}

/**
 * @addtogroup custom_renderers_prims_state
 * @{
 */

/**
 * Register a new primitive.  After registering a primitive with this call it can
 * be used anywhere a normal primitive can be used (glBegin, glDrawArrays, etc.).
 * Defining a new primitive usually implies writing a renderer to go along with it.
 *
 * @param primType	    bit 31 must be set (this indicates a user prim to ps2gl).  The
 * 			    lower 31 bits should be a number from 0 to
 * 			    PGL_MAX_CUSTOM_PRIM_TYPES.
 * @param requirements	    gives the bit flags to be set in the renderer
 * 			    requirements bitfield (see the documentation for custom
 * 			    renderers).  Usually this will be one bit indicating
 * 			    the prim type used to select a renderer.
 * @param rendererReqMask   a mask to be applied to the renderer requirements
 * 			    bitfield before testing against renderer capabilities.
 * 			    This could be used, for example, to mask off the
 * 			    default lower 32 bits if they are irrelevent to this
 * 			    custom primitive type.
 * @param mergeContiguous   rather calling a renderer with every block of geometry
 * 			    that is specified (with glBegin/End, DrawArrays, etc.),
 * 			    ps2gl tries to combine multiple blocks into a single
 * 			    call to the renderer.  This flag tells ps2gl whether it
 * 			    can treat blocks of geometry that were specified
 * 			    independently but are contiguous in memory as a single
 * 			    block.  (State changes will of course force them to be
 * 			    treated separately.)  This is done, for example, with
 * 			    points, lines, triangles, and quads, but not with
 * 			    strips, since merging would lose the strip boundaries.
 */
void pglRegisterCustomPrimType(GLenum primType,
    pglU64_t requirements, pglU64_t rendererReqMask, int mergeContiguous)
{
    mErrorIf(!CGeomManager::IsUserPrimType(primType), "custom prim types must have bit 31 set");
    CGeomManager::RegisterUserPrimType(primType, requirements, rendererReqMask, mergeContiguous);
}

/**
 * Enable a custom attribute/state change.  Call this to enable the corresponding
 * bit(s) in the renderer requirements bitfield (see above).  The lower 32 bits should
 * be zero.
 * @param flag the bit(s) to enable (lower 32 should be zero)
 */
void pglEnableCustom(pglU64_t flag)
{
    flag &= ~(uint64_t)0xffffffff;
    pGLContext->GetGeomManager().EnableCustom(flag);
}

/**
 * Disable a custom attribute/state change.  Call this to disable the corresponding
 * bit(s) in the renderer requirements bitfield.  The lower 32 bits should be zero.
 * @param flag the bit(s) to disable (lower 32 should be zero).  This is the same
 *             constant passed to pglEnableCustom.
 */
void pglDisableCustom(pglU64_t flag)
{
    flag &= ~(uint64_t)0xffffffff;
    pGLContext->GetGeomManager().DisableCustom(flag);
}

void pglUserRenderContextChanged()
{
    pGLContext->GetGeomManager().SetUserRenderContextChanged();
}

/** @} */

/** @} */
