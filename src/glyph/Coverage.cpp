// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Raster.h"

#include <cmath>
#include <utility>

namespace vt
{
bool GlyphSource::BuildCoverage(const RasterContext &context, int gameAdvance, GlyphCell &cell)
{
    const FT_GlyphSlot g = context.face->glyph;
    const int ss = m_ss;
    // Area coverage from the high-resolution bitmap. Every source
    // sample contributes to its target pixel, so no rows are skipped.
    int rows = m_lines;
    if (gameAdvance <= 0)
        cell.inkX = (int)floor((double)g->bitmap_left / ss);
    if (g->bitmap.rows)
    {
        const int firstY = m_baseline * ss - g->bitmap_top;
        cell.inkY = (int)floor((double)firstY / ss);
        rows = (firstY + (int)g->bitmap.rows - cell.inkY * ss + ss - 1) / ss;
        if (rows > 32)
        {
            return false;
        }
        cell.inkRows = rows;
    }
    if (gameAdvance <= 0 && (int)g->bitmap.width + g->bitmap_left - cell.inkX * ss > 24 * ss)
    {
        return false;
    }
    unsigned int acc[24 * 32] = {0};
    for (unsigned int r = 0; r < g->bitmap.rows; ++r)
    {
        const int sy = m_baseline * ss - g->bitmap_top + (int)r - cell.inkY * ss;
        if (sy < 0 || sy >= rows * ss)
            continue;
        const unsigned char *src = g->bitmap.buffer + r * g->bitmap.pitch;
        for (unsigned int c = 0; c < g->bitmap.width; ++c)
        {
            const int sx = g->bitmap_left + (int)c - cell.inkX * ss;
            if (sx < 0 || sx >= m_stride * 8 * ss)
                continue;
            const int cov = (g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                                ? (((src[c >> 3] >> (7 - (c & 7))) & 1) ? 255 : 0)
                                : src[c];
            acc[(sy / ss) * 24 + sx / ss] += cov;
        }
    }
    const unsigned int div = ss * ss;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < m_stride * 8; ++x)
        {
            int cov = (acc[y * 24 + x] + div / 2) / div;
            // Fixed-point outline fitting can leave a tiny fringe just
            // beyond an explicit legacy cell. Clip that compatibility
            // path exactly; natural line glyphs keep their bearings.
            if (m_fit && gameAdvance > 0 && x >= gameAdvance)
                cov = 0;
            cell.cov[y * 24 + x] = (unsigned char)cov;
            if (cov >= 128)
                cell.bits[y * m_stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
        }
    return true;
}

void GlyphSource::BuildHighResolution(unsigned int codepoint, int gameAdvance, unsigned long long key,
                                      const RasterContext &context, GlyphCell &cell)
{
    // FreeType takes a mutable matrix pointer but does not modify it.
    FT_Matrix mat = context.mat;
    const FT_Vector &delta = context.delta;
    const FT_Int32 loadFlags = context.loadFlags;
    // Hint directly on a grid at least as dense as the observed output.
    // D3D9 filters down to the actual viewport, including fractions;
    // logical advances/bearings above remain the same 1x layout.
    if (m_highResolution)
    {
        const int scale = m_highScale;
        const bool cjk = UsesCJKFace(codepoint) && m_faceB;
        void **high = cjk ? &m_highB : &m_highA;
        if (!*high)
            *high = OpenFace((cjk ? m_sizeCJK : m_sizeLatin) * scale, cjk ? NULL : (m_latinPath[0] ? m_latinPath : NULL));
        FT_Face hf = (FT_Face)*high;
        FT_Vector hd = {delta.x * scale / m_ss, delta.y * scale / m_ss};
        if (hf)
            FT_Set_Transform(hf, &mat, &hd);
        if (hf && !FT_Load_Char(hf, (FT_ULong)codepoint, loadFlags | FT_LOAD_RENDER))
        {
            const auto &bitmap = hf->glyph->bitmap;
            if (bitmap.width <= 256 && bitmap.rows <= 256)
            {
                GlyphRaster2 raster;
                raster.scale = scale;
                raster.left = hf->glyph->bitmap_left;
                raster.top = m_baseline * scale - hf->glyph->bitmap_top;
                raster.width = (int)bitmap.width;
                raster.rows = (int)bitmap.rows;
                raster.coverage.resize((size_t)raster.width * raster.rows);
                for (int r = 0; r < raster.rows; ++r)
                    for (int c = 0; c < raster.width; ++c)
                    {
                        const auto *src = bitmap.buffer + r * bitmap.pitch;
                        int cov = bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? ((src[c / 8] & (0x80 >> (c & 7))) ? 255 : 0)
                                                                          : src[c];
                        if (m_fit && gameAdvance > 0 && (raster.left + c < 0 || raster.left + c >= gameAdvance * scale))
                            cov = 0;
                        raster.coverage[(size_t)r * raster.width + c] = (unsigned char)cov;
                    }
                cell.raster2 = &m_highCache.emplace(key, std::move(raster)).first->second;
            }
        }
    }
}
} // namespace vt
