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
bool GlyphSource::Init(const char *ttfPath, int pixelSize, int weight, int strideBytes, int lines, int baselineRow)
{
    Shutdown();

    m_stride = strideBytes;
    m_lines = lines;
    m_baseline = baselineRow;
    m_sizeLatin = pixelSize;
    m_sizeCJK = pixelSize;
    m_path = ttfPath;
    m_latinPath[0] = 0;
    m_weight = weight;

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
    m_faceA = OpenFace(pixelSize * m_ss);
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

void *GlyphSource::OpenFace(int pixelSize, const char *path)
{
    if (!m_lib || !m_path)
        return NULL;

    FT_Face face = NULL;
    if (FT_New_Face((FT_Library)m_lib, path && *path ? path : m_path, 0, &face))
        return NULL;

    // Apply the configured wght coordinate when the variable face has that
    // axis; leave other axes at their defaults. Static faces have no axes.
    if (m_weight > 0 && FT_HAS_MULTIPLE_MASTERS(face))
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
                        coords[a] = m_weight << 16;
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

void GlyphSource::SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom)
{
    if (latinPx > 0)
        m_sizeLatin = latinPx;
    if (cjkPx > 0)
        m_sizeCJK = cjkPx;
    m_cjkFrom = cjkFrom;

    if (!m_faceA)
        return;

    if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)(m_sizeLatin * m_ss)))
        return;
    if (m_sizeCJK == m_sizeLatin && !m_latinPath[0])
        CloseFace(&m_faceB);
    else if (!m_faceB)
        m_faceB = OpenFace(m_sizeCJK * m_ss);
    else if (FT_Set_Pixel_Sizes((FT_Face)m_faceB, 0, (FT_UInt)(m_sizeCJK * m_ss)))
        return;

    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
}

bool GlyphSource::SetLatinFont(const char *path)
{
    if (!m_faceA || !path || !*path)
        return false;
    void *latin = OpenFace(m_sizeLatin * m_ss, path);
    if (!latin)
        return false;
    if (!m_faceB)
        m_faceB = OpenFace(m_sizeCJK * m_ss);
    if (!m_faceB)
    {
        CloseFace(&latin);
        return false;
    }
    EnterCriticalSection(&m_cs);
    CloseFace(&m_faceA);
    m_faceA = latin;
    strncpy_s(m_latinPath, path, _TRUNCATE);
    ClearCache();
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
    if (m_sizeCJK != m_sizeLatin || m_latinPath[0])
        m_faceB = OpenFace(m_sizeCJK * m_ss);
    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        ClearCache();
        LeaveCriticalSection(&m_cs);
    }
}
} // namespace vt
