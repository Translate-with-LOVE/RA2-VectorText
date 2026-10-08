// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelWriter.h"
#include <math.h>

namespace vt
{
    static PresentationWriter g_presentationWriter = nullptr;
    static volatile LONG outputRasterScale = 2;
    void SetOutputRasterScale(int scale) { InterlockedExchange(&outputRasterScale, scale < 2 ? 2 : (scale > 8 ? 8 : scale)); }
    int OutputRasterScale() { return (int)InterlockedCompareExchange(&outputRasterScale, 0, 0); }
    void SetPresentationWriter(PresentationWriter writer) { g_presentationWriter = writer; }
    const ColorFormat RGB565 = { 11, 5, 5, 6, 0, 5 };

    namespace
    {
    thread_local bool captureTextInk = false, capturedTextInk = false;
    thread_local bool subtitleOutline = false;
    thread_local TextInkRect textInk{};

    void CaptureCell(const Target &t, const GlyphCell &cell, int x, int y, int rows, bool aa)
    {
        if (!captureTextInk || !t.base || t.pitch <= 0)
            return;
        if (cell.inkRows > 0)
            rows = cell.inkRows;
        if (rows <= 0 || rows > 32)
            return;
        auto include = [&](int l, int top, int r, int bottom)
        {
            if (subtitleOutline) { --l; --top; ++r; ++bottom; }
            l = (l < t.clipL ? t.clipL : l);
            top = (top < t.clipT ? t.clipT : top);
            r = (r > t.clipR + 1 ? t.clipR + 1 : r);
            bottom = (bottom > t.clipB + 1 ? t.clipB + 1 : bottom);
            if (l >= r || top >= bottom)
                return;
            if (!capturedTextInk)
            {
                textInk = {l, top, r, bottom};
                capturedTextInk = true;
            }
            else
            {
                if (l < textInk.left)
                    textInk.left = l;
                if (top < textInk.top)
                    textInk.top = top;
                if (r > textInk.right)
                    textInk.right = r;
                if (bottom > textInk.bottom)
                    textInk.bottom = bottom;
            }
        };
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < 24; ++col)
                if (aa ? cell.cov[row * 24 + col] != 0 : (cell.bits[row * 3 + col / 8] & (0x80 >> (col & 7))) != 0)
                    include(x + cell.inkX + col, y + cell.inkY + row, x + cell.inkX + col + 1, y + cell.inkY + row + 1);
        if (aa && cell.raster2)
        {
            const auto &high = *cell.raster2;
            for (int row = 0; row < high.rows; ++row)
                for (int col = 0; col < high.width; ++col)
                    if (high.coverage[(size_t)row * high.width + col])
                    {
                        const int px = x + (int)floor((high.left + col) / (double)high.scale);
                        const int py = y + (int)floor((high.top + row) / (double)high.scale);
                        include(px, py, px + 1, py + 1);
                    }
        }
    }
    } // namespace

    void BeginTextInkCapture(bool outline)
    {
        captureTextInk = true;
        capturedTextInk = false;
        subtitleOutline = outline;
    }
    bool SubtitleOutlineActive() { return captureTextInk && subtitleOutline; }
    bool EndTextInkCapture(TextInkRect *ink)
    {
        const bool valid = captureTextInk && capturedTextInk && ink;
        captureTextInk = capturedTextInk = false;
        subtitleOutline = false;
        if (valid)
            *ink = textInk;
        return valid;
    }

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

    static int DrawSubtitleEdge(const Target&, const GlyphCell&, int, int, int, unsigned short, bool, const ColorFormat&);

    int DrawCell(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                 unsigned short color)
    {
        CaptureCell(t, cell, x, y, cellLines, false);
        if (g_presentationWriter)
        {
            const int result = g_presentationWriter(t, cell, x, y, cellLines, color, false);
            if (result >= 0) return result;
        }
        x += cell.inkX;
        y += cell.inkY;
        if (cell.inkRows > 0) cellLines = cell.inkRows;
        if (!t.base || t.pitch <= 0 || cellLines <= 0 || cellLines > 32)
            return 0;

        int written = DrawSubtitleEdge(t, cell, x, y, cellLines, color, false, RGB565);
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
    //  RGB565 fallback blending with wider integer intermediates
    //
    //  When enabled, LinearBlend uses a 12-bit linear-light lookup table;
    //  otherwise the blend stays in encoded colour space. The final value is
    //  quantized to 5/6/5, optionally with an ordered Bayer offset. These options
    //  reduce colour quantization artifacts; they do not change surface format.
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

    static int  g_outline = 0;          // 0 = off; either positive setting enables the fixed 3x3 pass
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

    static int DrawSubtitleEdge(const Target& t, const GlyphCell& cell, int x, int y, int rows,
                                unsigned short color, bool aa, const ColorFormat& fmt)
    {
        if (!SubtitleOutlineActive()) return 0;
        BuildLuts();
        const int rm=(1<<fmt.redBits)-1, gm=(1<<fmt.greenBits)-1, bm=(1<<fmt.blueBits)-1;
        const unsigned short white=(unsigned short)((rm<<fmt.redShift)|(gm<<fmt.greenShift)|(bm<<fmt.blueShift));
        const uint32_t textLuminance=2126u*g_srgbToLin[((color>>fmt.redShift)&rm)*255/rm] +
            7152u*g_srgbToLin[((color>>fmt.greenShift)&gm)*255/gm] +
            722u*g_srgbToLin[((color>>fmt.blueShift)&bm)*255/bm];
        const bool allowWhiteEdge=textLuminance<733u*10000u;
        auto coverage=[&](int r,int c) {
            if(r<0 || r>=rows || c<0 || c>=24) return 0;
            if(!aa) return (cell.bits[r*3+c/8] & (0x80>>(c&7))) ? 255 : 0;
            const int cov=cell.cov[r*24+c];
            return g_covLutReady ? (int)g_covLut[cov] : cov;
        };
        int written=0;
        for(int r=-1;r<=rows;++r) for(int c=-1;c<=24;++c) {
            const int sx=x+c,sy=y+r;
            if(sx<t.clipL || sx>t.clipR || sy<t.clipT || sy>t.clipB) continue;
            const int cov=coverage(r,c);
            int dilated=cov;
            for(int oy=-1;oy<=1;++oy) for(int ox=-1;ox<=1;++ox)
                if(coverage(r+oy,c+ox)>dilated) dilated=coverage(r+oy,c+ox);
            if(dilated==cov) continue;
            auto& pixel=t.base[(size_t)sy*t.pitch+sx];
            pixel=Blend(pixel,allowWhiteEdge ? white : 0,dilated-cov,fmt,sx,sy);
            // Subtitle caches are copied with zero as the transparent key.
            // Keep a black border observable through that copy and its erase.
            if(!pixel) pixel=1;
            ++written;
        }
        return written;
    }

    int DrawCellAA(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                   unsigned short color, const ColorFormat& fmt)
    {
        CaptureCell(t, cell, x, y, cellLines, true);
        if (g_presentationWriter)
        {
            const int result = g_presentationWriter(t, cell, x, y, cellLines, color, true);
            if (result >= 0) return result;
        }
        x += cell.inkX;
        y += cell.inkY;
        if (cell.inkRows > 0) cellLines = cell.inkRows;
        if (!t.base || t.pitch <= 0 || cellLines <= 0 || cellLines > 32)
            return 0;

        int written = DrawSubtitleEdge(t, cell, x, y, cellLines, color, true, fmt);
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

                // Fixed 3x3 max-coverage pass at nonzero glyph pixels. Blend the
                // difference before the glyph; this does not paint an outer
                // stroke into zero-coverage pixels or use a distance field.
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

}
