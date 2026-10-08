// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "LayoutState.h"

// Scoped native measurement capture and successful-return adjustment.
namespace vt::hooks
{
__declspec(thread) DynamicMeasurement t_measurement = {};
__declspec(thread) TooltipLayout t_tooltip = {};
__declspec(thread) MessageLayout t_message = {};
__declspec(thread) BackgroundMeasurement t_background = {};
void CaptureDynamicMeasurement(REGISTERS *R)
{
    // The engine reuses its text argument slot as a width accumulator.
    // Read the original arguments now; none of them may be reconstructed
    // from that slot in the successful-return hook.
    t_measurement = {};
    const DWORD caller = R->Stack32(0);
    if (t_background.entryEsp && caller == 0x00433EE6u && R->Stack32(0x14) == 0x004A59F6u &&
        t_background.text == (const wchar_t *)(uintptr_t)R->Stack32(4))
        t_background.valid =
            vt::Takeover::MeasureTextInkY((void *)(uintptr_t)R->ECX(), t_background.text, t_background.anchor,
                                          t_background.align, &t_background.ink) &&
            t_background.ink.lines == 1;
    const bool loading = caller == 0x00553199u || caller == 0x005531EFu;
    // Startup computes each copyright row's X = screen width - measured
    // width - 10. It then prints with left alignment, so the old bitmap
    // measurement must be replaced before that anchor is calculated.
    const bool startup = caller == 0x00433EE6u &&
        (R->Stack32(0x14)==0x00531459u || R->Stack32(0x14)==0x005314B3u);
    const bool message = caller == 0x00433EE6u && R->Stack32(0x14) == 0x00623A81u;
    if (message)
        t_message = {};
    // Native tooltip measures before adding 8px horizontal padding and
    // clamping its X against the selected surface width (0x478F2D..5A).
    const bool tooltip = caller == 0x00478F0Bu;
    if (tooltip)
        t_tooltip = {};
    int *output = (int *)(uintptr_t)R->Stack32(8);
    if ((!loading && !message && !tooltip && !startup) || !RangeOk(output, sizeof(int)))
        return;
    int measured = 0;
    if (vt::Takeover::MeasureDynamicWidth((void *)(uintptr_t)R->ECX(), (const wchar_t *)(uintptr_t)R->Stack32(4),
                                          (int)R->Stack32(0x10), &measured))
    {
        if(startup) {
            int padding=vt::Cfg::ConfigInt("LineWidthPadding",4);
            padding=padding<1 ? 1 : padding>32 ? 32 : padding;
            measured-=padding; // copyright has no background box to pad
        }
        t_measurement = {R->ESP(),
                         caller,
                         output,
                         measured,
                         loading,
                         tooltip,
                         message,
                         (void *)(uintptr_t)R->ECX(),
                         (const wchar_t *)(uintptr_t)R->Stack32(4)};
        t_measurement.startup=startup;
        if (tooltip)
        {
            t_measurement.height = (int *)(uintptr_t)R->Stack32(0xC);
            t_measurement.lineHeight = *(const int *)((const BYTE *)t_measurement.font + 0x1C);
            t_measurement.inkValid =
                RangeOk(t_measurement.height, sizeof(int)) &&
                vt::Takeover::MeasureTextInkY(t_measurement.font, t_measurement.text, 0, 0, &t_measurement.ink);
        }
        else if (message)
            t_measurement.inkValid =
                vt::Takeover::MeasureTextInkY(t_measurement.font, t_measurement.text, 0, 0, &t_measurement.ink) &&
                t_measurement.ink.lines == 1;
    }
}
} // namespace vt::hooks

using namespace vt::hooks;

// ---------------------------------------------------------------------------
// BitFont::GetTextDimension @ 0x433CF0 (capture caller-scoped UI metrics)
//   __thiscall: ECX = BitFont*, [esp+4]=pText [esp+8]=int* pWidth
//               [esp+0xC]=int* pHeight [esp+0x10]=nMaxWidth
// ---------------------------------------------------------------------------
VT_DEFINE_HOOK(yra::BitFont_GetTextDimension, VT_Hook_BitFont_GetTextDimension, yra::BitFont_GetTextDimensionSz)
{
    if (R->EFLAGS() & 0x400u)
        R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (vt::Takeover::DynamicTextWidthEnabled())
    {
        __try
        {
            CaptureDynamicMeasurement(R);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            t_measurement = {};
            vt::Takeover::NoteException();
        }
    }
    TextArg arg;
    arg.ptr = (const wchar_t *)(uintptr_t)R->Stack32(4);
    arg.offset = 4;
    arg.fromReg = false;

    char what[96];
    _snprintf_s(what, sizeof(what), _TRUNCATE, "pWidth=0x%08X pHeight=0x%08X maxW=%d ", R->Stack32(8), R->Stack32(0xC),
                (int)R->Stack32(0x10));
    ReportText(vt::Hook_BitFont_GetTextDimension, R, arg, 4, 4, what);
    return 0;
}

// Dynamic UI widths, after the engine has written its width/height outputs.
// Scoped callers preserve all other layout and wrapping measurements.
VT_DEFINE_HOOK(yra::BitFont_DimensionDone, VT_Hook_BitFont_DimensionDone, yra::BitFont_DimensionDoneSz)
{
    if (R->EFLAGS() & 0x400u)
        R->EFLAGS(R->EFLAGS() & ~0x400u);
    if (!vt::Takeover::DynamicTextWidthEnabled())
        return 0;
    __try
    {
        const DynamicMeasurement pending = t_measurement;
        if (pending.entryEsp && pending.entryEsp == R->ESP() + 0x20 && pending.caller == R->Stack32(0x20))
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
                    t_message = {pending.font, pending.text, pending.ink, true};
                if (pending.tooltip && pending.inkValid && RangeOk(pending.height, sizeof(int)))
                {
                    const int originalHeight = *pending.height;
                    const int inkHeight = pending.ink.bottom - pending.ink.top;
                    // Only explicit rows matching the engine's actual row count
                    // can use this height. Auto-wrapped rows keep native geometry.
                    if (pending.lineHeight >= 1 && pending.lineHeight <= 128 &&
                        originalHeight == pending.ink.lines * pending.lineHeight && originalHeight <= 32764 &&
                        inkHeight <= 32764)
                    {
                        *pending.height = inkHeight;
                        t_tooltip = {pending.font, pending.text, pending.ink.top, originalHeight + 4};
                        static LONG loggedHeight = 0;
                        if (InterlockedIncrement(&loggedHeight) <= 4)
                            vt::Log::Note("BACKGROUND height: tooltip old=%d new=%d top=%d rows=%d padding=4",
                                          originalHeight + 4, inkHeight + 4, pending.ink.top, pending.ink.lines);
                    }
                }
                static LONG logged = 0;
                if (InterlockedIncrement(&logged) <= 8)
                    vt::Log::Note("DYNAMIC width: %s old=%d new=%d caller=0x%08X",
                                  pending.loading   ? "loading"
                                  : pending.tooltip ? "tooltip"
                                  : pending.startup ? "startup copyright"
                                                    : "message",
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
