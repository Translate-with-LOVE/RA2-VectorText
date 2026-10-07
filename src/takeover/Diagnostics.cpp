// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "State.h"

// Refusal counters, native-font probes and skip-trampoline diagnostics.
namespace vt::Takeover::detail
{
unsigned long long g_drawn = 0, g_skipped = 0, g_failed = 0, g_unknown = 0;
unsigned long long g_reason[R_Count] = {};
} // namespace vt::Takeover::detail

namespace vt::Takeover
{
using namespace detail;
static const char *const kReasonNames[R_Count] = {
    "font-not-ready", "no-internal-data", "surface-not-locked", "bad-font-data", "font-metrics-mismatch",
    "no-game-glyph",  "no-vector-glyph",  "bad-bounds",         "exception"};
static char g_reasonLine[512] = {0};

// In-memory diagnostics; shutdown logging is best effort after a crash.
static int g_stage = 0;
static bool g_sawDF = false;
static int g_skipLogged = 0;
static const char *const kStageNames[] = {"start",      "probe",   "init",          "fontdata", "bounds",
                                          "rasterised", "written", "skip-computed", "skipped",  "skip-dispatched"};
static const int kStageCount = (int)(sizeof(kStageNames) / sizeof(kStageNames[0]));

const char *StageName()
{
    if (g_stage < 0 || g_stage >= kStageCount)
        return "?";
    return kStageNames[g_stage];
}

void SetStage(int stage)
{
    g_stage = stage;
}

void NoteDirectionFlag()
{
    if (!g_sawDF)
    {
        g_sawDF = true;
        Log::Note("WARNING direction flag (DF) was SET at hook entry; clearing it "
                  "(CRT memcpy/memset would otherwise write memory backwards)");
    }
}

// Syringe restores registers with popad, which ignores the saved ESP slot.
// Jumping directly to Blit's caller would leave the return address and four
// arguments on the stack. This trampoline performs the equivalent of ret 0x10
// using EDX as scratch, preserving the new pen X in EAX.
//     8B 14 24   mov edx, [esp]      ; the real return address
//     83 C4 14   add esp, 0x14       ; drop it + the 4 arguments
//     52         push edx
//     C3         ret                 ; -> ESP = entry + 0x14, EIP = caller
void *SkipTrampoline()
{
    static void *code = NULL;
    if (code)
        return code;

    void *mem = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem)
        return NULL;

    unsigned char *p = (unsigned char *)mem;
    const unsigned char body[] = {
        0x8B, 0x14, 0x24, // mov edx, [esp]
        0x83, 0xC4, 0x14, // add esp, 0x14
        0x52,             // push edx
        0xC3              // ret
    };
    memcpy(p, body, sizeof(body));
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(body));
    code = mem;
    return code;
}

void DiagLine(char *out, int cch)
{
    _snprintf_s(out, (size_t)cch, _TRUNCATE, "lastStage=%s(%d) sawDF=%d skipped=%d", StageName(), g_stage,
                g_sawDF ? 1 : 0, g_skipLogged);
}

// Log the first three skip operations for stack/return inspection.
// A later crash may require more evidence than these early samples.
void LogSkip(DWORD entryEsp, DWORD retAddr, DWORD newEsp, int newX, unsigned int ch)
{
    ++g_skipLogged;
    if (g_skipLogged > 3)
        return;
    Log::Note("SKIP #%d ch=U+%04X entryESP=0x%08X retAddr=0x%08X newESP=0x%08X "
              "(delta=0x%X) newX=%d",
              g_skipLogged, ch, entryEsp, retAddr, newEsp, newEsp - entryEsp, newX);
}

void NoteException()
{
    ++g_reason[R_Exception];
}

// Read-only dump for the first eight distinct BitFont addresses, once each.
// It samples storage, bounds and one glyph's advance in observe or draw mode;
// it does not validate every offset or every later font instance.
void Probe(void *bitFont, unsigned int ch, int x, int y, int colorArg)
{
    if (!bitFont || !Cfg::Probe())
        return;

    static const void *seen[8] = {0};
    static int seenCount = 0;

    for (int i = 0; i < seenCount; ++i)
        if (seen[i] == bitFont)
            return;
    if (seenCount >= 8)
        return;
    seen[seenCount++] = bitFont;

    const unsigned char *bf = (const unsigned char *)bitFont;
    const unsigned char *internal = NULL;
    void *base = NULL;
    int pitch = 0, lines = 0, symBytes = 0;
    const unsigned short *symTable = NULL;
    const int *bounds = NULL;
    unsigned short color = 0;

    __try
    {
        internal = *(const unsigned char *const *)(bf + BF_INTERNAL);
        base = *(void *const *)(bf + BF_BUFFER);
        pitch = *(const int *)(bf + BF_PITCH);
        color = *(const unsigned short *)(bf + BF_COLOR);
        bounds = (const int *)(bf + BF_BOUNDS);
        if (internal)
        {
            lines = *(const int *)(internal + IF_LINES);
            symBytes = (int)*(const unsigned int *)(internal + IF_SYMBOLBYTES);
            symTable = *(const unsigned short *const *)(internal + IF_SYMTABLE);
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
            const unsigned char *bitmaps = *(const unsigned char *const *)(internal + IF_BITMAPS);
            if (bitmaps && symBytes > 0)
                advance = bitmaps[(size_t)(idx - 1) * symBytes];
        }
    }

    Log::Note("PROBE bf=0x%p internal=0x%p base=0x%p pitch=%d color=0x%04X "
              "bounds=%d,%d,%d,%d lines=%d symBytes=%d firstCh=U+%04X idx=%u advance=%u "
              "(x=%d y=%d colorArg=%d mode=%d)",
              bitFont, internal, base, pitch, color, bounds ? bounds[0] : -1, bounds ? bounds[1] : -1,
              bounds ? bounds[2] : -1, bounds ? bounds[3] : -1, lines, symBytes, ch, idx, advance, x, y, colorArg,
              Cfg::Mode());
}

const char *ReasonSummary()
{
    size_t used = 0;
    g_reasonLine[0] = 0;
    for (int i = 0; i < R_Count; ++i)
    {
        if (!g_reason[i])
            continue;
        const int n = _snprintf_s(g_reasonLine + used, sizeof(g_reasonLine) - used, _TRUNCATE, "%s%s=%llu",
                                  used ? " " : "", kReasonNames[i], g_reason[i]);
        if (n <= 0)
            break;
        used += (size_t)n;
    }
    if (!used)
        _snprintf_s(g_reasonLine, sizeof(g_reasonLine), _TRUNCATE, "none");
    return g_reasonLine;
}

void Stats(unsigned long long *drawn, unsigned long long *skipped, unsigned long long *failed,
           unsigned long long *unknown)
{
    if (drawn)
        *drawn = g_drawn;
    if (skipped)
        *skipped = g_skipped;
    if (failed)
        *failed = g_failed;
    if (unknown)
        *unknown = g_unknown;
}
} // namespace vt::Takeover
