// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "GlyphSource.h"

// ===========================================================================
//  PixelWriter -- presentation adapter first, native RGB565 fallback.
//
//  Layout verified from BitFont::Blit (0x434120) and its 0x4343F8 path:
//      dst = base + (pitch * y + x) * 2          [bytes]
//      base  = BitFont[+0x0C]   (set by BitFont::Lock)
//      pitch = BitFont[+0x10]   (in pixels)
//      pixel = the 16-bit colour word written verbatim (no palette lookup)
//  Native mono drawing uses MSB-first 1bpp masks and direct colour writes.
//  AA drawing blends coverage against the destination. A presentation adapter
//  may retain either form outside RGB565 before this fallback is reached.
// ===========================================================================

namespace vt
{
    // Bit layout of the game's 16-bit surface colours (Drawing::RedShift* /
    // GreenShift* / BlueShift* globals describe the same thing at run time).
    struct ColorFormat
    {
        int redShift, redBits;
        int greenShift, greenBits;
        int blueShift, blueBits;
    };
    extern const ColorFormat RGB565;      // typical DirectDraw 16-bit layout

    struct Target
    {
        unsigned short* base;        // BitFont[+0x0C]
        int pitch;                   // BitFont[+0x10], in pixels
        int clipL, clipT, clipR, clipB;   // inclusive clip rect (font bounds / surface)
    };

    // Optional presentation adapter. -1 declines and keeps the native path;
    // any nonnegative result means the glyph was retained outside RGB565.
    using PresentationWriter = int (*)(const Target&, const GlyphCell&, int, int,
                                       int, unsigned short, bool);
    void SetPresentationWriter(PresentationWriter writer);
    // Presenter publishes the observed output grid; the game thread updates
    // its font cache at the next glyph lookup, never on the renderer thread.
    void SetOutputRasterScale(int scale);
    int OutputRasterScale();

    // Scoped capture for the movie subtitle's next-frame erase rectangle.
    // Bounds include clipped vector ink and independent HiDPI fringes.
    struct TextInkRect { int left, top, right, bottom; };
    void BeginTextInkCapture(bool subtitleOutline = false);
    bool SubtitleOutlineActive();
    bool EndTextInkCapture(TextInkRect* ink);

    // Draw one glyph cell at (x, y). Native fallback returns pixels written;
    // an accepting presentation adapter returns its nonnegative status.
    int DrawCell(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                 unsigned short color);

    // Same return convention, using 8-bit coverage. The native fallback
    // reads RGB565 destination pixels and blends in the selected colour space;
    // the presentation adapter retains coverage for later BGRA8 composition.
    int DrawCellAA(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                   unsigned short color, const ColorFormat& fmt);

    unsigned short Blend(unsigned short dst, unsigned short src, int coverage, const ColorFormat& fmt,
                         int x = 0, int y = 0);

    // Coverage gamma (1.0 = unchanged coverage, >1 = heavier text).
    // This coverage adjustment is separate from linear-light colour blending.
    void SetCoverageGamma(double gamma);

    // RGB565 fallback can blend in linear light using wider intermediates
    // and dither its final quantization; BGRA8 composition lives in PixelPlane.
    void SetLinearBlend(bool on);
    void SetDither(bool on);

    // Nonzero pixels enables the fixed 3x3 neighbor-coverage pass. Values
    // 1 and 2 use the same radius; this is not an SDF or a variable-width stroke.
    // The native pass visits only pixels with nonzero glyph coverage.
    void SetOutline(int pixels, unsigned short color);
}
