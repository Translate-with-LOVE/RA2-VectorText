// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Config.h"
#include <windows.h>

// Private INI/logging bridge. ReadConfig runs under Log::Init's existing lock;
// named rendering getters reuse those cached values. Generic Cfg::Config*
// queries read the INI separately.
namespace vt::config
{
extern bool g_enabled, g_detailed, g_blitDetail;
extern int g_maxUnique, g_cfgMode;
extern DWORD g_flushMs;
extern char g_dir[MAX_PATH], g_logName[MAX_PATH];
void GetGameDir();
void ReadConfig();
} // namespace vt::config
