// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelMath.h"
#include <cmath>

namespace vt::plane::detail
{
ColorTables::ColorTables()
{
    for (int i = 0; i < 256; ++i)
        linear[i] = (uint16_t)std::lround(std::pow(i / 255.0, 2.2) * 65535.0);
    for (int i = 0; i < 65536; ++i)
    {
        srgb[i] = (unsigned char)std::lround(std::pow(i / 65535.0, 1.0 / 2.2) * 255.0);
        const unsigned int r = (((i >> 11) & 31) * 255 + 15) / 31;
        const unsigned int g = (((i >> 5) & 63) * 255 + 31) / 63;
        const unsigned int b = ((i & 31) * 255 + 15) / 31;
        rgb565[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

PlanePixel Color(unsigned short color, int coverage, bool linear)
{
    const uint32_t rgb = Expand565(color);
    const uint16_t a = (uint16_t)(coverage * 257);
    const auto channel = [&](int shift) {
        const int c = (rgb >> shift) & 255;
        return Multiply(linear ? Tables().linear[c] : (uint16_t)(c * 257), a);
    };
    return { channel(16), channel(8), channel(0), a };
}

PlanePixel Over(PlanePixel source, PlanePixel dest)
{
    if(!dest.a && !dest.high) {
        source.tracked=false;source.background=source.marker=0;
        return source;
    }
    const uint16_t remain = (uint16_t)(65535 - source.a);
    PlanePixel result={ (uint16_t)(source.r + Multiply(dest.r, remain)),
             (uint16_t)(source.g + Multiply(dest.g, remain)),
             (uint16_t)(source.b + Multiply(dest.b, remain)),
             (uint16_t)(source.a + Multiply(dest.a, remain)) };
    result.edge=(uint16_t)(source.edge+Multiply(dest.edge,remain));
    result.lightEdge=(uint16_t)(source.lightEdge+Multiply(dest.lightEdge,remain));
    result.high=source.high || dest.high;
    result.subtitle=source.subtitle || dest.subtitle;
    if(result.high) for(int i=0;i<4;++i) {
        const InkPixel s=Sample(source,i%2,i/2,2);
        const InkPixel d=Sample(dest,i%2,i/2,2);
        const uint16_t left=(uint16_t)(65535-s.a);
        result.samples[i]={(uint16_t)(s.r+Multiply(d.r,left)),(uint16_t)(s.g+Multiply(d.g,left)),
            (uint16_t)(s.b+Multiply(d.b,left)),(uint16_t)(s.a+Multiply(d.a,left)),
            (uint16_t)(s.edge+Multiply(d.edge,left)),
            (uint16_t)(s.lightEdge+Multiply(d.lightEdge,left))};
    }
    const int n=std::max(GridScale(source),GridScale(dest));
    if(result.high && n>2) {
        auto grid=std::make_shared<InkGrid>();grid->scale=n;grid->pixels.resize(n*n);
        for(int y=0;y<n;++y) for(int x=0;x<n;++x) {
            const auto s=Sample(source,x,y,n),d=Sample(dest,x,y,n);
            const auto left=(uint16_t)(65535-s.a);
            grid->pixels[y*n+x]={(uint16_t)(s.r+Multiply(d.r,left)),(uint16_t)(s.g+Multiply(d.g,left)),
                (uint16_t)(s.b+Multiply(d.b,left)),(uint16_t)(s.a+Multiply(d.a,left)),
                (uint16_t)(s.edge+Multiply(d.edge,left)),
                (uint16_t)(s.lightEdge+Multiply(d.lightEdge,left))};
        }
        result.grid=std::move(grid);
    }
    return result;
}

uint32_t CompositeInk(PlanePixel p, unsigned short background, bool linear)
{
    const unsigned short scene=p.tracked ? p.background : background;
    const uint32_t rgb = Expand565(scene);
    if (!p.a) return rgb;
    const int edge=p.lightEdge;
    const auto channel = [&](uint16_t foreground, int shift) {
        const int c = (rgb >> shift) & 255;
        const uint16_t bg = linear ? Tables().linear[c] : (uint16_t)(c * 257);
        const int value = foreground + edge + Multiply(bg, (uint16_t)(65535 - p.a));
        return linear ? (int)Tables().srgb[std::min(65535, value)] : (value + 128) / 257;
    };
    return 0xFF000000u | (channel(p.r, 16) << 16) | (channel(p.g, 8) << 8) | channel(p.b, 0);
}
}
