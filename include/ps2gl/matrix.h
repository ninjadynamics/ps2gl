/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_matrix_h
#define ps2gl_matrix_h

#include "GL/ps2gl.h"
#include "ps2gl/debug.h"
#include "ps2s/cpu_matrix.h"

#include "ps2gl/drawcontext.h"
#include "ps2gl/glcontext.h"

/********************************************
 * CMatrixStack
 */

class CGLContext;

class CMatrixStack {
protected:
    CGLContext& GLContext;
    static const int MaxStackDepth = 16;
    cpu_mat_44 Matrices[MaxStackDepth];
    mutable cpu_mat_44 InverseMatrices[MaxStackDepth];
    int CurStackDepth;

public:
    CMatrixStack(CGLContext& context)
        : GLContext(context)
        , CurStackDepth(0)
    {
        Matrices[0].set_identity();
        InverseMatrices[0].set_identity();
    }
    virtual ~CMatrixStack()
    {
    }

    virtual void Pop()  = 0;
    virtual void Push() = 0;
    virtual void Concat(const cpu_mat_44& xform, const cpu_mat_44& inverse) = 0;
    virtual void SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv) = 0;
#if PGL_LAZY_MATRIX_INVERSE
    // The default path computes eagerly, including display-list recording.
    virtual void SetTopFromMatrix(const cpu_mat_44& newMat);
#endif
};

/********************************************
 * CImmMatrixStack
 */

class CImmMatrixStack : public CMatrixStack {
#if PGL_LAZY_MATRIX_INVERSE
    // A pending inverse slot holds its original glLoadMatrixf operand. Never
    // invert the later composed forward matrix: that changes rounding/order.
    mutable bool InversePending[MaxStackDepth];
    void EnsureInverse() const;
#endif
public:
    CImmMatrixStack(CGLContext& context)
        : CMatrixStack(context)
#if PGL_LAZY_MATRIX_INVERSE
        , InversePending{}
#endif
    {
    }

    void Pop()
    {
        mErrorIf(CurStackDepth == 0, "No matrices to pop!");
        --CurStackDepth;
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    void Push()
    {
        mErrorIf(CurStackDepth == MaxStackDepth - 1,
            "No room on stack!");
        Matrices[CurStackDepth + 1]        = Matrices[CurStackDepth];
        InverseMatrices[CurStackDepth + 1] = InverseMatrices[CurStackDepth];
#if PGL_LAZY_MATRIX_INVERSE
        InversePending[CurStackDepth + 1] = InversePending[CurStackDepth];
#endif
        ++CurStackDepth;
    }

    void Concat(const cpu_mat_44& xform, const cpu_mat_44& inverse)
    {
#if PGL_LAZY_MATRIX_INVERSE
        // Preserve inverse * curInv exactly, including analytic inverses from
        // glTranslate/Rotate/Scale and eagerly inverted glMultMatrixf input.
        EnsureInverse();
#endif
        cpu_mat_44& curMat = Matrices[CurStackDepth];
        cpu_mat_44& curInv = InverseMatrices[CurStackDepth];
        curMat             = curMat * xform;
        curInv             = inverse * curInv;
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    void SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv)
    {
        Matrices[CurStackDepth]        = newMat;
        InverseMatrices[CurStackDepth] = newInv;
#if PGL_LAZY_MATRIX_INVERSE
        InversePending[CurStackDepth] = false;
#endif
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    const cpu_mat_44& GetTop() const { return Matrices[CurStackDepth]; }
    const cpu_mat_44& GetInvTop() const
    {
#if PGL_LAZY_MATRIX_INVERSE
        EnsureInverse();
#endif
        return InverseMatrices[CurStackDepth];
    }
#if PGL_LAZY_MATRIX_INVERSE
    void SetTopFromMatrix(const cpu_mat_44& newMat);
#endif
};

/********************************************
 * CDListMatrixStack
 */

class CDListMatrixStack : public CMatrixStack {
public:
    CDListMatrixStack(CGLContext& context)
        : CMatrixStack(context)
    {
    }

    void Pop();
    void Push();
    void Concat(const cpu_mat_44& xform, const cpu_mat_44& inverse);
    void SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv);
};

#endif // ps2gl_matrix_h
