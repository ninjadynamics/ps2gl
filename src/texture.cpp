/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#include "ps2gl/texture.h"

#include "GL/ps2gl.h"

#include "ps2gl/debug.h"
#include "ps2gl/dlgmanager.h"
#include "ps2gl/dlist.h"
#include "ps2gl/glcontext.h"
#include "ps2gl/immgmanager.h"
#include "ps2gl/metrics.h"

#include "kernel.h"
#include <stdio.h>

/********************************************
 * CTexManager
 */

CTexManager::CTexManager(CGLContext& context)
    : GLContext(context)
    , IsTexEnabled(false)
    , InsideDListDef(false)
    , Cursor(0)
    , LastTexSent(NULL)
    , CurClut(NULL)
    , TexMode(GS::TexMode::kModulate)
{
    // clear the texture name entries
    for (int i      = 0; i < NumTexNames; i++)
        TexNames[i] = NULL;

    // create the default texture
    DefaultTex = new CMMTexture(GS::kContext1);
    CurTexture = DefaultTex;
}

CTexManager::~CTexManager()
{
    delete DefaultTex;
    // CurClut is a non-owning ref now (each CMMTexture owns its palette and
    // frees it in ~CMMTexture), so it must not be deleted here.

    for (int i = 0; i < NumTexNames; i++) {
        if (TexNames[i])
            delete TexNames[i];
    }
}

class CSetTexEnabledCmd : public CDListCmd {
    bool IsEnabled;

public:
    CSetTexEnabledCmd(bool enabled)
        : IsEnabled(enabled)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetTexEnabled(IsEnabled);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetTexEnabled(bool yesNo)
{
    if (!InsideDListDef) {
        if (IsTexEnabled != yesNo) {
            GLContext.TexEnabledChanged();
            GLContext.GetImmGeomManager().GetRendererManager().TexEnabledChanged(yesNo);
            IsTexEnabled = yesNo;
        }
    } else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetTexEnabledCmd(yesNo);
        GLContext.TexEnabledChanged();
    }
}

class CSetTexModeCmd : public CDListCmd {
    GS::tTexMode Mode;

public:
    CSetTexModeCmd(GS::tTexMode mode)
        : Mode(mode)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetTexMode(Mode);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetTexMode(GS::tTexMode mode)
{
    if (!InsideDListDef)
        TexMode = mode;
    else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetTexModeCmd(mode);
    }
}

void CTexManager::GenTextures(GLsizei numNewTexNames, GLuint* newTexNames)
{
    for (int curTexName = 0; curTexName < numNewTexNames; curTexName++) {
        // find the next free tex name and assign it
        int i;
        for (i = 0; i < NumTexNames; i++) {
            // 0 is a reserved tex name in OGL -- don't alloc it
            if (Cursor != 0 && TexNames[Cursor] == NULL)
                break;

            IncCursor();
        }
        // did we go through all the names without finding any free ones?
        if (i == NumTexNames) {
            mError("No free texture names.  Time to write a less braindead tex manager.");

            // In release build, return sensible names on failure
            while (i < NumTexNames) {
                newTexNames[i] = 0;
                ++i;
            }
        } else {
            newTexNames[curTexName] = Cursor;
            IncCursor();
        }
    }
}

void CTexManager::UseCurTexture(CVifSCDmaPacket& renderPacket)
{
    if (IsTexEnabled) {
        GS::tPSM psm = CurTexture->GetPSM();
        // do we need to send the clut?
        if (psm == GS::kPsm8 || psm == GS::kPsm8h) {
            // HyperSolar: prefer the texture's own palette so simultaneous
            // PSMT8 textures don't collide on the manager-global CurClut. Fall
            // back to CurClut for the legacy single-paletted-texture path.
            CMMClut* clut = CurTexture->GetOwnClut();
            if (clut == NULL)
                clut = CurClut;
            mErrorIf(clut == NULL,
                "Trying to use an indexed-color texture with no color table given!");
            clut->Load(renderPacket);
            CurTexture->SetClut(*clut);
        }
        // use the texture
        CurTexture->SetTexMode(TexMode);
        CurTexture->Use(renderPacket);
    }
}

class CBindTextureCmd : public CDListCmd {
    unsigned int TexName;

public:
    CBindTextureCmd(unsigned int name)
        : TexName(name)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().BindTexture(TexName);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::BindTexture(GLuint texNameToBind)
{
    GLContext.TextureChanged();

    if (!InsideDListDef) {
        if (texNameToBind == 0) {
            // default texture
            CurTexture = DefaultTex;
        } else {
            if (TexNames[texNameToBind] == NULL)
                TexNames[texNameToBind] = new CMMTexture(GS::kContext1);
            CurTexture                  = TexNames[texNameToBind];
        }

        pglAddToMetric(kMetricsBindTexture);
    } else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CBindTextureCmd(texNameToBind);
    }
}

void CTexManager::DeleteTextures(GLsizei numToDelete, const GLuint* texNames)
{
    for (int i = 0; i < numToDelete; i++) {
        mErrorIf(TexNames[texNames[i]] == NULL,
            "Trying to delete a texture that doesn't exist!");
        GLuint texName = texNames[i];
        if (CurTexture == TexNames[texName])
            CurTexture = DefaultTex;
        delete TexNames[texName];
        TexNames[texName] = NULL;
    }
}

class CSetCurTexParamCmd : public CDListCmd {
    GLenum PName;
    int Param;

public:
    CSetCurTexParamCmd(GLenum pname, int param)
        : PName(pname)
        , Param(param)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetCurTexParam(PName, Param);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetCurTexParam(GLenum pname, GLint param)
{
    if (!InsideDListDef) {
        CMMTexture& tex = *CurTexture;

        switch (pname) {
        case GL_TEXTURE_MIN_FILTER:
            tex.SetMinMode((GS::tMinMode)(param & 0xf));
            break;
        case GL_TEXTURE_MAG_FILTER:
            tex.SetMagMode((GS::tMagMode)(param & 0xf));
            break;
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_PRIORITY:
        case GL_TEXTURE_BORDER_COLOR:
            mNotImplemented();
            break;
        case GL_TEXTURE_WRAP_S:
            tex.SetWrapModeS((GS::tTexWrapMode)(param & 0xf));
            break;
        case GL_TEXTURE_WRAP_T:
            tex.SetWrapModeT((GS::tTexWrapMode)(param & 0xf));
            break;
        case GL_TEXTURE_WRAP_R:
            mNotImplemented("Sorry, only 2d textures.");
            break;
        }
    } else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetCurTexParamCmd(pname, param);
    }
}

class CSetCurTexImageCmd : public CDListCmd {
    uint128_t* Image;
    unsigned int Width, Height;
    GS::tPSM Psm;

public:
    CSetCurTexImageCmd(uint128_t* image, unsigned int w, unsigned int h,
        GS::tPSM psm)
        : Image(image)
        , Width(w)
        , Height(h)
        , Psm(psm)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetCurTexImage(Image, Width, Height, Psm);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetCurTexImage(uint128_t* imagePtr, uint32_t w, uint32_t h,
    GS::tPSM psm)
{
    GLContext.TextureChanged();

    if (!InsideDListDef) {
        CurTexture->SetImage(imagePtr, w, h, psm);
        if (psm != GS::kPsm24)
            CurTexture->SetUseTexAlpha(true);
        else
            CurTexture->SetUseTexAlpha(false);

        CurTexture->Free();
    } else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetCurTexImageCmd(imagePtr, w, h, psm);
    }
}

class CSetCurClutCmd : public CDListCmd {
    const void* Clut;
    int NumEntries;

public:
    CSetCurClutCmd(const void* clut, int numEntries)
        : Clut(clut)
        , NumEntries(numEntries)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetCurClut(Clut, NumEntries);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetCurClut(const void* clut, int numEntries)
{
    GLContext.TextureChanged();

    if (!InsideDListDef) {
        // HyperSolar: hand the new palette to the bound texture (it owns/frees
        // it); CurClut stays a non-owning "last set" ref used only as a legacy
        // fallback. Previously the manager owned the single CurClut, so a second
        // paletted texture's glColorTable replaced the first's palette outright.
        CMMClut* newClut = new CMMClut(clut, numEntries);
        if (CurTexture)
            CurTexture->SetOwnClut(newClut);
        CurClut = newClut;
    } else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetCurClutCmd(clut, numEntries);
    }
}

class CSetGsTextureCmd : public CDListCmd {
    GS::CMemArea& Texture;

public:
    CSetGsTextureCmd(GS::CMemArea& tex)
        : Texture(tex)
    {
    }
    CDListCmd* Play()
    {
        pGLContext->GetTexManager().SetGsTexture(Texture);
        return CDListCmd::GetNextCmd(this);
    }
};

void CTexManager::SetGsTexture(GS::CMemArea& area)
{
    GLContext.TextureChanged();

    if (!InsideDListDef)
        CurTexture->SetImage(area);
    else {
        CDList& dlist = GLContext.GetDListManager().GetOpenDList();
        dlist += CSetGsTextureCmd(area);
    }
}

/********************************************
 * CMMTexture methods
 */

CMMTexture::CMMTexture(GS::tContext context)
    : CTexture(context)
    , pImageMem(NULL)
    , XferImage(false)
    , IsResident(false)
    , OwnClut(NULL)
{
    // always load the clut
    // FIXME:  this is obviously not the best way to do
    // things...fix this once there is reasonable clut
    // allocation
    SetClutLoadConditions(1);
}

CMMTexture::~CMMTexture()
{
    delete pImageMem;
    delete OwnClut;   // HyperSolar: free this texture's palette (NULL-safe)
}

/**
 * Use the given image in main ram as the texture.
 */
void CMMTexture::SetImage(uint128_t* imagePtr, uint32_t w, uint32_t h, GS::tPSM psm, uint32_t* clutPtr)
{
    if (pImageMem) {
        // we are being re-initialized
        delete pImageMem;
        CTexture::Reset();
    }

    CTexture::SetImage(imagePtr, w, h, psm, clutPtr);

    // create a memarea for the image
    uint32_t bufWidth = gsrTex0.tb_width * 64;
    pImageMem     = new GS::CMemArea(bufWidth, h, psm, GS::kAlignBlock);

    XferImage = true;
}

/**
 * Texture from the given gs memory area.  This means that no texture
 * will be uploaded; only the register settings will be sent to the gs.
 */
void CMMTexture::SetImage(const GS::CMemArea& area)
{
    CTexEnv::SetPSM(area.GetPixFormat());
    CTexEnv::SetDimensions(area.GetWidth(), area.GetHeight());
    SetImageGsAddr(area.GetWordAddr());

    XferImage = false;
}

void CMMTexture::ChangePsm(GS::tPSM psm)
{
    // we only want changes like 8 -> 8h, or 4hh -> 4, not 8 -> 32

    using namespace GS;

    // printf("changing (%d,%d) into ", GetPSM(), psm);

    if (GS::GetBitsPerPixel(psm) == GS::GetBitsPerPixel(GetPSM())) {
        // printf("%d\n", psm);
        CTexEnv::SetPSM(psm);
        pImageUploadPkt->ChangePsm(psm);
    } else {
        // on the other hand, don't put an 8h into a 32

        int bpp = GS::GetBitsPerPixel(GetPSM());
        if (bpp == 8 && GetPSM() == GS::kPsm8h) {
            // printf("%d\n", kPsm8 );
            ChangePsm(GS::kPsm8);
        } else if (bpp == 4 && GetPSM() != GS::kPsm4) {
            // printf("%d\n", kPsm4 );
            ChangePsm(GS::kPsm4);
        } else {
            // printf("<no change>\n");
        }
    }
}

void CMMTexture::Load(bool waitForEnd)
{
    mErrorIf(pImageMem == NULL,
        "Trying to load a texture that hasn't been defined!");
    // first set the gs address and flush the cache
    if (!pImageMem->IsAllocated()) {
        pImageMem->Alloc();
        if (GetPSM() != pImageMem->GetPixFormat())
            ChangePsm(pImageMem->GetPixFormat());
        SetImageGsAddr(pImageMem->GetWordAddr());
        IsResident = false;
    }
    if (!IsResident) {
        FlushCache(0);
        // send the image
        SendImage(waitForEnd, Packet::kDontFlushCache);
        IsResident = true;

        pglAddToMetric(kMetricsTextureUploadCount);
    }
}

// these two should be templates or something..

void CMMTexture::Load(CSCDmaPacket& packet)
{
    mErrorIf(pImageMem == NULL,
        "Trying to load a texture that hasn't been defined!");
    if (!pImageMem->IsAllocated()) {
        pImageMem->Alloc();
        if (GetPSM() != pImageMem->GetPixFormat())
            ChangePsm(pImageMem->GetPixFormat());
        SetImageGsAddr(pImageMem->GetWordAddr());
        IsResident = false;
    }
    if (!IsResident) {
        SendImage(packet);
        IsResident = true;

        pglAddToMetric(kMetricsTextureUploadCount);
    }
}
void CMMTexture::Load(CVifSCDmaPacket& packet)
{
    mErrorIf(pImageMem == NULL,
        "Trying to load a texture that hasn't been defined!");
    if (!pImageMem->IsAllocated()) {
        pImageMem->Alloc();
        if (GetPSM() != pImageMem->GetPixFormat())
            ChangePsm(pImageMem->GetPixFormat());
        SetImageGsAddr(pImageMem->GetWordAddr());
        IsResident = false;
    }
    if (!IsResident) {
        //        printf("allocing memarea of psm %d (%dx%d)...", pImageMem->GetPixFormat(),
        //  	     GetW(), GetH() );
        //        printf("at addr %d\n", pImageMem->GetWordAddr() /2048);
        SendImage(packet);
        IsResident = true;

        pglAddToMetric(kMetricsTextureUploadCount);
    }
}

// again, should be templates..

void CMMTexture::Use(bool waitForEnd)
{
    if (XferImage)
        Load();
    SendSettings(waitForEnd, Packet::kDontFlushCache);
}
void CMMTexture::Use(CSCDmaPacket& packet)
{
    if (XferImage)
        Load(packet);
    SendSettings(packet);
}
void CMMTexture::Use(CVifSCDmaPacket& packet)
{
    if (XferImage)
        Load(packet);
    SendSettings(packet);
}

// HyperSolar P2 mip packing (GS Supplement pp. 9-10: trilinear's adjacent
// levels should share a GS page). Place this texture's image at an explicit
// GS word address inside a shared pack page and upload it NOW.
// SetImageGsAddr feeds BOTH TEX0.tb_addr and the upload packet's DBP, so
// upload and sampling agree by construction. XferImage=false + resident
// afterward: no later Use()/Load() can lazily allocate this texture's own
// pImageMem slot or retarget TEX0 off the pack. The pack CMemArea
// (allocated + locked, owned by the mip registry) holds the VRAM; this
// texture's own pImageMem stays unallocated, so LockGsSlot/UnlockGsSlot and
// deletion are no-ops on it — the pack cannot be double-unbound.
void CMMTexture::UploadPacked(uint32_t gsWordAddr)
{
    SetImageGsAddr(gsWordAddr);
    FlushCache(0);
    SendImage(true, Packet::kDontFlushCache);
    XferImage  = false;
    IsResident = true;
}

void CMMTexture::Free(void)
{
    pImageMem->Free();
    IsResident = false;
}

void CMMTexture::BindToSlot(GS::CMemSlot& slot)
{
    //     slot.Bind(*pImageMem, 0);
    //     if ( GetPSM() != pImageMem->GetPixFormat() )
    //        ChangePsm(pImageMem->GetPixFormat());
    //     SetImageGsAddr( pImageMem->GetWordAddr() );
    //     IsResident = false;
}

/********************************************
 * CMMClut
 */

void CMMClut::Load(CVifSCDmaPacket& packet)
{
    if (!GsMem.IsAllocated()) {
        GsMem.Alloc();
        SetGsAddr(GsMem.GetWordAddr());
        Send(packet);

        pglAddToMetric(kMetricsClutUploadCount);
    }
}

/********************************************
 * gl api
 */

void glGenTextures(GLsizei numNewTexNames, GLuint* newTexNames)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CTexManager& texManager = pGLContext->GetTexManager();
    texManager.GenTextures(numNewTexNames, newTexNames);
}

void glBindTexture(GLenum target, GLuint texName)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mErrorIf(target != GL_TEXTURE_2D, "There are only 2D textures in ps2gl");

    CTexManager& texManager = pGLContext->GetTexManager();
    texManager.BindTexture(texName);
}

void glDeleteTextures(GLsizei numToDelete, const GLuint* texNames)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CTexManager& texManager = pGLContext->GetTexManager();
    texManager.DeleteTextures(numToDelete, texNames);
}

// HyperSolar: see GL/ps2gl.h. Give the current texture's image buffer to ps2gl so
// ~CTexture free()s it on delete — fixes the rlLoadTexturePS2 pglutAllocDmaMem leak.
extern "C" void pglTexImageTakeOwnership(void)
{
    pGLContext->GetTexManager().GetCurTexture().SetFreeImageOnExit(true);
}

void glTexImage2D(GLenum target,
    GLint level,
    GLint internalFormat,
    GLsizei width,
    GLsizei height,
    GLint border,
    GLenum format,
    GLenum type,
    const GLvoid* pixels)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    if (target == GL_PROXY_TEXTURE_2D) {
        mNotImplemented();
        return;
    }
    if (level > 0) {
        mNotImplemented("mipmapping");
        return;
    }
    if (border > 0) {
        mNotImplemented("texture borders");
        return;
    }
    if ((unsigned int)pixels & 0xf) {
        mNotImplemented("texture data needs to be aligned to at least 16 bytes, "
                        "preferably 9 quads");
    }

    GS::tPSM psm = GS::kInvalidPsm;
    switch (format) {
    case GL_RGBA:
        if (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8)
            psm = GS::kPsm32;
        else if (type == GL_UNSIGNED_SHORT_5_5_5_1)
            psm = GS::kPsm16;
        else {
            mNotImplemented("RGBA textures should have a type of GL_UNSIGNED_BYTE, "
                            "GL_UNSIGNED_INT_8_8_8_8, or GL_UNSIGNED_SHORT_5_5_5_1");
        }
        break;
    case GL_RGB:
        if (type == GL_UNSIGNED_BYTE)
            psm = GS::kPsm24;
        else {
            mNotImplemented("RGB textures should have a type of GL_UNSIGNED_BYTE");
        }
        break;
    case GL_COLOR_INDEX:
        if (type == GL_UNSIGNED_BYTE)
            psm = GS::kPsm8;
        else {
            mNotImplemented("indexed textures should have a type of GL_UNSIGNED_BYTE");
        }
        break;
    default:
        mError("Unknown texture format");
    }

    if (psm != GS::kInvalidPsm) {
        CTexManager& tm = pGLContext->GetTexManager();
        tm.SetCurTexImage((uint128_t*)pixels, width, height, psm);
    }
}

void glColorTable(GLenum target, GLenum internalFormat,
    GLsizei width, GLenum format, GLenum type,
    const GLvoid* table)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mWarnIf(target != GL_COLOR_TABLE,
        "glColorTable only supports GL_COLOR_TABLE");
    // ignore internalFormat
    mErrorIf(width != 16 && width != 256,
        "A color table must contain either 16 or 256 entries");
    mErrorIf(format != GL_RGB && format != GL_RGBA,
        "The pixel format of color tables must be either GL_RGB or GL_RGBA");
    mWarnIf(type != GL_UNSIGNED_INT && type != GL_UNSIGNED_INT_8_8_8_8,
        "The type of color table data must be either GL_UNSIGNED_INT or"
        "GL_UNSIGNED_INT_8_8_8_8");
    mErrorIf((unsigned int)table & (16 - 1),
        "Color tables in ps2gl need to be 16-byte aligned in memory..");

    CTexManager& tm = pGLContext->GetTexManager();
    tm.SetCurClut(table, width);
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    if (target != GL_TEXTURE_2D) {
        mNotImplemented("Sorry, only 2d textures.");
        return;
    }

    pGLContext->GetTexManager().SetCurTexParam(pname, param);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexParameteri(target, pname, (GLint)param);
}

void glTexParameteriv(GLenum target, GLenum pname, GLint* param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexParameteri(target, pname, *param);
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexParameteri(target, pname, (GLint)*param);
}

void glTexEnvi(GLenum target, GLenum pname, GLint param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    CTexManager& tm = pGLContext->GetTexManager();

    switch (param) {
    case GL_MODULATE:
        tm.SetTexMode((GS::tTexMode)(param & 0xf));
        break;
    case GL_DECAL:
        mWarn("GL_DECAL functions exactly as GL_REPLACE right now.");
    case GL_REPLACE:
        tm.SetTexMode((GS::tTexMode)(param & 0xf));
        break;
    case GL_BLEND:
        mNotImplemented();
        break;
    }
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexEnvi(target, pname, (GLint)param);
}

void glTexEnvfv(GLenum target, GLenum pname, GLfloat* param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexEnvi(target, pname, (GLint)*param);
}

void glTexEnviv(GLenum target, GLenum pname, GLint* param)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    glTexEnvi(target, pname, *param);
}

void glTexSubImage2D(GLenum target, int level,
    int xoffset, int yoffset, int width, int height,
    GLenum format, GLenum type,
    const void* pixels)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mNotImplemented();
}

void glCopyTexImage2D(GLenum target, int level,
    GLenum iformat,
    int x, int y, int width, int height,
    int border)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mNotImplemented();
}

void glCopyTexSubImage2D(GLenum target, int level,
    int xoffset, int yoffset, int x, int y,
    int width, int height)
{
    GL_FUNC_DEBUG("%s\n", __FUNCTION__);

    mNotImplemented();
}

/********************************************
 * ps2gl C interface
 */

/**
 * @addtogroup pgl_api
 * @{
 */

/**
 * Free the named GL texture object.  Note that this only frees
 * any GS ram the texture is using, not main ram.
 */
void pglFreeTexture(GLuint texId)
{
    CTexManager& texManager = pGLContext->GetTexManager();
    CMMTexture& texture     = texManager.GetNamedTexture(texId);
    texture.Free();
}

/**
 * Bind the named GL texture object to the given GS memory slot.
 * This functions allows the application to bypass the GS memory
 * manager.
 */
void pglBindTextureToSlot(GLuint texId, pgl_slot_handle_t mem_slot)
{
    CTexManager& texManager = pGLContext->GetTexManager();
    CMMTexture& texture     = texManager.GetNamedTexture(texId);
    texture.BindToSlot(*reinterpret_cast<GS::CMemSlot*>(mem_slot));
}

/**
 * Texture from the given memory area.  Used in the same context as
 * glTexImage2D(), this call would probably be used with procedural
 * textures.
 */
void pglTextureFromGsMemArea(pgl_area_handle_t tex_area_handle)
{
    CTexManager& texManager = pGLContext->GetTexManager();
    GS::CMemArea* texArea   = reinterpret_cast<GS::CMemArea*>(tex_area_handle);
    texManager.SetGsTexture(*texArea);
}

/** @} */

/* ===========================================================================
 * HyperSolar: manual 16-bit (PSMCT16) MIPMAPPED texture upload.
 *
 * ps2gl/ps2stuff/GLdc ship no mipmap path (glTexImage2D rejects level>0). The GS
 * does mipmaps via TEX1 (MXL + mip min-filter) + MIPTBP1/2 (explicit per-level
 * base pointers). ps2stuff emits the texture's settings packet every bind, so
 * MXL/filter (CMMTexture::SetMipLevels) AND the per-level pointers (CTexEnv::
 * SetMiptbp — MIPTBP1/2 ride the same packet, like TEXA) are all per-texture:
 * any number of mipmapped textures can coexist (the old one-shot global MIPTBP
 * write limited us to ONE — the floor). Textures with MXL=0 ignore the zero
 * MIPTBP their packet carries. Mip levels are uploaded as their own resident
 * textures and don't need to be contiguous (MIPTBP holds an explicit address
 * per level).
 * =========================================================================== */

/* MIPTBP1/2 register packers — local copies so we don't include <gs_gp.h>, which
   redefines GS_DISABLE/GS_ENABLE against libgs.h (warnings). TBA = base/256 (14b),
   TBW = width/64 (6b); fields per the GS manual / gs_gp.h GS_SET_MIPTBP1/2. */
#define SS_MIPTBP1(TBA1, TBW1, TBA2, TBW2, TBA3, TBW3)            \
    ((u64)((TBA1) & 0x3FFF) << 0  | (u64)((TBW1) & 0x3F) << 14 |  \
     (u64)((TBA2) & 0x3FFF) << 20 | (u64)((TBW2) & 0x3F) << 34 |  \
     (u64)((TBA3) & 0x3FFF) << 40 | (u64)((TBW3) & 0x3F) << 54)
#define SS_MIPTBP2(TBA4, TBW4, TBA5, TBW5, TBA6, TBW6)            \
    ((u64)((TBA4) & 0x3FFF) << 0  | (u64)((TBW4) & 0x3F) << 14 |  \
     (u64)((TBA5) & 0x3FFF) << 20 | (u64)((TBW5) & 0x3F) << 34 |  \
     (u64)((TBA6) & 0x3FFF) << 40 | (u64)((TBW6) & 0x3F) << 54)

/* HyperSolar: registry of the manually-created mip-level CMMTextures, keyed by
   the base texture's GL name, so the game can RELEASE a whole mip pyramid when
   a stage/scene switch swaps textures. Without this the locked slots +
   CMMTextures leak (~14 pages per swap), and the only escape was a full GS
   layout reinit. Sized for the worst co-resident set: 2 floors + the 8 city
   wall/window tiles, with headroom. */
#define PGL_MIP_REGISTRY_MAX 16
/* NOTE: the identifier "mips" is unusable here — the MIPS GCC target
   predefines it as a macro (`#define mips 1`), like `unix`/`linux`. */
struct SMipRegistryEntry {
    unsigned int baseId;
    CMMTexture* levels[6];
    int count;
    GS::CMemArea* pack; /* P2 packed pyramids: the ONE allocated+locked
                           pack-page owner (NULL = per-level slots). Freed
                           exactly once in pgl_delete_mips. */
};
static SMipRegistryEntry MipRegistry[PGL_MIP_REGISTRY_MAX];

/* P2 packed-pyramid block offsets, valid ONLY for the exact 64x64 four-level
   pyramids (do not generalize — the in-page block arrangement is PSM-specific
   and these footprints were derived from the GS manual block tables, secs 8.3
   + 8.5). One block = 256 bytes = 64 words. PSMT8 packs the WHOLE pyramid in
   one page (TEX0.TBW stays 2 for L0; MIPTBP TBWn=1 for L1-L3); PSMCT16's L0
   exactly fills its own page, so only L1-L3 pack into a second one (TBW 1). */
struct SMipPackSlot {
    int block, nBlocks;
};
static const SMipPackSlot kPackPsmt8[4] = { { 0, 16 }, { 16, 4 }, { 20, 1 }, { 21, 1 } };
static const SMipPackSlot kPackPsmct16[3] = { { 0, 8 }, { 8, 2 }, { 10, 1 } };

static void pgl_pack_check(const SMipPackSlot* t, int n)
{
    for (int i = 0; i < n; i++) {
        mErrorIf(t[i].block + t[i].nBlocks > 32,
            "mip pack slot spills past the page (32 blocks)");
        mErrorIf(i < n - 1 && t[i].block + t[i].nBlocks > t[i + 1].block,
            "mip pack slots overlap");
    }
}

static void pgl_mips_register(unsigned int baseId, CMMTexture** levels, int count,
                              GS::CMemArea* pack)
{
    for (int i = 0; i < PGL_MIP_REGISTRY_MAX; i++) {
        if (MipRegistry[i].baseId == 0) {
            MipRegistry[i].baseId = baseId;
            for (int j = 0; j < count; j++)
                MipRegistry[i].levels[j] = levels[j];
            MipRegistry[i].count = count;
            MipRegistry[i].pack  = pack;
            return;
        }
    }
    printf("pgl_mips_register: table full — mip levels for %u will leak\n", baseId);
    mError("PGL_MIP_REGISTRY_MAX exceeded — bump it before adding more mipped assets");
}

/* Release the mip pyramid registered for `baseId` (no-op if none): unlock each
   level's GS slot (back onto its type list) and delete the CMMTexture (~CMemArea
   unbinds the slot -> LRU-reusable). Call BEFORE glDeleteTextures(baseId). */
extern "C" void pgl_delete_mips(unsigned int baseId)
{
    for (int i = 0; i < PGL_MIP_REGISTRY_MAX; i++) {
        if (MipRegistry[i].baseId != baseId)
            continue;
        for (int j = 0; j < MipRegistry[i].count; j++) {
            CMMTexture* m = MipRegistry[i].levels[j];
            if (!m)
                continue;
            m->UnlockGsSlot(); /* packed levels own no slot — no-op there */
            delete m;
            MipRegistry[i].levels[j] = NULL;
        }
        if (MipRegistry[i].pack) {
            /* the ONE pack-page owner: unlock, then delete (~CMemArea
               Free()s the slot). Level CMMTextures above never allocated
               their own pImageMem, so this is the only unbind. */
            MipRegistry[i].pack->Unlock();
            delete MipRegistry[i].pack;
            MipRegistry[i].pack = NULL;
        }
        MipRegistry[i].baseId = 0;
        MipRegistry[i].count  = 0;
        return;
    }
}

extern "C" unsigned int pgl_create_mip16(void** levels, const int* lw,
                                         const int* lh, int count, int kbias,
                                         int min_filter)
{
    if (!levels || count < 1) return 0;
    CTexManager& tm = pGLContext->GetTexManager();

    /* Base level (0) is a real GL texture name so the game's glBindTexture binds
       it like any other texture. */
    GLuint id = 0;
    tm.GenTextures(1, &id);
    tm.BindTexture(id);
    CMMTexture& base = tm.GetNamedTexture(id);
    base.SetImage((uint128_t*)levels[0], (uint32_t)lw[0], (uint32_t)lh[0], GS::kPsm16);
    base.SetUseTexAlpha(true);
    base.Load();                         /* allocate base slot + upload it */

    int nmip = count - 1;
    if (nmip > 6) nmip = 6;              /* GS TEX1.MXL caps at 6 */

    /* P2 packing (special-cased to the exact 64x64 four-level pyramid): L0
       already fills its page exactly, so L1-L3 pack into ONE extra page
       (11 of 32 blocks) instead of a page each — 2 pages total vs 4. Any
       other shape keeps the per-level allocator path below. */
    bool packedPyramid = (count == 4 && lw[0] == 64 && lh[0] == 64
        && lw[1] == 32 && lh[1] == 32 && lw[2] == 16 && lh[2] == 16
        && lw[3] == 8 && lh[3] == 8);
    GS::CMemArea* pack = NULL;
    if (packedPyramid) {
        pgl_pack_check(kPackPsmct16, 3);
        pack = new GS::CMemArea(64, 64, GS::kPsm16, GS::kAlignPage); /* = 1 page */
        pack->Alloc();
        pack->Lock();
    }

    /* Levels 1..nmip: own resident CMMTextures (intentionally never freed — they
       back the floor for the whole run); harvest each GS address for MIPTBP.
       GetImageGsAddr() returns tb_addr*64 (word addr); /64 gives the TBP value
       (256-byte units), exactly what MIPTBP wants. TBW is width/64 (min 1). */
    u32 tba[6] = {0}, tbw[6] = {0};
    CMMTexture* mlist[6] = {0};
    for (int i = 1; i <= nmip; i++) {
        CMMTexture* m = new CMMTexture(GS::kContext1);
        m->SetImage((uint128_t*)levels[i], (uint32_t)lw[i], (uint32_t)lh[i], GS::kPsm16);
        if (packedPyramid) {
            /* one block = 64 words; the level's own pImageMem never allocates */
            m->UploadPacked(pack->GetWordAddr() + kPackPsmct16[i - 1].block * 64);
        } else {
            m->Load();
            m->LockGsSlot();   // pin it — never drawn, so the allocator would evict it
        }
        mlist[i - 1] = m;
        tba[i - 1] = m->GetImageGsAddr() / 64;
        tbw[i - 1] = (u32)((lw[i] + 63) / 64);
        if (tbw[i - 1] < 1) tbw[i - 1] = 1;
    }
    pgl_mips_register(id, mlist, nmip, pack);   /* releasable via pgl_delete_mips */

    base.SetMipLevels(nmip, kbias, min_filter);  /* TEX1 MXL/LOD/min-filter, re-emitted
                                         per draw. kbias = GS TEX1.K (S7.4, -16 = -1.0
                                         level, negative = sharper); min_filter = GS
                                         MMIN (5 trilinear / 4 bilinear-mip). Both
                                         passed from the game so they tune with no
                                         ps2gl rebuild. */

    /* Per-texture MIPTBP: rides the settings packet with TEX0/TEX1/TEXA, so
       every bind of THIS texture re-points the GS at its own mip pyramid. */
    base.SetMiptbp(
        SS_MIPTBP1(tba[0], tbw[0], tba[1], tbw[1], tba[2], tbw[2]),
        SS_MIPTBP2(tba[3], tbw[3], tba[4], tbw[4], tba[5], tbw[5]));

    printf("[MIP] id=%u base_tbp=%u mxl=%d pack_tbp=%d\n",
           (unsigned)id, (unsigned)(base.GetImageGsAddr() / 64), nmip,
           pack ? (int)(pack->GetWordAddr() / 64) : -1);
    for (int i = 0; i < nmip; i++)
        printf("[MIP]   L%d %dx%d tbp=%u tbw=%u off=%d\n",
               i + 1, lw[i + 1], lh[i + 1], (unsigned)tba[i], (unsigned)tbw[i],
               pack ? kPackPsmct16[i].block : -1);

    return (unsigned int)id;
}

/* 8-bit (PSMT8) MIPMAPPED texture: the PSMT8 twin of pgl_create_mip16 above.
   Base level goes through the standard paletted path (pgl_create_index8 below)
   so the CLUT rides along per-texture (OwnClut); mip levels are raw kPsm8
   resident textures whose GS addresses feed MIPTBP. All levels sample through
   the base's CLUT — the GS has ONE CLUT per texture regardless of MXL. `levels`
   are index planes (1 byte/texel, 16-byte aligned); `clut` as pgl_create_index8. */
extern "C" unsigned int pgl_create_index8(const void* indices, int w, int h,
                                          const void* clut);

extern "C" unsigned int pgl_create_index8_mip(const void** levels, const int* lw,
                                              const int* lh, int count,
                                              const void* clut, int kbias,
                                              int min_filter)
{
    if (!levels || !clut || count < 1) return 0;

    GLuint id = pgl_create_index8(levels[0], lw[0], lh[0], clut);
    if (id == 0) return 0;

    CTexManager& tm = pGLContext->GetTexManager();
    tm.BindTexture(id);
    CMMTexture& base = tm.GetNamedTexture(id);

    int nmip = count - 1;
    if (nmip > 6) nmip = 6;              /* GS TEX1.MXL caps at 6 */

    /* P2 packing (special-cased to the exact 64x64 four-level pyramid): the
       WHOLE pyramid fits one page (22 of 32 blocks) — L0 is adopted into the
       pack (its own pImageMem never allocates; OwnClut untouched). MIPTBP
       TBWn is 1 for the packed sub-64 levels per the register spec (width/64,
       clamp to 1) — the UPLOAD keeps DBW=2 (SetDimensions' 128-texel floor):
       these rectangles never cross the physical 128x64 page, so the
       within-page block walk matches sampling. Other shapes keep the
       per-level path below (TBW=2, matching what their upload used). */
    bool packedPyramid = (count == 4 && lw[0] == 64 && lh[0] == 64
        && lw[1] == 32 && lh[1] == 32 && lw[2] == 16 && lh[2] == 16
        && lw[3] == 8 && lh[3] == 8);
    GS::CMemArea* pack = NULL;
    if (packedPyramid) {
        pgl_pack_check(kPackPsmt8, 4);
        pack = new GS::CMemArea(128, 64, GS::kPsm8, GS::kAlignPage); /* = 1 page */
        pack->Alloc();
        pack->Lock();
        base.UploadPacked(pack->GetWordAddr() + kPackPsmt8[0].block * 64);
    }

    /* Levels 1..nmip: resident kPsm8 CMMTextures (packed: at block offsets in
       the pack page; unpacked: own pinned slots, TBW matching the 128-px-wide
       upload buffer). */
    u32 tba[6] = {0}, tbw[6] = {0};
    CMMTexture* mlist[6] = {0};
    for (int i = 1; i <= nmip; i++) {
        CMMTexture* m = new CMMTexture(GS::kContext1);
        m->SetImage((uint128_t*)levels[i], (uint32_t)lw[i], (uint32_t)lh[i], GS::kPsm8);
        if (packedPyramid) {
            m->UploadPacked(pack->GetWordAddr() + kPackPsmt8[i].block * 64);
            tbw[i - 1] = 1;
        } else {
            m->Load();
            m->LockGsSlot();
            tbw[i - 1] = (u32)(((lw[i] + 127) / 128) * 2);
            if (tbw[i - 1] < 2) tbw[i - 1] = 2;
        }
        mlist[i - 1] = m;
        tba[i - 1] = m->GetImageGsAddr() / 64;
    }
    pgl_mips_register(id, mlist, nmip, pack);   /* releasable via pgl_delete_mips */

    base.SetMipLevels(nmip, kbias, min_filter);
    base.SetMiptbp(
        SS_MIPTBP1(tba[0], tbw[0], tba[1], tbw[1], tba[2], tbw[2]),
        SS_MIPTBP2(tba[3], tbw[3], tba[4], tbw[4], tba[5], tbw[5]));

    printf("[MIP8] id=%u base_tbp=%u mxl=%d pack_tbp=%d\n",
           (unsigned)id, (unsigned)(base.GetImageGsAddr() / 64), nmip,
           pack ? (int)(pack->GetWordAddr() / 64) : -1);
    for (int i = 0; i < nmip; i++)
        printf("[MIP8]   L%d %dx%d tbp=%u tbw=%u off=%d\n",
               i + 1, lw[i + 1], lh[i + 1], (unsigned)tba[i], (unsigned)tbw[i],
               pack ? kPackPsmt8[i + 1].block : -1);

    return (unsigned int)id;
}

/* GS TEXA override for the named texture: how 16-bit (5551) texel alpha
   expands when sampled — A=0 texels read ta0, A=1 read ta1 (0x80 = 1.0). The
   CTexEnv constructor default (ta0=0x80, "identity") makes 1-bit-TRANSPARENT
   texels read as OPAQUE to the GS alpha test, so punch-through tiles rendered
   as solid quads (the city windows drew as full walls). ta0=0 fixes that.
   TEXA rides the texture's own settings packet, so the override is applied on
   every bind of THIS texture only — other 16-bit textures (e.g. full-screen
   splashes whose alpha bits are all 0) keep the opaque default. */
extern "C" void pgl_texture_texalpha(unsigned int name, unsigned int ta0,
                                     unsigned int ta1)
{
    CTexManager& tm = pGLContext->GetTexManager();
    tm.GetNamedTexture((GLuint)name).SetTexAlpha((uint8_t)ta0, (uint8_t)ta1);
}

/* Create an 8-bit paletted (PSMT8) texture from index data + a 256-entry RGBA
   CLUT. The clean one-call uploader for the game (mirrors pgl_create_mip16's
   role): wraps the standard glTexImage2D(GL_COLOR_INDEX) + glColorTable path the
   ps2gl logo example proves. Returns a GL texture name bindable like any other.
   `clut` is 256 * 4 bytes (R8G8B8A8), MUST be 16-byte aligned (ps2gl requires it)
   and **already in GS CSM1 storage order** — the build step (tex8.py) does that
   reorder, so this stays read-only (the blob is embedded in .rodata; reordering
   here would write read-only data). Defaults REPEAT/LINEAR/MODULATE — re-set wrap
   or filter afterward if needed. 8-bit halves a 16-bit texture's VRAM (PSMT8 page
   = 128x64); the 32-bit CLUT gives full alpha (vs PSMCT16's 1 bit). */
extern "C" unsigned int pgl_create_index8(const void* indices, int w, int h,
                                          const void* clut)
{
    if (!indices || !clut) return 0;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);

    /* internalformat 3 + GL_COLOR_INDEX -> PSMT8 (texture.cpp glTexImage2D). */
    glTexImage2D(GL_TEXTURE_2D, 0, 3, w, h, 0,
                 GL_COLOR_INDEX, GL_UNSIGNED_BYTE, indices);
    glColorTable(GL_COLOR_TABLE, GL_RGBA, 256, GL_RGBA,
                 GL_UNSIGNED_INT_8_8_8_8, clut);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBindTexture(GL_TEXTURE_2D, 0);
    return (unsigned int)id;
}
