// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "../GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>

namespace vt
{
void GlyphSource::SetHighResolution(bool on)
{
    EnterCriticalSection(&m_cs);
    if (m_highResolution != on)
    {
        ClearCache();
        m_highResolution = on;
    }
    LeaveCriticalSection(&m_cs);
}

void GlyphSource::SetAntiAlias(bool on)
{
    if (m_csInit)
    {
        EnterCriticalSection(&m_cs);
        m_aa = on;
        ClearCache(); // coverage is baked into the cell
        LeaveCriticalSection(&m_cs);
    }
    else
    {
        m_aa = on;
    }
}

void GlyphSource::SetHinting(int mode)
{
    if (mode < 0 || mode > 2)
        mode = 0;
    EnterCriticalSection(&m_cs);
    if (mode != m_hinting)
    {
        m_hinting = mode;
        ClearCache();
    }
    LeaveCriticalSection(&m_cs);
}

void GlyphSource::SetLegacyCodepage1252(bool on)
{
    EnterCriticalSection(&m_cs);
    if (m_legacy1252 != on)
    {
        m_legacy1252 = on;
        ClearCache();
    }
    LeaveCriticalSection(&m_cs);
}

unsigned int GlyphSource::RenderCodepoint(unsigned int cp) const
{
    // Undefined CP1252 positions retain their original values. Only this
    // bounded C1 range is reinterpreted; Unicode and ASCII stay unchanged.
    static const unsigned short unicode[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                               0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                               0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                               0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    return m_legacy1252 && cp >= 0x80 && cp <= 0x9F ? unicode[cp - 0x80] : cp;
}

bool GlyphSource::UsesCJKFace(unsigned int cp) const
{
    // These characters lie below U+2E80 but belong with Chinese typography
    // in this game's text. ASCII punctuation stays with the Latin face.
    return cp >= m_cjkFrom || cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || (cp >= 0x2018 && cp <= 0x201F) ||
           cp == 0x2025 || cp == 0x2026;
}

int GlyphSource::KerningQuarter(unsigned int left, unsigned int right)
{
    left = RenderCodepoint(left);
    right = RenderCodepoint(right);
    if (!left || !right || !m_faceA)
        return 0;
    EnterCriticalSection(&m_cs);
    FT_Face face = (FT_Face)FaceHandleFor(left);
    FT_Vector delta = {0, 0};
    if (face == FaceHandleFor(right) && FT_HAS_KERNING(face))
        FT_Get_Kerning(face, FT_Get_Char_Index(face, left), FT_Get_Char_Index(face, right), FT_KERNING_UNFITTED,
                       &delta);
    const int q = (int)floor((double)delta.x / (16 * m_ss) + 0.5);
    LeaveCriticalSection(&m_cs);
    return q;
}
} // namespace vt
