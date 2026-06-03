/*	  Copyright (C) 2000,2001,2002  Sony Computer Entertainment America

       	  This file is subject to the terms and conditions of the GNU Lesser
	  General Public License Version 2.1. See the file "COPYING" in the
	  main directory of this archive for more details.                             */

#ifndef ps2gl_texture_h
#define ps2gl_texture_h

#include "ps2s/gsmem.h"
#include "ps2s/texture.h"

#include "GL/gl.h"

/********************************************
 * CTexManager
 */

class CGLContext;
class CMMTexture;
class CMMClut;
class CVifSCDmaPacket;

class CTexManager {
    CGLContext& GLContext;

    bool IsTexEnabled;
    bool InsideDListDef;

    static const int NumTexNames = 512; // :TODO: Make configurable
    CMMTexture* TexNames[NumTexNames];
    unsigned int Cursor;

    CMMTexture *DefaultTex, *CurTexture, *LastTexSent;
    CMMClut* CurClut;
    GS::tTexMode TexMode;

    void IncCursor() { Cursor = (Cursor + 1) & (NumTexNames - 1); }

public:
    CTexManager(CGLContext& context);
    ~CTexManager();

    void SetTexEnabled(bool yesNo);
    bool GetTexEnabled() const { return IsTexEnabled; }

    void GenTextures(GLsizei numNewTexNames, GLuint* newTexNames);
    void BindTexture(GLuint texNameToBind);
    void DeleteTextures(GLsizei numToDelete, const GLuint* texNames);

    CMMTexture& GetCurTexture() const { return *CurTexture; }

    CMMTexture& GetNamedTexture(GLuint tex) const
    {
        mErrorIf(TexNames[tex] == NULL, "Trying to access a null texture");
        return *TexNames[tex];
    }

    void UseCurTexture(CVifSCDmaPacket& renderPacket);

    void SetTexMode(GS::tTexMode mode);

    void SetCurTexParam(GLenum pname, GLint param);
    void SetCurTexImage(uint128_t* imagePtr, uint32_t w, uint32_t h,
        GS::tPSM psm);
    void SetGsTexture(GS::CMemArea& area);
    void SetCurClut(const void* clut, int numEntries);

    void BeginDListDef() { InsideDListDef = true; }
    void EndDListDef() { InsideDListDef = false; }
};

/********************************************
 * CMMClut
 */

/**
 * A memory-managed color lookup table.
 */
class CMMClut : public GS::CClut {
    GS::CMemArea GsMem;

public:
    CMMClut(const void* table, int numEntries = 256)
        : GS::CClut(table, numEntries)
        , GsMem(16, 16, GS::kPsm32, GS::kAlignPage)
    {
    }

    ~CMMClut() {}

    void Load(CVifSCDmaPacket& packet);
};

/********************************************
 * CMMTexture (Mem Managed Texture)
 */

namespace GS {
class CMemArea;
}

class CMMTexture : public GS::CTexture {
    GS::CMemArea* pImageMem;
    bool XferImage;
    bool IsResident;

public:
    CMMTexture(GS::tContext context);
    ~CMMTexture();

    void SetImage(const GS::CMemArea& area);
    void SetImage(uint128_t* imagePtr, uint32_t w, uint32_t h, GS::tPSM psm, uint32_t* clutPtr = NULL);

    void SetClut(const CMMClut& clut)
    {
        CTexEnv::SetClutGsAddr(clut.GetGsAddr());
    }

    void ChangePsm(GS::tPSM psm);

    // the following Load methods will check to see if a texture is resident and
    // transfer it if necessary.  The Use methods invoke the corresponding Load but
    // also transfer the gs register settings that belong to the texture

    // warning! these two will flush the data cache!
    void Load(bool waitForEnd = true);
    void Use(bool waitForEnd = false);

    void Load(CSCDmaPacket& packet);
    void Load(CVifSCDmaPacket& packet);

    void Use(CSCDmaPacket& packet);
    void Use(CVifSCDmaPacket& packet);

    void BindToSlot(GS::CMemSlot& slot);
    void Free(void);

    // SuperSolar manual mipmaps (ps2gl/ps2stuff have none). Sets GS TEX1 so this
    // texture samples a mip chain: mxl = highest level (0..6), manual MIPTBP
    // (mtba=0, sent once separately), LOD from Q (lcm=0), and bilinear-mipmap
    // filtering (mmin=4 LINEAR_MIPMAP_NEAREST; mmin=5 trilinear renders black on
    // single-pass GS submission, same as GLdc). ps2gl re-emits gsrTex1 every draw
    // of this texture, so MXL+filter persist for free.
    void SetMipLevels(int mxl, int kbias) {
        if (mxl < 0) mxl = 0;
        if (mxl > 6) mxl = 6;
        gsrTex1.mxl  = (uint64_t)mxl;
        gsrTex1.mtba = 0;
        gsrTex1.lcm  = 0;       // LOD computed from Q (per-pixel)
        gsrTex1.l    = 0;       // LOD scale = 0: a nonzero default MULTIPLIES the
                                // LOD and blurs the whole surface — must be 0
        gsrTex1.k    = (uint64_t)(kbias & 0xFFF);  // LOD bias, S7.4 (-16 = -1.0
                                // level); negative = sharper near, more far alias
        gsrTex1.mmin = 4;       // LINEAR_MIPMAP_NEAREST (bilinear + mip)
        gsrTex1.mmag = 1;       // LINEAR
    }

    // SuperSolar: pin this texture's GS slot so the memory manager can never
    // evict it. Mip levels are never *drawn* (only sampled via the base's
    // MIPTBP), so without this ps2gl reuses their VRAM for the next texture
    // upload and the floor samples garbage in the mip band. Call after Load().
    void LockGsSlot() { if (pImageMem && pImageMem->IsAllocated()) pImageMem->Lock(); }
};

#endif // ps2gl_texture_h
