// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <windows.h>
#include "Config.h"

// Logger: counts text-hook calls and, when detailed logging is enabled,
// aggregates distinct strings per hook up to MaxUniqueStrings. Logging itself
// does not alter glyph pixels; the hook/takeover modules own rendering changes.

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

const char *HookName(int id);

namespace Log
{
void Prepare();  // DllMain: create the lock only (no file I/O)
void Init();     // lazy + idempotent; opens the log in *this* process
void Shutdown(const char *finalSummary = nullptr); // caller-supplied final summary + close
bool Enabled();
bool Detailed();        // false -> counters only
bool WantBlitDetails(); // BitFont::Blit is per-glyph; off by default
void Count(int hookId); // counter only (hot paths)
void Call(int hookId, const void *caller, const wchar_t *text, const char *extra);
void Miss(int hookId, const void *caller, const char *dump); // no readable string
void Note(const char *fmt, ...);                             // free-form line
} // namespace Log

} // namespace vt
