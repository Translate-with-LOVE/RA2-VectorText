// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GlyphSource.h"
#include <array>
#include <map>
#include <stdint.h>
#include <vector>

namespace vt
{
    struct PixelRect { int left, top, right, bottom; }; // exclusive right/bottom
    struct InkPixel { uint16_t r=0,g=0,b=0,a=0; };
    struct PlanePixel
    {
        uint16_t r, g, b, a; // premultiplied 16-bit linear (or configured sRGB) channels
        uint16_t background = 0, marker = 0;
        bool tracked = false; // native compatibility pixel, excluded from final blend
        bool high = false;
        std::array<InkPixel,4> samples{}; // four independent output pixels, not enlarged 1x ink
    };
    struct PlaneOptions
    {
        bool linear = true, antialias = true;
        double gamma = 1.0;
        int outline = 0;
        unsigned short outlineColor = 0;
        bool highResolution = false;
    };

    // High-precision text sidecar with optional native compatibility pixels.
    // Sparse tiles avoid allocating a full 32-bit layer for every game surface.
    // Copy/clear operations follow the surfaces before cnc-ddraw presents them.
    class PixelPlane
    {
    public:
        PixelPlane(int width, int height, const PlaneOptions& options);
        PixelPlane(const PixelPlane& other);
        PixelPlane& operator=(const PixelPlane& other);
        void Paint(const GlyphCell& cell, int x, int y, int rows,
                   unsigned short color, PixelRect clip,unsigned short* native = nullptr,int pitch = 0);
        // Detect opaque CPU/SHP writes even when they restore the same original
        // background. The native pixel differs while retained text is present.
        void ValidateNative(const unsigned short* native,int pitch);
        void Clear(PixelRect rect);
        void ClearPixel(int x, int y);
        PlanePixel At(int x, int y) const;
        void Set(int x, int y, PlanePixel pixel);
        bool Empty() const { return m_tiles.empty(); }
        size_t TileCount() const { return m_tiles.size(); }
        int Width() const { return m_width; }
        int Height() const { return m_height; }

        // Native base pixels are read only for colour keys and occlusion.
        // A transparent keyed background still transfers its text mask.
        // Tracked compatibility pixels carry the original background/key.
        void Copy(const PixelPlane* source, PixelRect sourceRect, PixelRect destRect,
                  const unsigned short* sourceBase, int sourcePitch,
                  const unsigned short* destBase = nullptr, int destPitch = 0,
                  bool sourceKey = false, unsigned short sourceKeyLow = 0,
                  unsigned short sourceKeyHigh = 0, bool destKey = false,
                  unsigned short destKeyLow = 0, unsigned short destKeyHigh = 0,
                  bool mirrorX = false, bool mirrorY = false);

        // BGRA8 output; antialiased text never undergoes 5/6/5 quantisation.
        uint32_t Composite(int x, int y, unsigned short background) const;
        void CompositeRect(const unsigned short* source, int sourcePitch,
                           uint32_t* output, int outputPitch, PixelRect rect) const;
        static uint32_t Expand565(unsigned short color);
        void BackgroundRect(const unsigned short* source,int sourcePitch,uint32_t* output,int outputPitch,PixelRect rect) const;
        // Include neighboring cells touched by the bilinear filter footprint.
        std::vector<PixelRect> TextTiles(bool filtered = false) const;
        void Overlay2Rect(uint32_t* output,int outputPitch,PixelRect rect) const;
        struct NativeSample { int x, y; unsigned short base; unsigned int generation; };
        std::vector<NativeSample> CaptureNative(const unsigned short* base, int pitch,
                                               PixelRect rect) const;
        void InvalidateNative(const unsigned short* base, int pitch,
                              const std::vector<NativeSample>& samples);
        void BeginWrite() { m_writeBoundary = m_generation; }
        bool Intersects(PixelRect rect) const;

    private:
        static const int TileW = 32, TileH = 16;
        struct Tile {
            std::array<PlanePixel, TileW * TileH> pixels{};
            std::array<unsigned int, TileW * TileH> generations{};
            int live = 0;
        };
        int m_width, m_height, m_tilesX;
        PlaneOptions m_options;
        unsigned char m_coverage[256];
        std::array<InkPixel,256> m_colors{};
        unsigned short m_color = 0;
        bool m_colorReady = false;
        using TileMap=std::map<int,Tile>;
        TileMap m_tiles;
        std::vector<TileMap::node_type> m_spareTiles;
        unsigned int m_generation = 0;
        unsigned int m_writeBoundary = 0;
        PixelRect Clip(PixelRect rect) const;
        PlanePixel Color(unsigned short color, int coverage) const;
        static PlanePixel Over(PlanePixel source, PlanePixel dest);
        uint32_t CompositeInk(PlanePixel pixel,unsigned short background) const;
        TileMap::iterator NewTile(int key);
        TileMap::iterator RecycleTile(TileMap::iterator tile);
    };
}
