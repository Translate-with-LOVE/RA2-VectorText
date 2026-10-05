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
    struct Target
    {
        unsigned short* base;        // BitFont[+0x0C]
        int pitch;                   // BitFont[+0x10], in pixels
        int clipL, clipT, clipR, clipB;   // inclusive clip rect (font bounds / surface)
    };

    // Draw one glyph cell at (x, y). Returns the number of pixels written.
    int DrawCell(const Target& t, const GlyphCell& cell, int x, int y, int cellLines,
                 unsigned short color);

    // Draw a whole string with per-character advances taken from `advances`
    // (pass NULL to use each cell's own width). Returns the end pen X.
    // `reveal` reproduces the engine's per-character fade toward white:
    // ratio = ((9 - reveal) * 31 + 31 * i) & 0xFF, applied only for 1..8.
    int DrawString(const Target& t, GlyphSource& src, const unsigned int* codepoints,
                   const int* advances, int count, int x, int y, int cellLines,
                   unsigned short color, int reveal);

    // Engine's colour ramp: blend `color` toward white by `ratio` (0..255),
    // using the same bit layout as Drawing::RedShift* / GreenShift* / BlueShift*.
    // Caller supplies the shift/mask description of the surface format.
    struct ColorFormat
    {
        int redShift, redBits;
        int greenShift, greenBits;
        int blueShift, blueBits;
    };
    extern const ColorFormat RGB565;      // typical DirectDraw 16-bit layout
    unsigned short BlendTowardWhite(unsigned short color, int ratio, const ColorFormat& fmt);
}
