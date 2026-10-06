#include "PixelWriter.h"
#include <math.h>

namespace vt
{
    const ColorFormat RGB565 = { 11, 5, 5, 6, 0, 5 };

    // Coverage gamma: linear coverage makes antialiased small text look thin and
    // hazy, because the perceived weight of partial pixels is sub-linear.  A
    // gamma table (cov' = 255 * (cov/255)^(1/gamma), gamma > 1) restores the
    // weight the eye expects.  Built once, applied per blended pixel.
    static unsigned char g_covLut[256];
    static bool g_covLutReady = false;
    static double g_gamma = 1.0;

    void SetCoverageGamma(double gamma)
    {
        if (gamma < 0.5) gamma = 0.5;
        if (gamma > 3.0) gamma = 3.0;
        g_gamma = gamma;
        for (int i = 0; i < 256; ++i)
        {
            const double v = (double)i / 255.0;
            double o = (gamma == 1.0) ? v : pow(v, 1.0 / gamma);
            int iv = (int)(o * 255.0 + 0.5);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            g_covLut[i] = (unsigned char)iv;
        }
        g_covLutReady = true;
    }

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

    // ---------------------------------------------------------------------
    //  32-bit compositing path
    //
    //  The game's surface is 16-bit (5/6/5), so blending straight into it
    //  quantises every partial pixel and the rounding error shows up as
    //  banding on large antialiased areas.  Here the blend is computed in a
    //  32-bit linear-light domain with a 12-bit fixed point table, and only the
    //  final value is quantised - with an ordered (Bayer) offset that turns the
    //  remaining quantisation into a fine dither instead of a hard step.
    // ---------------------------------------------------------------------
    static bool g_linearBlend = false;
    static bool g_dither = false;

    static unsigned short g_srgbToLin[256];        // 0..4095, gamma 2.2
    static unsigned short g_linToSrgb[4096];       // back to 0..255
    static bool g_lutReady = false;

    static void BuildLuts()
    {
        if (g_lutReady)
            return;
        for (int i = 0; i < 256; ++i)
        {
            double lin = pow((double)i / 255.0, 2.2);
            int v = (int)(lin * 4095.0 + 0.5);
            g_srgbToLin[i] = (unsigned short)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
        }
        for (int i = 0; i < 4096; ++i)
        {
            double srgb = pow((double)i / 4095.0, 1.0 / 2.2);
            int v = (int)(srgb * 255.0 + 0.5);
            g_linToSrgb[i] = (unsigned short)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        g_lutReady = true;
    }

    void SetLinearBlend(bool on) { BuildLuts(); g_linearBlend = on; }
    void SetDither(bool on)      { g_dither = on; }

    static int  g_outline = 0;          // 0 = off, 1 = one-pixel ring
    static unsigned short g_outlineColor = 0x0000;

    void SetOutline(int pixels, unsigned short color)
    {
        g_outline = (pixels <= 0) ? 0 : (pixels > 2 ? 2 : pixels);
        g_outlineColor = color;
    }

    // 4x4 Bayer matrix, centred on zero, scaled to +/- half a destination LSB
    static const int k_bayer[4][4] =
    {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    unsigned short Blend(unsigned short dst, unsigned short src, int coverage, const ColorFormat& fmt,
                         int x, int y)
    {
        if (coverage >= 255)
            return src;
        if (coverage <= 0)
            return dst;
        if (dst == src)
            return dst;

        const int rMask = (1 << fmt.redBits) - 1;
        const int gMask = (1 << fmt.greenBits) - 1;
        const int bMask = (1 << fmt.blueBits) - 1;

        const int dr = (dst >> fmt.redShift) & rMask, sr = (src >> fmt.redShift) & rMask;
        const int dg = (dst >> fmt.greenShift) & gMask, sg = (src >> fmt.greenShift) & gMask;
        const int db = (dst >> fmt.blueShift) & bMask, sb = (src >> fmt.blueShift) & bMask;

        int r, g, b;
        if (g_linearBlend)
        {
            BuildLuts();
            // scale the 5/6/5 samples up to 8 bits, blend in linear light
            const int dr8 = (dr * 255) / rMask, sr8 = (sr * 255) / rMask;
            const int dg8 = (dg * 255) / gMask, sg8 = (sg * 255) / gMask;
            const int db8 = (db * 255) / bMask, sb8 = (sb * 255) / bMask;

            const int drl = g_srgbToLin[dr8], srl = g_srgbToLin[sr8];
            const int dgl = g_srgbToLin[dg8], sgl = g_srgbToLin[sg8];
            const int dbl = g_srgbToLin[db8], sbl = g_srgbToLin[sb8];

            const int rl = drl + ((srl - drl) * coverage) / 255;
            const int gl = dgl + ((sgl - dgl) * coverage) / 255;
            const int bl = dbl + ((sbl - dbl) * coverage) / 255;

            const int r8 = g_linToSrgb[rl < 0 ? 0 : (rl > 4095 ? 4095 : rl)];
            const int g8 = g_linToSrgb[gl < 0 ? 0 : (gl > 4095 ? 4095 : gl)];
            const int b8 = g_linToSrgb[bl < 0 ? 0 : (bl > 4095 ? 4095 : bl)];

            r = r8; g = g8; b = b8;
        }
        else
        {
            r = (dr * (255 - coverage) + sr * coverage) / rMask;
            g = (dg * (255 - coverage) + sg * coverage) / gMask;
            b = (db * (255 - coverage) + sb * coverage) / bMask;
        }

        // Dither the rounding threshold BEFORE RGB565 quantisation. A spatial
        // Bayer pattern changes a channel by at most one output level and never
        // invents light in a zero-valued channel.
        const int threshold = g_dither ? (2 * k_bayer[y & 3][x & 3] + 1) : 16;
        r = (r * rMask * 32 + threshold * 255) / (255 * 32);
        g = (g * gMask * 32 + threshold * 255) / (255 * 32);
        b = (b * bMask * 32 + threshold * 255) / (255 * 32);

        return (unsigned short)((r << fmt.redShift) | (g << fmt.greenShift) | (b << fmt.blueShift));
    }

    int DrawCellAA(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                   unsigned short color, const ColorFormat& fmt)
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

            for (int c = 0; c < 24; ++c)
            {
                const int sx = x + c;
                if (sx < t.clipL)
                    continue;
                if (sx > t.clipR)
                    break;

                int cov = cell.cov[r * 24 + c];
                if (!cov)
                    continue;
                if (g_covLutReady && g_gamma != 1.0)
                    cov = g_covLut[cov];

                // outline (SDF-style dilation): the ring is the dilated coverage
                // minus the glyph coverage, painted before the glyph itself, so
                // small text stays readable on busy backgrounds
                if (g_outline)
                {
                    int dil = cov;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int cy = r + oy, cx = c + ox;
                            if (cy < 0 || cx < 0 || cy >= cellLines || cx >= 24)
                                continue;
                            int n = cell.cov[cy * 24 + cx];
                            if (g_covLutReady && g_gamma != 1.0)
                                n = g_covLut[n];
                            if (n > dil)
                                dil = n;
                        }
                    int ring = dil - cov;
                    if (ring > 0)
                        row[sx] = Blend(row[sx], g_outlineColor, ring, fmt, sx, sy);
                }

                row[sx] = Blend(row[sx], color, cov, fmt, sx, sy);
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
