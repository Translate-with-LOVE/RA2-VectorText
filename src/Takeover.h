#pragma once

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

        void Stats(unsigned long long* drawn, unsigned long long* skipped,
                   unsigned long long* failed, unsigned long long* unknown);
    }
}
