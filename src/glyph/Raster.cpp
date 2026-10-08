// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Raster.h"

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
} // namespace vt
