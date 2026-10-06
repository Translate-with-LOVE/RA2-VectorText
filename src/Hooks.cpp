// ===========================================================================
//  VectorText M0 -- text pipeline observation hooks
//
//  Every hook returns 0, which tells Syringe to replay the original bytes and
//  continue: the game behaves exactly as it does without this DLL.
//
//  String arguments are read from the offsets established by YRpp + the first
//  log run, with a bounded fallback scan; calls without a readable string are
//  reported as MISS with a full register/argument dump (that is the evidence
//  used to pin the remaining signatures).
// ===========================================================================

#include "SyringeABI.h"
#include "YRAddresses.h"
#include "Logger.h"
#include "Takeover.h"
#include "PixelWriter.h"

#include <stdio.h>
#include <stdint.h>
#include <string>
#include <string.h>

namespace
{
    // ---------------------------------------------------------------- safety
    bool RangeOk(const void* p, size_t bytes)
    {
        if (!p)
            return false;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;
        const BYTE* end       = (const BYTE*)p + bytes;
        const BYTE* regionEnd = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
        return end <= regionEnd;
    }

    // Copy at most cap-1 wide characters.  No C++ objects in here: __try is not
    // allowed in functions that require object unwinding.
    bool SafeW(const void* p, wchar_t* dst, size_t cap)
    {
        if (!cap)
            return false;
        dst[0] = 0;
        if (!RangeOk(p, sizeof(wchar_t)))
            return false;

        __try
        {
            const wchar_t* s = (const wchar_t*)p;
            size_t i = 0;
            for (; i + 1 < cap; ++i)
            {
                const wchar_t c = s[i];
                if (c == 0)
                    break;
                dst[i] = c;
            }
            dst[i] = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            dst[cap - 1] = 0;
            return false;
        }
    }

    bool Printable(wchar_t c)
    {
        const unsigned int o = (unsigned int)c;
        if (o == 0x09 || o == 0x0A || o == 0x0D)          return true;   // line breaks in briefings
        if (o >= 0x20 && o < 0x7F)                          return true;   // ASCII
        if (o >= 0x2010 && o <= 0x203F)                     return true;   // punctuation
        if (o >= 0x3000 && o <= 0x303F)                     return true;   // CJK punctuation
        if (o >= 0x4E00 && o <= 0x9FFF)                     return true;   // CJK
        if (o >= 0xFF00 && o <= 0xFFEF)                     return true;   // fullwidth forms
        return false;
    }

    // >= 1 character, >= 90% printable: single characters are legitimate text
    // (digit rendering, width probes) and must not be dropped.
    bool LooksLikeText(const wchar_t* s)
    {
        if (!s || !*s)
            return false;
        int total = 0, good = 0;
        for (int i = 0; s[i] && i < 128; ++i)
        {
            ++total;
            if (Printable(s[i]))
                ++good;
        }
        return total >= 1 && good * 10 >= total * 9;
    }

    void PreviewPtr(const void* p, char* out, size_t cap)
    {
        if (!p)
        {
            _snprintf_s(out, cap, _TRUNCATE, "null");
            return;
        }

        unsigned char b[16];
        bool ok = false;
        __try
        {
            memcpy(b, p, sizeof(b));
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }

        if (!ok)
        {
            _snprintf_s(out, cap, _TRUNCATE, "0x%08X unreadable", (unsigned int)(uintptr_t)p);
            return;
        }

        static const char* H = "0123456789ABCDEF";
        char hex[64];
        int k = 0;
        for (int i = 0; i < 16; ++i)
        {
            hex[k++] = H[b[i] >> 4];
            hex[k++] = H[b[i] & 0x0F];
            hex[k++] = ' ';
        }
        hex[k] = 0;

        char wide[16];
        int n = 0;
        for (int i = 0; i + 1 < 16 && n < 8; i += 2)
        {
            const unsigned int cu = (unsigned int)(b[i] | (b[i + 1] << 8));
            if (cu == 0)
                break;
            wide[n++] = (cu >= 0x20 && cu < 0x7F) ? (char)cu : '?';
        }
        wide[n] = 0;

        _snprintf_s(out, cap, _TRUNCATE, "0x%08X [%s] w=\"%s\"",
                    (unsigned int)(uintptr_t)p, hex, wide);
    }

    std::string ArgDump(REGISTERS* R, int count, int firstOffset)
    {
        std::string s;
        char tmp[48];
        for (int i = 0; i < count; ++i)
        {
            const int off = firstOffset + i * 4;
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "esp+0x%X=0x%08X ", off, R->Stack32(off));
            s += tmp;
        }
        return s;
    }

    std::string RegsDump(REGISTERS* R)
    {
        char tmp[64];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "ecx=0x%08X edx=0x%08X ", R->ECX(), R->EDX());
        return std::string(tmp);
    }

    // Walk `count` stack dwords from esp+firstOffset, return the offset of the
    // first one pointing at plausible text (0 if none).
    int ScanForText(REGISTERS* R, int firstOffset, int count, wchar_t* buf, size_t cap)
    {
        for (int i = 0; i < count; ++i)
        {
            const int off = firstOffset + i * 4;
            if (SafeW((const void*)(uintptr_t)R->Stack32(off), buf, cap) && LooksLikeText(buf))
                return off;
        }
        buf[0] = 0;
        return 0;
    }

    // --------------------------------------------------- reporting helpers
    struct TextArg
    {
        const wchar_t* ptr;
        int            offset;      // 0 when the value did not come from the stack
        bool           fromReg;
    };

    void ReportText(int hookId, REGISTERS* R, TextArg arg, int nArgs, int firstArgOffset,
                    const char* what)
    {
        wchar_t buf[512];
        const void* caller = (const void*)(uintptr_t)R->Stack32(0);

        bool ok = false;
        if (arg.ptr && SafeW(arg.ptr, buf, sizeof(buf) / sizeof(buf[0])) && LooksLikeText(buf))
        {
            ok = true;
        }
        else if (!arg.fromReg)
        {
            // documented offset failed -> note which offset the string really is at
            const int off = ScanForText(R, firstArgOffset, nArgs, buf, sizeof(buf) / sizeof(buf[0]));
            if (off)
            {
                ok = true;
                arg.offset = off;
            }
        }

        if (ok)
        {
            std::string note = RegsDump(R);
            if (arg.offset)
            {
                char pref[48];
                _snprintf_s(pref, sizeof(pref), _TRUNCATE, "text@esp+0x%X ", arg.offset);
                note += pref;
            }
            else if (arg.fromReg)
            {
                note += "text@edx ";
            }
            if (what && *what)
                note += what;
            note += " ";
            note += ArgDump(R, nArgs, firstArgOffset);
            vt::Log::Call(hookId, caller, buf, note.c_str());
        }
        else
        {
            char prev[200], prev2[200];
            PreviewPtr(arg.ptr, prev, sizeof(prev));
            PreviewPtr((const void*)(uintptr_t)R->Stack32(firstArgOffset), prev2, sizeof(prev2));

            std::string dump = RegsDump(R);
            dump += ArgDump(R, nArgs, firstArgOffset);
            char tail[480];
            _snprintf_s(tail, sizeof(tail), _TRUNCATE, " expect=%s arg1=%s", prev, prev2);
            dump += tail;
            vt::Log::Miss(hookId, caller, dump.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing::GetTextDimensions  @ 0x4A59E0   (tooltips, boxes, line wrapping)
//   __fastcall: ECX = RectangleStruct* out, EDX = const wchar_t* text,
//               [esp+4..] = Point2D location, WORD flags, int marginX, marginY
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::Drawing_GetTextDimensions, VT_Hook_Drawing_GetTextDimensions,
               yra::Drawing_GetTextDimensionsSz)
{
    TextArg arg;
    arg.ptr      = (const wchar_t*)(uintptr_t)R->EDX();
    arg.offset   = 0;
    arg.fromReg  = true;
    ReportText(vt::Hook_Drawing_GetTextDimensions, R, arg, 4, 4, "");
    return 0;
}

// ---------------------------------------------------------------------------
// Drawing::PrintUnicode  @ 0x4A61C0   (48 direct call sites -> main text API)
//   Log evidence: the string is the second stack argument (esp+8) in 203 of
//   205 first-seen calls; esp+4 is the destination/surface-ish argument.
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::Drawing_PrintUnicode, VT_Hook_Drawing_PrintUnicode,
               yra::Drawing_PrintUnicodeSz)
{
    TextArg arg;
    arg.ptr     = (const wchar_t*)(uintptr_t)R->Stack32(8);
    arg.offset  = 8;
    arg.fromReg = false;
    ReportText(vt::Hook_Drawing_PrintUnicode, R, arg, 6, 4, "");
    return 0;
}

// ---------------------------------------------------------------------------
// BitFont::GetTextDimension  @ 0x433CF0   (metrics; 27 direct call sites)
//   __thiscall: ECX = BitFont*, [esp+4]=pText [esp+8]=int* pWidth
//               [esp+0xC]=int* pHeight [esp+0x10]=nMaxWidth
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::BitFont_GetTextDimension, VT_Hook_BitFont_GetTextDimension,
               yra::BitFont_GetTextDimensionSz)
{
    TextArg arg;
    arg.ptr     = (const wchar_t*)(uintptr_t)R->Stack32(4);
    arg.offset  = 4;
    arg.fromReg = false;

    char what[96];
    _snprintf_s(what, sizeof(what), _TRUNCATE, "pWidth=0x%08X pHeight=0x%08X maxW=%d ",
                R->Stack32(8), R->Stack32(0xC), (int)R->Stack32(0x10));
    ReportText(vt::Hook_BitFont_GetTextDimension, R, arg, 4, 4, what);
    return 0;
}

// ---------------------------------------------------------------------------
// BitText::Print  @ 0x434B90   (single line draw)
//   ECX = BitText* (unused by the callee), args:
//   [esp+4]=BitFont* [esp+8]=Surface* [esp+0xC]=const wchar_t* [esp+0x10..]=X,Y,W,H
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::BitText_Print, VT_Hook_BitText_Print, yra::BitText_PrintSz)
{
    TextArg arg;
    arg.ptr     = (const wchar_t*)(uintptr_t)R->Stack32(0xC);
    arg.offset  = 0xC;
    arg.fromReg = false;
    ReportText(vt::Hook_BitText_Print, R, arg, 7, 4, "");
    return 0;
}

// ---------------------------------------------------------------------------
// BitText::DrawText  @ 0x434CD0   (formatting / colour / alignment path)
//   ECX = BitText*, [esp+4]=BitFont* [esp+8]=Surface* [esp+0xC]=const wchar_t*
//   [esp+0x10..]=X,Y,W,H, then colour/align extras
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::BitText_DrawText, VT_Hook_BitText_DrawText, yra::BitText_DrawTextSz)
{
    TextArg arg;
    arg.ptr     = (const wchar_t*)(uintptr_t)R->Stack32(0xC);
    arg.offset  = 0xC;
    arg.fromReg = false;
    ReportText(vt::Hook_BitText_DrawText, R, arg, 10, 4, "");
    return 0;
}

// ---------------------------------------------------------------------------
// BitFont::Blit  @ 0x434120   (one call per glyph -- the real hot path)
//   ECX = BitFont*, [esp+4]=wchar_t wch [esp+8]=X [esp+0xC]=Y [esp+0x10]=color
//
//   Mode=observe: counters only (LogBitFontBlitDetails=1 also records which
//   characters are drawn).
//   Mode=draw:    M1 takeover -- we rasterise the glyph with FreeType and write
//   the pixels ourselves into the (already locked) 16-bit surface, then return
//   the pen X the engine expects (X + the original advance).  Layout, wrapping,
//   alignment, shadows and the per-character colour ramp all stay the engine's.
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::BitFont_Blit, VT_Hook_BitFont_Blit, yra::BitFont_BlitSz)
{
    const unsigned int wch = R->Stack32(4) & 0xFFFFu;
    const int x = (int)R->Stack32(8);
    const int y = (int)R->Stack32(0xC);
    const int color = (int)R->Stack32(0x10);

    // Hook code must never assume DF=0: the CRT routines our rasteriser uses
    // (memcpy/memset) write *backwards* when the direction flag is set, which
    // corrupts memory instead of filling the glyph cell.
    if (R->EFLAGS() & 0x400u)
    {
        vt::Takeover::NoteDirectionFlag();
        R->EFLAGS(R->EFLAGS() & ~0x400u);
    }

    // Read-only probe: logs the runtime BitFont layout once per object -- in
    // observe mode too, so the takeover's assumptions can be checked against
    // real data before draw mode is ever enabled.
    if (vt::Cfg::Probe())
    {
        __try
        {
            vt::Takeover::Probe((void*)(uintptr_t)R->ECX(), wch, x, y, color);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt::Log::Note("PROBE <exception reading BitFont 0x%08X>", R->ECX());
        }
    }

    if (vt::Cfg::Mode() == vt::Cfg::Mode_Draw && wch != 0)
    {
        int newX = x;
        bool drawn = false;

        __try
        {
            drawn = vt::Takeover::TryBlit((void*)(uintptr_t)R->ECX(), wch, x, y, color, &newX);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            drawn = false;
            vt::Takeover::NoteException();
            vt::Log::Note("FALLBACK blit-exception wch=U+%04X x=%d y=%d", wch, x, y);
        }

        if (drawn && !vt::Takeover::SkipOriginal())
        {
            // Bisect mode: our pixels are written, but the engine still runs its
            // own Blit on top, so the call flow is untouched.  Nothing visible
            // changes; this only proves our drawing is safe in the real game.
            vt::Takeover::SetStage(9);
            vt::Log::Count(vt::Hook_BitFont_Blit);
            return 0;
        }

        if (drawn)
        {
            vt::Log::Count(vt::Hook_BitFont_Blit);
            const DWORD retAddr = (DWORD)R->Stack32(0);
            const DWORD entryEsp = R->ESP();
            R->ESP(entryEsp + 4 + 0x10);        // pop return address + 4 arguments
            R->EAX((DWORD)newX);
            vt::Takeover::SetStage(9);
            vt::Takeover::LogSkip(entryEsp, retAddr, R->ESP(), newX, wch);
            return retAddr;                     // skip the original implementation
        }
    }

    if (vt::Log::WantBlitDetails())
    {
        wchar_t buf[4];
        buf[0] = (wchar_t)wch;
        buf[1] = 0;

        char extra[128];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE, "X=%d Y=%d color=0x%04X",
                    x, y, color & 0xFFFF);
        vt::Log::Call(vt::Hook_BitFont_Blit, (const void*)(uintptr_t)R->Stack32(0), buf, extra);
    }
    else
    {
        vt::Log::Count(vt::Hook_BitFont_Blit);
    }

    return 0;
}
