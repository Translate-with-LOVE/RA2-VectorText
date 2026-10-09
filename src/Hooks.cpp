// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GameAddresses.h"
#include "hooks/Support.h"

// Native glyph return/trampoline and row-boundary ABI adapters.
using namespace vt::hooks;

// ---------------------------------------------------------------------------
// BitFont::Blit  @ 0x434120   (one call per glyph -- the real hot path)
//   ECX = BitFont*, [esp+4]=wchar_t wch [esp+8]=X [esp+0xC]=Y [esp+0x10]=color
//
//   Mode=observe: count native calls; optional detail/probe logging reads
//   their text and font fields without replacing glyph pixels.
//   Mode=draw: try the prepared row plan, then the per-glyph fallback. Pixels
//   go through the presentation sidecar when accepted, otherwise RGB565.
//   A row plan returns native advance + tracking; the fallback returns its
//   selected integer advance + tracking. The engine still chooses row ranges,
//   Y positions, shadow passes and per-character reveal colours.
// ---------------------------------------------------------------------------
VT_HOOK_FUNC(VT_Hook_BitFont_Blit)
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

    // Read-only probe: logs each of the first eight distinct BitFont addresses
    // once, including observe mode, to inspect the fields used by takeover.
    vt::Log::Init(); // First glyph diagnostics precede the per-glyph counter.
    if (vt::Cfg::Probe())
    {
        __try
        {
            vt::Takeover::Probe((void *)(uintptr_t)R->ECX(), wch, x, y, color);
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
            drawn = vt::Takeover::TryLineBlit((void *)(uintptr_t)R->ECX(), wch, x, y, color, R->Stack32(0), &newX);
            if (!drawn)
                drawn = vt::Takeover::TryBlit((void *)(uintptr_t)R->ECX(), wch, x, y, color, &newX, R->Stack32(0));
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
            void *tramp = vt::Takeover::SkipTrampoline();
            if (!tramp)
                return 0; // no trampoline -> let the engine draw
            R->EAX((DWORD)newX);
            vt::Takeover::SetStage(9);
            vt::Takeover::LogSkip(entryEsp, retAddr, entryEsp + 0x14, newX, wch);
            return (DWORD)(uintptr_t)tramp; // trampoline acts as ret 0x10, preserving EAX
        }
    }

    if (vt::Log::WantBlitDetails())
    {
        wchar_t buf[4];
        buf[0] = (wchar_t)wch;
        buf[1] = 0;

        char extra[128];
        _snprintf_s(extra, sizeof(extra), _TRUNCATE, "X=%d Y=%d color=0x%04X", x, y, color & 0xFFFF);
        vt::Log::Call(vt::Hook_BitFont_Blit, (const void *)(uintptr_t)R->Stack32(0), buf, extra);
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
VT_HOOK_FUNC(VT_Hook_BitFont_DrawString)
{
    if (R->EFLAGS() & 0x400u)
        R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (!vt::Takeover::LineEnabled())
        return 0;
    __try
    {
        int count = (int)R->Stack32(0x10);
        if (!count)
            count = -1;
        vt::Takeover::BeginStringLine((void *)(uintptr_t)R->ECX(), (const wchar_t *)(uintptr_t)R->Stack32(4), count,
                                      (int)R->Stack32(8), (int)R->Stack32(0xC));
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
DWORD PrepareRichLine(REGISTERS *R, unsigned int caller)
{
    if (R->EFLAGS() & 0x400u)
        R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (!vt::Takeover::LineEnabled())
        return 0;
    __try
    {
        const DWORD end = R->Stack32(0x4C), start = R->EBP();
        if (end < start || ((end - start) & 1) || (end - start) / 2 > 2048)
            vt::Takeover::EndLine(NULL);
        else
            vt::Takeover::BeginLine((void *)(uintptr_t)R->Stack32(0x44), (const wchar_t *)(uintptr_t)start,
                                    (int)((end - start) / 2), (int)R->Stack32(0x50) + (int)R->EAX(),
                                    (int)R->Stack32(0x54), (int)R->Stack32(0x50), (int)R->Stack32(0x58),
                                    (int)R->Stack32(0x60), caller);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt::Takeover::EndLine(NULL);
        vt::Takeover::NoteException();
    }
    return 0;
}
} // namespace

VT_HOOK_FUNC(VT_Hook_BitText_LineBreak)
{
    return PrepareRichLine(R, game::Break_BlitReturn);
}
VT_HOOK_FUNC(VT_Hook_BitText_LineWrap)
{
    return PrepareRichLine(R, game::Wrap_BlitReturn);
}
VT_HOOK_FUNC(VT_Hook_BitText_LineLast)
{
    return PrepareRichLine(R, game::Last_BlitReturn);
}

VT_HOOK_FUNC(VT_Hook_BitFont_Unlock)
{
    vt::Takeover::EndLine((void *)(uintptr_t)R->ECX());
    return 0;
}

VT_HOOK_FUNC(VT_Hook_Drawing_LineBox)
{
    if (!vt::Takeover::LineEnabled())
        return 0;
    __try
    {
        const int *rect = (const int *)(uintptr_t)R->EDI();
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
                left = centre - half;
                right = centre + half;
                align = 1;
            }
            else if (flags & 2)
            {
                right = gameX + oldWidth;
                align = 2;
            }
            else
                left = gameX;
            vt::Takeover::SetLineBox((void *)(uintptr_t)R->EBX(), (const wchar_t *)(uintptr_t)R->Stack32(0x10), gameX,
                                     (int)R->EBP(), left, right - left, align);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        vt::Takeover::EndLine(NULL);
        vt::Takeover::NoteException();
    }
    return 0;
}
