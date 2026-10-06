#pragma once
#include "GlyphSource.h"

// ===========================================================================
//  PixelWriter -- writes glyph masks into the game's own 16-bit surfaces.
//
//  Layout verified from BitFont::Blit (0x434120) and its 0x4343F8 path:
//      dst = base + (pitch * y + x) * 2          [bytes]
//      base  = BitFont[+0x0C]   (set by BitFont::Lock)
//      pitch = BitFont[+0x10]   (in pixels)
//      pixel = the 16-bit colour word written verbatim (no palette lookup)
//  The engine writes MSB-first 1bpp masks; we do exactly the same, so our
//  output is byte-identical in shape to the original for the same mask.
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

    // Draw one glyph cell at (x, y). Returns the number of pixels written.
    int DrawCell(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                 unsigned short color);

    // Same, but blends using the cell's 8-bit coverage: dst = lerp(dst, color, cov).
    // This is real antialiasing *inside the game's own 16-bit surface* -- the
    // destination value is read back, so text composites correctly over
    // whatever was drawn before it.
    int DrawCellAA(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                   unsigned short color, const ColorFormat& fmt);

    // Draw a whole string with per-character advances taken from `advances`
    // (pass NULL to use each cell's own width). Returns the end pen X.
    // `reveal` reproduces the engine's per-character fade toward white:
    // ratio = ((9 - reveal) * 31 + 31 * i) & 0xFF, applied only for 1..8.
    int DrawString(const Target& t, GlyphSource& src, const unsigned int* codepoints,
                   const int* advances, int count, int x, int y, int cellLines,
                   unsigned short color, int reveal);

    unsigned short BlendTowardWhite(unsigned short color, int ratio, const ColorFormat& fmt);
    unsigned short Blend(unsigned short dst, unsigned short src, int coverage, const ColorFormat& fmt);

    // Coverage gamma for the antialiased path (1.0 = linear, >1 = heavier text).
    void SetCoverageGamma(double gamma);

    // 32-bit compositing path: blend in linear light and dither the final
    // quantisation to 16-bit (kills the banding of plain 5/6/5 rounding)
    void SetLinearBlend(bool on);
    void SetDither(bool on);

    // Outline: dilate the coverage by pixels and paint the ring in color`r
    // before the glyph (SDF-style), which is what keeps small text readable.
    void SetOutline(int pixels, unsigned short color);
}
