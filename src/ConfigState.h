// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Config.h"
#include <windows.h>

// Private cached INI values consumed by logging. Production initialization is
// owned by Cfg::Load; ReadConfig is also exposed to isolated parser tests.
// Generic Cfg::Config* queries read the INI separately.
namespace vt::config
{
extern bool g_enabled, g_detailed, g_blitDetail;
extern int g_maxUnique, g_cfgMode;
extern DWORD g_flushMs;
extern char g_dir[MAX_PATH], g_logName[MAX_PATH];
void GetGameDir();
void ReadConfig();
} // namespace vt::config
