// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace vt
{
    struct PixelRect { int left, top, right, bottom; }; // exclusive right/bottom
    struct InkPixel { uint16_t r=0,g=0,b=0,a=0,edge=0,lightEdge=0; };
    struct InkGrid { int scale = 2; std::vector<InkPixel> pixels; };
    struct PlanePixel
    {
        uint16_t r, g, b, a; // premultiplied 16-bit linear (or configured sRGB) channels
        uint16_t background = 0, marker = 0;
        bool tracked = false; // native compatibility pixel, excluded from final blend
        bool high = false;
        bool subtitle = false; // body markers need refresh too when copied onto matching scene colors
        std::array<InkPixel,4> samples{}; // four independent output pixels, not enlarged 1x ink
        uint16_t edge = 0; // total border contribution, also tracked for native copies/erasure
        uint16_t lightEdge = 0; // stable white inner rim for dark text; outer rim stays black
        std::shared_ptr<const InkGrid> grid; // allocated only above 2x; copies share immutable samples
    };
    struct PlaneOptions
    {
        bool linear = true, antialias = true;
        double gamma = 1.0;
        int outline = 0;
        unsigned short outlineColor = 0;
        bool highResolution = false;
    };
}
