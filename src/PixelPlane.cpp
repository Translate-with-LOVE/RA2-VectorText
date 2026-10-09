// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "PixelPlane.h"
#include <set>
#include <algorithm>
#include <cmath>
#include <limits>

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
        bool DarkText(unsigned short color)
        {
            const auto& tables=Tables();
            const uint32_t rgb=tables.rgb565[color];
            // Pick a rim that separates from the text itself. Scene pixels
            // must not change the rim's color within a glyph or across frames.
            const uint32_t luminance=2126u*tables.linear[(rgb>>16)&255] +
                7152u*tables.linear[(rgb>>8)&255] + 722u*tables.linear[rgb&255];
            return luminance < 11725u*10000u;
        }
        int GridScale(const PlanePixel& p) { return p.grid ? p.grid->scale : 2; }
        InkPixel Sample(const PlanePixel& p,int x,int y,int scale)
        {
            if(!p.high) return {p.r,p.g,p.b,p.a,p.edge,p.lightEdge};
            const int n=GridScale(p);
            const int sx=std::min(n-1,(2*x+1)*n/(2*scale)),sy=std::min(n-1,(2*y+1)*n/(2*scale));
            return p.grid ? p.grid->pixels[(size_t)sy*n+sx] : p.samples[sy*2+sx];
        }
        bool Adaptive(const PlanePixel& p)
        {
            if(p.subtitle || p.edge) return true;
            if(p.grid) { for(const auto& s:p.grid->pixels) if(s.edge) return true; }
            else if(p.high) for(const auto& s:p.samples) if(s.edge) return true;
            return false;
        }
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

    void PixelPlane::Paint(const GlyphCell& cell, int x, int y, int rows,
                           unsigned short color, PixelRect clip,unsigned short* native,int pitch,bool subtitleOutline)
    {
        const int penX=x,penY=y;
        x += cell.inkX; y += cell.inkY;
        if (cell.inkRows) rows = cell.inkRows;
        if (rows <= 0 || rows > 32) return;
        clip = Clip(clip);
        if(subtitleOutline) m_adaptiveEdge=true;
        const auto* high=m_options.highResolution ? cell.raster2 : nullptr;
        const int n=high ? std::max(2,std::min(8,high->scale)) : 2,count=n*n;
        // Bright text already contrasts with dark scenes. A white border would
        // merge with yellow/white strokes; keep that border black on every frame.
        const bool allowWhiteEdge=DarkText(color);
        // White rims read heavier than black ones. Split the dark caption's
        // outline into equal quarter-pixel white/black bands instead of a
        // half-pixel white band followed by a quarter-pixel black band.
        const int edgeRadius=std::max(1,allowWhiteEdge ? n/4 : n/2);
        const int outerRadius=allowWhiteEdge ? edgeRadius*2 : edgeRadius;
        m_rasterScale=std::max(m_rasterScale,n);
        auto floor2=[&](int v) { return (int)std::floor(v/(double)n); };
        const int margin=subtitleOutline ? 1 : 0;
        const int firstRow=(high ? std::min(0,floor2(high->top)-cell.inkY) : 0)-margin;
        const int lastRow=(high ? std::max(rows,floor2(high->top+high->rows+n-1)-cell.inkY) : rows)+margin;
        const int firstCol=(high ? std::min(0,floor2(high->left)-cell.inkX) : 0)-margin;
        const int lastCol=(high ? std::max(24,floor2(high->left+high->width+n-1)-cell.inkX) : 24)+margin;
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
                int ring=0;
                if(subtitleOutline) {
                    int dilated=cov;
                    for(int oy=-1;oy<=1;++oy) for(int ox=-1;ox<=1;++ox)
                        dilated=std::max(dilated,coverage(row+oy,col+ox));
                    ring=dilated-cov;
                }
                int highCov[64]={},highRing[64]={},highWhiteRing[64]={},highSum=0;
                if(high) for(int i=0;i<count;++i) {
                    const int hx=(dx-penX)*n+i%n-high->left;
                    const int hy=(dy-penY)*n+i/n-high->top;
                    auto sample=[&](int sx,int sy) {
                        return sx>=0 && sx<high->width && sy>=0 && sy<high->rows ?
                            (int)m_coverage[high->coverage[(size_t)sy*high->width+sx]] : 0;
                    };
                    highCov[i]=sample(hx,hy);
                    if(subtitleOutline) {
                        int dilated=highCov[i],inner=highCov[i];
                        for(int oy=-outerRadius;oy<=outerRadius;++oy) for(int ox=-outerRadius;ox<=outerRadius;++ox) {
                            const int sampleCoverage=sample(hx+ox,hy+oy);
                            dilated=std::max(dilated,sampleCoverage);
                            if(allowWhiteEdge && abs(ox)<=edgeRadius && abs(oy)<=edgeRadius)
                                inner=std::max(inner,sampleCoverage);
                        }
                        highRing[i]=dilated-highCov[i];
                        highWhiteRing[i]=inner-highCov[i];
                    }
                    highSum+=highCov[i]+highRing[i];
                }
                if (!cov && !ring && !highSum) continue;
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
                // Dark text has a thin white separating rim and a black outer
                // rim. Bright text has a single black rim. Carry both through
                // cached/keyed copies without selecting colors per scene pixel.
                if(subtitleOutline) {
                    PlanePixel edge{};edge.a=edge.edge=(uint16_t)(ring*257);
                    if(allowWhiteEdge) edge.lightEdge=edge.edge;
                    if(high) {
                        edge.high=true;
                        if(n==2) for(int i=0;i<4;++i) {
                            auto& s=edge.samples[i];s.a=s.edge=(uint16_t)(highRing[i]*257);
                            s.lightEdge=(uint16_t)(highWhiteRing[i]*257);
                        }
                        else {
                            auto grid=std::make_shared<InkGrid>();grid->scale=n;grid->pixels.resize(count);
                            for(int i=0;i<count;++i) {
                                auto& s=grid->pixels[i];s.a=s.edge=(uint16_t)(highRing[i]*257);
                                s.lightEdge=(uint16_t)(highWhiteRing[i]*257);
                            }
                            edge.grid=std::move(grid);
                        }
                    }
                    dest=Over(dest,edge); // neighboring glyph ink stays above the border
                }
                const auto c=m_colors[cov];
                PlanePixel ink{c.r,c.g,c.b,c.a};
                if(high) {
                    ink.high=true;
                    if(n==2) for(int i=0;i<4;++i) ink.samples[i]=m_colors[highCov[i]];
                    else {
                        auto grid=std::make_shared<InkGrid>();grid->scale=n;grid->pixels.resize(count);
                        for(int i=0;i<count;++i) grid->pixels[i]=m_colors[highCov[i]];
                        ink.grid=std::move(grid);
                    }
                }
                PlanePixel result=Over(ink, dest);
                if(subtitleOutline) result.subtitle=true;
                if (native) { result.tracked=true; result.background=background; }
                if (native) {
                    const uint32_t rgb=CompositeInk(result,background);
                    result.marker=(unsigned short)((((rgb>>19)&31)<<11)|(((rgb>>10)&63)<<5)|((rgb>>3)&31));
                    // Even invisible black-on-black/very faint ink needs an
                    // observable native write, or restoring black is ambiguous.
                    if (result.marker==background) result.marker^=1;
                    if (subtitleOutline && !result.marker) result.marker=1;
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
                const uint32_t rgb=CompositeInk(p,p.background);
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

    uint32_t PixelPlane::Composite(int x, int y, unsigned short background) const
    {
        return CompositeInk(At(x,y),background);
    }

    uint32_t PixelPlane::CompositeInk(PlanePixel p,unsigned short background) const
    {
        const unsigned short scene=p.tracked ? p.background : background;
        const uint32_t rgb = Expand565(scene);
        if (!p.a) return rgb;
        const int edge=p.lightEdge;
        const auto channel = [&](uint16_t foreground, int shift) {
            const int c = (rgb >> shift) & 255;
            const uint16_t bg = m_options.linear ? Tables().linear[c] : (uint16_t)(c * 257);
            const int value = foreground + edge + Multiply(bg, (uint16_t)(65535 - p.a));
            return m_options.linear ? (int)Tables().srgb[std::min(65535, value)] : (value + 128) / 257;
        };
        return 0xFF000000u | (channel(p.r, 16) << 16) | (channel(p.g, 8) << 8) | channel(p.b, 0);
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
