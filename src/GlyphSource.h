#pragma once
#include <windows.h>
#include <map>

// ===========================================================================
//  GlyphSource -- turns codepoints into glyph cells in *the engine's* format.
//
//  Engine format (verified against game.fnt and BitFont::Blit):
//      byte 0            : advance width in pixels
//      strideBytes*lines : 1bpp rows, MSB first, 1 = ink
//
//  M1 draws these cells itself (plan B); the same layout is what the engine's
//  own Blit consumes, so swapping the source later is a one-line change.
//
//  Two faces are kept open when the Latin and CJK pixel sizes differ (the
//  game's own font is two sizes as well): calling FT_Set_Pixel_Sizes per glyph
//  would flush FreeType's caches on every script switch and cost real frames.
// ===========================================================================

namespace vt
{
    struct GlyphCell
    {
        unsigned char width;                 // engine advance (pixels)
        unsigned char bits[3 * 32];          // strideBytes * lines, <= 96 bytes
        unsigned char cov[24 * 32];          // 8-bit coverage (only filled in AA mode)
    };

    class GlyphSource
    {
    public:
        GlyphSource();
        ~GlyphSource();

        // strideBytes/lines must match the game's font (game.fnt: 3 / 16).
        // baselineRow is the row inside the cell that sits on the text baseline;
        // the engine's own 'A' occupies rows 4..12 of a 16 row cell, which puts
        // the baseline at row 13.
        bool Init(const char* ttfPath, int pixelSize, int weight,
                  int strideBytes = 3, int lines = 16, int baselineRow = 13);
        void Shutdown();

        // The game's own font is not one size: Latin ink is ~9 rows tall while
        // CJK fills the whole 16-row cell.  Matching that needs two sizes.
        // cjkFrom defaults to U+2E80 (CJK radicals supplement).
        void SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom = 0x2E80);
        // Optional Latin face chosen for the game's narrow ASCII advances.
        // The CJK face stays on FontFile; no character proportions are changed.
        bool SetLatinFont(const char* path);

        // When set, a glyph whose ink is wider than the engine advance is
        // condensed horizontally to fit its cell (keeps layout identical and
        // stops neighbouring CJK glyphs from touching).
        void SetFitToAdvance(bool on) { m_fit = on; }

        // AA mode also fills GlyphCell::cov with 8-bit coverage, which lets the
        // pixel writer blend into the game's 16-bit surfaces.
        void SetAntiAlias(bool on);

        // FreeType's stem darkening: thickens stems during rendering, which is
        // the intended cure for antialiased text looking too light at small sizes.
        void SetStemDarkening(int amount) { m_darkening = amount; }

        // Supersampling factor (1 = off, 2 = rasterise at 2x and box-downsample).
        // More accurate coverage at small sizes: cleaner edges, better weight.
        void SetSupersample(int ss);

        // Fitting preserves proportions and happens on the outline before
        // rendering. Kept for compatibility with older INI files/tests.
        void SetFitMode(int mode) { m_fitMode = mode; }

        // Legacy switch retained for callers. Glyphs share the font baseline;
        // individual characters are never stretched or moved by their ink box.
        void SetClassAlign(bool on) { m_classAlign = on; }

        bool Ready() const { return m_faceA != NULL; }
        bool AntiAlias() const { return m_aa; }
        void* LibHandle() const { return m_lib; }         // FT_Library, for diagnostics
        void* FaceHandle() const { return m_faceA; }      // Latin face
        void* FaceHandleFor(unsigned int codepoint) const
        {
            return (UsesCJKFace(codepoint) && m_faceB) ? m_faceB : m_faceA;
        }

        // gameAdvance: the advance taken from the game's font (Metrics=game).
        // Pass -1 to use the vector font's own advance (Metrics=freetype).
        // Returns NULL if the codepoint has no glyph.
        // phase = signed horizontal subpixel shift in quarter pixels (-3..3); it is
        // baked into the rasterised coverage, so the engine can keep drawing at
        // integer positions while the ink lands on a fractional pen position.
        const GlyphCell* Get(unsigned int codepoint, int gameAdvance, int phase = 0);

        int  StrideBytes() const { return m_stride; }
        int  Lines() const { return m_lines; }
        int  BaselineRow() const { return m_baseline; }
        void SetBaselineRow(int row) { m_baseline = row; }
        size_t CachedGlyphs() const { return m_cache.size(); }

        void SetAAInternal(bool on) { m_aa = on; }        // used by SetAntiAlias

    private:
        bool UsesCJKFace(unsigned int codepoint) const;
        void* OpenFace(int pixelSize, const char* path = NULL);
        void  CloseFace(void** face);

        void* m_lib;                          // FT_Library
        void* m_faceA;                        // Latin / default size
        void* m_faceB;                        // CJK size (NULL when identical)
        int   m_size;
        int   m_sizeLatin;
        int   m_sizeCJK;
        unsigned int m_cjkFrom;
        int   m_stride;
        int   m_lines;
        int   m_baseline;
        bool  m_fit;
        bool  m_aa;
        int   m_darkening;
        int   m_ss;
        int   m_fitMode;
        int   m_darkErr[3];
        bool  m_classAlign;
    public:
        const int* StemDarkeningErrors() const { return m_darkErr; }
    private:
        const char* m_path;
        char  m_latinPath[MAX_PATH];
        int   m_weight;

        CRITICAL_SECTION m_cs;
        bool  m_csInit;
        std::map<unsigned long long, GlyphCell> m_cache;
    };
}
