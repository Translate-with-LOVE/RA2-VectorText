#pragma once
#include <windows.h>

// M0 logger: counts every intercepted text call and records each *distinct*
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

    // M1 configuration (VectorText.ini, read lazily in-process like the logger)
    namespace Cfg
    {
        enum Mode { Mode_Off = 0, Mode_Observe = 1, Mode_Draw = 2, Mode_Swap = 3, Mode_AA = 4 };

        void Load();                       // idempotent; called by the first accessor
        int  Mode();
        const char* FontFile();
        int  FontWeight();
        int  FontSizeLatin();
        int  FontSizeCJK();
        int  BaselineRow();
        bool FitToAdvance();
        bool AntiAlias();
        bool FallbackOnError();
        bool Probe();                      // log the runtime BitFont layout once
        int  StemDarkening();              // FreeType stem darkening (0 = off)
        double Gamma();                    // coverage gamma for the AA path
        int  ConfigInt(const char* key, int def);
        void ConfigStr(const char* key, const char* def, char* out, int cch);
    }
}
