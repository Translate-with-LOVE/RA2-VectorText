// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
// ===========================================================================
//  VectorText -- text observation and per-glyph / single-line X takeover.
//  Observation and row-boundary hooks replay original bytes. Successful Blit
//  takeover returns through a ret-10h trampoline with the engine's next pen X.
//
//  String arguments are read from the offsets established by YRpp + the first
//  log run, with a bounded fallback scan; calls without a readable string are
//  reported as MISS with a full register/argument dump (that is the evidence
//  used to pin the remaining signatures).
// ===========================================================================

#include "SyringeABI.h"
#include "YRAddresses.h"
#include "Presentation32.h"
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

    struct DynamicMeasurement
    {
        DWORD entryEsp, caller;
        int* output;
        int measured;
        bool loading;
        bool tooltip;
        bool message;
        void* font;
        const wchar_t* text;
        int* height;
        vt::Takeover::InkY ink;
        int lineHeight;
        bool inkValid;
    };
    static __declspec(thread) DynamicMeasurement t_measurement = {};
    struct TooltipLayout { void* font; const wchar_t* text; int top, drawHeight; };
    static __declspec(thread) TooltipLayout t_tooltip = {};
    struct MessageLayout { void* font; const wchar_t* text; vt::Takeover::InkY ink; bool valid; };
    static __declspec(thread) MessageLayout t_message = {};
    struct BackgroundMeasurement
    {
        DWORD entryEsp, caller;
        int* output;
        vt::Takeover::InkY ink;
        int margin;
        bool valid;
        const wchar_t* text;
        int anchor, align, y;
    };
    static __declspec(thread) BackgroundMeasurement t_background = {};

    void CaptureDynamicMeasurement(REGISTERS* R)
    {
        // The engine reuses its text argument slot as a width accumulator.
        // Read the original arguments now; none of them may be reconstructed
        // from that slot in the successful-return hook.
        t_measurement = {};
        const DWORD caller = R->Stack32(0);
        if (t_background.entryEsp && caller == 0x00433EE6u && R->Stack32(0x14) == 0x004A59F6u &&
            t_background.text == (const wchar_t*)(uintptr_t)R->Stack32(4))
            t_background.valid = vt::Takeover::MeasureTextInkY((void*)(uintptr_t)R->ECX(), t_background.text,
                t_background.anchor, t_background.align, &t_background.ink) && t_background.ink.lines == 1;
        const bool loading = caller == 0x00553199u || caller == 0x005531EFu;
        const bool message = caller == 0x00433EE6u && R->Stack32(0x14) == 0x00623A81u;
        if (message) t_message = {};
        // Native tooltip measures before adding 8px horizontal padding and
        // clamping its X against the selected surface width (0x478F2D..5A).
        const bool tooltip = caller == 0x00478F0Bu;
        if (tooltip) t_tooltip = {};
        int* output = (int*)(uintptr_t)R->Stack32(8);
        if ((!loading && !message && !tooltip) || !RangeOk(output, sizeof(int))) return;
        int measured = 0;
        if (vt::Takeover::MeasureDynamicWidth((void*)(uintptr_t)R->ECX(),
            (const wchar_t*)(uintptr_t)R->Stack32(4), (int)R->Stack32(0x10), &measured))
        {
            t_measurement = { R->ESP(), caller, output, measured, loading, tooltip, message,
                (void*)(uintptr_t)R->ECX(), (const wchar_t*)(uintptr_t)R->Stack32(4) };
            if (tooltip)
            {
                t_measurement.height = (int*)(uintptr_t)R->Stack32(0xC);
                t_measurement.lineHeight = *(const int*)((const BYTE*)t_measurement.font + 0x1C);
                t_measurement.inkValid = RangeOk(t_measurement.height, sizeof(int)) &&
                    vt::Takeover::MeasureTextInkY(t_measurement.font, t_measurement.text, 0, 0, &t_measurement.ink);
            }
            else if (message)
                t_measurement.inkValid = vt::Takeover::MeasureTextInkY(t_measurement.font, t_measurement.text, 0, 0,
                    &t_measurement.ink) && t_measurement.ink.lines == 1;
        }
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
    t_background = {};
    if (vt::Takeover::DynamicTextWidthEnabled() && R->Stack32(0) == 0x006A9DD1u)
    {
        __try
        {
            int* output = (int*)(uintptr_t)R->ECX();
            const int margin = (int)R->Stack32(0x14);
            if (margin >= 0 && margin <= 32 && RangeOk(output, 16) && RangeOk(arg.ptr, sizeof(wchar_t)))
            {
                const int padding = margin > 2 ? margin : 2;
                t_background = { R->ESP(), R->Stack32(0), output, {},
                    padding + vt::Cfg::Outline(), false, arg.ptr,
                    (int)R->Stack32(4), (int)((R->Stack32(0xC) >> 8) & 3), (int)R->Stack32(8) };
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { t_background = {}; vt::Takeover::NoteException(); }
    }
    return 0;
}

VT_DEFINE_HOOK(yra::Drawing_TextDimensionsDone, VT_Hook_Drawing_TextDimensionsDone, yra::Drawing_TextDimensionsDoneSz)
{
    __try
    {
        const BackgroundMeasurement pending = t_background;
        if (pending.valid && pending.entryEsp == R->ESP() + 0x10 &&
            pending.caller == R->Stack32(0x10) && pending.output == (int*)(uintptr_t)R->EBX())
        {
            t_background = {};
            if (RangeOk(pending.output, 16))
            {
                const int oldHeight = (int)R->EDI();
                pending.output[0] = pending.ink.left - pending.margin;
                pending.output[2] = pending.ink.right - pending.ink.left + pending.margin * 2;
                pending.output[1] = pending.y + pending.ink.top - pending.margin;
                // Replayed native mov [ebx+0Ch],edi writes the final height.
                R->edi = pending.ink.bottom - pending.ink.top + pending.margin * 2;
                static LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 3)
                    vt::Log::Note("BACKGROUND height: sidebar old=%d new=%d top=%d bottom=%d margin=%d",
                        oldHeight, (int)R->EDI(), pending.ink.top, pending.ink.bottom, pending.margin);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { t_background = {}; vt::Takeover::NoteException(); }
    return 0;
}

// The campaign message path builds its own background rectangle rather than
// going through tooltip or Drawing::GetTextDimensions. Only this rectangle is
// adjusted; the message's Y, reveal loop and next row's spacing stay native.
// Run before 623A9F: Phobos owns that slot and consumes EBP as the height for
// its translucent fill. Both its fill and the original fill see our Y/height.
VT_DEFINE_HOOK(yra::Message_Background, VT_Hook_Message_Background, yra::Message_BackgroundSz)
{
    __try
    {
        const MessageLayout pending = t_message;
        t_message = {};
        if (pending.valid && pending.text == (const wchar_t*)(uintptr_t)R->EDI() &&
            pending.font == (void*)(uintptr_t)R->Stack32(0x104C) &&
            RangeOk((const void*)(uintptr_t)R->ESI(), 16) && RangeOk((const void*)(uintptr_t)(R->ESP()+0x30),16))
        {
            const int y = *(const int*)(uintptr_t)(R->ESI()+4);
            int margin = vt::Cfg::Outline();
            if (margin < 1) margin = 1;
            if (margin > 2) margin = 2;
            // One extra pixel below joins adjacent 19px message rows without
            // moving the text or the background's top edge.
            const int height = pending.ink.bottom - pending.ink.top + margin*2 + 1;
            if (y >= -32768 && y <= 32768 && height > 0 && height <= 128)
            {
                const int originalHeight = (int)R->EBP();
                const int x = *(const int*)(uintptr_t)R->ESI();
                int horizontal = vt::Cfg::ConfigInt("LineWidthPadding", 4);
                if (horizontal < 4) horizontal = 4;
                if (horizontal > 32) horizontal = 32;
                const int paddingX = horizontal/2 + vt::Cfg::Outline();
                // Replayed native instruction writes EDX to the rectangle X.
                R->edx = x + pending.ink.left - paddingX;
                *(int*)(uintptr_t)(R->ESP()+0x38) = pending.ink.right - pending.ink.left + horizontal + vt::Cfg::Outline()*2;
                *(int*)(uintptr_t)(R->ESP()+0x34) = y + pending.ink.top - margin;
                // Native fill writes EBP after two pushes; Phobos writes it
                // directly through the rectangle pointer in EAX at 623A9F.
                R->ebp = height;
                static LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 4)
                    vt::Log::Note("BACKGROUND height: message old=%d new=%d top=%d bottom=%d margin=%d",
                        originalHeight,height,pending.ink.top,pending.ink.bottom,margin);
            }
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { t_message = {}; vt::Takeover::NoteException(); }
    return 0;
}

// ---------------------------------------------------------------------------
// Drawing::PrintUnicode  @ 0x4A61C0   (48 direct call sites -> main text API)
//   Log evidence: the string is the second stack argument (esp+8) in 203 of
//   205 first-seen calls; esp+4 is output Point2D*, esp+C is Surface*.
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
    if (R->EFLAGS() & 0x400u) R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (vt::Takeover::DynamicTextWidthEnabled())
    {
        __try { CaptureDynamicMeasurement(R); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t_measurement = {};
            vt::Takeover::NoteException();
        }
    }
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

// Dynamic UI widths, after the engine has written its width/height outputs.
// Scoped callers preserve all other layout and wrapping measurements.
VT_DEFINE_HOOK(yra::BitFont_DimensionDone, VT_Hook_BitFont_DimensionDone, yra::BitFont_DimensionDoneSz)
{
    if (R->EFLAGS() & 0x400u) R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (!vt::Takeover::DynamicTextWidthEnabled()) return 0;
    __try
    {
        const DynamicMeasurement pending = t_measurement;
        if (pending.entryEsp && pending.entryEsp == R->ESP() + 0x20 &&
            pending.caller == R->Stack32(0x20))
        {
            t_measurement = {};
            if (RangeOk(pending.output, sizeof(int)))
            {
                // Loading uses one width for both layout and its visible box.
                // Tooltip also keeps its old width minimum to preserve wrapping.
                const int original = *pending.output;
                if ((!pending.loading && !pending.tooltip) || pending.measured > original)
                    *pending.output = pending.measured;
                if (pending.message && pending.inkValid)
                    t_message = { pending.font, pending.text, pending.ink, true };
                if (pending.tooltip && pending.inkValid && RangeOk(pending.height, sizeof(int)))
                {
                    const int originalHeight = *pending.height;
                    const int inkHeight = pending.ink.bottom - pending.ink.top;
                    // Only explicit rows matching the engine's actual row count
                    // can use this height. Auto-wrapped rows keep native geometry.
                    if (pending.lineHeight >= 1 && pending.lineHeight <= 128 &&
                        originalHeight == pending.ink.lines * pending.lineHeight &&
                        originalHeight <= 32764 && inkHeight <= 32764)
                    {
                        *pending.height = inkHeight;
                        t_tooltip = { pending.font, pending.text, pending.ink.top, originalHeight + 4 };
                        static LONG loggedHeight = 0;
                        if (InterlockedIncrement(&loggedHeight) <= 4)
                            vt::Log::Note("BACKGROUND height: tooltip old=%d new=%d top=%d rows=%d padding=4",
                                originalHeight + 4, inkHeight + 4, pending.ink.top, pending.ink.lines);
                    }
                }
                static LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 8)
                    vt::Log::Note("DYNAMIC width: %s old=%d new=%d caller=0x%08X",
                        pending.loading ? "loading" : pending.tooltip ? "tooltip" : "message",
                        original, *pending.output, pending.caller);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt::Takeover::NoteException();
    }
    return 0;
}

// BitText::Print: ECX=BitText; stack +4=font, +8=surface, +C=text,
// +10..=X,Y,W,H. This observation hook leaves the original call intact.
VT_DEFINE_HOOK(yra::BitText_Print, VT_Hook_BitText_Print, yra::BitText_PrintSz)
{
    if (vt::Cfg::Mode() == vt::Cfg::Mode_Draw) {
        __try { vt::Presentation32::TrackTextSurface((void*)(uintptr_t)R->Stack32(8)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { vt::Takeover::NoteException(); }
    }
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
    if (vt::Cfg::Mode() == vt::Cfg::Mode_Draw) {
        __try { vt::Presentation32::TrackTextSurface((void*)(uintptr_t)R->Stack32(8)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { vt::Takeover::NoteException(); }
    }
    if (R->Stack32(0) == 0x00479041u)
    {
        const TooltipLayout pending = t_tooltip;
        t_tooltip = {};
        if (pending.font == (void*)(uintptr_t)R->Stack32(4) &&
            pending.text == (const wchar_t*)(uintptr_t)R->Stack32(0xC) &&
            pending.drawHeight > 0 && pending.drawHeight <= 32768 && RangeOk((void*)(uintptr_t)(R->ESP() + 0x14), 12))
        {
            // Native popup adds 2px above/below the measured ink. Translate
            // the complete text block into it; native row spacing stays intact.
            *(int*)(uintptr_t)(R->ESP() + 0x14) = (int)R->Stack32(0x14) - pending.top;
            // DrawText uses H as a row budget, separate from the font's clip
            // rectangle. Keep the original budget so the last row still runs.
            *(int*)(uintptr_t)(R->ESP() + 0x1C) = pending.drawHeight;
        }
    }
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
            drawn = vt::Takeover::TryLineBlit((void*)(uintptr_t)R->ECX(), wch, x, y, color,
                                            R->Stack32(0), &newX);
            if (!drawn)
                drawn = vt::Takeover::TryBlit((void*)(uintptr_t)R->ECX(), wch, x, y, color, &newX,
                                             R->Stack32(0));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            drawn = false;
            vt::Takeover::EndLine(NULL);
            vt::Takeover::NoteException();
            vt::Log::Note("FALLBACK blit-exception wch=U+%04X x=%d y=%d", wch, x, y);
        }

        if (drawn)
        {
            vt::Log::Count(vt::Hook_BitFont_Blit);
            const DWORD retAddr = (DWORD)R->Stack32(0);
            const DWORD entryEsp = R->ESP();
            // R->ESP() would be dropped by Syringe's popad, so the stack fix-up
            // happens in our trampoline (see Takeover::SkipTrampoline)
            void* tramp = vt::Takeover::SkipTrampoline();
            if (!tramp)
                return 0;                       // no trampoline -> let the engine draw
            R->EAX((DWORD)newX);
            vt::Takeover::SetStage(9);
            vt::Takeover::LogSkip(entryEsp, retAddr, entryEsp + 0x14, newX, wch);
            return (DWORD)(uintptr_t)tramp;      // -> add esp,0x14 ; jmp return address
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

// The original loops still select characters, Y, reveal colours and passes.
// We only replace horizontal placement in their Blit calls. No font metric
// table is rewritten, so the engine's wrapping/height measurements stay intact.
VT_DEFINE_HOOK(yra::BitFont_DrawString, VT_Hook_BitFont_DrawString, yra::BitFont_DrawStringSz)
{
    if (R->EFLAGS() & 0x400u) R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (!vt::Takeover::LineEnabled()) return 0;
    __try
    {
        int count = (int)R->Stack32(0x10);
        if (!count) count = -1;
        vt::Takeover::BeginStringLine((void*)(uintptr_t)R->ECX(), (const wchar_t*)(uintptr_t)R->Stack32(4),
                                     count, (int)R->Stack32(8), (int)R->Stack32(0xC));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt::Takeover::EndLine(NULL);
        vt::Takeover::NoteException();
    }
    return 0;
}

namespace
{
    DWORD PrepareRichLine(REGISTERS* R, unsigned int caller)
    {
        if (R->EFLAGS() & 0x400u) R->EFLAGS(R->EFLAGS() & ~0x400u);
        if (!vt::Takeover::LineEnabled()) return 0;
        __try
        {
            const DWORD end = R->Stack32(0x4C), start = R->EBP();
            if (end < start || ((end - start) & 1) || (end - start) / 2 > 2048)
                vt::Takeover::EndLine(NULL);
            else
                vt::Takeover::BeginLine((void*)(uintptr_t)R->Stack32(0x44),
                    (const wchar_t*)(uintptr_t)start, (int)((end - start) / 2),
                    (int)R->Stack32(0x50) + (int)R->EAX(), (int)R->Stack32(0x54),
                    (int)R->Stack32(0x50), (int)R->Stack32(0x58), (int)R->Stack32(0x60), caller);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt::Takeover::EndLine(NULL);
            vt::Takeover::NoteException();
        }
        return 0;
    }
}

VT_DEFINE_HOOK(yra::BitText_LineBreak, VT_Hook_BitText_LineBreak, yra::BitText_LineSz)
{ return PrepareRichLine(R, 0x00434EA6u); }
VT_DEFINE_HOOK(yra::BitText_LineWrap, VT_Hook_BitText_LineWrap, yra::BitText_LineSz)
{ return PrepareRichLine(R, 0x004350E1u); }
VT_DEFINE_HOOK(yra::BitText_LineLast, VT_Hook_BitText_LineLast, yra::BitText_LineSz)
{ return PrepareRichLine(R, 0x004352BAu); }

VT_DEFINE_HOOK(yra::BitFont_Unlock, VT_Hook_BitFont_Unlock, yra::BitFont_UnlockSz)
{
    vt::Takeover::EndLine((void*)(uintptr_t)R->ECX());
    return 0;
}

VT_DEFINE_HOOK(yra::Drawing_LineBox, VT_Hook_Drawing_LineBox, yra::Drawing_LineBoxSz)
{
    if (!vt::Takeover::LineEnabled()) return 0;
    __try
    {
        const int* rect = (const int*)(uintptr_t)R->EDI();
        if (RangeOk(rect, 16) && rect[2] > 0 && rect[2] <= 32768)
        {
            int left = rect[0], right = left + rect[2]; // RectangleStruct uses X,Y,W,H.
            const int flags = (R->Stack32(0x50) >> 8) & 3;
            const int gameX = (int)R->ESI(), oldWidth = (int)R->Stack32(0x44);
            int align = 0;
            if (flags & 1)
            {
                const int centre = gameX + oldWidth / 2;
                const int half = centre - left < right - centre ? centre - left : right - centre;
                left = centre - half; right = centre + half; align = 1;
            }
            else if (flags & 2) { right = gameX + oldWidth; align = 2; }
            else left = gameX;
            vt::Takeover::SetLineBox((void*)(uintptr_t)R->EBX(),
                (const wchar_t*)(uintptr_t)R->Stack32(0x10), gameX, (int)R->EBP(), left, right - left, align);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt::Takeover::EndLine(NULL);
        vt::Takeover::NoteException();
    }
    return 0;
}
