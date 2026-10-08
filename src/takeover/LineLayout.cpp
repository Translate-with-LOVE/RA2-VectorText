// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GameAddresses.h"
#include "LineLayout.h"

// Natural row spacing, width fitting and TLS reveal plan; native pen returns stay unchanged.
namespace vt::Takeover::detail
{
bool OpeningMark(unsigned int cp)
{
    return cp == 0xFF08 || cp == 0x3010 || cp == 0x300C || cp == 0x300E || cp == 0x2018 || cp == 0x201C;
}
bool CompactMark(unsigned int cp)
{
    return OpeningMark(cp) || cp == 0xFF09 || cp == 0x3011 || cp == 0x300D || cp == 0x300F || cp == 0x2019 ||
           cp == 0x201D || cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF1A ||
           cp == 0xFF1B || cp == 0xFF01 || cp == 0xFF1F;
}
bool ChineseLetter(unsigned int cp)
{
    return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}
bool LatinWord(unsigned int cp)
{
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
}
bool MixedBoundary(unsigned int left, unsigned int right)
{
    return (ChineseLetter(left) && LatinWord(right)) || (LatinWord(left) && ChineseLetter(right));
}
int MixedLatinShiftQ(const LineGlyph* items,int count,int scale1024)
{
    static const bool enabled=Cfg::ConfigBool("MixedScriptCenter",true);
    if(!enabled) return 0;
    bool chinese=false,latin=false;
    for(int i=0;i<count;++i) {
        if(items[i].bitmap) continue;
        const auto cp=g_src.RenderCodepoint(items[i].cp);
        chinese=chinese || ChineseLetter(cp);
        latin=latin || LatinWord(cp);
    }
    return chinese && latin ? g_src.LatinCenterShiftQuarter(scale1024) : 0;
}
int LatinRasterY(unsigned int cp,int shiftQ,int* phase)
{
    if(g_src.IsCJK(cp)) shiftQ=0;
    const int y=(int)floor((shiftQ+2)/4.0);
    *phase=shiftQ-y*4;
    return y;
}
bool NaturalGlyph(LineGlyph &item, unsigned int previous, const LineGlyph *prior, int &penQ, int scale1024,
                  int &mixedAddedQ)
{
    const GlyphCell *cell = g_src.Get(item.cp, -1, 0, scale1024);
    if (!cell || cell->advanceQ <= 0)
        return false;
    penQ += (int)floor(g_src.KerningQuarter(previous, item.cp) * scale1024 / 1024.0 + 0.5);
    int advanceQ = cell->advanceQ;
    const unsigned int renderCp = g_src.RenderCodepoint(item.cp);
    if (CompactMark(renderCp))
    {
        const int halfQ = Cfg::FontSizeCJK() * 2 * scale1024 / 1024;
        const int inkQ = cell->inkRightQ - cell->inkLeftQ;
        advanceQ = halfQ > inkQ + 2 ? halfQ : inkQ + 2;
        const int leftQ = OpeningMark(renderCp) ? advanceQ - 1 - inkQ : 1;
        item.shiftQ = leftQ - cell->inkLeftQ;
    }
    item.leftQ = cell->inkLeftQ + item.shiftQ;
    item.rightQ = cell->inkRightQ + item.shiftQ;
    if (prior && MixedBoundary(g_src.RenderCodepoint(previous), renderCp))
    {
        const int inkGapQ = penQ + item.leftQ - prior->penQ - prior->rightQ;
        const int extraQ = Cfg::FontSizeCJK() * scale1024 / 1024 - inkGapQ;
        if (extraQ > 0)
        {
            penQ += extraQ;
            mixedAddedQ += extraQ;
        }
    }
    item.penQ = penQ;
    penQ += advanceQ;
    return true;
}
bool BitmapGlyph(LineGlyph &item, const unsigned char *slot, unsigned int bytes, int lines, int extra, int &penQ,
                 int scale1024)
{
    // An embedded game icon must not reject the entire vector row.
    // Native bitmaps have no scalable outline; retain them at 1:1.
    if (!slot || scale1024 != 1024 || lines != 16 || bytes != 49 || !slot[0] || slot[0] > 24)
        return false;
    int left = slot[0], right = 0;
    for (int y = 0; y < lines; ++y)
        for (int x = 0; x < slot[0]; ++x)
            if (slot[1 + y * 3 + x / 8] & (0x80 >> (x & 7)))
            {
                if (x < left)
                    left = x;
                if (x + 1 > right)
                    right = x + 1;
            }
    item.bitmap = slot;
    item.penQ = penQ;
    item.leftQ = right ? left * 4 : 0;
    item.rightQ = right * 4;
    penQ += (slot[0] + extra) * 4;
    return true;
}
int TabEnd(int x, int origin, int tab)
{
    // Matches Blit's signed integer remainder (including x<origin).
    return x + tab - ((x + tab - origin) % tab);
}
} // namespace vt::Takeover::detail

namespace vt::Takeover
{
using namespace detail;
namespace
{
struct LineState
{
    bool active;
    void *font;
    void *buffer;
    unsigned int caller;
    int y, pitch, bounds[4], count, consumed, widthQ, originQ, boxWidth, mixedAddedQ, tightenedQ, scale1024;
    int latinShiftQ;
    LineGlyph glyphs[kLineLimit];
};
// Only POD in static TLS: no loader-time allocation or constructors.
static __declspec(thread) LineState t_line = {};
struct LineBox
{
    bool valid;
    void *font;
    const wchar_t *text;
    int gameX, y, boxX, width, align;
};
static __declspec(thread) LineBox t_box = {};
static LONG g_lineLogged = 0, g_lineRejected = 0, g_bitmapLineLogged = 0;
} // namespace
bool LineEnabled()
{
    static const bool requested = Cfg::ConfigBool("LineRender", false);
    return requested && Cfg::Mode() == Cfg::Mode_Draw && !Cfg::VectorMetrics() && Cfg::AdvanceScale() <= 1.0;
}

bool DynamicTextWidthEnabled()
{
    static const bool requested = Cfg::ConfigBool("DynamicTextWidth", true);
    return requested && LineEnabled();
}

void EndLine(void *bitFont)
{
    if (!bitFont || t_line.font == bitFont)
        t_line.active = false;
    if (!bitFont || t_box.font == bitFont)
        t_box.valid = false;
}

void SetLineBox(void *bitFont, const wchar_t *text, int gameX, int y, int boxX, int width, int align)
{
    t_box = {true, bitFont, text, gameX, y, boxX, width, align};
}

bool BeginStringLine(void *bitFont, const wchar_t *text, int count, int x, int y)
{
    const LineBox box = t_box;
    t_box.valid = false;
    if (box.valid && box.font == bitFont && box.text == text && box.gameX == x && box.y == y)
        return BeginLine(bitFont, text, count, x, y, box.boxX, box.width, box.align, game::String_BlitReturn);
    return BeginLine(bitFont, text, count, x, y, x, 0, 0, game::String_BlitReturn);
}

bool GetLineInfo(LineInfo *info)
{
    if (!t_line.active || !info)
        return false;
    info->count = t_line.count;
    info->widthQ = t_line.widthQ;
    info->originQ = t_line.originQ;
    info->consumed = t_line.consumed;
    info->boxWidth = t_line.boxWidth;
    info->mixedAddedQ = t_line.mixedAddedQ;
    info->tightenedQ = t_line.tightenedQ;
    info->scale1024 = t_line.scale1024;
    return true;
}

bool BeginLine(void *bitFont, const wchar_t *text, int count, int gameX, int y, int boxX, int width, int align,
               unsigned int blitCaller, int scale1024)
{
    t_line.active = false;
    if (!LineEnabled() || !bitFont || !text || count < -1 || count > kLineLimit || scale1024 < 922 ||
        scale1024 > 1024 || width < 0 || width > 32768 || gameX < -32768 || gameX > 32768 || boxX < -32768 ||
        boxX > 32768 || y < -32768 || y > 32768)
        return false;
    if (!g_tried)
        Init();
    if (!g_ready)
        return false;
    const unsigned char *bf = (const unsigned char *)bitFont;
    const unsigned char *fd = *(const unsigned char *const *)(bf + BF_INTERNAL);
    void *base = *(void *const *)(bf + BF_BUFFER);
    const int pitch = *(const int *)(bf + BF_PITCH);
    const int *bounds = (const int *)(bf + BF_BOUNDS);
    if (!fd || !base || pitch <= 0 || pitch > 32768 || *(const int *)(fd + IF_LINES) != g_src.Lines() ||
        bounds[0] > bounds[2] || bounds[1] > bounds[3])
        return false;
    const int clipRight = bounds[2] < pitch - 1 ? bounds[2] : pitch - 1;
    if (!width)
    {
        // Ordinary DrawString inherits the verified BitFont clip box.
        // This is a real available width, not a guessed text length.
        boxX = gameX;
        width = clipRight - gameX + 1;
        align = 0;
    }
    else if (boxX + width > clipRight + 1)
        width = clipRight - boxX + 1;
    if (width <= 0 || width > 65536)
        return false;
    const unsigned short *map = *(const unsigned short *const *)(fd + IF_SYMTABLE);
    const unsigned char *data = *(const unsigned char *const *)(fd + IF_BITMAPS);
    const unsigned int bytes = *(const unsigned int *)(fd + IF_SYMBOLBYTES);
    const int extra = *(const int *)(bf + 0x2C);
    const int tab = *(const int *)(bf + 0x28);
    const int tabOrigin = *(const int *)(bf + 0x20);
    if (!map || !data || bytes < 1 || bytes > 4096 || extra < 0 || extra > 16)
        return false;
    t_line.font = bitFont;
    t_line.buffer = base;
    t_line.pitch = pitch;
    memcpy(t_line.bounds, bounds, sizeof(t_line.bounds));
    t_line.caller = blitCaller;
    t_line.y = y;
    t_line.count = t_line.consumed = 0;
    t_line.boxWidth = width;
    t_line.mixedAddedQ = t_line.tightenedQ = 0;
    t_line.scale1024 = scale1024;
    int legacyX = gameX, penQ = 0;
    unsigned int previous = 0;
    int i = 0;
    for (; i < kLineLimit && (count < 0 || i < count); ++i)
    {
        const unsigned int cp = text[i];
        if (!cp)
            break;
        if (cp == '\r' || cp == '\n')
            continue;
        LineGlyph &item = t_line.glyphs[t_line.count++];
        memset(&item, 0, sizeof(item));
        item.cp = cp;
        item.gameX = legacyX;
        if (cp == '\t')
        {
            if (tab <= 0 || tab > 32768)
                return false;
            // Centre/right alignment changes the starting pen; rather
            // than guess a different tab grid, retain the legacy row.
            if (width > 0 && (align & 3))
                return false;
            legacyX = TabEnd(legacyX, tabOrigin, tab);
            item.penQ = penQ;
            penQ = TabEnd(penQ, (tabOrigin - gameX) * 4, tab * 4);
            item.leftQ = item.rightQ = 0;
            previous = 0;
        }
        else
        {
            if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || !map[cp])
                return false;
            legacyX += data[(size_t)(map[cp] - 1) * bytes] + extra;
            const LineGlyph *prior = t_line.count > 1 ? &t_line.glyphs[t_line.count - 2] : NULL;
            if (!NaturalGlyph(item, previous, prior, penQ, scale1024, t_line.mixedAddedQ) &&
                !BitmapGlyph(item, data + (size_t)(map[cp] - 1) * bytes, bytes, g_src.Lines(), extra, penQ, scale1024))
                return false;
            previous = item.bitmap ? 0 : cp;
        }
        item.gameEnd = legacyX;
    }
    if (count < 0 && i == kLineLimit && text[i])
        return false;
    if (!t_line.count)
        return false;
    int widthQ = penQ;
    LineGlyph &last = t_line.glyphs[t_line.count - 1];
    if (last.penQ + last.rightQ > widthQ)
        widthQ = last.penQ + last.rightQ;
    if (width > 0 && widthQ > width * 4)
    {
        // Tighten only existing inter-glyph whitespace, keeping >=0.5px
        // between ink boxes. If that cannot fit, reject BEFORE painting.
        int totalQ = 0;
        for (int j = 1; j < t_line.count; ++j)
        {
            LineGlyph &a = t_line.glyphs[j - 1];
            LineGlyph &b = t_line.glyphs[j];
            if (a.cp == '\t' || b.cp == '\t')
                continue;
            // Mixed-script boundaries keep >=1px; others keep >=0.5px.
            const int floorQ = MixedBoundary(a.cp, b.cp) ? 4 : 2;
            const int gapQ = b.penQ + b.leftQ - a.penQ - a.rightQ - floorQ;
            b.capacityQ = gapQ > 0 ? gapQ : 0;
            totalQ += b.capacityQ;
        }
        const int neededQ = widthQ - width * 4;
        if (totalQ < neededQ)
        {
            // The complete row scales as one unit, including advances,
            // bearings and kerning. Never shrink characters separately.
            const int nextScale = scale1024 * width * 4 / widthQ - 1;
            if (nextScale >= 922 && nextScale < scale1024)
                return BeginLine(bitFont, text, count, gameX, y, boxX, width, align, blitCaller, nextScale);
            if (InterlockedIncrement(&g_lineRejected) <= 3)
                Log::Note("LINE fallback: natural=%d/4px box=%dpx (uniform scale would be below 90%%)", widthQ, width);
            return false;
        }
        int remainingQ = neededQ, remainingCapacityQ = totalQ, shiftQ = 0;
        for (int j = 1; j < t_line.count; ++j)
        {
            LineGlyph &b = t_line.glyphs[j];
            int dropQ = remainingCapacityQ ? remainingQ * b.capacityQ / remainingCapacityQ : 0;
            remainingQ -= dropQ;
            remainingCapacityQ -= b.capacityQ;
            shiftQ += dropQ;
            b.penQ -= shiftQ;
        }
        widthQ -= neededQ;
        t_line.tightenedQ = neededQ;
    }
    int offsetQ = 0;
    if (width > 0 && width * 4 > widthQ)
    {
        if (align & 1)
            offsetQ = (width * 4 - widthQ) / 2;
        else if (align & 2)
            offsetQ = width * 4 - widthQ;
    }
    t_line.originQ = (width > 0 ? boxX : gameX) * 4 + offsetQ;
    t_line.widthQ = widthQ;
    t_line.latinShiftQ=MixedLatinShiftQ(t_line.glyphs,t_line.count,scale1024);
    // Warm every final phase before accepting the line. An unsupported
    // raster cannot fail halfway through a run already being painted.
    for (int j = 0; j < t_line.count; ++j)
    {
        const LineGlyph &item = t_line.glyphs[j];
        if (item.cp == '\t' || item.bitmap)
            continue;
        const int q = t_line.originQ + item.penQ + item.shiftQ;
        int phase;
        LineRasterX(q, &phase);
        int verticalPhase=0;LatinRasterY(item.cp,t_line.latinShiftQ,&verticalPhase);
        if (!g_src.Get(item.cp, -1, phase, scale1024,verticalPhase))
            return false;
    }
    t_line.active = true;
    for (int j = 0; j < t_line.count; ++j)
        if (t_line.glyphs[j].bitmap)
        {
            if (InterlockedIncrement(&g_bitmapLineLogged) <= 3)
                Log::Note("LINE native icon: U+%04X advance=%dpx row=%d/4px count=%d x=%d/4px y=%d",
                          t_line.glyphs[j].cp, t_line.glyphs[j].bitmap[0] + extra, widthQ, t_line.count, t_line.originQ,
                          y);
            break;
        }
    if (InterlockedIncrement(&g_lineLogged) <= 3)
        Log::Note("LINE ready: count=%d width=%d/4px box=%dpx scale=%d/1024 mixed-gap=%d/4px tightened=%d/4px "
                  "x=%d/4px y=%d caller=0x%08X raster-subpixel=%d",
                  t_line.count, widthQ, width, scale1024, t_line.mixedAddedQ, t_line.tightenedQ, t_line.originQ, y,
                  blitCaller, g_subpixel ? 1 : 0);
    return true;
}

bool TryLineBlit(void *bitFont, unsigned int ch, int x, int y, int colorArg, unsigned int blitCaller, int *newX)
{
    g_src.SetHighResolutionScale(OutputRasterScale());
    if (!t_line.active || t_line.font != bitFont || t_line.caller != blitCaller)
        return false;
    const unsigned char *bf = (const unsigned char *)bitFont;
    if (t_line.consumed >= t_line.count || t_line.y != y || t_line.buffer != *(void *const *)(bf + BF_BUFFER) ||
        t_line.pitch != *(const int *)(bf + BF_PITCH) || memcmp(t_line.bounds, bf + BF_BOUNDS, sizeof(t_line.bounds)))
    {
        t_line.active = false;
        return false;
    }
    const LineGlyph &item = t_line.glyphs[t_line.consumed];
    if (item.cp != ch || item.gameX != x)
    {
        t_line.active = false;
        return false;
    }
    if (ch != '\t')
    {
        const int q = t_line.originQ + item.penQ + item.shiftQ;
        int phase;
        phase = 0;
        // Native 1bpp icons already use integer pixel coordinates.
        const int drawX = item.bitmap ? (int)floor(q / 4.0) : LineRasterX(q, &phase);
        GlyphCell native;
        const GlyphCell *cell;
        int verticalPhase=0;
        const int drawY=y+(item.bitmap ? 0 : LatinRasterY(ch,t_line.latinShiftQ,&verticalPhase));
        if (item.bitmap)
        {
            memset(&native, 0, sizeof(native));
            native.width = item.bitmap[0];
            native.inkRows = g_src.Lines();
            memcpy(native.bits, item.bitmap + 1, 3 * native.inkRows);
            for (int row = 0; row < native.inkRows; ++row)
                for (int col = 0; col < native.width; ++col)
                    if (native.bits[row * 3 + col / 8] & (0x80 >> (col & 7)))
                        native.cov[row * 24 + col] = 255;
            cell = &native;
        }
        else
            cell = g_src.Get(ch, -1, phase, t_line.scale1024,verticalPhase);
        if (!cell)
        {
            t_line.active = false;
            return false;
        }
        Target target = {(unsigned short *)t_line.buffer,
                         t_line.pitch,
                         t_line.bounds[0] > 0 ? t_line.bounds[0] : 0,
                         t_line.bounds[1] > 0 ? t_line.bounds[1] : 0,
                         t_line.bounds[2] < t_line.pitch - 1 ? t_line.bounds[2] : t_line.pitch - 1,
                         t_line.bounds[3]};
        const unsigned short color =
            colorArg == -1 ? *(const unsigned short *)(bf + BF_COLOR) : (unsigned short)colorArg;
        if (Cfg::AntiAlias())
            DrawCellAA(target, *cell, drawX, drawY, g_src.Lines(), color, RGB565);
        else
            DrawCell(target, *cell, drawX, drawY, g_src.Lines(), color);
        ++g_drawn;
    }
    if (newX)
        *newX = item.gameEnd;
    ++t_line.consumed;
    return true;
}
} // namespace vt::Takeover
