// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include "plane/PixelMath.h"
#include <algorithm>
#include <cmath>

namespace vt
{
using plane::detail::Adaptive;
using plane::detail::CompositeInk;
using plane::detail::GridScale;
using plane::detail::Over;
using plane::detail::Tables;

PixelPlane::PixelPlane(int width, int height, const PlaneOptions& options)
    : m_width(width), m_height(height), m_tilesX((width + TileW - 1) / TileW), m_options(options)
{
    const double gamma = std::max(0.5, std::min(3.0, options.gamma));
    for (int i = 0; i < 256; ++i)
        m_coverage[i] = (unsigned char)std::lround(
            (gamma == 1.0 ? i / 255.0 : std::pow(i / 255.0, 1.0 / gamma)) * 255.0);
    if (options.linear) (void)Tables();
}

PixelRect PixelPlane::Clip(PixelRect r) const
{
    return { std::max(0, r.left), std::max(0, r.top),
             std::min(m_width, r.right), std::min(m_height, r.bottom) };
}

PixelPlane::PixelPlane(const PixelPlane& other) :
    m_width(other.m_width),m_height(other.m_height),m_tilesX(other.m_tilesX),m_options(other.m_options),
    m_colors(other.m_colors),m_color(other.m_color),m_colorReady(other.m_colorReady),m_adaptiveEdge(other.m_adaptiveEdge),
    m_rasterScale(other.m_rasterScale),m_tiles(other.m_tiles),m_generation(other.m_generation),m_writeBoundary(other.m_writeBoundary)
{ std::copy(std::begin(other.m_coverage),std::end(other.m_coverage),m_coverage); }

PixelPlane& PixelPlane::operator=(const PixelPlane& other)
{
    if(this==&other) return *this;
    m_width=other.m_width;m_height=other.m_height;m_tilesX=other.m_tilesX;m_options=other.m_options;
    std::copy(std::begin(other.m_coverage),std::end(other.m_coverage),m_coverage);
    m_colors=other.m_colors;m_color=other.m_color;m_colorReady=other.m_colorReady;m_adaptiveEdge=other.m_adaptiveEdge;m_tiles=other.m_tiles;
    m_generation=other.m_generation;m_writeBoundary=other.m_writeBoundary;
    m_rasterScale=other.m_rasterScale;
    return *this;
}

PixelPlane::TileMap::iterator PixelPlane::NewTile(int key)
{
    if(m_spareTiles.empty()) return m_tiles.emplace(key,Tile{}).first;
    auto tile=std::move(m_spareTiles.back());m_spareTiles.pop_back();tile.key()=key;
    return m_tiles.insert(std::move(tile)).position;
}

PixelPlane::TileMap::iterator PixelPlane::RecycleTile(TileMap::iterator tile)
{
    // Cleared blocks reuse their allocation; snapshots copy only live data.
    // Bound retained storage to 256 sparse blocks per heavily used surface.
    if(m_spareTiles.size()>=256) return m_tiles.erase(tile);
    auto next=std::next(tile);
    tile->second.pixels.fill({});tile->second.live=0;
    m_spareTiles.push_back(m_tiles.extract(tile));return next;
}

PlanePixel PixelPlane::At(int x, int y) const
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return {};
    const auto it = m_tiles.find((y / TileH) * m_tilesX + x / TileW);
    return it == m_tiles.end() ? PlanePixel{} : it->second.pixels[(y % TileH) * TileW + x % TileW];
}

void PixelPlane::Set(int x, int y, PlanePixel pixel)
{
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return;
    if(!m_adaptiveEdge) m_adaptiveEdge=Adaptive(pixel);
    m_rasterScale=std::max(m_rasterScale,GridScale(pixel));
    const int key = (y / TileH) * m_tilesX + x / TileW;
    auto it = m_tiles.find(key);
    if (it == m_tiles.end())
    {
        if (!pixel.a && !pixel.high) return;
        it = NewTile(key);
    }
    PlanePixel& dest = it->second.pixels[(y % TileH) * TileW + x % TileW];
    it->second.live += ((pixel.a != 0) || pixel.high) - ((dest.a != 0) || dest.high);
    dest = pixel;
    it->second.generations[(y % TileH) * TileW + x % TileW] = ++m_generation;
    if (!it->second.live) RecycleTile(it);
    if(m_tiles.empty()) { m_adaptiveEdge=false;m_rasterScale=2; }
}

void PixelPlane::ClearPixel(int x, int y) { Set(x, y, {}); }

bool PixelPlane::Intersects(PixelRect rect) const
{
    rect=Clip(rect);
    if(rect.left>=rect.right || rect.top>=rect.bottom) return false;
    for(const auto& entry:m_tiles) {
        const int x=(entry.first%m_tilesX)*TileW,y=(entry.first/m_tilesX)*TileH;
        if(x<rect.right && x+TileW>rect.left && y<rect.bottom && y+TileH>rect.top) return true;
    }
    return false;
}

void PixelPlane::Clear(PixelRect rect)
{
    rect = Clip(rect);
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;
    if (rect.left == 0 && rect.top == 0 && rect.right == m_width && rect.bottom == m_height)
    { for(auto it=m_tiles.begin();it!=m_tiles.end();) it=RecycleTile(it); m_adaptiveEdge=false;m_rasterScale=2;return; }
    // Walk only live tiles, rather than the potentially huge empty region.
    for (auto it = m_tiles.begin(); it != m_tiles.end();)
    {
        const int tx = (it->first % m_tilesX) * TileW, ty = (it->first / m_tilesX) * TileH;
        const PixelRect part = { std::max(tx, rect.left), std::max(ty, rect.top),
            std::min(tx + TileW, rect.right), std::min(ty + TileH, rect.bottom) };
        for (int y = part.top; y < part.bottom; ++y)
            for (int x = part.left; x < part.right; ++x)
            {
                PlanePixel& p = it->second.pixels[(y - ty) * TileW + x - tx];
                if (p.a || p.high) { p = {}; --it->second.live; }
            }
        if (!it->second.live) it = RecycleTile(it); else ++it;
    }
    if(m_tiles.empty()) { m_adaptiveEdge=false;m_rasterScale=2; }
}

void PixelPlane::Copy(const PixelPlane* source, PixelRect sr, PixelRect dr,
                     const unsigned short* sourceBase, int sourcePitch,
                     const unsigned short* destBase, int destPitch,
                     bool sourceKey, unsigned short skLow, unsigned short skHigh,
                     bool destKey, unsigned short dkLow, unsigned short dkHigh,
                     bool mirrorX, bool mirrorY)
{
    const int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    const int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if ((sourceKey && (!sourceBase || sourcePitch <= 0)) || (destKey && (!destBase || destPitch <= 0))) return;
    const PixelRect clipped = Clip(dr);
    auto orient=[&](PlanePixel p) {
        if(p.high) {
            if(p.grid && (mirrorX || mirrorY)) {
                const int n=p.grid->scale;
                auto grid=std::make_shared<InkGrid>();grid->scale=n;grid->pixels.resize(n*n);
                for(int y=0;y<n;++y) for(int x=0;x<n;++x)
                    grid->pixels[y*n+x]=p.grid->pixels[(mirrorY ? n-1-y : y)*n+(mirrorX ? n-1-x : x)];
                p.grid=std::move(grid);
            }
            if(mirrorX) { std::swap(p.samples[0],p.samples[1]);std::swap(p.samples[2],p.samples[3]); }
            if(mirrorY) { std::swap(p.samples[0],p.samples[2]);std::swap(p.samples[1],p.samples[3]); }
        }
        return p;
    };
    if (Empty() && (!source || source->Empty())) return;
    if (source == this)
    {
        // A camera scroll can cover the whole screen, but only live text
        // tiles need a snapshot. Preserve overlap semantics without a
        // sw*sh allocation (about 100 MB at 1920x1080 with 2x samples).
        const PixelPlane snapshot(*this);
        Copy(&snapshot,sr,dr,sourceBase,sourcePitch,destBase,destPitch,
             sourceKey,skLow,skHigh,destKey,dkLow,dkHigh,mirrorX,mirrorY);
        return;
    }
    if (source != this && !destKey && !sourceKey)
    {
        // Plain copies replace the destination. Iterate only projected
        // live tiles, not millions of empty pixels on a full-screen blit.
        Clear(clipped);
        if (!source) return;
        auto ceilScale = [](int v,int numerator,int denominator) {
            return (int)(((int64_t)v*numerator+denominator-1)/denominator);
        };
        for (const auto& entry : source->m_tiles)
        {
            const int tx=(entry.first%source->m_tilesX)*TileW;
            const int ty=(entry.first/source->m_tilesX)*TileH;
            int l=std::max(tx,sr.left)-sr.left, r=std::min(tx+TileW,sr.right)-sr.left;
            int t=std::max(ty,sr.top)-sr.top, b=std::min(ty+TileH,sr.bottom)-sr.top;
            if (l>=r || t>=b) continue;
            if (mirrorX) { int old=l; l=sw-r; r=sw-old; }
            if (mirrorY) { int old=t; t=sh-b; b=sh-old; }
            const int dl=std::max(clipped.left,dr.left+ceilScale(l,dw,sw));
            const int dt=std::max(clipped.top,dr.top+ceilScale(t,dh,sh));
            const int right=std::min(clipped.right,dr.left+ceilScale(r,dw,sw));
            const int bottom=std::min(clipped.bottom,dr.top+ceilScale(b,dh,sh));
            for (int y=dt;y<bottom;++y) for (int x=dl;x<right;++x)
            {
                int ox=(int)(((int64_t)x-dr.left)*sw/dw);
                int oy=(int)(((int64_t)y-dr.top)*sh/dh);
                const auto p=orient(source->At(sr.left+(mirrorX ? sw-1-ox : ox),
                                       sr.top+(mirrorY ? sh-1-oy : oy)));
                if (p.a || p.high) Set(x,y,p);
            }
        }
        return;
    }
    for (int y = clipped.top; y < clipped.bottom; ++y)
        for (int x = clipped.left; x < clipped.right; ++x)
        {
            if (destKey)
            {
                const unsigned short v = destBase[(size_t)y * destPitch + x];
                if (v < dkLow || v > dkHigh) continue;
            }
            int ox = (int)(((int64_t)x - dr.left) * sw / dw);
            int oy = (int)(((int64_t)y - dr.top) * sh / dh);
            if (mirrorX) ox = sw - 1 - ox;
            if (mirrorY) oy = sh - 1 - oy;
            const int sx = sr.left + ox, sy = sr.top + oy;
            if (source && (sx < 0 || sy < 0 || sx >= source->Width() || sy >= source->Height())) continue;
            const PlanePixel p = orient(source ? source->At(sx, sy) : PlanePixel{});
            const unsigned short value=sourceKey ?
                (p.tracked ? p.background : sourceBase[(size_t)sy*sourcePitch+sx]) : 0;
            const bool transparent=sourceKey && value>=skLow && value<=skHigh;
            if (transparent && !p.a && !p.high) continue;
            PlanePixel result=transparent ? Over(p,At(x,y)) : p;
            if (transparent && p.tracked) {
                const PlanePixel prior=At(x,y);
                result.tracked=true;
                result.background=prior.tracked ? prior.background :
                    destBase ? destBase[(size_t)y*destPitch+x] : 0;
                result.marker=p.marker; // the native blit copies this word
            }
            Set(x,y,result);
        }
}

void PixelPlane::ResolveSubtitleNative(unsigned short* native,int pitch,PixelRect rect)
{
    if(!m_adaptiveEdge || !native || pitch<m_width) return;
    rect=Clip(rect);
    for(auto& entry:m_tiles) {
        const int tx=(entry.first%m_tilesX)*TileW,ty=(entry.first/m_tilesX)*TileH;
        const PixelRect part{std::max(tx,rect.left),std::max(ty,rect.top),
            std::min(tx+TileW,rect.right),std::min(ty+TileH,rect.bottom)};
        for(int y=part.top;y<part.bottom;++y) for(int x=part.left;x<part.right;++x) {
            auto& p=entry.second.pixels[(y-ty)*TileW+x-tx];
            const bool adaptive=Adaptive(p);
            auto& actual=native[(size_t)y*pitch+x];
            if(!adaptive || !p.tracked || actual!=p.marker) continue;
            const uint32_t rgb=CompositeInk(p,p.background,m_options.linear);
            p.marker=(unsigned short)((((rgb>>19)&31)<<11)|(((rgb>>10)&63)<<5)|((rgb>>3)&31));
            if(p.marker==p.background) p.marker^=1;
            // Black remains an observable word through a later black-key copy.
            if(!p.marker) p.marker=1;
            actual=p.marker;
        }
    }
}

void PixelPlane::ValidateNative(const unsigned short* native,int pitch)
{
    if (!native || pitch<m_width) return;
    for(auto it=m_tiles.begin();it!=m_tiles.end();) {
        const int tx=(it->first%m_tilesX)*TileW,ty=(it->first/m_tilesX)*TileH;
        for(int oy=0;oy<TileH && ty+oy<m_height;++oy)
            for(int ox=0;ox<TileW && tx+ox<m_width;++ox) {
                auto& p=it->second.pixels[oy*TileW+ox];
                if((p.a || p.high) && p.tracked && native[(size_t)(ty+oy)*pitch+tx+ox]!=p.marker) {
                    p={}; --it->second.live;
                }
            }
        if(!it->second.live) it=RecycleTile(it); else ++it;
    }
    if(m_tiles.empty()) { m_adaptiveEdge=false;m_rasterScale=2; }
}

std::vector<PixelPlane::NativeSample> PixelPlane::CaptureNative(
    const unsigned short* base, int pitch, PixelRect rect) const
{
    std::vector<NativeSample> result;
    rect = Clip(rect);
    for (const auto& entry : m_tiles)
    {
        const int tx = (entry.first % m_tilesX) * TileW, ty = (entry.first / m_tilesX) * TileH;
        for (int oy = 0; oy < TileH; ++oy) for (int ox = 0; ox < TileW; ++ox)
        {
            const int x = tx + ox, y = ty + oy;
            const auto p = entry.second.pixels[oy * TileW + ox];
            if ((p.a || p.high) && x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom)
                result.push_back({ x, y, base[(size_t)y * pitch + x],
                    entry.second.generations[oy * TileW + ox] });
        }
    }
    return result;
}

void PixelPlane::InvalidateNative(const unsigned short* base, int pitch,
                                 const std::vector<NativeSample>& samples)
{
    for (const auto& s : samples)
    {
        auto it = m_tiles.find((s.y / TileH) * m_tilesX + s.x / TileW);
        // Identical glyphs repainted during the lock are new content too.
        if (it != m_tiles.end() &&
            it->second.generations[(s.y % TileH) * TileW + s.x % TileW] == s.generation &&
            base[(size_t)s.y * pitch + s.x] != s.base) ClearPixel(s.x, s.y);
    }
}
}
