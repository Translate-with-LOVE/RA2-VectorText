// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GameAddresses.h"
#include "LayoutState.h"
#include "../PixelWriter.h"

// Text-call observation, loading surface tracking and tooltip block placement.
using namespace vt::hooks;

namespace
{
thread_local DWORD subtitleDoneEsp = 0;
}

// ---------------------------------------------------------------------------
// Drawing::PrintUnicode @ 0x4A61C0: observe the format string at [esp+8].
//   [esp+4] is output Point2D*, [esp+C] is Surface*. Native formatting and
//   the downstream draw call continue unchanged (see YRAddresses.h).
// ---------------------------------------------------------------------------
VT_HOOK_FUNC(VT_Hook_Drawing_PrintUnicode)
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
VT_HOOK_FUNC(VT_Hook_BitText_Print)
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
VT_HOOK_FUNC(VT_Hook_BitText_DrawText)
{
    if (R->Stack32(0) == game::Subtitle_DrawDone && vt::Cfg::Mode() == vt::Cfg::Mode_Draw)
    {
        // DrawText pops ten arguments; its caller resumes 44 bytes above
        // this entry. Capture real draw positions, including wrapped rows,
        // fallback layout, negative bearings and the independent output raster.
        subtitleDoneEsp = R->ESP() + 0x2C;
        vt::BeginTextInkCapture(vt::Cfg::ConfigBool("SubtitleOutline", true));
    }
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
    if (R->Stack32(0) == game::Tooltip_DrawReturn)
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

VT_HOOK_FUNC(VT_Hook_Subtitle_DrawDone)
{
    vt::TextInkRect ink{};
    const bool sameFrame = subtitleDoneEsp && subtitleDoneEsp == R->ESP();
    subtitleDoneEsp = 0;
    if (!vt::EndTextInkCapture(&ink) || !sameFrame || !RangeOk((void *)(uintptr_t)R->ESP(), 0x28))
        return 0;
    const int x = (int)R->Stack32(0x10), y = (int)R->EDI();
    const int width = (int)R->Stack32(0x14), height = (int)R->Stack32(0x24);
    if (x < -32768 || x > 32768 || y < -32768 || y > 32768 || width < 0 || width > 32768 || height < 0 ||
        height > 32768)
        return 0;
    const int left = ink.left < x ? ink.left : x;
    const int top = ink.top < y ? ink.top : y;
    const int right = ink.right > x + width ? ink.right : x + width;
    const int bottom = ink.bottom > y + height ? ink.bottom : y + height;
    *(int *)(uintptr_t)(R->ESP() + 0x10) = left;
    *(int *)(uintptr_t)(R->ESP() + 0x14) = right - left;
    *(int *)(uintptr_t)(R->ESP() + 0x24) = bottom - top;
    R->edi = top;
    static LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 3)
        vt::Log::Note("SUBTITLE erase: native=(%d,%d,%d,%d) ink=(%d,%d,%d,%d) dirty=(%d,%d,%d,%d)", x, y, width, height,
                      ink.left, ink.top, ink.right, ink.bottom, left, top, right - left, bottom - top);
    return 0;
}
