// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GlyphSource.h"

namespace vt
{
GlyphSource::GlyphSource()
    : m_lib(NULL), m_faceA(NULL), m_faceB(NULL), m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u), m_stride(3),
      m_lines(16), m_baseline(13), m_fit(true), m_aa(false), m_darkening(0), m_ss(1), m_hinting(0), m_darkErr{0, 0, 0},
      m_path(NULL), m_weight(400), m_csInit(false)
{
    m_latinPath[0] = 0;
    InitializeCriticalSection(&m_cs);
    m_csInit = true;
}

GlyphSource::~GlyphSource()
{
    Shutdown();
    if (m_csInit)
    {
        DeleteCriticalSection(&m_cs);
        m_csInit = false;
    }
}

const GlyphCell *GlyphSource::Get(unsigned int codepoint, int gameAdvance, int phase, int scale1024, int verticalPhase)
{
    codepoint = RenderCodepoint(codepoint);
    if (!m_faceA)
        return NULL;
    if (scale1024 < 1 || scale1024 > 1024)
        return NULL;

    // Signed phase is in quarter logical pixels (-3..3). One logical pixel
    // spans m_ss raster pixels: in FreeType 26.6 units, the shift is
    // phase * 16 * m_ss.
    if (phase < -3)
        phase = -3;
    if (phase > 3)
        phase = 3;
    if (verticalPhase < -3) verticalPhase = -3;
    if (verticalPhase > 3) verticalPhase = 3;

    // Cache key includes codepoint, clamped compatibility advance (0 means
    // natural), signed raster phase and uniform row scale. Each variant
    // needs its own coverage; font/raster setting changes clear the cache.
    const int advKey = (gameAdvance > 0) ? (gameAdvance > 255 ? 255 : gameAdvance) : 0;
    const unsigned long long key = (unsigned long long)codepoint | ((unsigned long long)advKey << 32) |
                                   ((unsigned long long)(phase + 3) << 40) | ((unsigned long long)scale1024 << 44) |
                                   ((unsigned long long)(verticalPhase + 3) << 55);

    EnterCriticalSection(&m_cs);
    std::map<unsigned long long, GlyphCell>::iterator it = m_cache.find(key);
    if (it != m_cache.end())
    {
        const GlyphCell *hit = &it->second; // stable until ClearCache/Shutdown; callers must not retain across those
        LeaveCriticalSection(&m_cs);
        return hit;
    }
    LeaveCriticalSection(&m_cs);

    // ---- rasterise (serialised: one FT_Face is not thread safe) ---------
    // A miss is per codepoint/advance/phase/scale variant. Recheck after
    // locking in case another caller populated it; hits avoid raster work.
    EnterCriticalSection(&m_cs);
    {
        std::map<unsigned long long, GlyphCell>::iterator again = m_cache.find(key);
        if (again != m_cache.end())
        {
            const GlyphCell *hit = &again->second;
            LeaveCriticalSection(&m_cs);
            return hit;
        }

        // Bound logical variants to 16384 entries. Clear their optional 2x
        // rasters together; raster allocations make memory use variable.
        if (m_cache.size() >= 16384)
            ClearCache();

        GlyphCell cell;
        if (!Rasterize(codepoint, gameAdvance, phase, scale1024, verticalPhase, key, cell))
        {
            LeaveCriticalSection(&m_cs);
            return NULL;
        }
        std::pair<std::map<unsigned long long, GlyphCell>::iterator, bool> ins =
            m_cache.insert(std::make_pair(key, cell));
        const GlyphCell *result = &ins.first->second;
        LeaveCriticalSection(&m_cs);
        return result;
    }
}
} // namespace vt
