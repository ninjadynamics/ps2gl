/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include "ps2gl/dlist.h"
#include "GL/ps2gl.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"

#include "kernel.h"
#include <new>

/********************************************
 * CDList
 */

CDList::CDList()
    : VertexBuf(NULL)
    , NormalBuf(NULL)
    , TexCoordBuf(NULL)
    , ColorBuf(NULL)
    , NumRenderPackets(0)
{
    FlushCache(0); // hopefully we're not allocating these buffers too often..
    FirstCmdBlock = CurCmdBlock = new CDListCmdBlock;
}

CDList::~CDList()
{
    if (VertexBuf)
        delete VertexBuf;
    if (NormalBuf)
        delete NormalBuf;
    if (TexCoordBuf)
        delete TexCoordBuf;
    if (ColorBuf)
        delete ColorBuf;

    // this will get them all
    delete FirstCmdBlock;
    for (int i = 0; i < NumRenderPackets; i++)
        delete RenderPackets[i];
}

bool CDList::ReserveGeometry(unsigned int vertices, unsigned int normals,
    unsigned int texCoords, unsigned int colors)
{
    if (VertexBuf || NormalBuf || TexCoordBuf || ColorBuf)
        return false;

    const unsigned int counts[4] = { vertices, normals, texCoords, colors };
    const unsigned int bytesPerElement[4] = { 16, 12, 8, 16 };
    CDmaPacket* buffers[4] = { NULL, NULL, NULL, NULL };
    for (int i = 0; i < 4; i++) {
        if (counts[i] > (0x7fffffffu - 63u) / bytesPerElement[i])
            break;
        unsigned int bytes = (counts[i] * bytesPerElement[i] + 63u) & ~63u;
        // BeginGeom records each attribute cursor even when no values follow.
        if (!bytes)
            bytes = 64;
        void* memory = CDmaPacket::AllocBuffer(bytes / 16,
            Core::MemMappings::UncachedAccl);
        if (!memory)
            break;
        buffers[i] = new (std::nothrow) CDmaPacket((uint128_t*)memory, bytes / 16,
            DMAC::Channels::vif1, Core::MemMappings::UncachedAccl);
        if (!buffers[i]) {
            free(Core::MakePtrNormal(memory));
            break;
        }
        buffers[i]->TakeBufferOwnership();
    }
    if (!buffers[3]) {
        for (int i = 0; i < 4; i++)
            delete buffers[i];
        return false;
    }
    VertexBuf = buffers[0];
    NormalBuf = buffers[1];
    TexCoordBuf = buffers[2];
    ColorBuf = buffers[3];
    return true;
}

void CDList::Begin()
{
}

CDmaPacket&
CDList::GetVertexBuf()
{
    if (VertexBuf == NULL)
        VertexBuf = new CDmaPacket(kBufferMaxQwordLength, DMAC::Channels::vif1,
            Core::MemMappings::UncachedAccl);
    return *VertexBuf;
}

CDmaPacket&
CDList::GetNormalBuf()
{
    if (NormalBuf == NULL)
        NormalBuf = new CDmaPacket(kBufferMaxQwordLength, DMAC::Channels::vif1,
            Core::MemMappings::UncachedAccl);
    return *NormalBuf;
}

CDmaPacket&
CDList::GetTexCoordBuf()
{
    if (TexCoordBuf == NULL)
        TexCoordBuf = new CDmaPacket(kBufferMaxQwordLength, DMAC::Channels::vif1,
            Core::MemMappings::UncachedAccl);
    return *TexCoordBuf;
}

CDmaPacket&
CDList::GetColorBuf()
{
    if (ColorBuf == NULL)
        ColorBuf = new CDmaPacket(kBufferMaxQwordLength, DMAC::Channels::vif1,
            Core::MemMappings::UncachedAccl);
    return *ColorBuf;
}

/********************************************
 * CDListManager
 */

void CDListManager::SwapBuffers()
{
    CurBuffer = 1 - CurBuffer;
    for (int i = 0; i < NumListsToBeFreed[CurBuffer]; i++)
        delete ListsToBeFreed[CurBuffer][i];
    NumListsToBeFreed[CurBuffer] = 0;
}

bool CDListManager::ListsAreFree(int firstListID, int numLists)
{
    bool listsAreFree = true;
    for (int i = 0; i < numLists; i++)
        if (Lists[firstListID + i] != NULL)
            listsAreFree = false;
    return listsAreFree;
}

unsigned int
CDListManager::GenLists(int numLists)
{
    // very temporary..
    int firstID = NextFreeListID;

    mErrorIf(!ListsAreFree(NextFreeListID, numLists),
        "Uh-oh.. time to write some real display list management code.");

    NextFreeListID += numLists;

    // wrap around and hope for the best.. (relying on ListsAreFree to do error checking)
    if (NextFreeListID == kMaxListID - 1)
        NextFreeListID = 1;

    mErrorIf(NextFreeListID >= kMaxListID, "Ran out of lists.. time to rewrite this code.");

    return firstID;
}

void CDListManager::DeleteLists(unsigned int firstListID, int numLists)
{
    for (int i = 0; i < numLists; i++) {
        if (Lists[firstListID + i]) {
            // don't delete the dlist immediately -- it might not have been dma'ed yet..
            AddListToBeFreed(Lists[firstListID + i]);
            Lists[firstListID + i] = NULL;
        }
    }
}

void CDListManager::NewList(unsigned int listID, GLenum mode)
{
    mWarnIf(mode == GL_COMPILE_AND_EXECUTE,
        "GL_COMPILE_AND_EXECUTE is not yet supported");

    OpenListID = listID;
    OpenList   = new CDList;
    OpenList->Begin();
}

void CDListManager::EndList()
{
    mErrorIf(OpenListID == 0, "There is no list to end!");

    if (Lists[OpenListID])
        AddListToBeFreed(Lists[OpenListID]);
    Lists[OpenListID] = OpenList;
    Lists[OpenListID]->End();
    OpenListID = 0;
    OpenList   = NULL;
}

class CCallListCmd : public CDListCmd {
    unsigned int ListID;

public:
    CCallListCmd(unsigned listID)
        : ListID(listID)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetDListManager().CallList(ListID);
        return CDListCmd::GetNextCmd(this);
    }
};

void CDListManager::CallList(unsigned int listID)
{
    if (OpenListID != 0) {
        // we're inside a list definition, so add the call to listID to the open list
        *OpenList += CCallListCmd(listID);
    } else {
        mErrorIf(Lists[listID] == NULL, "List does not exist.");
        pGLContext->GetImmGeomManager().Flush();
        Lists[listID]->Play();
    }
}

/********************************************
 * gl api
 */

GLuint glGenLists(GLsizei range)
{
    GL_FUNC_DEBUG("%s(%d)\n", __FUNCTION__, range);

    return pGLContext->GetDListManager().GenLists(range);
}

int pglReserveDListGeometry(unsigned int vertices, unsigned int normals,
    unsigned int texCoords, unsigned int colors)
{
    return pGLContext->GetDListManager().GetOpenDList().ReserveGeometry(
        vertices, normals, texCoords, colors);
}

void glDeleteLists(GLuint list, GLsizei range)
{
    GL_FUNC_DEBUG("%s(%d,%d)\n", __FUNCTION__, list, range);

    pGLContext->GetDListManager().DeleteLists(list, range);
}

void glNewList(GLuint listID, GLenum mode)
{
    GL_FUNC_DEBUG("%s(%d,%d)\n", __FUNCTION__, listID, mode);

    pGLContext->BeginDListDef(listID, mode);
}

void glEndList(void)
{
    GL_FUNC_DEBUG("%s()\n", __FUNCTION__);

    pGLContext->EndDListDef();
}

void glCallList(GLuint listID)
{
    GL_FUNC_DEBUG("%s(%d)\n", __FUNCTION__, listID);

    pGLContext->GetDListManager().CallList(listID);
}
