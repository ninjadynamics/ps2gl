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
    // Immediate stacks may defer Invert2; display-list recording uses the
    // default eager implementation and stores the same inverse as before.
    virtual void ConcatFromMatrix(const cpu_mat_44& xform);
    virtual void SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv) = 0;
    // The default path computes eagerly, including display-list recording.
    virtual void SetTopFromMatrix(const cpu_mat_44& newMat);
};

/********************************************
 * CImmMatrixStack
 */

class CImmMatrixStack : public CMatrixStack {
    // A pending inverse slot holds its original glLoadMatrixf operand. Never
    // invert the later composed forward matrix: that changes rounding/order.
    mutable bool InversePending[MaxStackDepth];
    void EnsureInverse() const;
    // Parent histories are shared by index across Push; only two matrices
    // and constant-sized metadata are copied. Child append storage is freed
    // on Pop/SetTop/materialization, never while an ancestor still owns it.
    static const int MaxPendingInverseOps = 32;
    struct PendingInverseOp {
        cpu_mat_44 Operand;
        int Previous;
        bool NeedsInverse;
    };
    PendingInverseOp InverseOps[MaxPendingInverseOps];
    mutable int NumInverseOps;
    mutable int InverseHead[MaxStackDepth];
    int InverseCheckpoint[MaxStackDepth];

    bool DeferInverse(const cpu_mat_44& operand, bool needsInverse);
    void DiscardInverseOps() const
    {
        NumInverseOps = InverseCheckpoint[CurStackDepth];
        InverseHead[CurStackDepth] = -1;
    }
public:
    CImmMatrixStack(CGLContext& context)
        : CMatrixStack(context)
        , InversePending{}
        , NumInverseOps(0)
    {
        InverseHead[0] = -1;
        InverseCheckpoint[0] = 0;
    }

    void Pop()
    {
        mErrorIf(CurStackDepth == 0, "No matrices to pop!");
        DiscardInverseOps();
        --CurStackDepth;
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    void Push()
    {
        mErrorIf(CurStackDepth == MaxStackDepth - 1,
            "No room on stack!");
        Matrices[CurStackDepth + 1]        = Matrices[CurStackDepth];
        InverseMatrices[CurStackDepth + 1] = InverseMatrices[CurStackDepth];
        InversePending[CurStackDepth + 1] = InversePending[CurStackDepth];
        InverseHead[CurStackDepth + 1] = InverseHead[CurStackDepth];
        InverseCheckpoint[CurStackDepth + 1] = NumInverseOps;
        ++CurStackDepth;
    }

    void Concat(const cpu_mat_44& xform, const cpu_mat_44& inverse)
    {
        if (!DeferInverse(inverse, false)) {
            cpu_mat_44& curInv = InverseMatrices[CurStackDepth];
            curInv = inverse * curInv;
        }
        cpu_mat_44& curMat = Matrices[CurStackDepth];
        curMat             = curMat * xform;
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    void SetTop(const cpu_mat_44& newMat, const cpu_mat_44& newInv)
    {
        Matrices[CurStackDepth]        = newMat;
        InverseMatrices[CurStackDepth] = newInv;
        InversePending[CurStackDepth] = false;
        DiscardInverseOps();
        GLContext.GetImmDrawContext().SetVertexXformValid(false);
    }

    const cpu_mat_44& GetTop() const { return Matrices[CurStackDepth]; }
    const cpu_mat_44& GetInvTop() const
    {
        EnsureInverse();
        return InverseMatrices[CurStackDepth];
    }
    void SetTopFromMatrix(const cpu_mat_44& newMat);
    void ConcatFromMatrix(const cpu_mat_44& xform);
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
