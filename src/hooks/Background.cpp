// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "LayoutState.h"

// Sidebar/tooltip/message boxes; preserve native Y and Phobos fill ownership.
using namespace vt::hooks;

// ---------------------------------------------------------------------------
// Drawing::GetTextDimensions  @ 0x4A59E0   (tooltips, boxes, line wrapping)
//   __fastcall: ECX = RectangleStruct* out, EDX = const wchar_t* text,
//               [esp+4..] = Point2D location, WORD flags, int marginX, marginY
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::Drawing_GetTextDimensions, VT_Hook_Drawing_GetTextDimensions, yra::Drawing_GetTextDimensionsSz)
{
    TextArg arg;
    arg.ptr = (const wchar_t *)(uintptr_t)R->EDX();
    arg.offset = 0;
    arg.fromReg = true;
    ReportText(vt::Hook_Drawing_GetTextDimensions, R, arg, 4, 4, "");
    t_background = {};
    if (vt::Takeover::DynamicTextWidthEnabled() && R->Stack32(0) == 0x006A9DD1u)
    {
        __try
        {
            int *output = (int *)(uintptr_t)R->ECX();
            const int margin = (int)R->Stack32(0x14);
            if (margin >= 0 && margin <= 32 && RangeOk(output, 16) && RangeOk(arg.ptr, sizeof(wchar_t)))
            {
                const int padding = margin > 2 ? margin : 2;
                t_background = {R->ESP(),
                                R->Stack32(0),
                                output,
                                {},
                                padding + vt::Cfg::Outline(),
                                false,
                                arg.ptr,
                                (int)R->Stack32(4),
                                (int)((R->Stack32(0xC) >> 8) & 3),
                                (int)R->Stack32(8)};
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t_background = {};
            vt::Takeover::NoteException();
        }
    }
    return 0;
}

VT_DEFINE_HOOK(yra::Drawing_TextDimensionsDone, VT_Hook_Drawing_TextDimensionsDone, yra::Drawing_TextDimensionsDoneSz)
{
    __try
    {
        const BackgroundMeasurement pending = t_background;
        if (pending.valid && pending.entryEsp == R->ESP() + 0x10 && pending.caller == R->Stack32(0x10) &&
            pending.output == (int *)(uintptr_t)R->EBX())
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
                    vt::Log::Note("BACKGROUND height: sidebar old=%d new=%d top=%d bottom=%d margin=%d", oldHeight,
                                  (int)R->EDI(), pending.ink.top, pending.ink.bottom, pending.margin);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        t_background = {};
        vt::Takeover::NoteException();
    }
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
        if (pending.valid && pending.text == (const wchar_t *)(uintptr_t)R->EDI() &&
            pending.font == (void *)(uintptr_t)R->Stack32(0x104C) && RangeOk((const void *)(uintptr_t)R->ESI(), 16) &&
            RangeOk((const void *)(uintptr_t)(R->ESP() + 0x30), 16))
        {
            const int y = *(const int *)(uintptr_t)(R->ESI() + 4);
            int margin = vt::Cfg::Outline();
            if (margin < 1)
                margin = 1;
            if (margin > 2)
                margin = 2;
            // One extra pixel below joins adjacent 19px message rows without
            // moving the text or the background's top edge.
            const int height = pending.ink.bottom - pending.ink.top + margin * 2 + 1;
            if (y >= -32768 && y <= 32768 && height > 0 && height <= 128)
            {
                const int originalHeight = (int)R->EBP();
                const int x = *(const int *)(uintptr_t)R->ESI();
                int horizontal = vt::Cfg::ConfigInt("LineWidthPadding", 4);
                if (horizontal < 4)
                    horizontal = 4;
                if (horizontal > 32)
                    horizontal = 32;
                const int paddingX = horizontal / 2 + vt::Cfg::Outline();
                // Replayed native instruction writes EDX to the rectangle X.
                R->edx = x + pending.ink.left - paddingX;
                *(int *)(uintptr_t)(R->ESP() + 0x38) =
                    pending.ink.right - pending.ink.left + horizontal + vt::Cfg::Outline() * 2;
                *(int *)(uintptr_t)(R->ESP() + 0x34) = y + pending.ink.top - margin;
                // Native fill writes EBP after two pushes; Phobos writes it
                // directly through the rectangle pointer in EAX at 623A9F.
                R->ebp = height;
                static LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 4)
                    vt::Log::Note("BACKGROUND height: message old=%d new=%d top=%d bottom=%d margin=%d", originalHeight,
                                  height, pending.ink.top, pending.ink.bottom, margin);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        t_message = {};
        vt::Takeover::NoteException();
    }
    return 0;
}
