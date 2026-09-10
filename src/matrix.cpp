/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include "ps2s/math.h"

#include "ps2gl/dlgmanager.h"
#include "ps2gl/dlist.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/matrix.h"

/********************************************
 * CDListMatrixStack
 */

class CMatrixPopCmd : public CDListCmd {
public:
    CMatrixPopCmd() {}
    CDListCmd* Play()
    {
        glPopMatrix();
        return CDListCmd::GetNextCmd(this);
    }
};

void CDListMatrixStack::Pop()
{
    GLContext.GetDListGeomManager().Flush();
    GLContext.GetDListManager().GetOpenDList() += CMatrixPopCmd();
    GLContext.XformChanged();
}

class CMatrixPushCmd : public CDListCmd {
public:
    CMatrixPushCmd() {}
    CDListCmd* Play()
    {
        glPushMatrix();
        return CDListCmd::GetNextCmd(this);
    }
};

void CDListMatrixStack::Push()
{
    GLContext.GetDListManager().GetOpenDList() += CMatrixPushCmd();
}

class CMatrixConcatCmd : public CDListCmd {
    cpu_mat_44 Matrix, Inverse;

public:
    CMatrixConcatCmd(const cpu_mat_44& mat, const cpu_mat_44& inv)
        : Matrix(mat)
        , Inverse(inv)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetCurMatrixStack().Concat(Matrix, Inverse);
        return CDListCmd::GetNextCmd(this);
    }
};

void CDListMatrixStack::Concat(const cpu_mat_44& xform, const cpu_mat_44& inverse)
{
    GLContext.GetDListGeomManager().Flush();
    GLContext.GetDListManager().GetOpenDList() += CMatrixConcatCmd(xform, inverse);
    GLContext.XformChanged();
}

class CMatrixSetTopCmd : public CDListCmd {
    cpu_mat_44 Matrix, Inverse;

public:
    CMatrixSetTopCmd(const cpu_mat_44& mat, const cpu_mat_44& inv)
        : Matrix(mat)
        , Inverse(inv)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetCurMatrixStack().SetTop(Matrix, Inverse);
        return CDListCmd::GetNextCmd(this);
    }
};

void CDListMatrixStack::SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv)
{
    GLContext.GetDListGeomManager().Flush();
    GLContext.GetDListManager().GetOpenDList() += CMatrixSetTopCmd(newMat, newInv);
    GLContext.XformChanged();
}

/********************************************
 * gl api
 */

void glMatrixMode(GLenum mode)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    pGLContext->SetMatrixMode(mode);
}

void glLoadIdentity(void)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();

    cpu_mat_44 ident;
    ident.set_identity();
    matStack.SetTop(ident, ident);
}

void glPushMatrix(void)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Push();
}

void glPopMatrix(void)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Pop();
}

// courtesy the lovely folks at Intel
extern void Invert2(float* mat, float* dst);

#if PGL_LAZY_MATRIX_INVERSE
void CMatrixStack::SetTopFromMatrix(const cpu_mat_44& newMat)
{
    cpu_mat_44 invMatrix;
    Invert2((float*)&newMat, (float*)&invMatrix);
    SetTop(newMat, invMatrix);
}

void CImmMatrixStack::SetTopFromMatrix(const cpu_mat_44& newMat)
{
    Matrices[CurStackDepth] = newMat;
    InverseMatrices[CurStackDepth] = newMat;
    InversePending[CurStackDepth] = true;
#if PGL_DEFER_MATRIX_CONCAT_INVERSE
    DiscardInverseOps();
#endif
    GLContext.GetImmDrawContext().SetVertexXformValid(false);
}
#endif

#if PGL_LAZY_MATRIX_INVERSE || PGL_DEFER_MATRIX_CONCAT_INVERSE
void CImmMatrixStack::EnsureInverse() const
{
#if PGL_LAZY_MATRIX_INVERSE
    if (InversePending[CurStackDepth]) {
        float* pending = (float*)&InverseMatrices[CurStackDepth];
        // Invert2 reads/transposes every input before writing any output.
        // This slot owns its base; pending journal entries may be shared.
        Invert2(pending, pending);
        InversePending[CurStackDepth] = false;
    }
#endif
#if PGL_DEFER_MATRIX_CONCAT_INVERSE
    if (InverseHead[CurStackDepth] < 0) return;

    // Reverse the predecessor chain, then apply the original left-multiplies
    // in their exact order. Inverting the final forward matrix or combining
    // operands first would change rounding, analytic inverses and singulars.
    unsigned char order[MaxPendingInverseOps];
    int count = 0;
    for (int i = InverseHead[CurStackDepth]; i >= 0; i = InverseOps[i].Previous)
        order[count++] = (unsigned char)i;

    cpu_mat_44& curInv = InverseMatrices[CurStackDepth];
    while (count) {
        const PendingInverseOp& op = InverseOps[order[--count]];
        if (op.NeedsInverse) {
            cpu_mat_44 inverse;
            // Invert only this original glMultMatrixf operand. Keep the
            // journal immutable: parent stack levels may still refer to it.
            Invert2((float*)&op.Operand, (float*)&inverse);
            curInv = inverse * curInv;
        } else {
            curInv = op.Operand * curInv;
        }
    }
    DiscardInverseOps();
#endif
}
#endif

#if PGL_DEFER_MATRIX_CONCAT_INVERSE
void CMatrixStack::ConcatFromMatrix(const cpu_mat_44& xform)
{
    // Recording a display list keeps the established command payload and
    // eager inverse calculation. Playback calls the ordinary Concat method.
    cpu_mat_44 inverse;
    Invert2((float*)&xform, (float*)&inverse);
    Concat(xform, inverse);
}

bool CImmMatrixStack::DeferInverse(const cpu_mat_44& operand, bool needsInverse)
{
    if (NumInverseOps == MaxPendingInverseOps) {
        EnsureInverse();
        // A full ancestor prefix cannot be reclaimed by this child. The
        // caller then does exactly the old eager operation; nothing drops.
        if (NumInverseOps == MaxPendingInverseOps) return false;
    }
    PendingInverseOp& op = InverseOps[NumInverseOps];
    op.Operand = operand;
    op.Previous = InverseHead[CurStackDepth];
    op.NeedsInverse = needsInverse;
    InverseHead[CurStackDepth] = NumInverseOps++;
    return true;
}

void CImmMatrixStack::ConcatFromMatrix(const cpu_mat_44& xform)
{
    if (!DeferInverse(xform, true)) {
        cpu_mat_44 inverse;
        Invert2((float*)&xform, (float*)&inverse);
        cpu_mat_44& curInv = InverseMatrices[CurStackDepth];
        curInv = inverse * curInv;
    }
    cpu_mat_44& curMat = Matrices[CurStackDepth];
    curMat = curMat * xform;
    GLContext.GetImmDrawContext().SetVertexXformValid(false);
}
#endif

void glLoadMatrixf(const GLfloat* m)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    cpu_mat_44 newMat;

    // unfortunately we can't assume the matrix is qword-aligned
    float* dest = reinterpret_cast<float*>(&newMat);
    for (int i = 0; i < 16; i++)
        *dest++ = *m++;

#if PGL_LAZY_MATRIX_INVERSE
    matStack.SetTopFromMatrix(newMat);
#else
    cpu_mat_44 invMatrix;
    Invert2((float*)&newMat, (float*)&invMatrix);
    matStack.SetTop(newMat, invMatrix);
#endif
}

void glFrustum(GLdouble left, GLdouble right,
    GLdouble bottom, GLdouble top,
    GLdouble zNear, GLdouble zFar)
{
    /*
     * NOTE:
     *   The PS2 does not support GL_LESS/GL_LEQUAL
     *   but this is what 99% of OpenGL programs use.
     *
     *   As a result depth is inverted.
     *   See glDepthFunc/glFrustum/glOrtho
     */
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    cpu_mat_44 xform(
        cpu_vec_xyzw(
            (2.0f * zNear) / (right - left),
            0.0f,
            0.0f,
            0.0f),
        cpu_vec_xyzw(
            0.0f,
            (2.0f * zNear) / (top - bottom),
            0.0f,
            0.0f),
        cpu_vec_xyzw(
            (right + left) / (right - left),
            (top + bottom) / (top - bottom),
            -(zFar + zNear) / (zFar - zNear),
            -1.0f),
        cpu_vec_xyzw(
            0.0f,
            0.0f,
            (-2.0f * zFar * zNear) / (zFar - zNear),
            0.0f)
    );

    cpu_mat_44 inv(
        cpu_vec_xyzw(
            (right - left) / (2 * zNear),
            0,
            0,
            0),
        cpu_vec_xyzw(
            0,
            (top - bottom) / (2 * zNear),
            0,
            0),
        cpu_vec_xyzw(
            0,
            0,
            0,
            -(zFar - zNear) / (2 * zFar * zNear)),
        cpu_vec_xyzw(
            (right + left) / (2 * zNear),
            (top + bottom) / (2 * zNear),
            -1,
            (zFar + zNear) / (2 * zFar * zNear))
    );

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Concat(xform, inv);
}

void glOrtho(GLdouble left, GLdouble right,
    GLdouble bottom, GLdouble top,
    GLdouble zNear, GLdouble zFar)
{
    /*
     * NOTE:
     *   The PS2 does not support GL_LESS/GL_LEQUAL
     *   but this is what 99% of OpenGL programs use.
     *
     *   As a result depth is inverted.
     *   See glDepthFunc/glFrustum/glOrtho
     */
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    cpu_mat_44 xform(
        cpu_vec_xyzw(
            (2.0f) / (right - left),
            0.0f,
            0.0f,
            0.0f),
        cpu_vec_xyzw(
            0.0f,
            (2.0f) / (top - bottom),
            0.0f,
            0.0f),
        cpu_vec_xyzw(
            0.0f,
            0.0f,
            -2 / (zFar - zNear),
            0.0f),
        cpu_vec_xyzw(
            -(right + left) / (right - left),
            -(top + bottom) / (top - bottom),
            -(zFar + zNear) / (zFar - zNear),
            1.0f)
    );

    cpu_mat_44 inv(
        cpu_vec_xyzw(
            (right - left) / 2,
            0,
            0,
            0),
        cpu_vec_xyzw(
            0,
            (top - bottom) / 2,
            0,
            0),
        cpu_vec_xyzw(
            0,
            0,
            (zFar - zNear) / -2,
            0),
        cpu_vec_xyzw(
            (right + left) / 2,
            (top + bottom) / 2,
            (zFar + zNear) / 2,
            1)
    );

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Concat(xform, inv);
}

void glMultMatrixf(const GLfloat* m)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    // unfortunately we can't assume the matrix is qword-aligned
    // the casts to float below fix an apparent parse error... something's up here..
    cpu_mat_44 newMatrix(cpu_vec_xyzw((float)m[0], (float)m[1], (float)m[2], (float)m[3]),
        cpu_vec_xyzw((float)m[4], (float)m[5], (float)m[6], (float)m[7]),
        cpu_vec_xyzw((float)m[8], (float)m[9], (float)m[10], (float)m[11]),
        cpu_vec_xyzw((float)m[12], (float)m[13], (float)m[14], (float)m[15]));

    // close your eyes.. this is a temporary hack

    /*
   // assume that newMatrix consists of rotations, uniform scales, and translations
   cpu_vec_xyzw scaledVec( 1, 0, 0, 0 );
   scaledVec = newMatrix * scaledVec;
   float scale = scaledVec.length();

   cpu_mat_44 invMatrix = newMatrix;
   invMatrix.set_col3( cpu_vec_xyzw(0,0,0,0) );
   invMatrix.transpose_in_place();
   invMatrix.set_col3( -newMatrix.get_col3() );
   cpu_mat_44 scaleMat;
   scaleMat.set_scale( cpu_vec_xyz(scale, scale, scale) );
   invMatrix = scaleMat * invMatrix;
   */

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
#if PGL_DEFER_MATRIX_CONCAT_INVERSE
    matStack.ConcatFromMatrix(newMatrix);
#else
    cpu_mat_44 invMatrix;
    //     invMatrix.set_identity();
    Invert2((float*)&newMatrix, (float*)&invMatrix);
    matStack.Concat(newMatrix, invMatrix);
#endif

    //     mWarn( "glMultMatrix is not correct" );
}

void glRotatef(GLfloat angle,
    GLfloat x, GLfloat y, GLfloat z)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    cpu_mat_44 xform, inverse;
    cpu_vec_xyz axis(x, y, z);
    axis.normalize();
    xform.set_rotate(Math::DegToRad(angle), axis);
    inverse.set_rotate(Math::DegToRad(-angle), axis);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Concat(xform, inverse);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    cpu_mat_44 xform, inverse;
    xform.set_scale(cpu_vec_xyz(x, y, z));
    inverse.set_scale(cpu_vec_xyz(1.0f / x, 1.0f / y, 1.0f / z));

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Concat(xform, inverse);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    cpu_mat_44 xform, inverse;
    cpu_vec_xyz direction(x, y, z);
    xform.set_translate(direction);
    inverse.set_translate(-direction);

    CMatrixStack& matStack = pGLContext->GetCurMatrixStack();
    matStack.Concat(xform, inverse);
}
