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
// ===========================================================================

namespace vt
{
    struct GlyphCell
    {
        unsigned char width;                 // engine advance (pixels)
        unsigned char bits[3 * 32];          // strideBytes * lines, <= 96 bytes
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

        // When set, a glyph whose ink is wider than the engine advance is
        // condensed horizontally to fit its cell (keeps layout identical and
        // stops neighbouring CJK glyphs from touching).
        void SetFitToAdvance(bool on) { m_fit = on; }

        // The game's own font is not one size: Latin ink is ~9 rows tall while
        // CJK fills the whole 16-row cell.  Matching that needs two sizes.
        // cjkFrom defaults to U+2E80 (CJK radicals supplement).
        void SetSizes(int latinPx, int cjkPx, unsigned int cjkFrom = 0x2E80);

        bool Ready() const { return m_face != NULL; }
        void* LibHandle() const { return m_lib; }         // FT_Library, for diagnostics
        void* FaceHandle() const { return m_face; }       // FT_Face

        // gameAdvance: the advance taken from the game's font (Metrics=game).
        // Pass -1 to use the vector font's own advance (Metrics=freetype).
        // Returns NULL if the codepoint has no glyph.
        const GlyphCell* Get(unsigned int codepoint, int gameAdvance);

        int  StrideBytes() const { return m_stride; }
        int  Lines() const { return m_lines; }
        int  BaselineRow() const { return m_baseline; }
        void SetBaselineRow(int row) { m_baseline = row; }
        size_t CachedGlyphs() const { return m_cache.size(); }

    private:
        void* m_lib;                          // FT_Library
        void* m_face;                         // FT_Face
        int   m_size;
        int   m_sizeLatin;
        int   m_sizeCJK;
        unsigned int m_cjkFrom;
        int   m_currentSize;
        int   m_stride;
        int   m_lines;
        int   m_baseline;
        bool  m_fit;
        std::map<unsigned int, GlyphCell> m_cache;
    };
}
