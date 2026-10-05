#include "Takeover.h"
#include "GlyphSource.h"
#include "PixelWriter.h"
#include "Logger.h"

#include <string.h>

namespace vt
{
    namespace Takeover
    {
        // ------------------------------------------------------------ state --
        static GlyphSource   g_src;
        static bool          g_tried = false;
        static bool          g_ready = false;
        static unsigned long long g_drawn = 0, g_skipped = 0, g_failed = 0, g_unknown = 0;

        // Offsets verified by disassembling BitFont::Blit / BitFont::Lock:
        //   BitFont   +0x04 InternalData*   +0x0C locked buffer   +0x10 pitch(px)
        //             +0x24 colour word     +0x30..0x3C bounds L,T,R,B
        //   Internal  +0x08 lines           +0x14 symbolBytes
        //             +0x18 symbol table    +0x1C glyph bitmaps
        static const int BF_INTERNAL   = 0x04;
        static const int BF_BUFFER     = 0x0C;
        static const int BF_PITCH      = 0x10;
        static const int BF_COLOR      = 0x24;
        static const int BF_BOUNDS     = 0x30;

        static const int IF_LINES       = 0x08;
        static const int IF_SYMBOLBYTES = 0x14;
        static const int IF_SYMTABLE    = 0x18;
        static const int IF_BITMAPS     = 0x1C;

        bool Ready() { return g_ready; }

        bool Init()
        {
            if (g_tried)
                return g_ready;
            g_tried = true;

            const char* ttf = Cfg::FontFile();
            g_src.SetFitToAdvance(Cfg::FitToAdvance());
            g_src.SetAntiAlias(Cfg::AntiAlias());

            const DWORD t0 = GetTickCount();

            // the game's font cell is 3 bytes x 16 rows (game.fnt header)
            if (!g_src.Init(ttf, Cfg::FontSizeLatin(), Cfg::FontWeight(), 3, 16, Cfg::BaselineRow()))
            {
                Log::Note("FALLBACK font-load-failed file=\"%s\"", ttf);
                g_ready = false;
                return false;
            }
            g_src.SetSizes(Cfg::FontSizeLatin(), Cfg::FontSizeCJK());
            g_ready = true;

            Log::Note("M1 font ready in %u ms: \"%s\" latin=%dpx cjk=%dpx wght=%d baseline=%d fit=%d aa=%d",
                      GetTickCount() - t0, ttf, Cfg::FontSizeLatin(), Cfg::FontSizeCJK(),
                      Cfg::FontWeight(), Cfg::BaselineRow(),
                      Cfg::FitToAdvance() ? 1 : 0, Cfg::AntiAlias() ? 1 : 0);
            return true;
        }

        bool TryBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg, int* newX)
        {
            // lazy one-time font load; TryBlit is the only entry point the hook
            // uses, so the init lives here (and nowhere else can forget it)
            if (!g_tried)
                Init();

            if (!bitFont || !g_ready)
                return false;

            const unsigned char* bf = (const unsigned char*)bitFont;

            const unsigned char* internal = *(const unsigned char* const*)(bf + BF_INTERNAL);
            if (!internal)
                return false;

            void* base = *(void* const*)(bf + BF_BUFFER);          // set by BitFont::Lock
            const int pitch = *(const int*)(bf + BF_PITCH);
            if (!base || pitch <= 0)
                return false;

            const unsigned short* symTable = *(const unsigned short* const*)(internal + IF_SYMTABLE);
            const unsigned int symbolBytes = *(const unsigned int*)(internal + IF_SYMBOLBYTES);
            const unsigned char* bitmaps = *(const unsigned char* const*)(internal + IF_BITMAPS);
            const int lines = *(const int*)(internal + IF_LINES);

            if (!symTable || !bitmaps || symbolBytes == 0 || lines <= 0 || lines > 32)
                return false;

            // The glyph cell we rasterise is built for the game font's metrics.
            // A different bitmap font (other line count) is handed back to the
            // engine rather than drawn with the wrong baseline.
            if (lines != g_src.Lines())
            {
                ++g_failed;
                return false;
            }

            const unsigned int idx = symTable[ch & 0xFFFF];
            if (idx == 0)
            {
                // no such glyph in the game's font: let the engine deal with it
                ++g_unknown;
                return false;
            }

            const unsigned char* glyph = bitmaps + (size_t)(idx - 1) * symbolBytes;
            const int advance = glyph[0];                            // original advance

            const GlyphCell* cell = g_src.Get(ch, advance > 0 ? advance : -1);
            if (!cell)
            {
                ++g_failed;
                return false;
            }

            const unsigned short color = (colorArg == -1)
                ? *(const unsigned short*)(bf + BF_COLOR)
                : (unsigned short)colorArg;

            const int* bounds = (const int*)(bf + BF_BOUNDS);

            // BitFont::Lock guarantees a usable clip rectangle: if the caller
            // never called SetBounds, Lock fills it with (0, 0, width-1,
            // height-1) of the surface, otherwise it intersects the caller's
            // box with the surface.  So a degenerate box means "do not touch
            // anything" -> hand the glyph back to the engine.
            if (bounds[2] <= bounds[0] || bounds[3] <= bounds[1])
            {
                ++g_failed;
                return false;
            }

            Target t;
            t.base  = (unsigned short*)base;
            t.pitch = pitch;
            t.clipL = bounds[0] > 0 ? bounds[0] : 0;
            t.clipT = bounds[1] > 0 ? bounds[1] : 0;
            t.clipR = bounds[2] < pitch - 1 ? bounds[2] : pitch - 1;
            t.clipB = bounds[3];
            if (t.clipR < t.clipL || t.clipB < t.clipT)
            {
                ++g_failed;
                return false;
            }

            if (Cfg::AntiAlias())
                DrawCellAA(t, *cell, x, y, lines, color, RGB565);
            else
                DrawCell(t, *cell, x, y, lines, color);

            ++g_drawn;
            if (newX)
                *newX = x + advance;
            return true;
        }

        void Stats(unsigned long long* drawn, unsigned long long* skipped,
                   unsigned long long* failed, unsigned long long* unknown)
        {
            if (drawn)   *drawn = g_drawn;
            if (skipped) *skipped = g_skipped;
            if (failed)  *failed = g_failed;
            if (unknown) *unknown = g_unknown;
        }
    }
}
