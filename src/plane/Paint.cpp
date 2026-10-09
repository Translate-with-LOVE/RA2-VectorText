// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "../PixelPlane.h"
#include "../GlyphSource.h"
#include "PixelMath.h"
#include <cmath>

namespace vt
{
using plane::detail::Color;
using plane::detail::CompositeInk;
using plane::detail::Over;
using plane::detail::Tables;

namespace
{
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
            const auto p=Color(color,i,m_options.linear);m_colors[i]={p.r,p.g,p.b,p.a};
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
                if (dilated > cov) dest = Over(Color(m_options.outlineColor, dilated - cov, m_options.linear), dest);
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
                const uint32_t rgb=CompositeInk(result,background,m_options.linear);
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
}
