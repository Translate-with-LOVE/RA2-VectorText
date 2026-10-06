#include "GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_MODULE_H

#include <string.h>
#include <stdlib.h>

namespace vt
{
    GlyphSource::GlyphSource()
        : m_lib(NULL), m_faceA(NULL), m_faceB(NULL), m_size(13),
          m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u),
          m_stride(3), m_lines(16), m_baseline(13), m_fit(true), m_aa(false), m_darkening(0),
          m_path(NULL), m_weight(400), m_csInit(false)
    {
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

    void GlyphSource::CloseFace(void** face)
    {
        if (*face)
        {
            FT_Done_Face((FT_Face)*face);
            *face = NULL;
        }
    }

    void* GlyphSource::OpenFace(int pixelSize)
    {
        if (!m_lib || !m_path)
            return NULL;

        FT_Face face = NULL;
        if (FT_New_Face((FT_Library)m_lib, m_path, 0, &face))
            return NULL;

        // Variable fonts: the default instance is often not the weight we want
        // (NotoSerifSC-VF defaults to ExtraLight 200).
        if (m_weight > 0 && FT_HAS_MULTIPLE_MASTERS(face))
        {
            FT_MM_Var* mm = NULL;
            if (!FT_Get_MM_Var(face, &mm))
            {
                FT_Fixed* coords = (FT_Fixed*)malloc(sizeof(FT_Fixed) * mm->num_axis);
                if (coords)
                {
                    for (FT_UInt a = 0; a < mm->num_axis; ++a)
                    {
                        coords[a] = mm->axis[a].def;
                        const unsigned int t = mm->axis[a].tag;
                        char tag[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
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

    bool GlyphSource::Init(const char* ttfPath, int pixelSize, int weight,
                           int strideBytes, int lines, int baselineRow)
    {
        Shutdown();

        m_stride   = strideBytes;
        m_lines    = lines;
        m_baseline = baselineRow;
        m_size     = pixelSize;
        m_sizeLatin = pixelSize;
        m_sizeCJK   = pixelSize;
        m_path     = ttfPath;
        m_weight   = weight;

        FT_Library lib = NULL;
        if (FT_Init_FreeType(&lib))
            return false;
        m_lib = lib;

        // stem darkening must be set before the faces are created
        if (m_darkening > 0)
        {
            FT_UInt amount = (FT_UInt)m_darkening;
            FT_Property_Set(lib, "truetype", "darkening", &amount);
        }

        m_faceA = OpenFace(pixelSize);
        if (!m_faceA)
        {
            FT_Done_FreeType(lib);
            m_lib = NULL;
            return false;
        }

        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
        return true;
    }

    void GlyphSource::Shutdown()
    {
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
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    void GlyphSource::SetAntiAlias(bool on)
    {
        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_aa = on;
            m_cache.clear();                 // coverage is baked into the cell
            LeaveCriticalSection(&m_cs);
        }
        else
        {
            m_aa = on;
        }
    }

    void GlyphSource::SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom)
    {
        if (latinPx > 0) m_sizeLatin = latinPx;
        if (cjkPx > 0)   m_sizeCJK   = cjkPx;
        m_cjkFrom = cjkFrom;

        if (!m_faceA)
            return;

        // keep one face per distinct size so nothing thrashes per glyph
        if (m_sizeCJK == m_sizeLatin)
        {
            if (m_faceB)
            {
                CloseFace(&m_faceB);
                if (FT_Set_Pixel_Sizes((FT_Face)m_faceA, 0, (FT_UInt)m_sizeLatin))
                    return;
            }
        }
        else
        {
            if (!m_faceB)
            {
                m_faceB = OpenFace(m_sizeCJK);
            }
            else if (FT_Set_Pixel_Sizes((FT_Face)m_faceB, 0, (FT_UInt)m_sizeCJK))
            {
                return;
            }
        }

        if (m_csInit)
        {
            EnterCriticalSection(&m_cs);
            m_cache.clear();
            LeaveCriticalSection(&m_cs);
        }
    }

    // Condense the drawn ink horizontally so it fits `advance` columns.
    // Area/OR merging: 1bpp safe, keeps one-pixel-wide strokes, and preserves
    // the vertical metrics (which is what the baseline depends on).
    static void FitColumns(GlyphCell& cell, int advance, int strideBytes, int lines)
    {
        if (advance <= 0)
            return;

        int minX = 1 << 30, maxX = -1;
        for (int y = 0; y < lines; ++y)
            for (int x = 0; x < strideBytes * 8; ++x)
                if (cell.bits[y * strideBytes + (x >> 3)] & (0x80 >> (x & 7)))
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                }

        if (maxX < 0 || maxX < advance)          // nothing to do
            return;

        const int inkW = maxX - minX + 1;
        const int dstW = advance;                // the first `advance` columns are ours
        unsigned char tmp[3 * 32];
        unsigned char tmpCov[24 * 32];
        memset(tmp, 0, sizeof(tmp));
        memset(tmpCov, 0, sizeof(tmpCov));

        for (int y = 0; y < lines; ++y)
        {
            for (int t = 0; t < dstW; ++t)
            {
                const int sBegin = minX + (t * inkW) / dstW;
                const int sEnd   = minX + (((t + 1) * inkW) / dstW) - 1;
                for (int s = sBegin; s <= sEnd && s <= maxX; ++s)
                {
                    if (s < minX)
                        continue;
                    if (cell.cov[y * 24 + s] > tmpCov[y * 24 + t])
                        tmpCov[y * 24 + t] = cell.cov[y * 24 + s];
                    if (!(cell.bits[y * strideBytes + (s >> 3)] & (0x80 >> (s & 7))))
                        continue;
                    tmp[y * strideBytes + (t >> 3)] |= (unsigned char)(0x80 >> (t & 7));
                }
            }
        }
        memcpy(cell.bits, tmp, sizeof(cell.bits));
        memcpy(cell.cov, tmpCov, sizeof(cell.cov));
    }

    const GlyphCell* GlyphSource::Get(unsigned int codepoint, int gameAdvance)
    {
        if (!m_faceA)
            return NULL;

        // cache key: codepoint + which advance source was used
        const unsigned int key = codepoint | (gameAdvance > 0 ? 0x80000000u : 0u);

        EnterCriticalSection(&m_cs);
        std::map<unsigned int, GlyphCell>::iterator it = m_cache.find(key);
        if (it != m_cache.end())
        {
            const GlyphCell* hit = &it->second;   // map nodes are stable
            LeaveCriticalSection(&m_cs);
            return hit;
        }
        LeaveCriticalSection(&m_cs);

        // ---- rasterise (serialised: one FT_Face is not thread safe) ---------
        // A miss happens once per glyph, so holding the lock here costs nothing
        // in practice while making concurrent calls safe.
        EnterCriticalSection(&m_cs);
        {
            std::map<unsigned int, GlyphCell>::iterator again = m_cache.find(key);
            if (again != m_cache.end())
            {
                const GlyphCell* hit = &again->second;
                LeaveCriticalSection(&m_cs);
                return hit;
            }

            // bounded cache: a pathological stream of distinct codepoints must
            // not grow without limit (131k keys x ~900 B would be ~118 MB)
            if (m_cache.size() >= 16384)
                m_cache.clear();

            FT_Face face = (FT_Face)((codepoint >= m_cjkFrom && m_faceB) ? m_faceB : m_faceA);
            const bool aa = m_aa;
            const FT_Int32 loadFlags = (aa ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO) | FT_LOAD_RENDER;
            if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags))
            {
                LeaveCriticalSection(&m_cs);
                return NULL;
            }

            const FT_GlyphSlot g = face->glyph;

            GlyphCell cell;
            memset(&cell, 0, sizeof(cell));

            const int advance = (gameAdvance > 0) ? gameAdvance : (int)(g->advance.x >> 6);
            cell.width = (unsigned char)(advance < 0 ? 0 : (advance > 255 ? 255 : advance));

            // place the bitmap: baselineRow - bitmap_top gives the first row
            const int x0 = g->bitmap_left;
            const int y0 = m_baseline - g->bitmap_top;

            if (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
            {
                for (unsigned int r = 0; r < g->bitmap.rows; ++r)
                {
                    const int y = y0 + (int)r;
                    if (y < 0 || y >= m_lines)
                        continue;
                    const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                    for (unsigned int c = 0; c < g->bitmap.width; ++c)
                    {
                        const int x = x0 + (int)c;
                        if (x < 0 || x >= m_stride * 8)
                            continue;
                        if (!((src[c >> 3] >> (7 - (c & 7))) & 1))
                            continue;
                        cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                        cell.cov[y * 24 + x] = 255;
                    }
                }
            }
            else
            {
                for (unsigned int r = 0; r < g->bitmap.rows; ++r)
                {
                    const int y = y0 + (int)r;
                    if (y < 0 || y >= m_lines)
                        continue;
                    const unsigned char* src = g->bitmap.buffer + r * g->bitmap.pitch;
                    for (unsigned int c = 0; c < g->bitmap.width; ++c)
                    {
                        const int x = x0 + (int)c;
                        if (x < 0 || x >= m_stride * 8)
                            continue;
                        const unsigned char cov = src[c];
                        if (!cov)
                            continue;
                        cell.cov[y * 24 + x] = cov;
                        if (cov < 128)
                            continue;
                        cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
                    }
                }
            }

            if (m_fit)
                FitColumns(cell, advance, m_stride, m_lines);

            std::pair<std::map<unsigned int, GlyphCell>::iterator, bool> ins =
                m_cache.insert(std::make_pair(key, cell));
            const GlyphCell* result = &ins.first->second;
            LeaveCriticalSection(&m_cs);
            return result;
        }
    }
}
