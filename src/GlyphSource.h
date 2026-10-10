// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include <map>
#include <vector>

// ===========================================================================
//  GlyphSource -- cached glyph masks, coverage, bearings and natural advances.
//
//  GlyphCell is our own structure, not a serialized game.fnt record. Its 1bpp
//  mask uses the game's MSB-first row layout; grayscale coverage and optional
//  2..8x rasters serve RGB565 drawing and presentation overlays.
//
//  Latin and CJK keep separate faces when their size or font differs, avoiding
//  FT_Set_Pixel_Sizes calls on each script switch.
// ===========================================================================

namespace vt
{
struct GlyphRaster2
{
    int left = 0, top = 0, width = 0, rows = 0;
    int scale = 2; // independent outline raster samples per logical pixel
    std::vector<unsigned char> coverage;
};
struct GlyphCell
{
    unsigned char width;               // integer compatibility advance (pixels)
    unsigned char bits[3 * 32];        // strideBytes * lines, <= 96 bytes
    unsigned char cov[24 * 32];        // 8-bit coverage; mono mode also fills it with sampled 0/255 ink
    int inkX;                          // natural glyph bearing, in pixels
    int inkY, inkRows;                 // natural vertical bearing/height; 0 rows = legacy cell
    int advanceQ, inkLeftQ, inkRightQ; // quarter-pixel natural metrics
    const GlyphRaster2 *raster2;       // optional outline raster; scale identifies density; cache-owned
};

class GlyphSource
{
  public:
    GlyphSource();
    ~GlyphSource();

    // Logical stride/line count match the native font (game.fnt: 3 / 16).
    // baselineRow is the baseline offset from the engine's text Y. Actual
    // raster bearings may place ink above or below that logical cell.
    bool Init(const char *ttfPath, int pixelSize, int weight, int strideBytes = 3, int lines = 16,
              int baselineRow = 13);
    void Shutdown();

    // Configure Latin and main-font pixel sizes independently. Codepoints at
    // or above cjkFrom use the main face; default U+2E80 is CJK radicals supplement.
    void SetSizes(int latinPx, int mainPx, unsigned int cjkFrom = 0x2E80);
    // Optional Latin face chosen for the game's narrow ASCII advances.
    // Empty path uses FontFile with independent weight. The main face keeps
    // its own weight; fitting is controlled separately.
    bool SetLatinFont(const char *path, int weight = 400);
    // Optional Unicode symbol face; an empty path disables it. Missing
    // symbols keep the normal Latin/CJK selection and native bitmap fallback.
    bool SetSymbolFont(const char *path, int pixelSize = 13, int weight = 400);
    void SetLegacyCodepage1252(bool on);
    unsigned int RenderCodepoint(unsigned int cp) const;

    // Uniformly fit an outline into an explicit engine advance. Natural
    // line rendering passes -1 and retains the font's horizontal metrics.
    void SetFitToAdvance(bool on) { m_fit = on; }

    // Select grayscale or monochrome FreeType rasterization. Both populate
    // coverage and a thresholded 1bpp mask for the pixel writers.
    void SetAntiAlias(bool on);

    // A positive value enables autofitter no-stem-darkening=false at Init.
    // This is a Boolean request, not a numeric stroke-width adjustment;
    // its effect depends on the font/driver and selected load flags.
    void SetStemDarkening(int amount) { m_darkening = amount; }

    // Supersampling factor (1 = target-size AA, 2/4 = box-downsample).
    // Higher sampling improves outline fidelity, but hinting then uses the
    // larger raster grid rather than the final small-text pixel grid.
    void SetSupersample(int ss);

    // Grayscale grid fitting: 0=light (vertical), 1=normal, 2=unhinted.
    // With Supersample=1, the logical raster is hinted on its 1x grid;
    // the optional high-resolution face is hinted at its output density.
    void SetHinting(int mode);
    void SetHighResolution(bool on);
    void SetHighResolutionScale(int scale);

    bool Ready() const { return m_faceA != NULL; }
    void *FaceHandle() const { return m_faceA; } // Latin face
    void *FaceHandleFor(unsigned int codepoint) const;

    // gameAdvance: explicit integer compatibility advance (native or scaled).
    // Pass -1 for the vector font's natural advance (Metrics=vector or row
    // rendering). scale1024 uniformly scales a row raster, in 1/1024 units.
    // Returns NULL if the codepoint has no glyph.
    // phase = signed horizontal subpixel shift in quarter pixels (-3..3); it is
    // baked into the rasterised coverage, so the engine can keep drawing at
    // integer positions while the ink lands on a fractional pen position.
    // Direct pixel drawing preserves vertical bearings for every script,
    // even when horizontal fitting uses an explicit legacy advance.
    const GlyphCell *Get(unsigned int codepoint, int gameAdvance, int phase = 0, int scale1024 = 1024,
                         int verticalPhase = 0);
    // Align a Latin run's cap-height centre to the CJK ideographic centre.
    // The entire run shifts together, preserving descenders and superscripts.
    int LatinCenterShiftQuarter(int scale1024 = 1024);
    bool IsCJK(unsigned int cp) const { return UsesCJKFace(RenderCodepoint(cp)); }
    int KerningQuarter(unsigned int left, unsigned int right);

    int Lines() const { return m_lines; }
    size_t CachedGlyphs() const { return m_cache.size(); }
    const int *StemDarkeningErrors() const { return m_darkErr; }

  private:
    bool UsesCJKFace(unsigned int codepoint) const;
    bool UsesSymbolFace(unsigned int codepoint) const;
    void *OpenFace(int pixelSize, const char *path, int weight);
    void CloseFace(void **face);
    void ClearCache();

    // Cache misses run these stages with m_cs held. FreeType types stay
    // in glyph/Raster.h rather than leaking into the public interface.
    struct RasterContext;
    bool Rasterize(unsigned int codepoint, int gameAdvance, int phase, int scale1024, int verticalPhase, unsigned long long key,
                   GlyphCell &cell);
    void FitOutline(unsigned int codepoint, int gameAdvance, int phase, int scale1024, RasterContext &context,
                    GlyphCell &cell);
    bool BuildCoverage(const RasterContext &context, int gameAdvance, GlyphCell &cell);
    void BuildHighResolution(unsigned int codepoint, int gameAdvance, unsigned long long key,
                             const RasterContext &context, GlyphCell &cell);

    void *m_lib;   // FT_Library
    void *m_faceA; // Latin / default size
    void *m_faceB; // CJK face (NULL when font and size match Latin)
    void *m_faceSymbol = nullptr;
    int m_sizeSymbol = 13;
    int m_sizeLatin;
    int m_sizeMain;
    unsigned int m_cjkFrom;
    int m_stride;
    int m_lines;
    int m_baseline;
    bool m_fit;
    bool m_aa;
    int m_darkening;
    int m_ss;
    int m_hinting;
    int m_darkErr[3];
    const char *m_path;
    char m_latinPath[MAX_PATH];
    char m_symbolPath[MAX_PATH] = {};
    int m_weight;
    int m_weightLatin = 400;
    int m_weightSymbol = 400;

    CRITICAL_SECTION m_cs;
    bool m_csInit;
    std::map<unsigned long long, GlyphCell> m_cache;
    std::map<unsigned long long, GlyphRaster2> m_highCache;
    void *m_highA = nullptr;
    void *m_highB = nullptr;
    void *m_highSymbol = nullptr;
    bool m_highResolution = false;
    int m_highScale = 2;
    bool m_legacy1252 = false;
    bool m_centerReady = false;
    double m_centerShift = 0;
};
} // namespace vt
