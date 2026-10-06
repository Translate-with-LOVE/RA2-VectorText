#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ===========================================================================
//  Takeover -- replaces the *glyph pixels* the engine draws, one glyph at a
//  time, by hooking BitFont::Blit (0x434120).
//
//  Why per-glyph and not per-string:
//    * the engine keeps doing layout, wrapping, alignment, clipping, shadows
//      and the per-character `reveal` colour ramp -> zero risk of drift;
//    * BitFont::Lock has already run, so the locked 16-bit surface and its
//      pitch are simply read from the BitFont object (no Surface guesswork);
//    * the function's return value is the next pen X, so we return
//      X + <original advance> and the layout stays byte-identical.
//  We only ever *write* pixels ourselves -- the M1 plan B requirement.
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
        // On success, *newX = x + <advance of the original glyph>.
        bool TryBlit(void* bitFont, unsigned int ch, int x, int y, int colorArg, int* newX);

        // Mode=swap: write our rasterised cell into the *game's own* glyph slot
        // in memory, then let the engine draw it.  No control flow, no stack
        // manipulation -- the engine keeps doing addressing, clipping, shadows
        // and the reveal ramp, it just draws vector shapes instead of bitmaps.
        // Returns true when the font data now holds our glyph.
        bool SwapGlyph(void* bitFont, unsigned int ch);

        // Mode=aa: antialiased text without touching the call flow.
        //   1. we blend our vector glyph into the game's 16-bit surface
        //      ourselves (that is where the antialiasing comes from),
        //   2. we zero the *bitmap* of the engine's glyph slot for this
        //      character (the advance byte stays), so the engine's own pass
        //      writes nothing over our pixels,
        //   3. the hook returns 0: the engine runs normally, its epilogue
        //      restores the registers, and its return value (X + advance) is
        //      the correct pen position.
        bool DrawAA(void* bitFont, unsigned int ch, int x, int y, int colorArg);

        // Read-only field dump, logged once per distinct BitFont object.  Runs
        // in observe mode too, so a single safe run proves (or disproves) the
        // runtime layout the takeover relies on before draw mode is enabled.
        void Probe(void* bitFont, unsigned int ch, int x, int y, int colorArg);

        // Why glyphs were handed back to the engine, as a short summary string.
        const char* ReasonSummary();

        // Called by the hook when its SEH guard caught something.
        void NoteException();

        // Diagnostics: which step the takeover reached (the FINAL log line
        // reports it, so even a hard crash tells us how far we got), and
        // whether the direction flag was set on entry (CRT string routines
        // assume DF=0 and would otherwise write memory backwards).
        const char* StageName();
        void SetStage(int stage);
        void NoteDirectionFlag();

        // SkipOriginal=0 draws our glyph and then lets the engine draw its own
        // on top: identical drawing code, but the call flow is never modified.
        // Used to bisect "our drawing" from "our stack manipulation" in-game.
        bool SkipOriginal();

        // One-line diagnostic state, reported in the FINAL log summary so that
        // even a hard crash tells us how far the takeover got.
        void DiagLine(char* out, int cch);

        // Logs the first few skip-the-callee operations in full.
        void LogSkip(DWORD entryEsp, DWORD retAddr, DWORD newEsp, int newX, unsigned int ch);

        void Stats(unsigned long long* drawn, unsigned long long* skipped,
                   unsigned long long* failed, unsigned long long* unknown);
    }
}
