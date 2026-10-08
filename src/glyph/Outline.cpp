// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Raster.h"

#include FT_OUTLINE_H
#include <cmath>

namespace vt
{
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
} // namespace vt
