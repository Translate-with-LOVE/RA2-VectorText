// SPDX-FileCopyrightText: 2026 VectorText contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "ConfigState.h"
#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace vt::config
{
bool g_enabled = true;
bool g_detailed = true;
bool g_blitDetail = false;
int g_maxUnique = 4000;
DWORD g_flushMs = 2000;
char g_dir[MAX_PATH] = {0};
char g_logName[MAX_PATH] = "VectorText.log";
// Cached rendering values; loaded under the logger initialization lock.
int g_cfgMode = Cfg::Mode_Observe;
char g_cfgFont[MAX_PATH] = "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf";
int g_cfgWeight = 400;
int g_cfgSizeLatin = 13;
int g_cfgSizeCJK = 16;
int g_cfgBaseline = 13;
bool g_cfgFit = true;
bool g_cfgAA = true;
bool g_cfgProbe = true;
int g_cfgDarkening = 0;
double g_cfgGamma = 1.0;
bool g_cfgVecMetrics = false;
int g_cfgSS = 2;
bool g_cfgLinear = true;
bool g_cfgDither = true;
int g_cfgOutline = 0;
unsigned int g_cfgOutlineColor = 0x0000;
double g_cfgAdvScale = 1.0;
bool g_cfgLegacy1252 = true;
bool g_cfgHiDPI = true;

void GetGameDir()
{
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash)
        *(slash + 1) = 0;
    strncpy_s(g_dir, path, _TRUNCATE);
}

bool ReadBool(const char *ini, const char *key, bool def)
{
    char value[64] = {};
    GetPrivateProfileStringA("VectorText", key, "", value, sizeof(value), ini);
    char *first = value;
    while (isspace((unsigned char)*first))
        ++first;
    char *end = first + strlen(first);
    while (end > first && isspace((unsigned char)end[-1]))
        --end;
    *end = 0;
    if (!_stricmp(first, "true") || !_stricmp(first, "yes") || !_stricmp(first, "on"))
        return true;
    if (!_stricmp(first, "false") || !_stricmp(first, "no") || !_stricmp(first, "off"))
        return false;
    char *numberEnd = NULL;
    const long number = strtol(first, &numberEnd, 0);
    return numberEnd != first && !*numberEnd ? number != 0 : def;
}

void ReadConfig()
{
    char ini[MAX_PATH];
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);

    g_enabled = ReadBool(ini, "Enabled", true);
    g_detailed = ReadBool(ini, "Detailed", true);
    g_blitDetail = ReadBool(ini, "LogBitFontBlitDetails", false);
    g_maxUnique = GetPrivateProfileIntA("VectorText", "MaxUniqueStrings", 4000, ini);
    g_flushMs = (DWORD)GetPrivateProfileIntA("VectorText", "FlushIntervalMs", 2000, ini);
    GetPrivateProfileStringA("VectorText", "LogFileName", "VectorText.log", g_logName, MAX_PATH, ini);

    if (g_maxUnique < 16)
        g_maxUnique = 16;
    if (g_flushMs < 250)
        g_flushMs = 250;

    // ---- cached rendering configuration ---------------------------
    char mode[32] = {0};
    GetPrivateProfileStringA("VectorText", "Mode", "observe", mode, sizeof(mode), ini);
    if (!_stricmp(mode, "off"))
        g_cfgMode = Cfg::Mode_Off;
    else if (!_stricmp(mode, "draw"))
        g_cfgMode = Cfg::Mode_Draw;
    else
        g_cfgMode = Cfg::Mode_Observe;

    GetPrivateProfileStringA("VectorText", "FontFile", "C:\\Windows\\Fonts\\NotoSerifSC-VF.ttf", g_cfgFont, MAX_PATH,
                             ini);
    g_cfgWeight = GetPrivateProfileIntA("VectorText", "FontWeight", 400, ini);
    g_cfgSizeLatin = GetPrivateProfileIntA("VectorText", "FontSizeLatin", 13, ini);
    g_cfgSizeCJK = GetPrivateProfileIntA("VectorText", "FontSizeCJK", 16, ini);
    g_cfgBaseline = GetPrivateProfileIntA("VectorText", "BaselineRow", 13, ini);
    g_cfgFit = ReadBool(ini, "FitToAdvance", true);
    g_cfgAA = ReadBool(ini, "AntiAlias", true);
    g_cfgProbe = ReadBool(ini, "Probe", true);
    g_cfgDarkening = GetPrivateProfileIntA("VectorText", "StemDarkening", 0, ini);
    g_cfgSS = GetPrivateProfileIntA("VectorText", "Supersample", 2, ini);
    g_cfgLinear = ReadBool(ini, "LinearBlend", true);
    g_cfgDither = ReadBool(ini, "Dither", true);
    g_cfgLegacy1252 = ReadBool(ini, "LegacyCodepage1252", true);
    g_cfgHiDPI = ReadBool(ini, "HiDPI", true);
    g_cfgOutline = GetPrivateProfileIntA("VectorText", "Outline", 0, ini);
    {
        char oc[32] = {0};
        GetPrivateProfileStringA("VectorText", "OutlineColor", "0x0000", oc, sizeof(oc), ini);
        g_cfgOutlineColor = (unsigned int)strtoul(oc, NULL, 0) & 0xFFFFu;
    }
    if (g_cfgSS < 1)
        g_cfgSS = 1;
    if (g_cfgSS > 4)
        g_cfgSS = 4;
    {
        char m[32] = {0};
        char s[32] = {0};
        GetPrivateProfileStringA("VectorText", "Metrics", "scaled", m, sizeof(m), ini);
        GetPrivateProfileStringA("VectorText", "AdvanceScale", "1.05", s, sizeof(s), ini);
        g_cfgVecMetrics = !_stricmp(m, "vector");
        g_cfgAdvScale = (!_stricmp(m, "scaled")) ? atof(s) : 1.0;
        if (g_cfgAdvScale < 1.0)
            g_cfgAdvScale = 1.0;
        if (g_cfgAdvScale > 2.0)
            g_cfgAdvScale = 2.0;
    }
    {
        char buf[32] = {0};
        GetPrivateProfileStringA("VectorText", "Gamma", "1.0", buf, sizeof(buf), ini);
        g_cfgGamma = atof(buf);
        if (g_cfgGamma < 0.5)
            g_cfgGamma = 0.5;
        if (g_cfgGamma > 3.0)
            g_cfgGamma = 3.0;
    }

    if (g_cfgSizeLatin < 6)
        g_cfgSizeLatin = 6;
    if (g_cfgSizeCJK < 6)
        g_cfgSizeCJK = 6;
    if (g_cfgBaseline < 1)
        g_cfgBaseline = 1;
    if (g_cfgBaseline > 32)
        g_cfgBaseline = 32;
}

} // namespace vt::config

namespace vt
{
using namespace config;
// ---------------------------------------------------------------- Cfg ---
// Named rendering getters use Load() and the logger's cached INI values.
// Generic ConfigStr/ConfigBool/ConfigInt query the INI on each call; their
// callers cache settings where repeated reads would affect a hot path.
namespace Cfg
{
void Load()
{
    Log::Init();
}

int Mode()
{
    Load();
    return g_cfgMode;
}
const char *FontFile()
{
    Load();
    return g_cfgFont;
}
int FontWeight()
{
    Load();
    return g_cfgWeight;
}
int FontSizeLatin()
{
    Load();
    return g_cfgSizeLatin;
}
int FontSizeCJK()
{
    Load();
    return g_cfgSizeCJK;
}
int BaselineRow()
{
    Load();
    return g_cfgBaseline;
}
bool FitToAdvance()
{
    Load();
    return g_cfgFit;
}
bool AntiAlias()
{
    Load();
    return g_cfgAA;
}
bool Probe()
{
    Load();
    return g_cfgProbe;
}
int StemDarkening()
{
    Load();
    return g_cfgDarkening;
}
double Gamma()
{
    Load();
    return g_cfgGamma;
}
bool VectorMetrics()
{
    Load();
    return g_cfgVecMetrics;
}
double AdvanceScale()
{
    Load();
    return g_cfgAdvScale;
}
int Supersample()
{
    Load();
    return g_cfgSS;
}
bool LinearBlend()
{
    Load();
    return g_cfgLinear;
}
bool Dither()
{
    Load();
    return g_cfgDither;
}
int Outline()
{
    Load();
    return g_cfgOutline;
}
unsigned short OutlineColor()
{
    Load();
    return (unsigned short)g_cfgOutlineColor;
}
bool LegacyCodepage1252()
{
    Load();
    return g_cfgLegacy1252;
}
bool HiDPI()
{
    Load();
    return g_cfgHiDPI;
}

void ConfigStr(const char *key, const char *def, char *out, int cch)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    GetPrivateProfileStringA("VectorText", key, def, out, (DWORD)cch, ini);
}

bool ConfigBool(const char *key, bool def)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    return ReadBool(ini, key, def);
}

int ConfigInt(const char *key, int def)
{
    char ini[MAX_PATH];
    if (!g_dir[0])
        GetGameDir();
    _snprintf_s(ini, sizeof(ini), _TRUNCATE, "%sVectorText.ini", g_dir);
    return GetPrivateProfileIntA("VectorText", key, def, ini);
}
} // namespace Cfg
} // namespace vt
