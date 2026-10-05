#include "PixelWriter.h"

namespace vt
{
    const ColorFormat RGB565 = { 11, 5, 5, 6, 0, 5 };

    int DrawCell(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                 unsigned short color)
    {
        if (!t.base || t.pitch <= 0 || cellLines <= 0)
            return 0;

        int written = 0;
        for (int r = 0; r < cellLines; ++r)
        {
            const int sy = y + r;
            if (sy < t.clipT || sy > t.clipB)
                continue;

            unsigned short* row = t.base + (size_t)t.pitch * sy;

            for (int c = 0; c < 8 * 3; ++c)          // up to 24 columns (stride 3)
            {
                const int sx = x + c;
                if (sx < t.clipL)
                    continue;
                if (sx > t.clipR)
                    break;

                const unsigned char byte = cell.bits[r * 3 + (c >> 3)];
                if (!(byte & (0x80 >> (c & 7))))
                    continue;

                row[sx] = color;                      // engine writes the word verbatim
                ++written;
            }
        }
        return written;
    }

    unsigned short BlendTowardWhite(unsigned short color, int ratio, const ColorFormat& fmt)
    {
        ratio &= 0xFF;                                // the engine masks it too
        if (ratio == 0)
            return color;

        const int rMask = (1 << fmt.redBits) - 1;
        const int gMask = (1 << fmt.greenBits) - 1;
        const int bMask = (1 << fmt.blueBits) - 1;

        const int r = (color >> fmt.redShift) & rMask;
        const int g = (color >> fmt.greenShift) & gMask;
        const int b = (color >> fmt.blueShift) & bMask;

        const int nr = r + ((rMask - r) * ratio) / 256;
        const int ng = g + ((gMask - g) * ratio) / 256;
        const int nb = b + ((bMask - b) * ratio) / 256;

        return (unsigned short)((nr << fmt.redShift) | (ng << fmt.greenShift) | (nb << fmt.blueShift));
    }

    int DrawString(const Target& t, GlyphSource& src, const unsigned int* codepoints,
                   const int* advances, int count, int x, int y, int cellLines,
                   unsigned short color, int reveal)
    {
        const bool gradient = (reveal >= 1 && reveal <= 8);
        int ratio = gradient ? (9 - reveal) * 31 : 0;

        for (int i = 0; i < count; ++i)
        {
            const unsigned int cp = codepoints[i];

            // engine: CR/LF are skipped without advancing the pen
            if (cp == 0x0D || cp == 0x0A)
                continue;

            const int advance = advances ? advances[i] : -1;
            const GlyphCell* cell = src.Get(cp, advance);

            const unsigned short use = gradient
                ? BlendTowardWhite(color, ratio, RGB565)
                : color;

            if (cell)
                DrawCell(t, *cell, x, y, cellLines, use);

            x += (cell ? cell->width : (advance > 0 ? advance : 0));
            ratio += 31;
        }
        return x;
    }
}
