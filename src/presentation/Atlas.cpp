// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "Atlas.h"
#include "../PixelPlane.h"
#include <algorithm>
#include <cstring>

// Backend-independent sparse atlas layout and pixel packing.
namespace vt::Presentation32::detail
{
bool AtlasShape(size_t count, int maxWidth, int maxHeight, int &width, int &height, int &columns,int scale)
{
    const int cellW=34*scale,cellH=18*scale;
    width = 1024;
    while (width > maxWidth)
        width /= 2;
    while (width >= cellW)
    {
        columns = width / cellW;
        const size_t rows = (std::max<size_t>(1, count) + columns - 1) / columns;
        height = 1;
        while ((size_t)height < rows * cellH && height <= maxHeight)
            height *= 2;
        if (height <= maxHeight)
            return true;
        if (width > maxWidth / 2)
            return false;
        width *= 2;
    }
    return false;
}
void PackOverlay(const PixelPlane &plane, const std::vector<PixelRect> &tiles, uint32_t *output, int pitch, int columns,int scale)
{
    const int cellW=34*scale,cellH=18*scale;
    for (size_t i = 0; i < tiles.size(); ++i)
    {
        const int x = (int)(i % columns) * cellW, y = (int)(i / columns) * cellH;
        auto *pixels = output + (size_t)y * pitch + x;
        for (int row = 0; row < cellH; ++row)
            memset(pixels + (size_t)row * pitch, 0, cellW * 4);
        const auto r = tiles[i];
        plane.OverlayRect(pixels, pitch, {r.left - 1, r.top - 1, r.right + 1, r.bottom + 1},scale);
    }
}
} // namespace vt::Presentation32::detail
