// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Explicitly identify hosts which load hook exports without injecting a game.
extern "C" __declspec(dllexport) int __cdecl VT_OfflineTestHost()
{
    return 1;
}
