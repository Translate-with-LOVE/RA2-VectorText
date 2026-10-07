// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ===========================================================================
//  Takeover -- replaces the *glyph pixels* the engine draws, one glyph at a
//  time, by hooking BitFont::Blit (0x434120).
//
//  Optional LineRender precomputes natural X positions at verified row starts.
//  The engine keeps selecting row ranges/Y, shadow passes and reveal colours.
//  Without a valid line plan, the existing per-glyph rendering is the fallback:
//    * BitFont::Lock has already run, so the locked 16-bit surface and its
//      pitch are simply read from the BitFont object (no Surface guesswork);
//    * the return value is the next pen X: the per-glyph fallback uses its
//      configured integer advance, while a row plan preserves native advances.
//      Both paths add the native constant tracking value at BitFont+0x2C.
//  Glyph coverage is written through the native or presentation pixel writer.
// ===========================================================================

namespace vt
{
    namespace Takeover
    {
        bool Ready();                      // vector font loaded and usable
        bool Init();                       // lazy; reads VectorText.ini via Cfg

        // Try to draw `ch` at (x, y) instead of the bitmap glyph.
        // colorArg mirrors BitFont::Blit's 4th argument: -1 means "use the
        // font's current colour" (BitFont+0x24).
        // On success, *newX = x + glyph advance + native spacing at +0x2C.
        bool TryBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg, int* newX,
                     unsigned int caller = 0);

        // Prepare exactly the line selected by the engine. count=-1 reads a
        // bounded NUL-terminated DrawString; otherwise count is the number of
        // UTF-16 units in the engine-selected row. gameX is its legacy origin.
        // boxX/width/align describe the rich path's left/centre/right anchor.
        bool BeginLine(void* bitFont, const wchar_t* text, int count, int gameX,
                       int y, int boxX, int width, int align, unsigned int blitCaller, int scale1024 = 1024);
        void SetLineBox(void* bitFont, const wchar_t* text, int gameX, int y,
                        int boxX, int width, int align);
        bool BeginStringLine(void* bitFont, const wchar_t* text, int count, int x, int y);
        bool TryLineBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg,
                         unsigned int blitCaller, int* newX);
        void EndLine(void* bitFont);
        bool LineEnabled();
        // Only dynamic UI boxes use this result; game wrapping/reveal counts
        // retain their original metrics. Leaves an active draw plan untouched.
        bool DynamicTextWidthEnabled();
        bool MeasureDynamicWidth(void* bitFont, const wchar_t* text, int maxWidth, int* width);
        // Vertical raster bounds relative to the first row's Y. Explicit
        // newlines retain the native line height; bottom/right are exclusive.
        // Horizontal bounds include the supplied anchor and actual raster phase.
        struct InkY { int top, bottom, lines, left, right; };
        bool MeasureTextInkY(void* bitFont, const wchar_t* text, int anchorX, int align, InkY* ink);
        struct LineInfo { int count, widthQ, originQ, consumed, boxWidth, mixedAddedQ, tightenedQ, scale1024; };
        bool GetLineInfo(LineInfo* info);

        // Read-only field dump, once for each of the first eight distinct BitFont
        // addresses. Observe mode can inspect these fields before drawing;
        // the dump is evidence for those objects, not a proof of every caller.
        void Probe(void* bitFont, unsigned int ch, int x, int y, int colorArg);

        // Why glyphs were handed back to the engine, as a short summary string.
        const char* ReasonSummary();

        // Called by the hook when its SEH guard caught something.
        void NoteException();

        // In-memory takeover stage and whether DF was set at hook entry.
        // CRT string routines require DF=0. Shutdown reports this state, but
        // a hard crash may bypass shutdown and leave no final summary.
        const char* StageName();
        void SetStage(int stage);
        void NoteDirectionFlag();

        // The stack fix-up used instead of R->ESP (which Syringe drops).
        void* SkipTrampoline();

        // Format the current diagnostic state for the shutdown summary.
        // Already-written PROBE/REFUSE/SKIP records may help if shutdown fails.
        void DiagLine(char* out, int cch);

        // Logs the first few skip-the-callee operations in full.
        void LogSkip(DWORD entryEsp, DWORD retAddr, DWORD newEsp, int newX, unsigned int ch);

        void Stats(unsigned long long* drawn, unsigned long long* skipped,
                   unsigned long long* failed, unsigned long long* unknown);
    }
}
