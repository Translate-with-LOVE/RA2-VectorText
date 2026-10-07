// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Shared font and verified native offsets; no line TLS or diagnostics ownership here.
#include "Takeover.h"
#include "GlyphSource.h"
#include "PixelWriter.h"
#include "Logger.h"
#include <cstring>
#include <vector>
#include <cmath>

namespace vt::Takeover::detail
{
// Offsets verified by disassembling BitFont::Blit / BitFont::Lock:
//   BitFont   +0x04 InternalData*   +0x0C locked buffer   +0x10 pitch(px)
//             +0x24 colour word     +0x30..0x3C bounds L,T,R,B
//   Internal  +0x08 lines           +0x14 symbolBytes
//             +0x18 symbol table    +0x1C glyph bitmaps
static const int BF_INTERNAL = 0x04;
static const int BF_BUFFER = 0x0C;
static const int BF_PITCH = 0x10;
static const int BF_COLOR = 0x24;
static const int BF_BOUNDS = 0x30;

static const int IF_LINES = 0x08;
static const int IF_SYMBOLBYTES = 0x14;
static const int IF_SYMTABLE = 0x18;
static const int IF_BITMAPS = 0x1C;

enum Reason
{
    R_NotReady = 0,
    R_NoInternal,
    R_NotLocked,
    R_BadMetrics,
    R_FontMetrics,
    R_NoGlyph,
    R_NoVectorGlyph,
    R_BadBounds,
    R_Exception,
    R_Count
};

extern GlyphSource g_src;
extern bool g_tried, g_ready, g_subpixel;
extern unsigned long long g_drawn, g_skipped, g_failed, g_unknown;
extern unsigned long long g_reason[R_Count];
// Keep fractional layout/kerning regardless of raster positioning.
// Snap only the final origin; rounding advances would accumulate drift.
inline int LineRasterX(int q, int *phase)
{
    const int x = (int)floor((q + (g_subpixel ? 0 : 2)) / 4.0);
    *phase = g_subpixel ? q - x * 4 : 0;
    return x;
}
} // namespace vt::Takeover::detail
