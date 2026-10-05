#include "GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_SYNTHESIS_H

#include <string.h>
#include <stdlib.h>

namespace vt
{
    GlyphSource::GlyphSource()
        : m_lib(NULL), m_face(NULL), m_size(13),
          m_sizeLatin(13), m_sizeCJK(16), m_cjkFrom(0x2E80u), m_currentSize(0),
          m_stride(3), m_lines(16), m_baseline(13), m_fit(true)
    {
    }

    void GlyphSource::SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom)
    {
        m_sizeLatin = latinPx > 0 ? latinPx : m_sizeLatin;
        m_sizeCJK   = cjkPx   > 0 ? cjkPx   : m_sizeCJK;
        m_cjkFrom   = cjkFrom;
        m_currentSize = 0;                       // force FT_Set_Pixel_Sizes
        m_cache.clear();
    }

    GlyphSource::~GlyphSource()
    {
        Shutdown();
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
        m_currentSize = 0;

        FT_Library lib = NULL;
        if (FT_Init_FreeType(&lib))
            return false;

        FT_Face face = NULL;
        if (FT_New_Face(lib, ttfPath, 0, &face))
        {
            FT_Done_FreeType(lib);
            return false;
        }

        // Variable fonts: the default instance is often not the weight we want
        // (NotoSerifSC-VF defaults to ExtraLight 200).
        if (weight > 0 && FT_HAS_MULTIPLE_MASTERS(face))
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
                        const char* tag = (const char*)&mm->axis[a].tag;   // big endian in memory? use shifts
                        const unsigned int t = mm->axis[a].tag;
                        char t2[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
                        (void)tag;
                        if (!strcmp(t2, "wght"))
                            coords[a] = weight << 16;
                    }
                    FT_Set_Var_Design_Coordinates(face, mm->num_axis, coords);
                    free(coords);
                }
                FT_Done_MM_Var(lib, mm);
            }
        }

        if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixelSize))
        {
            FT_Done_Face(face);
            FT_Done_FreeType(lib);
            return false;
        }

        m_lib  = lib;
        m_face = face;
        m_cache.clear();
        return true;
    }

    void GlyphSource::Shutdown()
    {
        if (m_face)
        {
            FT_Done_Face((FT_Face)m_face);
            m_face = NULL;
        }
        if (m_lib)
        {
            FT_Done_FreeType((FT_Library)m_lib);
            m_lib = NULL;
        }
        m_cache.clear();
    }

    static void FitColumns(GlyphCell& cell, int advance, int strideBytes, int lines);

    const GlyphCell* GlyphSource::Get(unsigned int codepoint, int gameAdvance)
    {
        if (!m_face)
            return NULL;

        // cache key: codepoint + which advance source was used
        const unsigned int key = codepoint | (gameAdvance > 0 ? 0x80000000u : 0u);
        std::map<unsigned int, GlyphCell>::iterator it = m_cache.find(key);
        if (it != m_cache.end())
            return &it->second;

        FT_Face face = (FT_Face)m_face;

        // per-script pixel size (Latin and CJK are drawn at different sizes,
        // exactly like the game's own bitmap font)
        const int wantSize = (codepoint >= m_cjkFrom) ? m_sizeCJK : m_sizeLatin;
        if (wantSize != m_currentSize)
        {
            if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)wantSize))
                return NULL;
            m_currentSize = wantSize;
        }

        if (FT_Load_Char(face, (FT_ULong)codepoint, FT_LOAD_TARGET_MONO | FT_LOAD_RENDER))
            return NULL;

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
                    const int bit = (src[c >> 3] >> (7 - (c & 7))) & 1;
                    if (!bit)
                        continue;
                    unsigned char* dst = cell.bits + y * m_stride + (x >> 3);
                    *dst |= (unsigned char)(0x80 >> (x & 7));
                }
            }
        }
        else
        {
            // grayscale fallback (should not happen with FT_LOAD_TARGET_MONO)
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
                    if (src[c] < 128)
                        continue;
                    unsigned char* dst = cell.bits + y * m_stride + (x >> 3);
                    *dst |= (unsigned char)(0x80 >> (x & 7));
                }
            }
        }

        if (m_fit)
            FitColumns(cell, advance, m_stride, m_lines);

        m_cache[key] = cell;
        return &m_cache[key];
    }

    // Condense the drawn ink horizontally so it fits `advance` columns.
    // Nearest-column resampling with OR-combining: 1bpp safe, no blur, and it
    // keeps the vertical metrics (which is what the baseline depends on).
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
        const int dstW = advance;                // first `advance` columns are ours
        unsigned char tmp[3 * 32];
        memset(tmp, 0, sizeof(tmp));

        // OR-combine every source column that falls into a destination column.
        // (Plain nearest-column sampling silently drops one-pixel-wide strokes:
        // the vertical stem of U+4E2D sits in a single column that gets skipped.)
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
                    if (!(cell.bits[y * strideBytes + (s >> 3)] & (0x80 >> (s & 7))))
                        continue;
                    tmp[y * strideBytes + (t >> 3)] |= (unsigned char)(0x80 >> (t & 7));
                    break;                        // one hit is enough for this column
                }
            }
        }
        memcpy(cell.bits, tmp, sizeof(cell.bits));
    }
}
