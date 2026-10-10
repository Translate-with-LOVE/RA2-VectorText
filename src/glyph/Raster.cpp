// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Raster.h"
#include FT_OUTLINE_H
#include <utility>

#include <cmath>
#include <string.h>

namespace vt
{
// Called only on a cache miss with m_cs held. The same transform is
// reused for the logical coverage and optional presentation raster.
bool GlyphSource::Rasterize(unsigned int codepoint, int gameAdvance, int phase, int scale1024, int verticalPhase, unsigned long long key,
                            GlyphCell &cell)
{
    RasterContext context{};
    FT_Face &face = context.face;
    face = (FT_Face)FaceHandleFor(codepoint);
    if (!FT_Get_Char_Index(face, codepoint))
    {
        return false;
    }

    // The transform runs after grid fitting: fractional pen positions
    // shift the antialiasing coverage without moving the logical pen.
    FT_Matrix &mat = context.mat;
    FT_Vector &delta = context.delta;
    mat.xx = scale1024 * 64;
    mat.xy = 0;
    mat.yx = 0;
    mat.yy = scale1024 * 64;
    delta.x = (FT_Pos)(phase * 16 * m_ss); // quarter pixels -> rasteriser space
    delta.y = (FT_Pos)(-verticalPhase * 16 * m_ss); // screen Y grows downwards
    FT_Set_Transform(face, &mat, phase || verticalPhase ? &delta : NULL);

    const bool aa = m_aa;
    // LIGHT preserves spacing while aligning horizontal strokes to the
    // vertical grid. NORMAL and unhinted are available for comparison.
    const FT_Int32 grayFlags =
        m_hinting == 1 ? FT_LOAD_TARGET_NORMAL : (m_hinting == 2 ? FT_LOAD_NO_HINTING : FT_LOAD_TARGET_LIGHT);
    const FT_Int32 loadFlags = context.loadFlags = (aa ? grayFlags : FT_LOAD_TARGET_MONO) | FT_LOAD_NO_BITMAP;
    if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags))
    {
        return false;
    }

    const FT_GlyphSlot g = face->glyph;

    memset(&cell, 0, sizeof(cell));

    const int advance = (gameAdvance > 0) ? gameAdvance : (int)((g->advance.x + 32 * m_ss) / (64 * m_ss));
    cell.width = (unsigned char)(advance < 0 ? 0 : (advance > 255 ? 255 : advance));
    // Natural layout follows the font's fractional design advance,
    // independent of raster hinting and sampling resolution. Otherwise
    // normal hinting at 1x rounds every letter's pen to whole pixels.
    cell.advanceQ = (int)floor((double)g->linearHoriAdvance * scale1024 / (16384.0 * m_ss * 1024.0) + 0.5);
    FitOutline(codepoint, gameAdvance, phase, scale1024, context, cell);
    if (FT_Load_Char(face, (FT_ULong)codepoint, loadFlags | FT_LOAD_RENDER))
    {
        return false;
    }
    if (!BuildCoverage(context, gameAdvance, cell))
        return false;
    BuildHighResolution(codepoint, gameAdvance, key, context, cell);
    return true;
}

void GlyphSource::FitOutline(unsigned int codepoint, int gameAdvance, int phase, int scale1024, RasterContext &context,
                             GlyphCell &cell)
{
    const FT_Face face = context.face;
    const FT_GlyphSlot g = face->glyph;
    FT_Matrix &mat = context.mat;
    FT_Vector &delta = context.delta;
    // Fit the OUTLINE uniformly before rendering. Never resize an
    // already rasterised bitmap or stretch ideographs to fill 16 rows.
    // Keep the same baseline for all letters, including descenders.
    const int ss = m_ss;
    if (g->format == FT_GLYPH_FORMAT_OUTLINE && g->outline.n_points)
    {
        FT_BBox box;
        FT_Outline_Get_CBox(&g->outline, &box);
        const double unit = 64.0 * ss;
        const double inkW = (box.xMax - box.xMin) / unit;
        const bool smallMark = codepoint == '.' || codepoint == ',' || codepoint == ':' || codepoint == ';' ||
                               codepoint == '!' || codepoint == '?' || codepoint == 0x3001 || codepoint == 0x3002 ||
                               codepoint == 0xFF0C || codepoint == 0xFF0E || codepoint == 0xFF1A ||
                               codepoint == 0xFF1B || codepoint == 0xFF01 || codepoint == 0xFF1F;
        const bool opening = codepoint == '(' || codepoint == '[' || codepoint == '{' || codepoint == 0xFF08 ||
                             codepoint == 0x3010 || codepoint == 0x300C || codepoint == 0x300E || codepoint == 0x2018 ||
                             codepoint == 0x201C;
        const bool closing = codepoint == ')' || codepoint == ']' || codepoint == '}' || codepoint == 0xFF09 ||
                             codepoint == 0x3011 || codepoint == 0x300D || codepoint == 0x300F || codepoint == 0x2019 ||
                             codepoint == 0x201D;
        const bool dash = codepoint == 0x2013 || codepoint == 0x2014 || codepoint == 0x2015;
        const bool symbol = smallMark || opening || closing || dash || codepoint == 0x2025 || codepoint == 0x2026;
        const bool fitCell = m_fit && gameAdvance > 0;
        // Narrow legacy punctuation cells need fractional breathing room.
        // Reserve only 1/4 pixel per side; outline scaling remains uniform.
        const double inset = fitCell && symbol && gameAdvance >= 2 ? 0.25 : 0.0;
        const double limit = fitCell ? (double)gameAdvance : 24.0;
        const double available = limit - 2.0 * inset;
        double scale = 1.0;
        if (fitCell && inkW > available)
            scale = available / inkW;
        mat.xx = mat.yy = (FT_Fixed)(scale * scale1024 * 64.0 + 0.5);
        // The CBox already includes the initial phase. Reposition
        // bearings that overflow the cell without changing the shape.
        const double initialPhase = phase * 0.25;
        double left = (box.xMin / unit - initialPhase) * scale + initialPhase;
        double right = (box.xMax / unit - initialPhase) * scale + initialPhase;
        double offset = 0.0;
        if (fitCell && symbol)
        {
            // Align paired marks toward the text they enclose; centre
            // stops/colons in their existing cell. The pen never changes.
            double targetLeft = (limit - inkW * scale) * 0.5;
            if (opening)
                targetLeft = limit - inset - inkW * scale;
            if (closing)
                targetLeft = inset;
            offset = targetLeft - left;
        }
        if (gameAdvance > 0)
        {
            if (right + offset > limit - inset)
                offset = limit - inset - right;
            if (left + offset < inset)
                offset = inset - left;
        }
        cell.inkLeftQ = (int)floor((left + offset) * 4.0 + 0.5);
        cell.inkRightQ = (int)ceil((right + offset) * 4.0);
        delta.x = (FT_Pos)((initialPhase + offset) * unit);
        // All direct-drawing glyphs share the same baseline. Ink above
        // or below the old 16-row cell must retain its actual bearing;
        // fitting each ink box vertically made Chinese letters bounce.
        // Keep the run's fractional vertical placement from Rasterize.
        FT_Set_Transform(face, &mat, &delta);
    }
}

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
        const bool symbol = context.face == m_faceSymbol;
        const bool cjk = !symbol && context.face == m_faceB;
        void **high = symbol ? &m_highSymbol : (cjk ? &m_highB : &m_highA);
        if (!*high)
            *high = OpenFace((symbol ? m_sizeSymbol : (cjk ? m_sizeMain : m_sizeLatin)) * scale,
                             symbol ? m_symbolPath : (cjk ? NULL : (m_latinPath[0] ? m_latinPath : NULL)),
                             symbol ? m_weightSymbol : (cjk ? m_weight : m_weightLatin));
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
