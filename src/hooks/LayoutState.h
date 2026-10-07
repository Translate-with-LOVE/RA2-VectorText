// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Support.h"

// Pending measurements belong to the current thread and are consumed once.
namespace vt::hooks
{
struct DynamicMeasurement
{
    DWORD entryEsp, caller;
    int *output;
    int measured;
    bool loading;
    bool tooltip;
    bool message;
    void *font;
    const wchar_t *text;
    int *height;
    vt::Takeover::InkY ink;
    int lineHeight;
    bool inkValid;
};
extern __declspec(thread) DynamicMeasurement t_measurement;
struct TooltipLayout
{
    void *font;
    const wchar_t *text;
    int top, drawHeight;
};
extern __declspec(thread) TooltipLayout t_tooltip;
struct MessageLayout
{
    void *font;
    const wchar_t *text;
    vt::Takeover::InkY ink;
    bool valid;
};
extern __declspec(thread) MessageLayout t_message;
struct BackgroundMeasurement
{
    DWORD entryEsp, caller;
    int *output;
    vt::Takeover::InkY ink;
    int margin;
    bool valid;
    const wchar_t *text;
    int anchor, align, y;
};
extern __declspec(thread) BackgroundMeasurement t_background;

void CaptureDynamicMeasurement(REGISTERS *R);
} // namespace vt::hooks
