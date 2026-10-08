// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Private natural-spacing primitives shared with UI measurement.
#include "State.h"

namespace vt::Takeover::detail
{
struct LineGlyph
{
    unsigned int cp;
    int gameX, gameEnd, penQ, shiftQ, leftQ, rightQ, capacityQ;
    const unsigned char *bitmap; // missing vector glyph: retain the native icon
};

constexpr int kLineLimit = 2048;
bool NaturalGlyph(LineGlyph &item, unsigned int previous, const LineGlyph *prior, int &penQ, int scale1024,
                  int &mixedAddedQ);
bool BitmapGlyph(LineGlyph &item, const unsigned char *slot, unsigned int bytes, int lines, int extra, int &penQ,
                 int scale1024);
int TabEnd(int x, int origin, int tab);
int MixedLatinShiftQ(const LineGlyph* items,int count,int scale1024);
int LatinRasterY(unsigned int cp,int shiftQ,int* phase);
} // namespace vt::Takeover::detail
