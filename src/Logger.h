// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>

// Logger: counts every intercepted text call and records each *distinct*
// string once, so the log answers "which UI text goes through which hook".
// Observation only -- nothing in the drawing pipeline is modified.

namespace vt
{
    enum HookId
    {
        Hook_Drawing_GetTextDimensions = 0,
        Hook_Drawing_PrintUnicode,
        Hook_BitFont_GetTextDimension,
        Hook_BitText_Print,
        Hook_BitText_DrawText,
        Hook_BitFont_Blit,
        Hook_Count
    };

    const char* HookName(int id);

    namespace Log
    {
        void Prepare();                    // DllMain: create the lock only (no file I/O)
        void Init();                       // lazy + idempotent; opens the log in *this* process
        void Shutdown();                   // final summary + close
        bool Enabled();
        bool Detailed();                   // false -> counters only
        bool WantBlitDetails();            // BitFont::Blit is per-glyph; off by default
        void Count(int hookId);            // counter only (hot paths)
        void Call(int hookId, const void* caller, const wchar_t* text, const char* extra);
        void Miss(int hookId, const void* caller, const char* dump);   // no readable string
        void Note(const char* fmt, ...);   // free-form line
    }

    // Configuration (VectorText.ini, read lazily in-process like the logger)
    namespace Cfg
    {
        enum Mode { Mode_Off = 0, Mode_Observe = 1, Mode_Draw = 2 };

        void Load();                       // idempotent; called by the first accessor
        int  Mode();
        const char* FontFile();
        int  FontWeight();
        int  FontSizeLatin();
        int  FontSizeCJK();
        int  BaselineRow();
        bool FitToAdvance();
        bool AntiAlias();
        bool Probe();                      // log the runtime BitFont layout once
        int  StemDarkening();              // FreeType stem darkening (0 = off)
        double Gamma();                    // coverage gamma for the AA path
        bool VectorMetrics();              // true: our own advances (natural metrics)
        double AdvanceScale();             // Metrics=scaled: advance = game * scale
        int  Supersample();                // 1 = off, 2 = rasterise at 2x
        bool LinearBlend();                // 32-bit path: blend in linear light
        bool Dither();                     // 32-bit path: dither the 16-bit quantisation
        int  Outline();                    // SDF-style outline width in pixels
        unsigned short OutlineColor();     // the ring colour (16-bit surface word)

        bool LegacyCodepage1252();          // render C1 slots using legacy CP1252 glyphs
        bool HiDPI();                      // allow automatic output-resolution text
        bool ConfigBool(const char* key, bool def);
        int  ConfigInt(const char* key, int def);
        void ConfigStr(const char* key, const char* def, char* out, int cch);
    }
}
