// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "../GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_MODULE_H

#include <string.h>
#include <stdlib.h>

namespace vt
{
void GlyphSource::ClearCache()
{
    m_cache.clear();
    m_highCache.clear();
    m_centerReady = false;
    CloseFace(&m_highA);
    CloseFace(&m_highB);
    CloseFace(&m_highSymbol);
}

bool GlyphSource::Init(const char *ttfPath, int pixelSize, int weight, int strideBytes, int lines, int baselineRow)
{
    Shutdown();

    m_stride = strideBytes;
    m_lines = lines;
    m_baseline = baselineRow;
    m_sizeLatin = pixelSize;
    m_sizeMain = pixelSize;
    m_path = ttfPath;
    m_latinPath[0] = 0;
    m_symbolPath[0] = 0;
    m_weight = weight;
    m_weightLatin = weight;

    FT_Library lib = NULL;
    if (FT_Init_FreeType(&lib))
        return false;
    m_lib = lib;

    // Auto-hinter properties are Boolean/module-specific, not a numeric
    // "darkening" property on the TrueType driver. Record unsupported knobs.
    memset(m_darkErr, 0, sizeof(m_darkErr));
    if (m_darkening > 0)
    {
        FT_Bool noDark = 0;
        m_darkErr[0] = FT_Property_Set(lib, "autofitter", "no-stem-darkening", &noDark);
    }

    m_ss = (m_ss < 1) ? 1 : (m_ss > 4 ? 4 : m_ss);
    m_faceA = OpenFace(pixelSize * m_ss, nullptr, m_weight);
    if (!m_faceA)
    {
        FT_Done_FreeType(lib);
        m_lib = NULL;
        return false;
    }

    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
    return true;
}

void GlyphSource::Shutdown()
{
    CloseFace(&m_highSymbol);
    CloseFace(&m_faceSymbol);
    CloseFace(&m_highB);
    CloseFace(&m_highA);
    CloseFace(&m_faceB);
    CloseFace(&m_faceA);
    if (m_lib)
    {
        FT_Done_FreeType((FT_Library)m_lib);
        m_lib = NULL;
    }
    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
}

void *GlyphSource::OpenFace(int pixelSize, const char *path, int weight)
{
    if (!m_lib || !m_path)
        return NULL;

    FT_Face face = NULL;
    if (FT_New_Face((FT_Library)m_lib, path && *path ? path : m_path, 0, &face))
        return NULL;

    // Apply the configured wght coordinate when the variable face has that
    // axis; leave other axes at their defaults. Static faces have no axes.
    if (weight > 0 && FT_HAS_MULTIPLE_MASTERS(face))
    {
        FT_MM_Var *mm = NULL;
        if (!FT_Get_MM_Var(face, &mm))
        {
            FT_Fixed *coords = (FT_Fixed *)malloc(sizeof(FT_Fixed) * mm->num_axis);
            if (coords)
            {
                for (FT_UInt a = 0; a < mm->num_axis; ++a)
                {
                    coords[a] = mm->axis[a].def;
                    const unsigned int t = mm->axis[a].tag;
                    char tag[5] = {(char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0};
                    if (!strcmp(tag, "wght"))
                    {
                        const auto requested = (long long)weight * 65536;
                        coords[a] = (FT_Fixed)(requested < mm->axis[a].minimum ? mm->axis[a].minimum :
                                              requested > mm->axis[a].maximum ? mm->axis[a].maximum : requested);
                    }
                }
                FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
                free(coords);
            }
            FT_Done_MM_Var((FT_Library)m_lib, mm);
        }
    }

    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixelSize))
    {
        FT_Done_Face(face);
        return NULL;
    }
    return face;
}

void GlyphSource::CloseFace(void **face)
{
    if (*face)
    {
        FT_Done_Face((FT_Face)*face);
        *face = NULL;
    }
}

void GlyphSource::SetSizes(int latinPx, int mainPx, unsigned int cjkFrom)
{
    if (latinPx > 0)
        m_sizeLatin = latinPx;
    if (mainPx > 0)
        m_sizeMain = mainPx;
    m_cjkFrom = cjkFrom;

    if (!m_faceA)
        return;

    if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
        return;
    if (m_sizeMain == m_sizeLatin && !m_latinPath[0] && m_weightLatin == m_weight)
        CloseFace(&m_faceB);
    else if (!m_faceB)
        m_faceB = OpenFace(m_sizeMain * m_ss, nullptr, m_weight);
    else if (FT_Set_Pixel_Sizes((FT_Face)m_faceB, 0, (FT_UInt)(m_sizeMain * m_ss)))
        return;

    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
}

bool GlyphSource::SetLatinFont(const char *path, int weight)
{
    if (!m_faceA || !path)
        return false;
    void *latin = OpenFace(m_sizeLatin * m_ss, path, weight);
    if (!latin)
        return false;
    if (!m_faceB)
        m_faceB = OpenFace(m_sizeMain * m_ss, nullptr, m_weight);
    if (!m_faceB)
    {
        CloseFace(&latin);
        return false;
    }
    EnterCriticalSection(&m_cs);
    CloseFace(&m_faceA);
    m_faceA = latin;
    m_weightLatin = weight;
    strncpy_s(m_latinPath, path, _TRUNCATE);
    ClearCache();
    LeaveCriticalSection(&m_cs);
    return true;
}

bool GlyphSource::SetSymbolFont(const char *path, int pixelSize, int weight)
{
    if (!m_faceA || !path || pixelSize < 1)
        return false;
    EnterCriticalSection(&m_cs);
    void *symbol = *path ? OpenFace(pixelSize * m_ss, path, weight) : nullptr;
    if (*path && !symbol)
    {
        LeaveCriticalSection(&m_cs);
        return false;
    }
    ClearCache();
    CloseFace(&m_faceSymbol);
    m_faceSymbol = symbol;
    m_sizeSymbol = pixelSize;
    m_weightSymbol = weight;
    strncpy_s(m_symbolPath, path, _TRUNCATE);
    LeaveCriticalSection(&m_cs);
    return true;
}

void GlyphSource::SetSupersample(int ss)
{
    if (ss < 1)
        ss = 1;
    if (ss > 4)
        ss = 4;
    const int old = m_ss;
    m_ss = ss;
    if (!m_faceA || old == ss)
        return;
    // sizes are stored in target pixels, so the faces must be rebuilt
    CloseFace(&m_faceB);
    if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
        return;
    if (m_sizeMain != m_sizeLatin || m_latinPath[0] || m_weightLatin != m_weight)
        m_faceB = OpenFace(m_sizeMain * m_ss, nullptr, m_weight);
    if (m_faceSymbol)
        FT_Set_Pixel_Sizes((FT_Face)m_faceSymbol, 0, (FT_UInt)(m_sizeSymbol * m_ss));
    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
}
} // namespace vt
