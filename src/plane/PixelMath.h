// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../PixelPlaneTypes.h"
#include <algorithm>

// Private math shared by plane implementations; PixelPlane owns all mutable state.
namespace vt::plane::detail
{
struct ColorTables
{
    uint16_t linear[256];
    unsigned char srgb[65536];
    uint32_t rgb565[65536];
    ColorTables();
};

// Inline function-local storage has one shared instance across translation units.
inline const ColorTables& Tables()
{
    static const ColorTables tables;
    return tables;
}

inline uint32_t Expand565(unsigned short color)
{
    return Tables().rgb565[color];
}

inline uint16_t Multiply(uint16_t a, uint16_t b)
// The largest numerator is 65535^2+32767, still within uint32_t.
// Avoid the expensive 64-bit division helper in the 32-bit game.
{ return (uint16_t)(((uint32_t)a * b + 32767u) / 65535u); }

inline int GridScale(const PlanePixel& p) { return p.grid ? p.grid->scale : 2; }
inline InkPixel Sample(const PlanePixel& p,int x,int y,int scale)
{
    if(!p.high) return {p.r,p.g,p.b,p.a,p.edge,p.lightEdge};
    const int n=GridScale(p);
    const int sx=std::min(n-1,(2*x+1)*n/(2*scale)),sy=std::min(n-1,(2*y+1)*n/(2*scale));
    return p.grid ? p.grid->pixels[(size_t)sy*n+sx] : p.samples[sy*2+sx];
}
inline bool Adaptive(const PlanePixel& p)
{
    if(p.subtitle || p.edge) return true;
    if(p.grid) { for(const auto& s:p.grid->pixels) if(s.edge) return true; }
    else if(p.high) for(const auto& s:p.samples) if(s.edge) return true;
    return false;
}

// Pure pixel operations: no calls into PixelPlane or glyph rendering.
PlanePixel Color(unsigned short color, int coverage, bool linear);
PlanePixel Over(PlanePixel source, PlanePixel dest);
uint32_t CompositeInk(PlanePixel pixel, unsigned short background, bool linear);
}
