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
}
