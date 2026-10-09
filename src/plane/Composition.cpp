// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "../PixelPlane.h"
#include "PixelMath.h"
#include <cmath>
#include <limits>
#include <set>

namespace vt
{
using plane::detail::CompositeInk;
using plane::detail::Sample;
using plane::detail::Tables;

namespace
{
template <class Pixel>
bool ValidRows(const Pixel* pixels, int pitch, int width, int height)
{
    if (!pixels || width <= 0 || height <= 0 || pitch < width) return false;
    // Bound the final row and its width in bytes on the 32-bit host.
    constexpr auto limit = std::numeric_limits<size_t>::max() / sizeof(Pixel);
    return static_cast<size_t>(width) <= limit &&
        static_cast<size_t>(height - 1) <= (limit - width) / static_cast<size_t>(pitch);
}
}

uint32_t PixelPlane::Composite(int x, int y, unsigned short background) const
{
    return CompositeInk(At(x,y),background,m_options.linear);
}

std::optional<PixelRect> PixelPlane::ValidateBlitArguments(const unsigned short* source, int sourcePitch,
                                                         uint32_t* output, int outputPitch, PixelRect rect) const
{
    rect = Clip(rect);
    // Test ordering matters: don't subtract untrusted/extreme coordinates
    // until the rectangle is nonempty and bounded by the plane.
    if (rect.left >= rect.right || rect.top >= rect.bottom ||
        !ValidRows(source, sourcePitch, m_width, m_height) ||
        !ValidRows(output, outputPitch, rect.right - rect.left, rect.bottom - rect.top))
        return std::nullopt;
    return rect;
}

void PixelPlane::CompositeRect(const unsigned short* source, int sourcePitch,
                               uint32_t* output, int outputPitch, PixelRect rect) const
{
    const auto validated = ValidateBlitArguments(source, sourcePitch, output, outputPitch, rect);
    if (!validated) return;
    rect = *validated;
    for (int y = rect.top; y < rect.bottom; ++y)
        for (int x = rect.left; x < rect.right; ++x)
            output[(size_t)(y - rect.top) * outputPitch + x - rect.left] =
                    plane::detail::Expand565(source[(size_t)y * sourcePitch + x]);
    // Only text tiles need an alpha blend. Background conversion avoids
    // millions of map lookups on a full-screen frame.
    for (const auto& entry : m_tiles)
    {
        const int tx = (entry.first % m_tilesX) * TileW, ty = (entry.first / m_tilesX) * TileH;
        const PixelRect part = { std::max(tx, rect.left), std::max(ty, rect.top),
            std::min(tx + TileW, rect.right), std::min(ty + TileH, rect.bottom) };
        for (int y = part.top; y < part.bottom; ++y)
            for (int x = part.left; x < part.right; ++x)
                if (entry.second.pixels[(y - ty) * TileW + x - tx].a || entry.second.pixels[(y - ty) * TileW + x - tx].high)
                    output[(size_t)(y - rect.top) * outputPitch + x - rect.left] =
                        Composite(x, y, source[(size_t)y * sourcePitch + x]);
    }
}

void PixelPlane::BackgroundRect(const unsigned short* source,int sourcePitch,uint32_t* output,int outputPitch,PixelRect rect) const
{
    const auto validated = ValidateBlitArguments(source, sourcePitch, output, outputPitch, rect);
    if (!validated) return;
    rect = *validated;
    const auto* rgb565=Tables().rgb565;
    for(int y=rect.top;y<rect.bottom;++y) for(int x=rect.left;x<rect.right;++x)
        output[(size_t)(y-rect.top)*outputPitch+x-rect.left]=rgb565[source[(size_t)y*sourcePitch+x]];
    for(const auto& entry:m_tiles) {
        const int tx=(entry.first%m_tilesX)*TileW,ty=(entry.first/m_tilesX)*TileH;
        for(int oy=0;oy<TileH;++oy) for(int ox=0;ox<TileW;++ox) {
            const int x=tx+ox,y=ty+oy;
            const auto& p=entry.second.pixels[oy*TileW+ox];
            if((p.a || p.high) && p.tracked && x>=rect.left && x<rect.right && y>=rect.top && y<rect.bottom)
                output[(size_t)(y-rect.top)*outputPitch+x-rect.left]=plane::detail::Expand565(p.background);
        }
    }
}

std::vector<PixelRect> PixelPlane::TextTiles(bool filtered) const
{
    std::vector<PixelRect> result;
    std::set<int> keys;
    auto add=[&](int x,int y) {
        if(x>=0 && y>=0 && x<m_tilesX && y<(m_height+TileH-1)/TileH)
            keys.insert(y*m_tilesX+x);
    };
    auto visible=[](const PlanePixel& p) {
        if(!p.high) return p.a!=0;
        if(p.grid) { for(const auto& s:p.grid->pixels) if(s.a) return true;return false; }
        for(const auto& s:p.samples) if(s.a) return true;
        return false;
    };
    for(const auto& entry:m_tiles) {
        if(!filtered) { keys.insert(entry.first);continue; }
        const int tx=entry.first%m_tilesX,ty=entry.first/m_tilesX;
        add(tx,ty);
        const auto& pixels=entry.second.pixels;
        for(int y=0;y<TileH;++y) {
            if(visible(pixels[y*TileW])) add(tx-1,ty);
            if(visible(pixels[y*TileW+TileW-1])) add(tx+1,ty);
        }
        for(int x=0;x<TileW;++x) {
            if(visible(pixels[x])) add(tx,ty-1);
            if(visible(pixels[(TileH-1)*TileW+x])) add(tx,ty+1);
        }
        if(visible(pixels[0])) add(tx-1,ty-1);
        if(visible(pixels[TileW-1])) add(tx+1,ty-1);
        if(visible(pixels[(TileH-1)*TileW])) add(tx-1,ty+1);
        if(visible(pixels[TileW*TileH-1])) add(tx+1,ty+1);
    }
    for(int key:keys) {
        const int x=(key%m_tilesX)*TileW,y=(key/m_tilesX)*TileH;
        result.push_back({x,y,std::min(x+TileW,m_width),std::min(y+TileH,m_height)});
    }
    return result;
}

uint32_t PixelPlane::Expand565(unsigned short color)
{
    return plane::detail::Expand565(color);
}

void PixelPlane::CompositeScaledRect(uint32_t *output, int pitch, int width, int height, PixelRect source) const
{
    if (!output || width <= 0 || height <= 0 || pitch < width || source.right <= source.left ||
        source.bottom <= source.top)
        return;
    const double sx = width / (double)(source.right - source.left),
                 sy = height / (double)(source.bottom - source.top);
    const int n = RasterScale();
    const auto &tables = Tables();
    for (auto r : TextTiles(true))
    {
        const int l = std::max(0, (int)std::ceil((r.left - source.left) * sx - 0.5));
        const int t = std::max(0, (int)std::ceil((r.top - source.top) * sy - 0.5));
        const int right = std::min(width, (int)std::ceil((r.right - source.left) * sx - 0.5));
        const int bottom = std::min(height, (int)std::ceil((r.bottom - source.top) * sy - 0.5));
        if (l >= right || t >= bottom)
            continue;
        const int gw = (r.right - r.left + 2) * n, gh = (r.bottom - r.top + 2) * n;
        std::vector<InkPixel> ink((size_t)gw * gh);
        for (int y = r.top - 1; y <= r.bottom; ++y)
            for (int x = r.left - 1; x <= r.right; ++x)
            {
                if (x < 0 || y < 0 || x >= m_width || y >= m_height)
                    continue;
                const auto found = m_tiles.find((y / TileH) * m_tilesX + x / TileW);
                if (found == m_tiles.end())
                    continue;
                const auto &p = found->second.pixels[(y % TileH) * TileW + x % TileW];
                for (int yy = 0; yy < n; ++yy)
                    for (int xx = 0; xx < n; ++xx)
                        ink[(size_t)((y - r.top + 1) * n + yy) * gw + (x - r.left + 1) * n + xx] =
                            Sample(p, xx, yy, n);
            }
        for (int y = t; y < bottom; ++y)
            for (int x = l; x < right; ++x)
            {
                const double u = (source.left + (x + 0.5) / sx - r.left + 1) * n - 0.5;
                const double v = (source.top + (y + 0.5) / sy - r.top + 1) * n - 0.5;
                const int ix = (int)std::floor(u), iy = (int)std::floor(v);
                const double fx = u - ix, fy = v - iy;
                const InkPixel samples[] = {ink[iy * gw + ix], ink[iy * gw + ix + 1], ink[(iy + 1) * gw + ix],
                                            ink[(iy + 1) * gw + ix + 1]};
                const double weights[] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                double a = 0, channels[3]{};
                for (int i = 0; i < 4; ++i)
                {
                    a += weights[i] * samples[i].a;
                    channels[0] += weights[i] * (samples[i].r + samples[i].lightEdge);
                    channels[1] += weights[i] * (samples[i].g + samples[i].lightEdge);
                    channels[2] += weights[i] * (samples[i].b + samples[i].lightEdge);
                }
                if (a <= 0)
                    continue;
                auto &pixel = output[(size_t)y * pitch + x];
                uint32_t result = 0xFF000000u;
                for (int c = 0; c < 3; ++c)
                {
                    const int shift = 16 - c * 8, bg = (pixel >> shift) & 255;
                    const int background = m_options.linear ? tables.linear[bg] : bg * 257;
                    const int value = std::max(
                        0, std::min(65535, (int)std::lround(channels[c] + background * (1 - a / 65535.0))));
                    result |= (uint32_t)(m_options.linear ? tables.srgb[value] : (value + 128) / 257) << shift;
                }
                pixel = result;
            }
    }
}

void PixelPlane::Overlay2Rect(uint32_t *output, int outputPitch, PixelRect rect) const
{
    OverlayRect(output, outputPitch, rect, 2);
}

void PixelPlane::OverlayRect(uint32_t *output, int outputPitch, PixelRect rect, int scale) const
{
    // Premultiplied channels. With linear blending, encode them as sRGB
    // for the D3D9 sRGB sampler; it recovers linear premultiplied values.
    const auto *srgb = Tables().srgb;
    auto channel=[&](int v) { return m_options.linear ? (int)srgb[v] : (v+128)/257; };
    const Tile* tile=nullptr;int tileKey=-1;
    const PlanePixel empty{};
    for(int y=rect.top;y<rect.bottom;++y) for(int x=rect.left;x<rect.right;++x) {
        // Atlas gutters can extend beyond the surface. Preserve the
        // requested output origin and fill those samples with transparency.
        const bool inside=x>=0 && y>=0 && x<m_width && y<m_height;
        const int key=inside ? (y/TileH)*m_tilesX+x/TileW : -1;
        if(key!=tileKey) {
            const auto found=m_tiles.find(key);
            tile=found==m_tiles.end() ? nullptr : &found->second;tileKey=key;
        }
        const PlanePixel& p=tile ? tile->pixels[(y%TileH)*TileW+x%TileW] : empty;
        for(int i=0;i<scale*scale;++i) {
            const InkPixel s=Sample(p,i%scale,i/scale,scale);
            const int edge=s.lightEdge;
            output[(size_t)((y-rect.top)*scale+i/scale)*outputPitch+(x-rect.left)*scale+i%scale]=
                ((unsigned int)((s.a+128)/257)<<24)|(channel(s.r+edge)<<16)|(channel(s.g+edge)<<8)|channel(s.b+edge);
        }
    }
}
}
