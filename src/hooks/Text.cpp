// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "LayoutState.h"

// Text-call observation, loading surface tracking and tooltip block placement.
using namespace vt::hooks;

// ---------------------------------------------------------------------------
// Drawing::PrintUnicode @ 0x4A61C0: observe the format string at [esp+8].
//   [esp+4] is output Point2D*, [esp+C] is Surface*. Native formatting and
//   the downstream draw call continue unchanged (see YRAddresses.h).
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::Drawing_PrintUnicode, VT_Hook_Drawing_PrintUnicode, yra::Drawing_PrintUnicodeSz)
{
    TextArg arg;
    arg.ptr = (const wchar_t *)(uintptr_t)R->Stack32(8);
    arg.offset = 8;
    arg.fromReg = false;
    ReportText(vt::Hook_Drawing_PrintUnicode, R, arg, 6, 4, "");
    return 0;
}

// BitText::Print: ECX=BitText; stack +4=font, +8=surface, +C=text,
// +10..=X,Y,W,H. In draw mode, register loading-screen BSurface storage
// before BitFont::Lock; observation and native Print then continue.
VT_DEFINE_HOOK(yra::BitText_Print, VT_Hook_BitText_Print, yra::BitText_PrintSz)
{
    if (vt::Cfg::Mode() == vt::Cfg::Mode_Draw)
    {
        __try
        {
            vt::Presentation32::TrackTextSurface((void *)(uintptr_t)R->Stack32(8));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt::Takeover::NoteException();
        }
    }
    TextArg arg;
    arg.ptr = (const wchar_t *)(uintptr_t)R->Stack32(0xC);
    arg.offset = 0xC;
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
    if (vt::Cfg::Mode() == vt::Cfg::Mode_Draw)
    {
        __try
        {
            vt::Presentation32::TrackTextSurface((void *)(uintptr_t)R->Stack32(8));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt::Takeover::NoteException();
        }
    }
    if (R->Stack32(0) == 0x00479041u)
    {
        const TooltipLayout pending = t_tooltip;
        t_tooltip = {};
        if (pending.font == (void *)(uintptr_t)R->Stack32(4) &&
            pending.text == (const wchar_t *)(uintptr_t)R->Stack32(0xC) && pending.drawHeight > 0 &&
            pending.drawHeight <= 32768 && RangeOk((void *)(uintptr_t)(R->ESP() + 0x14), 12))
        {
            // Native popup adds 2px above/below the measured ink. Translate
            // the complete text block into it; native row spacing stays intact.
            *(int *)(uintptr_t)(R->ESP() + 0x14) = (int)R->Stack32(0x14) - pending.top;
            // DrawText uses H as a row budget, separate from the font's clip
            // rectangle. Restore the captured native height + 4px budget so
            // the compact popup does not prevent the last row from running.
            *(int *)(uintptr_t)(R->ESP() + 0x1C) = pending.drawHeight;
        }
    }
    TextArg arg;
    arg.ptr = (const wchar_t *)(uintptr_t)R->Stack32(0xC);
    arg.offset = 0xC;
    arg.fromReg = false;
    ReportText(vt::Hook_BitText_DrawText, R, arg, 10, 4, "");
    return 0;
}
