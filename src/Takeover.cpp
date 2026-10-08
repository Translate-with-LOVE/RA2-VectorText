// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "takeover/State.h"

// Font initialization and per-glyph fallback; full rows are handled by LineLayout.
namespace vt::Takeover::detail
{
GlyphSource g_src;
bool g_tried = false, g_ready = false, g_subpixel = true;
} // namespace vt::Takeover::detail

namespace vt::Takeover
{
using namespace detail;
// ---- subpixel pen -------------------------------------------------
// Per-glyph fallback carries advance-rounding error in thread-local state.
// A discontinuity resets the run; the raster phase is clamped to +/-0.5px
// and quantized to quarter pixels. Prepared rows use their own line plan.
static __declspec(thread) double t_ideal = 0.0;
static __declspec(thread) bool t_hasIdeal = false;

static int SubpixelPhase(int x, double trueAdvance, int *published)
{
    int rounded = (int)floor(trueAdvance + 0.5);
    if (rounded < 1)
        rounded = 1; // 0 would freeze the pen
    if (published)
        *published = rounded;
    if (!g_subpixel)
        return 0;

    if (!t_hasIdeal || fabs(t_ideal - (double)x) > 2.0)
    {
        t_ideal = (double)x; // new run: anchor on the engine
        t_hasIdeal = true;
    }
    double frac = t_ideal - (double)x; // our ideal pen vs the engine
    if (frac < -0.5)
        frac = -0.5; // keep it in half a pixel
    if (frac > 0.5)
        frac = 0.5;
    int phase = (int)floor(frac * 4.0 + 0.5); // SIGNED quarter pixels
    if (phase < -2)
        phase = -2; // drop the integer part:
    if (phase > 2)
        phase = 2; // |shift| stays <= 0.5 px
    t_ideal += trueAdvance;
    return phase;
}
// The engine's own advance, remembered the first time a glyph slot is
// seen.  Without this, publishing our advance back into the font data
// fed our own value into the next call, so measurement, cache and
// drawing disagreed (overlapping Latin, cramped punctuation).
static unsigned char g_origAdv[16384];
static bool g_origSeen[16384];

static int OrigAdvance(unsigned int idx, unsigned char current)
{
    if (idx >= 16384)
        return current;
    if (!g_origSeen[idx])
    {
        g_origAdv[idx] = current;
        g_origSeen[idx] = true;
    }
    return (int)g_origAdv[idx];
}

static bool g_vecMetrics = false;
static double g_advScale = 1.0;
bool Ready()
{
    return g_ready;
}

bool Init()
{
    if (g_tried)
        return g_ready;
    g_tried = true;

    const char *ttf = Cfg::FontFile();
    g_vecMetrics = Cfg::VectorMetrics();
    g_advScale = Cfg::AdvanceScale();
    g_subpixel = Cfg::ConfigBool("Subpixel", true);
    g_src.SetLegacyCodepage1252(Cfg::LegacyCodepage1252());
    Log::Note("M1 metrics: mode=%d advanceScale=%.3f subpixel=%d supersample=%d", (int)Cfg::Mode(), g_advScale,
              g_subpixel ? 1 : 0, Cfg::Supersample());
    // Vector metrics keep the fallback glyph's natural width and publish
    // that integer width to the native slot/pen; row plans keep native returns.
    g_src.SetFitToAdvance(!g_vecMetrics && Cfg::FitToAdvance());

    const bool useAA = Cfg::AntiAlias() && Cfg::Mode() == Cfg::Mode_Draw;
    g_src.SetAntiAlias(useAA);
    g_src.SetStemDarkening(Cfg::StemDarkening());
    g_src.SetSupersample(Cfg::Supersample());
    g_src.SetHinting(Cfg::ConfigInt("Hinting", 0));
    g_src.SetHighResolution(Cfg::ConfigBool("Present32", true) && Cfg::HiDPI());
    g_src.SetHighResolutionScale(OutputRasterScale());
    SetCoverageGamma(Cfg::Gamma());
    SetLinearBlend(Cfg::LinearBlend());
    SetDither(Cfg::Dither());
    SetOutline(Cfg::Outline(), Cfg::OutlineColor());

    const DWORD t0 = GetTickCount();

    // the game's font cell is 3 bytes x 16 rows (game.fnt header)
    if (!g_src.Init(ttf, Cfg::FontSizeLatin(), Cfg::FontWeight(), 3, 16, Cfg::BaselineRow()))
    {
        Log::Note("FALLBACK font-load-failed file=\"%s\"", ttf);
        g_ready = false;
        return false;
    }
    g_src.SetSizes(Cfg::FontSizeLatin(), Cfg::FontSizeCJK());
    static char latinFont[MAX_PATH] = {0};
    Cfg::ConfigStr("FontFileLatin", "", latinFont, sizeof(latinFont));
    if (latinFont[0])
        Log::Note("Latin font %s: \"%s\"", g_src.SetLatinFont(latinFont) ? "ready" : "fallback", latinFont);
    g_ready = true;
    Log::Note("Text options: LegacyCodepage1252=%s HiDPI=%s", Cfg::LegacyCodepage1252() ? "true" : "false",
              Cfg::HiDPI() ? "true" : "false");
    SetStage(2);

    Log::Note("M1 font ready in %u ms: \"%s\" latin=%dpx cjk=%dpx wght=%d baseline=%d fit=%d aa=%d hinting=%d",
              GetTickCount() - t0, ttf, Cfg::FontSizeLatin(), Cfg::FontSizeCJK(), Cfg::FontWeight(), Cfg::BaselineRow(),
              Cfg::FitToAdvance() ? 1 : 0, useAA ? 1 : 0, Cfg::ConfigInt("Hinting", 0));
    return true;
}

bool TryBlit(void *bitFont, unsigned int ch, int x, int y, int colorArg, int *newX, unsigned int caller)
{
    g_src.SetHighResolutionScale(OutputRasterScale());
    // Ensure the fallback font is ready. Line preparation and measurement
    // also call the same idempotent Init().
    if (!g_tried)
        Init();

    if (!bitFont || !g_ready)
    {
        ++g_reason[R_NotReady];
        return false;
    }

    const unsigned char *bf = (const unsigned char *)bitFont;

    const unsigned char *internal = *(const unsigned char *const *)(bf + BF_INTERNAL);
    if (!internal)
    {
        ++g_reason[R_NoInternal];
        return false;
    }

    void *base = *(void *const *)(bf + BF_BUFFER); // set by BitFont::Lock
    const int pitch = *(const int *)(bf + BF_PITCH);
    if (!base || pitch <= 0)
    {
        ++g_reason[R_NotLocked];
        return false;
    }

    const unsigned short *symTable = *(const unsigned short *const *)(internal + IF_SYMTABLE);
    const unsigned int symbolBytes = *(const unsigned int *)(internal + IF_SYMBOLBYTES);
    const unsigned char *bitmaps = *(const unsigned char *const *)(internal + IF_BITMAPS);
    const int lines = *(const int *)(internal + IF_LINES);

    if (!symTable || !bitmaps || symbolBytes == 0 || lines <= 0 || lines > 32)
    {
        ++g_reason[R_BadMetrics];
        return false;
    }

    // The glyph cell we rasterise is built for the game font's metrics.
    // A different bitmap font (other line count) is handed back to the
    // engine rather than drawn with the wrong baseline.
    if (lines != g_src.Lines())
    {
        ++g_failed;
        ++g_reason[R_FontMetrics];
        return false;
    }

    const unsigned int idx = symTable[ch & 0xFFFF];
    if (idx == 0)
    {
        // no such glyph in the game's font: let the engine deal with it
        ++g_unknown;
        ++g_reason[R_NoGlyph];
        return false;
    }

    const int *bounds = (const int *)(bf + BF_BOUNDS);
    Target t;
    t.base = (unsigned short *)base;
    t.pitch = pitch;
    t.clipL = bounds[0] > 0 ? bounds[0] : 0;
    t.clipT = bounds[1] > 0 ? bounds[1] : 0;
    t.clipR = bounds[2] < pitch - 1 ? bounds[2] : pitch - 1;
    t.clipB = bounds[3];
    const bool rawInvalid = bounds[2] <= bounds[0] || bounds[3] <= bounds[1];
    if (rawInvalid || t.clipR < t.clipL || t.clipB < t.clipT)
    {
        ++g_failed;
        const unsigned long long n = ++g_reason[R_BadBounds];
        // First failures plus exponentially spaced samples show later
        // gameplay without producing tens of thousands of log lines.
        if (n <= 8 || (n & (n - 1)) == 0)
            Log::Note("REFUSE bounds #%llu kind=%s caller=0x%08X bf=0x%p base=0x%p "
                      "pitch=%d bounds=%d,%d,%d,%d clip=%d,%d,%d,%d "
                      "ch=U+%04X x=%d y=%d tracking=%d nativeClip=%u",
                      n, rawInvalid ? "raw-clip" : "surface-clip", caller, bitFont, base, pitch, bounds[0], bounds[1],
                      bounds[2], bounds[3], t.clipL, t.clipT, t.clipR, t.clipB, ch, x, y, *(const int *)(bf + 0x2C),
                      (unsigned)bf[0x41]);
        return false; // Do not mutate glyph widths on a refusal.
    }

    unsigned char *slot0 = (unsigned char *)(bitmaps + (size_t)(idx - 1) * symbolBytes);
    const unsigned char *glyph = slot0;
    // Keep the remembered native advance when scaled/vector metrics
    // publish a different advance into the engine slot.
    const int gameAdvance = OrigAdvance(idx, glyph[0]);
    const double trueAdv = (g_advScale > 1.0) ? gameAdvance * g_advScale : (double)gameAdvance;
    int published = gameAdvance;
    const int phase = SubpixelPhase(x, trueAdv, &published);
    const int advance = g_vecMetrics ? -1 : published;

    const GlyphCell *cell = g_src.Get(ch, advance > 0 ? advance : -1, phase);
    if (!cell)
    {
        ++g_failed;
        ++g_reason[R_NoVectorGlyph];
        return false;
    }

    // Publish the fallback's chosen advance to the native glyph slot.
    // Prepared row rendering instead keeps the native slot unchanged.
    if (slot0 && *slot0 != cell->width)
        *slot0 = cell->width;

    const unsigned short color = (colorArg == -1) ? *(const unsigned short *)(bf + BF_COLOR) : (unsigned short)colorArg;

    if (Cfg::AntiAlias())
        DrawCellAA(t, *cell, x, y, lines, color, RGB565);
    else
        DrawCell(t, *cell, x, y, lines, color);
    SetStage(6); // pixels written

    ++g_drawn;
    if (newX)
        // Native BitFont::Blit reads +2C; it does not update a tracking
        // accumulator. Both the drawn and clipped native paths return
        // X + glyph advance + this constant spacing (0x4344E4).
        *newX = x + cell->width + *(const int *)(bf + 0x2C);
    return true;
}
} // namespace vt::Takeover
