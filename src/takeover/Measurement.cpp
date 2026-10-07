// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "LineLayout.h"

// Read-only dynamic-box width/ink measurement; never consumes an active row plan.
namespace vt::Takeover
{
using namespace detail;
bool MeasureDynamicWidth(void *bitFont, const wchar_t *text, int maxWidth, int *width)
{
    if (!DynamicTextWidthEnabled() || !bitFont || !text || !width || maxWidth < 0 || maxWidth > 32768)
        return false;
    if (!g_tried)
        Init();
    if (!g_ready)
        return false;
    const unsigned char *bf = (const unsigned char *)bitFont;
    const unsigned char *fd = *(const unsigned char *const *)(bf + BF_INTERNAL);
    if (!fd || *(const int *)(fd + IF_LINES) != g_src.Lines())
        return false;
    const unsigned short *map = *(const unsigned short *const *)(fd + IF_SYMTABLE);
    const unsigned char *data = *(const unsigned char *const *)(fd + IF_BITMAPS);
    const unsigned int bytes = *(const unsigned int *)(fd + IF_SYMBOLBYTES);
    const int extra = *(const int *)(bf + 0x2C);
    if (!map || !data || !bytes || bytes > 4096 || extra < 0 || extra > 16)
        return false;
    int penQ = 0, legacyWidth = 0, longestQ = 0, mixedQ = 0, count = 0;
    unsigned int previous = 0;
    LineGlyph prior = {};
    for (int i = 0; i <= kLineLimit; ++i)
    {
        const unsigned int cp = text[i];
        if (i == kLineLimit && cp)
            return false;
        if (!cp || cp == '\r' || cp == '\n')
        {
            // Preserve the engine's automatic wrapping/height semantics.
            // Only explicit lines fitting the old limit can resize a box.
            if (maxWidth && legacyWidth > maxWidth)
                return false;
            int rowQ = penQ;
            if (prior.penQ + prior.rightQ > rowQ)
                rowQ = prior.penQ + prior.rightQ;
            if (rowQ > longestQ)
                longestQ = rowQ;
            if (!cp)
            {
                if (!count)
                    return false;
                int padding = Cfg::ConfigInt("LineWidthPadding", 4);
                if (padding < 1)
                    padding = 1;
                if (padding > 32)
                    padding = 32;
                const int measured = (longestQ + 3) / 4 + padding;
                if (measured > 32768 || (maxWidth && measured > maxWidth))
                    return false;
                *width = measured;
                return true;
            }
            penQ = legacyWidth = 0;
            previous = 0;
            prior = {};
            continue;
        }
        if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || !map[cp])
            return false;
        LineGlyph item = {};
        item.cp = cp;
        if (!NaturalGlyph(item, previous, previous ? &prior : NULL, penQ, 1024, mixedQ) &&
            !BitmapGlyph(item, data + (size_t)(map[cp] - 1) * bytes, bytes, g_src.Lines(), extra, penQ, 1024))
            return false;
        legacyWidth += data[(size_t)(map[cp] - 1) * bytes] + extra;
        previous = item.bitmap ? 0 : cp;
        prior = item;
        ++count;
    }
    return false;
}

bool MeasureTextInkY(void *bitFont, const wchar_t *text, int anchorX, int align, InkY *ink)
{
    if (!DynamicTextWidthEnabled() || !bitFont || !text || !ink || anchorX < -32768 || anchorX > 32768)
        return false;
    if (!g_tried)
        Init();
    if (!g_ready)
        return false;
    const unsigned char *bf = (const unsigned char *)bitFont;
    const unsigned char *fd = *(const unsigned char *const *)(bf + BF_INTERNAL);
    if (!fd || *(const int *)(fd + IF_LINES) != g_src.Lines())
        return false;
    const unsigned short *map = *(const unsigned short *const *)(fd + IF_SYMTABLE);
    const unsigned char *data = *(const unsigned char *const *)(fd + IF_BITMAPS);
    const unsigned int bytes = *(const unsigned int *)(fd + IF_SYMBOLBYTES);
    const int extra = *(const int *)(bf + 0x2C);
    if (!map || !data || !bytes || bytes > 4096 || extra < 0 || extra > 16)
        return false;
    const int lineHeight = *(const int *)(bf + 0x1C);
    std::vector<LineGlyph> items;
    int penQ = 0, mixedQ = 0, rowY = 0, lines = 1;
    int inkTop = 32768, inkBottom = -32768;
    int inkLeft = 32768, inkRight = -32768;
    unsigned int previous = 0;
    for (int i = 0; i <= kLineLimit; ++i)
    {
        const unsigned int cp = text[i];
        if (i == kLineLimit && cp)
            return false;
        if (!cp || cp == '\r' || cp == '\n')
        {
            bool rowInk = false;
            const int widthQ =
                items.empty()
                    ? 0
                    : (penQ > items.back().penQ + items.back().rightQ ? penQ : items.back().penQ + items.back().rightQ);
            int originQ = anchorX * 4;
            if (align & 1)
                originQ -= (widthQ + 1) / 2;
            else if (align & 2)
                originQ -= widthQ;
            for (const LineGlyph &item : items)
            {
                int phase = 0;
                const int q = originQ + item.penQ + item.shiftQ;
                const int pixelX = item.bitmap ? (int)floor(q / 4.0) : LineRasterX(q, &phase);
                const GlyphCell *cell = item.bitmap ? NULL : g_src.Get(item.cp, -1, phase);
                if (!item.bitmap && !cell)
                    return false;
                const int rows = item.bitmap ? g_src.Lines() : cell->inkRows;
                for (int row = 0; row < rows; ++row)
                    for (int col = 0; col < 24; ++col)
                    {
                        const bool lit =
                            item.bitmap
                                ? col < item.bitmap[0] &&
                                      (item.bitmap[1 + row * 3 + col / 8] & (0x80 >> (col & 7))) != 0
                                : (Cfg::AntiAlias() ? cell->cov[row * 24 + col] != 0
                                                    : (cell->bits[row * 3 + col / 8] & (0x80 >> (col & 7))) != 0);
                        if (!lit)
                            continue;
                        rowInk = true;
                        const int litX = pixelX + col + (cell ? cell->inkX : 0);
                        if (litX < inkLeft)
                            inkLeft = litX;
                        if (litX + 1 > inkRight)
                            inkRight = litX + 1;
                        const int pixelY = rowY + row + (cell ? cell->inkY : 0);
                        if (pixelY < inkTop)
                            inkTop = pixelY;
                        if (pixelY + 1 > inkBottom)
                            inkBottom = pixelY + 1;
                    }
            }
            // Keep deliberately empty first/last rows rather than
            // mistaking them for unused font-cell padding.
            if ((!cp || lines == 1) && !rowInk)
                return false;
            if (!cp)
            {
                if (inkBottom <= inkTop || inkBottom > 32768)
                    return false;
                *ink = {inkTop, inkBottom, lines, inkLeft, inkRight};
                return true;
            }
            if (lineHeight < 1 || lineHeight > 128 || rowY > 32768 - lineHeight)
                return false;
            if (cp == '\r' && text[i + 1] == '\n')
                ++i;
            rowY += lineHeight;
            ++lines;
            items.clear();
            penQ = mixedQ = 0;
            previous = 0;
            continue;
        }
        if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || !map[cp])
            return false;
        LineGlyph item = {};
        item.cp = cp;
        if (!NaturalGlyph(item, previous, items.empty() ? NULL : &items.back(), penQ, 1024, mixedQ) &&
            !BitmapGlyph(item, data + (size_t)(map[cp] - 1) * bytes, bytes, g_src.Lines(), extra, penQ, 1024))
            return false;
        previous = item.bitmap ? 0 : cp;
        items.push_back(item);
    }
    return false;
}
} // namespace vt::Takeover
