// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../GlyphSource.h"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace vt
{
// Per-miss FreeType state shared by the raster stages. Faces and caches
// remain owned by GlyphSource; every stage runs under its cache lock.
struct GlyphSource::RasterContext
{
    FT_Face face = nullptr;
    FT_Matrix mat{};
    FT_Vector delta{};
    FT_Int32 loadFlags = 0;
};
} // namespace vt
