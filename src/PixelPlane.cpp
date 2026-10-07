// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include <set>
#include <algorithm>
#include <cmath>

namespace vt
{
    namespace
    {
        struct ColorTables
        {
            uint16_t linear[256];
            unsigned char srgb[65536];
            uint32_t rgb565[65536];
            ColorTables()
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
        };
        const ColorTables& Tables() { static const ColorTables tables; return tables; }
        uint16_t Multiply(uint16_t a, uint16_t b)
        // The largest numerator is 65535^2+32767, still within uint32_t.
        // Avoid the expensive 64-bit division helper in the 32-bit game.
        { return (uint16_t)(((uint32_t)a * b + 32767u) / 65535u); }
    }

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
        m_colors(other.m_colors),m_color(other.m_color),m_colorReady(other.m_colorReady),m_tiles(other.m_tiles),
        m_generation(other.m_generation),m_writeBoundary(other.m_writeBoundary)
    { std::copy(std::begin(other.m_coverage),std::end(other.m_coverage),m_coverage); }

    PixelPlane& PixelPlane::operator=(const PixelPlane& other)
    {
        if(this==&other) return *this;
        m_width=other.m_width;m_height=other.m_height;m_tilesX=other.m_tilesX;m_options=other.m_options;
        std::copy(std::begin(other.m_coverage),std::end(other.m_coverage),m_coverage);
        m_colors=other.m_colors;m_color=other.m_color;m_colorReady=other.m_colorReady;m_tiles=other.m_tiles;
        m_generation=other.m_generation;m_writeBoundary=other.m_writeBoundary;
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
        // Bound retained storage to about 6.5 MiB per heavily used surface.
        if(m_spareTiles.size()>=256) return m_tiles.erase(tile);
        auto next=std::next(tile);
        tile->second.pixels.fill({});tile->second.live=0;
        m_spareTiles.push_back(m_tiles.extract(tile));return next;
    }

    uint32_t PixelPlane::Expand565(unsigned short color)
    {
        return Tables().rgb565[color];
    }

    PlanePixel PixelPlane::Color(unsigned short color, int coverage) const
    {
        const uint32_t rgb = Expand565(color);
        const uint16_t a = (uint16_t)(coverage * 257);
        const auto channel = [&](int shift) {
            const int c = (rgb >> shift) & 255;
            return Multiply(m_options.linear ? Tables().linear[c] : (uint16_t)(c * 257), a);
        };
        return { channel(16), channel(8), channel(0), a };
    }

    PlanePixel PixelPlane::Over(PlanePixel source, PlanePixel dest)
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
        result.high=source.high || dest.high;
        if(result.high) for(int i=0;i<4;++i) {
            const InkPixel s=source.high ? source.samples[i] : InkPixel{source.r,source.g,source.b,source.a};
            const InkPixel d=dest.high ? dest.samples[i] : InkPixel{dest.r,dest.g,dest.b,dest.a};
            const uint16_t left=(uint16_t)(65535-s.a);
            result.samples[i]={(uint16_t)(s.r+Multiply(d.r,left)),(uint16_t)(s.g+Multiply(d.g,left)),
                (uint16_t)(s.b+Multiply(d.b,left)),(uint16_t)(s.a+Multiply(d.a,left))};
        }
        return result;
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
        { for(auto it=m_tiles.begin();it!=m_tiles.end();) it=RecycleTile(it); return; }
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
    }

    void PixelPlane::Paint(const GlyphCell& cell, int x, int y, int rows,
                           unsigned short color, PixelRect clip,unsigned short* native,int pitch)
    {
        const int penX=x,penY=y;
        x += cell.inkX; y += cell.inkY;
        if (cell.inkRows) rows = cell.inkRows;
        if (rows <= 0 || rows > 32) return;
        clip = Clip(clip);
        const auto* high=m_options.highResolution ? cell.raster2 : nullptr;
        auto floor2=[](int v) { return (int)std::floor(v/2.0); };
        const int firstRow=high ? std::min(0,floor2(high->top)-cell.inkY) : 0;
        const int lastRow=high ? std::max(rows,floor2(high->top+high->rows+1)-cell.inkY) : rows;
        const int firstCol=high ? std::min(0,floor2(high->left)-cell.inkX) : 0;
        const int lastCol=high ? std::max(24,floor2(high->left+high->width+1)-cell.inkX) : 24;
        if(!m_colorReady || m_color!=color) {
            for(int i=0;i<256;++i) {
                const auto p=Color(color,i);m_colors[i]={p.r,p.g,p.b,p.a};
            }
            m_color=color;m_colorReady=true;
        }
        Tile* paintTile=nullptr;int paintTileKey=-1;
        for (int row = firstRow; row < lastRow; ++row)
            for (int col = firstCol; col < lastCol; ++col)
            {
                const int dx = x + col, dy = y + row;
                if (dx < clip.left || dx >= clip.right || dy < clip.top || dy >= clip.bottom) continue;
                const auto coverage = [&](int r, int c) {
                    if (r < 0 || r >= rows || c < 0 || c >= 24) return 0;
                    return m_options.antialias ? (int)m_coverage[cell.cov[r * 24 + c]]
                        : (cell.bits[r * 3 + c / 8] & (0x80 >> (c & 7)) ? 255 : 0);
                };
                int cov = coverage(row, col);
                int highCov[4]={},highSum=0;
                if(high) for(int i=0;i<4;++i) {
                    const int hx=(dx-penX)*2+(i&1)-high->left;
                    const int hy=(dy-penY)*2+(i/2)-high->top;
                    if(hx>=0 && hx<high->width && hy>=0 && hy<high->rows)
                        highCov[i]=m_coverage[high->coverage[(size_t)hy*high->width+hx]];
                    highSum+=highCov[i];
                }
                if (!cov && !highSum) continue;
                // A high-grid fringe also needs a native marker for erasure.
                const int key=(dy/TileH)*m_tilesX+dx/TileW;
                if(!paintTile || paintTileKey!=key) {
                    auto tile=m_tiles.find(key);
                    if(tile==m_tiles.end()) tile=NewTile(key);
                    paintTile=&tile->second;paintTileKey=key;
                }
                const int index=(dy%TileH)*TileW+dx%TileW;
                PlanePixel& slot=paintTile->pixels[index];
                PlanePixel dest = slot;
                const unsigned short actual = native ? native[(size_t)dy*pitch+dx] : 0;
                const bool retained = native && dest.tracked && actual==dest.marker;
                const unsigned short background = retained ? dest.background : actual;
                if (native && dest.tracked && !retained) dest={};
                // Native memory contains the background, so a repaint in a
                // later lock replaces old coverage rather than accumulating
                // it until every edge becomes opaque. Multiple passes within
                // the same lock still alpha-compose in draw order.
                if (paintTile->generations[index] <= m_writeBoundary)
                    dest = {};
                if (m_options.outline)
                {
                    int dilated = cov;
                    for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox)
                        dilated = std::max(dilated, coverage(row + oy, col + ox));
                    if (dilated > cov) dest = Over(Color(m_options.outlineColor, dilated - cov), dest);
                }
                const auto c=m_colors[cov];
                PlanePixel ink{c.r,c.g,c.b,c.a};
                if(high) {
                    ink.high=true;
                    for(int i=0;i<4;++i) {
                        ink.samples[i]=m_colors[highCov[i]];
                    }
                }
                PlanePixel result=Over(ink, dest);
                if (native) { result.tracked=true; result.background=background; }
                if (native) {
                    const uint32_t rgb=CompositeInk(result,background);
                    result.marker=(unsigned short)((((rgb>>19)&31)<<11)|(((rgb>>10)&63)<<5)|((rgb>>3)&31));
                    // Even invisible black-on-black/very faint ink needs an
                    // observable native write, or restoring black is ambiguous.
                    if (result.marker==background) result.marker^=1;
                    native[(size_t)dy*pitch+dx]=result.marker;
                }
                paintTile->live+=((result.a!=0)||result.high)-((slot.a!=0)||slot.high);
                slot=result;paintTile->generations[index]=++m_generation;
            }
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

    uint32_t PixelPlane::Composite(int x, int y, unsigned short background) const
    {
        return CompositeInk(At(x,y),background);
    }

    uint32_t PixelPlane::CompositeInk(PlanePixel p,unsigned short background) const
    {
        const uint32_t rgb = Expand565(p.tracked ? p.background : background);
        if (!p.a) return rgb;
        const auto channel = [&](uint16_t foreground, int shift) {
            const int c = (rgb >> shift) & 255;
            const uint16_t bg = m_options.linear ? Tables().linear[c] : (uint16_t)(c * 257);
            const int value = foreground + Multiply(bg, (uint16_t)(65535 - p.a));
            return m_options.linear ? (int)Tables().srgb[std::min(65535, value)] : (value + 128) / 257;
        };
        return 0xFF000000u | (channel(p.r, 16) << 16) | (channel(p.g, 8) << 8) | channel(p.b, 0);
    }

    void PixelPlane::CompositeRect(const unsigned short* source, int sourcePitch,
                                   uint32_t* output, int outputPitch, PixelRect rect) const
    {
        rect = Clip(rect);
        if (!source || !output || sourcePitch < m_width || outputPitch < rect.right - rect.left) return;
        for (int y = rect.top; y < rect.bottom; ++y)
            for (int x = rect.left; x < rect.right; ++x)
                output[(size_t)(y - rect.top) * outputPitch + x - rect.left] =
                    Expand565(source[(size_t)y * sourcePitch + x]);
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
        rect=Clip(rect);
        const auto* rgb565=Tables().rgb565;
        for(int y=rect.top;y<rect.bottom;++y) for(int x=rect.left;x<rect.right;++x)
            output[(size_t)(y-rect.top)*outputPitch+x-rect.left]=rgb565[source[(size_t)y*sourcePitch+x]];
        for(const auto& entry:m_tiles) {
            const int tx=(entry.first%m_tilesX)*TileW,ty=(entry.first/m_tilesX)*TileH;
            for(int oy=0;oy<TileH;++oy) for(int ox=0;ox<TileW;++ox) {
                const int x=tx+ox,y=ty+oy;
                const auto& p=entry.second.pixels[oy*TileW+ox];
                if((p.a || p.high) && p.tracked && x>=rect.left && x<rect.right && y>=rect.top && y<rect.bottom)
                    output[(size_t)(y-rect.top)*outputPitch+x-rect.left]=Expand565(p.background);
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

    void PixelPlane::Overlay2Rect(uint32_t* output,int outputPitch,PixelRect rect) const
    {
        // Premultiplied channels. With linear blending, encode them as sRGB
        // for the D3D9 sRGB sampler; it recovers linear premultiplied values.
        const auto* srgb=Tables().srgb;
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
            for(int i=0;i<4;++i) {
                const InkPixel s=p.high ? p.samples[i] : InkPixel{p.r,p.g,p.b,p.a};
                output[(size_t)((y-rect.top)*2+i/2)*outputPitch+(x-rect.left)*2+i%2]=
                    ((unsigned int)((s.a+128)/257)<<24)|(channel(s.r)<<16)|(channel(s.g)<<8)|channel(s.b);
            }
        }
    }
}
