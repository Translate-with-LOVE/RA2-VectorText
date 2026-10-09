// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include "GlyphSource.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// Independent contiguous-image oracle for the sparse software presenter.
// It reads the exported overlay, decodes channels before filtering and never
// uses the production tile projection, pixel selection or native markers.
int main()
{
    int failures = 0, checks = 0, largest = 0;
    for (bool linear : {false, true})
        for (int n : {2, 5, 8})
        {
            vt::PlaneOptions options;
            options.highResolution = true;
            options.linear = linear;
            vt::PixelPlane plane(96, 48, options);
            vt::GlyphRaster2 raster;
            raster.scale = n;
            raster.left = raster.top = -n;
            raster.width = 6 * n;
            raster.rows = 7 * n;
            raster.coverage.resize(raster.width * raster.rows);
            for (int y = 0; y < raster.rows; ++y)
                for (int x = 0; x < raster.width; ++x)
                    raster.coverage[y * raster.width + x] = (unsigned char)((x * 17 + y * 29) % 256);
            vt::GlyphCell glyph{};
            glyph.inkX = glyph.inkY = -1;
            glyph.inkRows = 7;
            glyph.raster2 = &raster;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 6; ++x)
                    glyph.cov[y * 24 + x] = 96;
            plane.Paint(glyph, 31, 15, 7, 0x001F, {0, 0, 96, 48}, nullptr, 0, true);
            plane.Paint(glyph, 64, 33, 7, 0xFFE0, {0, 0, 96, 48}, nullptr, 0, true);
            std::vector<uint32_t> contiguous(96 * 48 * n * n);
            plane.OverlayRect(contiguous.data(), 96 * n, {0, 0, 96, 48}, n);
            for (auto source : {vt::PixelRect{0, 0, 96, 48}, vt::PixelRect{11, 3, 83, 43}})
                for (auto scale : {std::pair<double, double>{1.25, 1.25}, {2.4, 1.8}, {4.8, 3.6}})
                {
                    const int w = (int)std::lround((source.right - source.left) * scale.first);
                    const int h = (int)std::lround((source.bottom - source.top) * scale.second);
                    const double sx = w / (double)(source.right - source.left),
                                 sy = h / (double)(source.bottom - source.top);
                    std::vector<uint32_t> background(w * h);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            background[y * w + x] =
                                0xFF000000u | ((x * 7 % 256) << 16) | ((y * 11 % 256) << 8) | ((x + y) * 3 % 256);
                    auto actual = background;
                    plane.CompositeScaledRect(actual.data(), w, w, h, source);
                    int error = 0;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            const double u = (source.left + (x + 0.5) / sx) * n - 0.5,
                                         v = (source.top + (y + 0.5) / sy) * n - 0.5;
                            const int ix = (int)std::floor(u), iy = (int)std::floor(v);
                            const double fx = u - ix, fy = v - iy;
                            const double weights[] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                            double a = 0, colors[3]{};
                            for (int i = 0; i < 4; ++i)
                            {
                                const int xx = ix + i % 2, yy = iy + i / 2;
                                const auto pixel =
                                    xx < 0 || yy < 0 || xx >= 96 * n || yy >= 48 * n ? 0 : contiguous[yy * 96 * n + xx];
                                a += weights[i] * (pixel >> 24) / 255.0;
                                for (int c = 0; c < 3; ++c)
                                {
                                    const double channel = ((pixel >> (16 - c * 8)) & 255) / 255.0;
                                    colors[c] += weights[i] * (linear ? std::pow(channel, 2.2) : channel);
                                }
                            }
                            for (int c = 0; c < 3; ++c)
                            {
                                const int shift = 16 - c * 8;
                                const double bg = ((background[y * w + x] >> shift) & 255) / 255.0;
                                const double value =
                                    std::min(1.0, colors[c] + (linear ? std::pow(bg, 2.2) : bg) * (1 - a));
                                const int expected =
                                    (int)std::lround(255 * (linear ? std::pow(value, 1 / 2.2) : value));
                                error = std::max(error, std::abs((int)((actual[y * w + x] >> shift) & 255) - expected));
                            }
                        }
                    printf("linear=%d density=%d source=(%d,%d) scale=%.4gx%.4g max error=%d\n", linear, n, source.left,
                           source.top, sx, sy, error);
                    if (error > 2)
                        ++failures;
                    largest = std::max(largest, error);
                    ++checks;
                }
        }
    printf("%s: %d scaled comparisons, max channel error=%d, failures=%d\n", failures ? "FAIL" : "PASS", checks,
           largest, failures);
    return failures ? 1 : 0;
}
