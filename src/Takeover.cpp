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
        static bool          g_vecMetrics = false;   // Metrics=vector
        static double        g_advScale = 1.0;       // Metrics=scaled
        static bool          g_subpixel = true;      // Subpixel=1

        // ---- subpixel pen -------------------------------------------------
        // The engine only ever deals in integer pen positions, so we keep the
        // fractional part ourselves: the advance we publish is rounded, and the
        // rounding error is accumulated here and baked into the next glyph as a
        // quarter-pixel rasterisation phase.  Net effect: the ink lands on the
        // ideal fractional positions while the engine sees a normal integer run.
        static __declspec(thread) double t_ideal = 0.0;
        static __declspec(thread) bool   t_hasIdeal = false;

        // ---- per-caller metrics -------------------------------------------
        // A few fixed-size UI boxes were laid out for the bitmap font and cannot
        // take wider text (the mission-objectives panel is the only one the
        // offline audit found).  The caller is recognised by walking the stack
        // for a code address near a configured one: a UI function that measures
        // and then draws has *different* addresses for the two calls, so the
        // match is by neighbourhood (~1 KB) rather than by exact address.
        static bool CallerWantsGameMetrics(unsigned int esp)
        {
            const int n = Cfg::MetricsExceptCount();
            if (!n || !esp)
                return false;

            // the stack is plain memory; a bad read is caught by the caller
            const unsigned int* p = (const unsigned int*)(uintptr_t)esp;
            for (int i = 0; i < 24; ++i)
            {
                const unsigned int v = p[i];
                if (v < 0x00401000u || v > 0x00700000u)
                    continue;                        // not a code address
                for (int k = 0; k < n; ++k)
                {
                    const unsigned int e = Cfg::MetricsExceptAt(k);
                    if (v >= e && v < e + 0x400u)
                        return true;
                }
            }
            return false;
        }

        static int SubpixelPhase(int x, double trueAdvance, int* published)
        {
            int rounded = (int)floor(trueAdvance + 0.5);
            if (rounded < 1) rounded = 1;        // 0 would freeze the pen
            if (published)
                *published = rounded;
            if (!g_subpixel)
                return 0;

            if (!t_hasIdeal || fabs(t_ideal - (double)x) > 2.0)
            {
                t_ideal = (double)x;                 // new run: anchor on the engine
                t_hasIdeal = true;
            }
            double frac = t_ideal - (double)x;       // our ideal pen vs the engine
            if (frac < -0.5) frac = -0.5;            // keep it in half a pixel
            if (frac >  0.5) frac =  0.5;
            int phase = (int)floor(frac * 4.0 + 0.5);   // SIGNED quarter pixels
            if (phase < -2) phase = -2;                 // drop the integer part:
            if (phase >  2) phase =  2;                 // |shift| stays <= 0.5 px
            t_ideal += trueAdvance;
            return phase;
        }
        static bool          g_ready = false;
        static unsigned long long g_drawn = 0, g_skipped = 0, g_failed = 0, g_unknown = 0;

        // why we handed a glyph back to the engine (all zero in a healthy run)
        enum Reason
        {
            R_NotReady = 0, R_NoInternal, R_NotLocked, R_BadMetrics, R_FontMetrics,
            R_NoGlyph, R_NoVectorGlyph, R_BadBounds, R_Exception, R_Count
        };
        static const char* const kReasonNames[R_Count] =
        {
            "font-not-ready", "no-internal-data", "surface-not-locked", "bad-font-data",
            "font-metrics-mismatch", "no-game-glyph", "no-vector-glyph", "bad-bounds", "exception"
        };
        static unsigned long long g_reason[R_Count] = { 0 };
        static char g_reasonLine[512] = { 0 };

        // ---- diagnostics (they survive a hard crash via the FINAL log line) --
        static int  g_stage = 0;
        static bool g_sawDF = false;
        static int  g_skipLogged = 0;
        static const char* const kStageNames[] =
        {
            "start", "probe", "init", "fontdata", "bounds", "rasterised",
            "written", "skip-computed", "skipped", "returned"
        };
        static const int kStageCount = (int)(sizeof(kStageNames) / sizeof(kStageNames[0]));

        const char* StageName()
        {
            if (g_stage < 0 || g_stage >= kStageCount)
                return "?";
            return kStageNames[g_stage];
        }

        void SetStage(int stage) { g_stage = stage; }

        void NoteDirectionFlag()
        {
            if (!g_sawDF)
            {
                g_sawDF = true;
                Log::Note("WARNING direction flag (DF) was SET at hook entry; clearing it "
                          "(CRT memcpy/memset would otherwise write memory backwards)");
            }
        }

        // Syringe''s stub restores ESP with `popad`, which *ignores* the saved
        // ESP value -- so R->ESP(...) is silently dropped and a handler that
        // "returns the caller''s return address" leaves ESP 0x14 too low (that
        // was the in-game crash).  The stack has to be fixed up by us, from a
        // trampoline: entered with ESP at the return address, it does exactly
        // what `ret 0x10` would have done.
        //     8B 14 24   mov edx, [esp]      ; the real return address
        //     83 C4 14   add esp, 0x14       ; drop it + the 4 arguments
        //     52         push edx
        //     C3         ret                 ; -> ESP = entry + 0x14, EIP = caller
        bool TestCallerWantsGameMetrics(unsigned int esp) { return CallerWantsGameMetrics(esp); }

        void* SkipTrampoline()
        {
            static void* code = NULL;
            if (code)
                return code;

            void* mem = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!mem)
                return NULL;

            unsigned char* p = (unsigned char*)mem;
            const unsigned char body[] =
            {
                0x8B, 0x14, 0x24,        // mov edx, [esp]
                0x83, 0xC4, 0x14,        // add esp, 0x14
                0x52,                    // push edx
                0xC3                     // ret
            };
            memcpy(p, body, sizeof(body));
            FlushInstructionCache(GetCurrentProcess(), p, sizeof(body));
            code = mem;
            return code;
        }

        // The engine's own advance, remembered the first time a glyph slot is
        // seen.  Without this, publishing our advance back into the font data
        // fed our own value into the next call, so measurement, cache and
        // drawing disagreed (overlapping Latin, cramped punctuation).
        static unsigned char g_origAdv[16384];
        static bool          g_origSeen[16384];

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

        bool SawDirectionFlag() { return g_sawDF; }

        void DiagLine(char* out, int cch)
        {
            _snprintf_s(out, (size_t)cch, _TRUNCATE,
                        "lastStage=%s(%d) sawDF=%d skipped=%d",
                        StageName(), g_stage, g_sawDF ? 1 : 0, g_skipLogged);
        }

        // Log the first few skip-the-callee operations in full, so a crash in
        // that area is diagnosable from the log alone.
        void LogSkip(DWORD entryEsp, DWORD retAddr, DWORD newEsp, int newX, unsigned int ch)
        {
            ++g_skipLogged;
            if (g_skipLogged > 3)
                return;
            Log::Note("SKIP #%d ch=U+%04X entryESP=0x%08X retAddr=0x%08X newESP=0x%08X "
                      "(delta=0x%X) newX=%d",
                      g_skipLogged, ch, entryEsp, retAddr, newEsp, newEsp - entryEsp, newX);
        }

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

        namespace
        {
            const int kLineLimit = 2048;
            struct LineGlyph
            {
                unsigned int cp;
                int gameX, gameEnd, penQ, shiftQ, leftQ, rightQ, capacityQ;
                const unsigned char* bitmap; // missing vector glyph: retain the native icon
            };
            struct LineState
            {
                bool active;
                void* font;
                void* buffer;
                unsigned int caller;
                int y, pitch, bounds[4], count, consumed, widthQ, originQ, boxWidth, mixedAddedQ, tightenedQ, scale1024;
                LineGlyph glyphs[kLineLimit];
            };
            // Only POD in static TLS: no loader-time allocation or constructors.
            static __declspec(thread) LineState t_line = {};
            struct LineBox
            {
                bool valid;
                void* font;
                const wchar_t* text;
                int gameX, y, boxX, width, align;
            };
            static __declspec(thread) LineBox t_box = {};
            static LONG g_lineLogged = 0, g_lineRejected = 0, g_bitmapLineLogged = 0;

            bool OpeningMark(unsigned int cp)
            {
                return cp == 0xFF08 || cp == 0x3010 || cp == 0x300C || cp == 0x300E ||
                       cp == 0x2018 || cp == 0x201C;
            }
            bool CompactMark(unsigned int cp)
            {
                return OpeningMark(cp) || cp == 0xFF09 || cp == 0x3011 || cp == 0x300D ||
                       cp == 0x300F || cp == 0x2019 || cp == 0x201D || cp == 0x3001 ||
                       cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E || cp == 0xFF1A ||
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
            bool NaturalGlyph(LineGlyph& item, unsigned int previous, const LineGlyph* prior,
                              int& penQ, int scale1024, int& mixedAddedQ)
            {
                const GlyphCell* cell = g_src.Get(item.cp, -1, 0, scale1024);
                if (!cell || cell->advanceQ <= 0) return false;
                penQ += (int)floor(g_src.KerningQuarter(previous, item.cp) * scale1024 / 1024.0 + 0.5);
                int advanceQ = cell->advanceQ;
                if (CompactMark(item.cp))
                {
                    const int halfQ = Cfg::FontSizeCJK() * 2 * scale1024 / 1024;
                    const int inkQ = cell->inkRightQ - cell->inkLeftQ;
                    advanceQ = halfQ > inkQ + 2 ? halfQ : inkQ + 2;
                    const int leftQ = OpeningMark(item.cp) ? advanceQ - 1 - inkQ : 1;
                    item.shiftQ = leftQ - cell->inkLeftQ;
                }
                item.leftQ = cell->inkLeftQ + item.shiftQ;
                item.rightQ = cell->inkRightQ + item.shiftQ;
                if (prior && MixedBoundary(previous, item.cp))
                {
                    const int inkGapQ = penQ + item.leftQ - prior->penQ - prior->rightQ;
                    const int extraQ = Cfg::FontSizeCJK() * scale1024 / 1024 - inkGapQ;
                    if (extraQ > 0) { penQ += extraQ; mixedAddedQ += extraQ; }
                }
                item.penQ = penQ;
                penQ += advanceQ;
                return true;
            }
            bool BitmapGlyph(LineGlyph& item, const unsigned char* slot, unsigned int bytes,
                             int lines, int extra, int& penQ, int scale1024)
            {
                // An embedded game icon must not reject the entire vector row.
                // Native bitmaps have no scalable outline; retain them at 1:1.
                if (!slot || scale1024 != 1024 || lines != 16 || bytes != 49 ||
                    !slot[0] || slot[0] > 24) return false;
                int left = slot[0], right = 0;
                for (int y = 0; y < lines; ++y)
                    for (int x = 0; x < slot[0]; ++x)
                        if (slot[1 + y * 3 + x / 8] & (0x80 >> (x & 7)))
                        { if (x < left) left = x; if (x + 1 > right) right = x + 1; }
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
        }

        bool LineEnabled()
        {
            static const bool requested = Cfg::ConfigInt("LineRender", 0) != 0;
            return requested && Cfg::Mode() == Cfg::Mode_Draw &&
                   !Cfg::VectorMetrics() && Cfg::AdvanceScale() <= 1.0;
        }

        bool DynamicTextWidthEnabled()
        {
            static const bool requested = Cfg::ConfigInt("DynamicTextWidth", 1) != 0;
            return requested && LineEnabled();
        }

        bool MeasureDynamicWidth(void* bitFont, const wchar_t* text, int maxWidth, int* width)
        {
            if (!DynamicTextWidthEnabled() || !bitFont || !text || !width || maxWidth < 0 || maxWidth > 32768)
                return false;
            if (!g_tried) Init();
            if (!g_ready) return false;
            const unsigned char* bf = (const unsigned char*)bitFont;
            const unsigned char* fd = *(const unsigned char* const*)(bf + BF_INTERNAL);
            if (!fd || *(const int*)(fd + IF_LINES) != g_src.Lines()) return false;
            const unsigned short* map = *(const unsigned short* const*)(fd + IF_SYMTABLE);
            const unsigned char* data = *(const unsigned char* const*)(fd + IF_BITMAPS);
            const unsigned int bytes = *(const unsigned int*)(fd + IF_SYMBOLBYTES);
            const int extra = *(const int*)(bf + 0x2C);
            if (!map || !data || !bytes || bytes > 4096 || extra < 0 || extra > 16) return false;
            int penQ = 0, legacyWidth = 0, longestQ = 0, mixedQ = 0, count = 0;
            unsigned int previous = 0;
            LineGlyph prior = {};
            for (int i = 0; i <= kLineLimit; ++i)
            {
                const unsigned int cp = text[i];
                if (i == kLineLimit && cp) return false;
                if (!cp || cp == '\r' || cp == '\n')
                {
                    // Preserve the engine's automatic wrapping/height semantics.
                    // Only explicit lines fitting the old limit can resize a box.
                    if (maxWidth && legacyWidth > maxWidth) return false;
                    int rowQ = penQ;
                    if (prior.penQ + prior.rightQ > rowQ) rowQ = prior.penQ + prior.rightQ;
                    if (rowQ > longestQ) longestQ = rowQ;
                    if (!cp)
                    {
                        if (!count) return false;
                        int padding = Cfg::ConfigInt("LineWidthPadding", 4);
                        if (padding < 1) padding = 1;
                        if (padding > 32) padding = 32;
                        const int measured = (longestQ + 3) / 4 + padding;
                        if (measured > 32768 || (maxWidth && measured > maxWidth)) return false;
                        *width = measured;
                        return true;
                    }
                    penQ = legacyWidth = 0; previous = 0; prior = {};
                    continue;
                }
                if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || !map[cp]) return false;
                LineGlyph item = {}; item.cp = cp;
                if (!NaturalGlyph(item, previous, previous ? &prior : NULL, penQ, 1024, mixedQ) &&
                    !BitmapGlyph(item, data + (size_t)(map[cp] - 1) * bytes, bytes,
                                 g_src.Lines(), extra, penQ, 1024)) return false;
                legacyWidth += data[(size_t)(map[cp] - 1) * bytes] + extra;
                previous = item.bitmap ? 0 : cp; prior = item; ++count;
            }
            return false;
        }

        void EndLine(void* bitFont)
        {
            if (!bitFont || t_line.font == bitFont) t_line.active = false;
            if (!bitFont || t_box.font == bitFont) t_box.valid = false;
        }

        void SetLineBox(void* bitFont, const wchar_t* text, int gameX, int y,
                        int boxX, int width, int align)
        {
            t_box = { true, bitFont, text, gameX, y, boxX, width, align };
        }

        bool BeginStringLine(void* bitFont, const wchar_t* text, int count, int x, int y)
        {
            const LineBox box = t_box;
            t_box.valid = false;
            if (box.valid && box.font == bitFont && box.text == text && box.gameX == x && box.y == y)
                return BeginLine(bitFont, text, count, x, y, box.boxX, box.width, box.align, 0x0043464Du);
            return BeginLine(bitFont, text, count, x, y, x, 0, 0, 0x0043464Du);
        }

        bool GetLineInfo(LineInfo* info)
        {
            if (!t_line.active || !info) return false;
            info->count = t_line.count; info->widthQ = t_line.widthQ;
            info->originQ = t_line.originQ; info->consumed = t_line.consumed;
            info->boxWidth = t_line.boxWidth; info->mixedAddedQ = t_line.mixedAddedQ;
            info->tightenedQ = t_line.tightenedQ;
            info->scale1024 = t_line.scale1024;
            return true;
        }

        bool BeginLine(void* bitFont, const wchar_t* text, int count, int gameX,
                       int y, int boxX, int width, int align, unsigned int blitCaller, int scale1024)
        {
            t_line.active = false;
            if (!LineEnabled() || !bitFont || !text || count < -1 || count > kLineLimit || scale1024 < 922 || scale1024 > 1024 ||
                width < 0 || width > 32768 || gameX < -32768 || gameX > 32768 ||
                boxX < -32768 || boxX > 32768 || y < -32768 || y > 32768) return false;
            if (!g_tried) Init();
            if (!g_ready) return false;
            const unsigned char* bf = (const unsigned char*)bitFont;
            const unsigned char* fd = *(const unsigned char* const*)(bf + BF_INTERNAL);
            void* base = *(void* const*)(bf + BF_BUFFER);
            const int pitch = *(const int*)(bf + BF_PITCH);
            const int* bounds = (const int*)(bf + BF_BOUNDS);
            if (!fd || !base || pitch <= 0 || pitch > 32768 ||
                *(const int*)(fd + IF_LINES) != g_src.Lines() ||
                bounds[0] > bounds[2] || bounds[1] > bounds[3]) return false;
            const int clipRight = bounds[2] < pitch - 1 ? bounds[2] : pitch - 1;
            if (!width)
            {
                // Ordinary DrawString inherits the verified BitFont clip box.
                // This is a real available width, not a guessed text length.
                boxX = gameX; width = clipRight - gameX + 1; align = 0;
            }
            else if (boxX + width > clipRight + 1)
                width = clipRight - boxX + 1;
            if (width <= 0 || width > 65536) return false;
            const unsigned short* map = *(const unsigned short* const*)(fd + IF_SYMTABLE);
            const unsigned char* data = *(const unsigned char* const*)(fd + IF_BITMAPS);
            const unsigned int bytes = *(const unsigned int*)(fd + IF_SYMBOLBYTES);
            const int extra = *(const int*)(bf + 0x2C);
            const int tab = *(const int*)(bf + 0x28);
            const int tabOrigin = *(const int*)(bf + 0x20);
            if (!map || !data || bytes < 1 || bytes > 4096 || extra < 0 || extra > 16) return false;
            t_line.font = bitFont; t_line.buffer = base; t_line.pitch = pitch;
            memcpy(t_line.bounds, bounds, sizeof(t_line.bounds));
            t_line.caller = blitCaller; t_line.y = y;
            t_line.count = t_line.consumed = 0;
            t_line.boxWidth = width; t_line.mixedAddedQ = t_line.tightenedQ = 0;
            t_line.scale1024 = scale1024;
            int legacyX = gameX, penQ = 0;
            unsigned int previous = 0;
            int i = 0;
            for (; i < kLineLimit && (count < 0 || i < count); ++i)
            {
                const unsigned int cp = text[i];
                if (!cp) break;
                if (cp == '\r' || cp == '\n') continue;
                LineGlyph& item = t_line.glyphs[t_line.count++];
                memset(&item, 0, sizeof(item));
                item.cp = cp; item.gameX = legacyX;
                if (cp == '\t')
                {
                    if (tab <= 0 || tab > 32768) return false;
                    // Centre/right alignment changes the starting pen; rather
                    // than guess a different tab grid, retain the legacy row.
                    if (width > 0 && (align & 3)) return false;
                    legacyX = TabEnd(legacyX, tabOrigin, tab);
                    item.penQ = penQ;
                    penQ = TabEnd(penQ, (tabOrigin - gameX) * 4, tab * 4);
                    item.leftQ = item.rightQ = 0;
                    previous = 0;
                }
                else
                {
                    if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF) || !map[cp]) return false;
                    legacyX += data[(size_t)(map[cp] - 1) * bytes] + extra;
                    const LineGlyph* prior = t_line.count > 1 ? &t_line.glyphs[t_line.count - 2] : NULL;
                    if (!NaturalGlyph(item, previous, prior, penQ, scale1024, t_line.mixedAddedQ) &&
                        !BitmapGlyph(item, data + (size_t)(map[cp] - 1) * bytes, bytes,
                                     g_src.Lines(), extra, penQ, scale1024)) return false;
                    previous = item.bitmap ? 0 : cp;
                }
                item.gameEnd = legacyX;
            }
            if (count < 0 && i == kLineLimit && text[i]) return false;
            if (!t_line.count) return false;
            int widthQ = penQ;
            LineGlyph& last = t_line.glyphs[t_line.count - 1];
            if (last.penQ + last.rightQ > widthQ) widthQ = last.penQ + last.rightQ;
            if (width > 0 && widthQ > width * 4)
            {
                // Tighten only existing inter-glyph whitespace, keeping >=0.5px
                // between ink boxes. If that cannot fit, reject BEFORE painting.
                int totalQ = 0;
                for (int j = 1; j < t_line.count; ++j)
                {
                    LineGlyph& a = t_line.glyphs[j - 1]; LineGlyph& b = t_line.glyphs[j];
                    if (a.cp == '\t' || b.cp == '\t') continue;
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
                    LineGlyph& b = t_line.glyphs[j];
                    int dropQ = remainingCapacityQ ? remainingQ * b.capacityQ / remainingCapacityQ : 0;
                    remainingQ -= dropQ; remainingCapacityQ -= b.capacityQ;
                    shiftQ += dropQ; b.penQ -= shiftQ;
                }
                widthQ -= neededQ;
                t_line.tightenedQ = neededQ;
            }
            int offsetQ = 0;
            if (width > 0 && width * 4 > widthQ)
            {
                if (align & 1) offsetQ = (width * 4 - widthQ) / 2;
                else if (align & 2) offsetQ = width * 4 - widthQ;
            }
            t_line.originQ = (width > 0 ? boxX : gameX) * 4 + offsetQ;
            t_line.widthQ = widthQ;
            // Warm every final phase before accepting the line. An unsupported
            // raster cannot fail halfway through a run already being painted.
            for (int j = 0; j < t_line.count; ++j)
            {
                const LineGlyph& item = t_line.glyphs[j];
                if (item.cp == '\t' || item.bitmap) continue;
                const int q = t_line.originQ + item.penQ + item.shiftQ;
                const int x = (int)floor(q / 4.0);
                if (!g_src.Get(item.cp, -1, q - x * 4, scale1024)) return false;
            }
            t_line.active = true;
            for (int j = 0; j < t_line.count; ++j)
                if (t_line.glyphs[j].bitmap)
                {
                    if (InterlockedIncrement(&g_bitmapLineLogged) <= 3)
                        Log::Note("LINE native icon: U+%04X advance=%dpx row=%d/4px count=%d x=%d/4px y=%d",
                                  t_line.glyphs[j].cp, t_line.glyphs[j].bitmap[0] + extra,
                                  widthQ, t_line.count, t_line.originQ, y);
                    break;
                }
            if (InterlockedIncrement(&g_lineLogged) <= 3)
                Log::Note("LINE ready: count=%d width=%d/4px box=%dpx scale=%d/1024 mixed-gap=%d/4px tightened=%d/4px "
                          "x=%d/4px y=%d caller=0x%08X",
                          t_line.count, widthQ, width, scale1024, t_line.mixedAddedQ, t_line.tightenedQ,
                          t_line.originQ, y, blitCaller);
            return true;
        }

        bool TryLineBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg,
                         unsigned int blitCaller, int* newX)
        {
            if (!t_line.active || t_line.font != bitFont || t_line.caller != blitCaller) return false;
            const unsigned char* bf = (const unsigned char*)bitFont;
            if (t_line.consumed >= t_line.count || t_line.y != y ||
                t_line.buffer != *(void* const*)(bf + BF_BUFFER) ||
                t_line.pitch != *(const int*)(bf + BF_PITCH) ||
                memcmp(t_line.bounds, bf + BF_BOUNDS, sizeof(t_line.bounds)))
            { t_line.active = false; return false; }
            const LineGlyph& item = t_line.glyphs[t_line.consumed];
            if (item.cp != ch || item.gameX != x) { t_line.active = false; return false; }
            if (ch != '\t')
            {
                const int q = t_line.originQ + item.penQ + item.shiftQ;
                const int drawX = (int)floor(q / 4.0);
                const int phase = q - drawX * 4;
                GlyphCell native;
                const GlyphCell* cell;
                if (item.bitmap)
                {
                    memset(&native, 0, sizeof(native));
                    native.width = item.bitmap[0]; native.inkRows = g_src.Lines();
                    memcpy(native.bits, item.bitmap + 1, 3 * native.inkRows);
                    for (int row = 0; row < native.inkRows; ++row)
                        for (int col = 0; col < native.width; ++col)
                            if (native.bits[row * 3 + col / 8] & (0x80 >> (col & 7)))
                                native.cov[row * 24 + col] = 255;
                    cell = &native;
                }
                else cell = g_src.Get(ch, -1, phase, t_line.scale1024);
                if (!cell) { t_line.active = false; return false; }
                Target target = { (unsigned short*)t_line.buffer, t_line.pitch,
                    t_line.bounds[0] > 0 ? t_line.bounds[0] : 0,
                    t_line.bounds[1] > 0 ? t_line.bounds[1] : 0,
                    t_line.bounds[2] < t_line.pitch - 1 ? t_line.bounds[2] : t_line.pitch - 1,
                    t_line.bounds[3] };
                const unsigned short color = colorArg == -1 ? *(const unsigned short*)(bf + BF_COLOR) : (unsigned short)colorArg;
                if (Cfg::AntiAlias()) DrawCellAA(target, *cell, drawX, y, g_src.Lines(), color, RGB565);
                else DrawCell(target, *cell, drawX, y, g_src.Lines(), color);
                ++g_drawn;
            }
            if (newX) *newX = item.gameEnd;
            ++t_line.consumed;
            return true;
        }

        bool Init()
        {
            if (g_tried)
                return g_ready;
            g_tried = true;

            const char* ttf = Cfg::FontFile();
            g_vecMetrics = Cfg::VectorMetrics();
            g_advScale = Cfg::AdvanceScale();
            g_subpixel = Cfg::ConfigInt("Subpixel", 1) != 0;
            Log::Note("M1 metrics: mode=%d advanceScale=%.3f subpixel=%d supersample=%d dropout-exceptions=%d",
                      (int)Cfg::Mode(), g_advScale, g_subpixel ? 1 : 0, Cfg::Supersample(), Cfg::MetricsExceptCount());
            // natural metrics means no horizontal condensing: the glyph keeps
            // its own width and we hand that width to the engine (see below)
            g_src.SetFitToAdvance(!g_vecMetrics && Cfg::FitToAdvance());

            // Swap mode feeds the engine's 1bpp pipeline: the cell MUST be
            // rasterised monochrome.  Thresholding grayscale coverage at 128
            // hollows out thin serif strokes (seen in-game), because most edge
            // pixels of a 13 px serif glyph sit below 50% coverage.
            const bool useAA = Cfg::AntiAlias() && (Cfg::Mode() == Cfg::Mode_Draw || Cfg::Mode() == Cfg::Mode_AA);
            g_src.SetAntiAlias(useAA);
            g_src.SetStemDarkening(Cfg::StemDarkening());
            g_src.SetSupersample(Cfg::Supersample());
            g_src.SetHinting(Cfg::ConfigInt("Hinting", 0));
            g_src.SetClassAlign(Cfg::ConfigInt("ClassAlign", 1) != 0);
            g_src.SetFitMode(Cfg::ConfigInt("FitMode", 0) != 0 ? 1 : 0);
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
            static char latinFont[MAX_PATH] = { 0 };
            Cfg::ConfigStr("FontFileLatin", "", latinFont, sizeof(latinFont));
            if (latinFont[0])
                Log::Note("Latin font %s: \"%s\"", g_src.SetLatinFont(latinFont) ? "ready" : "fallback", latinFont);
            g_ready = true;
            SetStage(2);

            Log::Note("M1 font ready in %u ms: \"%s\" latin=%dpx cjk=%dpx wght=%d baseline=%d fit=%d aa=%d hinting=%d",
                      GetTickCount() - t0, ttf, Cfg::FontSizeLatin(), Cfg::FontSizeCJK(),
                      Cfg::FontWeight(), Cfg::BaselineRow(),
                      Cfg::FitToAdvance() ? 1 : 0, useAA ? 1 : 0, Cfg::ConfigInt("Hinting", 0));
            return true;
        }

        bool TryBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg, int* newX)
        {
            // lazy one-time font load; TryBlit is the only entry point the hook
            // uses, so the init lives here (and nowhere else can forget it)
            if (!g_tried)
                Init();

            if (!bitFont || !g_ready)
            {
                ++g_reason[R_NotReady];
                return false;
            }

            const unsigned char* bf = (const unsigned char*)bitFont;

            const unsigned char* internal = *(const unsigned char* const*)(bf + BF_INTERNAL);
            if (!internal)
            {
                ++g_reason[R_NoInternal];
                return false;
            }

            void* base = *(void* const*)(bf + BF_BUFFER);          // set by BitFont::Lock
            const int pitch = *(const int*)(bf + BF_PITCH);
            if (!base || pitch <= 0)
            {
                ++g_reason[R_NotLocked];
                return false;
            }

            const unsigned short* symTable = *(const unsigned short* const*)(internal + IF_SYMTABLE);
            const unsigned int symbolBytes = *(const unsigned int*)(internal + IF_SYMBOLBYTES);
            const unsigned char* bitmaps = *(const unsigned char* const*)(internal + IF_BITMAPS);
            const int lines = *(const int*)(internal + IF_LINES);

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

            unsigned char* slot0 = (unsigned char*)(bitmaps + (size_t)(idx - 1) * symbolBytes);
            const unsigned char* glyph = slot0;
            // Same metrics logic as the antialiased path: this mode used to read
            // the raw advance byte only, so Metrics=scaled/vector had no effect
            // here and the glyphs were condensed harder than in aa mode.
            // callerEsp is not available in this path (no hook stack), so the
            // exception table is applied by the caller of TryBlit via `x`.
            const int gameAdvance = OrigAdvance(idx, glyph[0]);
            const double trueAdv = (g_advScale > 1.0) ? gameAdvance * g_advScale : (double)gameAdvance;
            int published = gameAdvance;
            const int phase = SubpixelPhase(x, trueAdv, &published);
            const int advance = g_vecMetrics ? -1 : published;

            const GlyphCell* cell = g_src.Get(ch, advance > 0 ? advance : -1, phase);
            if (!cell)
            {
                ++g_failed;
                ++g_reason[R_NoVectorGlyph];
                return false;
            }

            // the engine measures text by summing these bytes, so publishing our
            // advance keeps measuring and drawing consistent
            if (slot0 && *slot0 != cell->width)
                *slot0 = cell->width;

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
                ++g_reason[R_BadBounds];
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
                ++g_reason[R_BadBounds];
                return false;
            }

            if (Cfg::AntiAlias())
                DrawCellAA(t, *cell, x, y, lines, color, RGB565);
            else
                DrawCell(t, *cell, x, y, lines, color);
            SetStage(6);                      // pixels written


            ++g_drawn;
            if (newX)
                *newX = x + cell->width;
            return true;
        }

        // ------------------------------------------------------------------
        //  Mode=swap: replace the glyph *data* in the engine's own font tables.
        //
        //  The bytes we write are exactly the bytes the engine reads: one width
        //  byte followed by strideBytes*lines 1bpp rows.  So the engine's Blit
        //  draws our vector glyph with its own addressing, clipping, colour,
        //  shadow and reveal logic -- and we never touch the stack or the
        //  instruction pointer.  Idempotent: the slot is compared first, which
        //  also makes it survive a font reload.
        // ------------------------------------------------------------------
        bool SwapGlyph(void* bitFont, unsigned int ch)
        {
            if (!g_tried)
                Init();
            if (!bitFont || !g_ready)
                return false;

            const unsigned char* bf = (const unsigned char*)bitFont;
            const unsigned char* internal = *(const unsigned char* const*)(bf + BF_INTERNAL);
            if (!internal)
                return false;

            const unsigned short* symTable = *(const unsigned short* const*)(internal + IF_SYMTABLE);
            const unsigned int symbolBytes = *(const unsigned int*)(internal + IF_SYMBOLBYTES);
            unsigned char* bitmaps = *(unsigned char* const*)(internal + IF_BITMAPS);
            const int lines = *(const int*)(internal + IF_LINES);

            if (!symTable || !bitmaps || lines != g_src.Lines())
                return false;

            // the slot must hold exactly: width byte + stride*lines bitmap bytes
            if (symbolBytes != 1 + (unsigned int)(g_src.StrideBytes() * lines))
                return false;

            const unsigned int idx = symTable[ch & 0xFFFF];
            if (idx == 0)
                return false;

            unsigned char* slot = bitmaps + (size_t)(idx - 1) * symbolBytes;

            // Metrics=game  : keep the engine's advance (layout byte-identical)
            // Metrics=vector: use FreeType's advance and write it back, so the
            //                 engine's drawing AND measuring follow our metrics
            // Swap mode never sees the pen position (the engine draws the cell
            // itself), so no subpixel phase is possible here
            const int gameAdvance = OrigAdvance(idx, slot[0]);   // the engine's own advance (remembered)
            const int advance = g_vecMetrics ? -1
                              : (g_advScale > 1.0 ? (int)(gameAdvance * g_advScale + 0.5) : gameAdvance);
            const GlyphCell* cell = g_src.Get(ch, advance > 0 ? advance : -1, 0, 1024, true);
            if (!cell)
                return false;

            // compare before writing: idempotent and cheap (49 bytes)
            if (slot[0] == cell->width &&
                memcmp(slot + 1, cell->bits, symbolBytes - 1) == 0)
                return true;

            slot[0] = cell->width;                           // same value as before
            memcpy(slot + 1, cell->bits, symbolBytes - 1);
            SetStage(6);                                     // engine data now holds our cell
            ++g_drawn;
            return true;
        }

        // ------------------------------------------------------------------
        //  Mode=aa: antialiased text with zero control-flow modification.
        // ------------------------------------------------------------------
        bool DrawAA(void* bitFont, unsigned int ch, int x, int y, int colorArg, unsigned int callerEsp)
        {
            if (!g_tried)
                Init();
            if (!bitFont || !g_ready)
                return false;

            const unsigned char* bf = (const unsigned char*)bitFont;
            const unsigned char* internal = *(const unsigned char* const*)(bf + BF_INTERNAL);
            if (!internal)
                return false;

            void* base = *(void* const*)(bf + BF_BUFFER);
            const int pitch = *(const int*)(bf + BF_PITCH);
            if (!base || pitch <= 0)
                return false;

            const unsigned short* symTable = *(const unsigned short* const*)(internal + IF_SYMTABLE);
            const unsigned int symbolBytes = *(const unsigned int*)(internal + IF_SYMBOLBYTES);
            unsigned char* bitmaps = *(unsigned char* const*)(internal + IF_BITMAPS);
            const int lines = *(const int*)(internal + IF_LINES);
            if (!symTable || !bitmaps || lines != g_src.Lines())
                return false;
            if (symbolBytes != 1 + (unsigned int)(g_src.StrideBytes() * lines))
                return false;

            const unsigned int idx = symTable[ch & 0xFFFF];
            if (idx == 0)
                return false;

            unsigned char* slot = bitmaps + (size_t)(idx - 1) * symbolBytes;
            // Antialiased path: we know the pen position, so the fractional
            // advance can be carried across glyphs (subpixel positioning)
            const int gameAdvance = OrigAdvance(idx, slot[0]);   // the engine's own advance (remembered)
            // fixed-size UI boxes keep the engine's metrics: no scaling, no
            // natural advance, no subpixel drift
            const bool keepGame = CallerWantsGameMetrics(callerEsp);
            if (keepGame && (g_drawn % 512) == 0)
                Log::Note("M1 metrics: excepted caller (stack 0x%08X) keeps the engine advance (x=%d wch=U+%04X)", callerEsp, x, ch);
            const double trueAdv = (!keepGame && g_advScale > 1.0) ? gameAdvance * g_advScale
                                                                  : (double)gameAdvance;
            int published = gameAdvance;
            const int phase = keepGame ? 0 : SubpixelPhase(x, trueAdv, &published);
            const int advance = (g_vecMetrics && !keepGame) ? -1 : published;

            const GlyphCell* cell = g_src.Get(ch, advance > 0 ? advance : -1, phase);
            if (!cell)
                return false;

            // with natural metrics the engine must advance (and measure) by our
            // own width, so it is written into the glyph's advance byte
            if (g_vecMetrics && slot[0] != cell->width)
                slot[0] = cell->width;

            const unsigned short color = (colorArg == -1)
                ? *(const unsigned short*)(bf + BF_COLOR)
                : (unsigned short)colorArg;

            const int* bounds = (const int*)(bf + BF_BOUNDS);
            Target t;
            t.base  = (unsigned short*)base;
            t.pitch = pitch;
            t.clipL = bounds[0] > 0 ? bounds[0] : 0;
            t.clipT = bounds[1] > 0 ? bounds[1] : 0;
            t.clipR = bounds[2] < pitch - 1 ? bounds[2] : pitch - 1;
            t.clipB = bounds[3];
            if (t.clipR < t.clipL || t.clipB < t.clipT)
                return false;

            // 1) our antialiased pixels go in first ...
            DrawCellAA(t, *cell, x, y, lines, color, RGB565);

            // 2) ... and the engine's own pass must draw nothing over them, so
            //    its glyph bitmap is zeroed UNCONDITIONALLY.  (A "clear it only
            //    if it looks non-empty" shortcut silently skipped most glyphs:
            //    the top-left and bottom-right bytes of our cells are usually
            //    zero already, which left the engine drawing its own bitmap on
            //    top of our antialiased glyph -> visibly doubled, smeared text.)
            memset(slot + 1, 0, symbolBytes - 1);

            SetStage(6);
            ++g_drawn;
            return true;
        }

        void NoteException()
        {
            ++g_reason[R_Exception];
        }

        // Read-only dump of one BitFont object, once per distinct pointer.
        // This is the empirical check of every offset the takeover relies on,
        // and it runs in observe mode too (so no drawing behaviour is needed to
        // validate the layout).
        void Probe(void* bitFont, unsigned int ch, int x, int y, int colorArg)
        {
            if (!bitFont || !Cfg::Probe())
                return;

            static const void* seen[8] = { 0 };
            static int seenCount = 0;

            for (int i = 0; i < seenCount; ++i)
                if (seen[i] == bitFont)
                    return;
            if (seenCount >= 8)
                return;
            seen[seenCount++] = bitFont;

            const unsigned char* bf = (const unsigned char*)bitFont;
            const unsigned char* internal = NULL;
            void* base = NULL;
            int pitch = 0, lines = 0, symBytes = 0;
            const unsigned short* symTable = NULL;
            const int* bounds = NULL;
            unsigned short color = 0;

            __try
            {
                internal = *(const unsigned char* const*)(bf + BF_INTERNAL);
                base     = *(void* const*)(bf + BF_BUFFER);
                pitch    = *(const int*)(bf + BF_PITCH);
                color    = *(const unsigned short*)(bf + BF_COLOR);
                bounds   = (const int*)(bf + BF_BOUNDS);
                if (internal)
                {
                    lines    = *(const int*)(internal + IF_LINES);
                    symBytes = (int)*(const unsigned int*)(internal + IF_SYMBOLBYTES);
                    symTable = *(const unsigned short* const*)(internal + IF_SYMTABLE);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Log::Note("PROBE bf=0x%p <unreadable>", bitFont);
                return;
            }

            unsigned int idx = 0, advance = 0;
            if (symTable && ch < 0x10000)
            {
                idx = symTable[ch & 0xFFFF];
                if (idx)
                {
                    const unsigned char* bitmaps = *(const unsigned char* const*)(internal + IF_BITMAPS);
                    if (bitmaps && symBytes > 0)
                        advance = bitmaps[(size_t)(idx - 1) * symBytes];
                }
            }

            Log::Note("PROBE bf=0x%p internal=0x%p base=0x%p pitch=%d color=0x%04X "
                      "bounds=%d,%d,%d,%d lines=%d symBytes=%d firstCh=U+%04X idx=%u advance=%u "
                      "(x=%d y=%d colorArg=%d mode=%d)",
                      bitFont, internal, base, pitch, color,
                      bounds ? bounds[0] : -1, bounds ? bounds[1] : -1,
                      bounds ? bounds[2] : -1, bounds ? bounds[3] : -1,
                      lines, symBytes, ch, idx, advance, x, y, colorArg, Cfg::Mode());
        }

        const char* ReasonSummary()
        {
            size_t used = 0;
            g_reasonLine[0] = 0;
            for (int i = 0; i < R_Count; ++i)
            {
                if (!g_reason[i])
                    continue;
                const int n = _snprintf_s(g_reasonLine + used, sizeof(g_reasonLine) - used,
                                          _TRUNCATE, "%s%s=%llu", used ? " " : "",
                                          kReasonNames[i], g_reason[i]);
                if (n <= 0)
                    break;
                used += (size_t)n;
            }
            if (!used)
                _snprintf_s(g_reasonLine, sizeof(g_reasonLine), _TRUNCATE, "none");
            return g_reasonLine;
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
